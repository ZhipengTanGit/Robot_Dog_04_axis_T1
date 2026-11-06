#include "UartBridgePolicy.h"
#include <limits.h>
#include <string.h>

static bool boundary(const UartBridgeFrameState *state, uint8_t byte)
{
    return byte == '\n' || (state->pc_input && byte == '\r');
}
void UartBridgeFrame_Init(UartBridgeFrameState *state, bool pc_input)
{
    *state = (UartBridgeFrameState){ .pc_input = pc_input, .at_boundary = true };
}
void UartBridgeFrame_Invalidate(UartBridgeFrameState *state)
{
    state->discarding = true;
    state->at_boundary = false;
}
size_t UartBridgeFrame_Discard(UartBridgeFrameState *state, const uint8_t *data, size_t length)
{
    size_t discarded = 0;
    while (state->discarding && discarded < length) {
        if (boundary(state, data[discarded++])) {
            state->discarding = false;
            state->at_boundary = true;
        }
    }
    return discarded;
}
size_t UartBridgeFrame_Segment(const UartBridgeFrameState *state, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (boundary(state, data[i])) return i + 1;
    }
    return length;
}
void UartBridgeFrame_Forwarded(UartBridgeFrameState *state, const uint8_t *data, size_t length)
{
    if (length) state->at_boundary = boundary(state, data[length - 1]);
}
uint32_t UartBridgeCommand_ReserveTicks(size_t length, uint32_t baud, uint32_t tick_ms)
{
    if (!baud || !tick_ms || length > 80) return UINT32_MAX;
    uint64_t wire_ms = ((uint64_t)length * 10000U + baud - 1U) / baud;
    return (uint32_t)((wire_ms + 20U + tick_ms - 1U) / tick_ms);
}
bool UartBridgeCommand_CanSend(uint32_t deadline, uint32_t now, uint32_t reserve_ticks)
{
    int32_t remaining = (int32_t)(deadline - now);
    return remaining > 0 && (uint32_t)remaining > reserve_ticks;
}

enum { LOCAL_PREFIX, LOCAL_PASS, LOCAL_CAPTURE, LOCAL_DROP };
void UartBridgeLocal_ClearSecret(void *data, size_t length)
{
    volatile unsigned char *p = data;
    while (length--) *p++ = 0;
}
void UartBridgeLocal_Reset(UartBridgeLocalState *state)
{
    UartBridgeLocal_ClearSecret(state, sizeof(*state));
}
static bool local_id(const char *id, char request_id[17])
{
    for (unsigned i=0; i<16; ++i) {
        /* Same lower-case hexadecimal format as HTTP command/session IDs. */
        if (!((id[i]>='0' && id[i]<='9') || (id[i]>='a' && id[i]<='f'))) return false;
    }
    memcpy(request_id,id,16); request_id[16]=0;
    return true;
}
/* Decimal token followed by exactly one space, without signs or overflow. */
static bool local_decimal(const char **cursor, uint32_t *value)
{
    const char *p=*cursor;
    uint32_t result=0;
    if (*p<'0' || *p>'9') return false;
    while (*p>='0' && *p<='9') {
        uint32_t digit=(uint32_t)(*p-'0');
        if (result>(UINT32_MAX-digit)/10U) return false;
        result=result*10U+digit;
        ++p;
    }
    if (*p!=' ') return false;
    *cursor=p+1; *value=result;
    return true;
}
static int local_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static bool wifi_decode(const char *source, size_t length, char *out, size_t capacity)
{
    if (!length || (length & 1) || length / 2 >= capacity) return false;
    for (size_t i=0; i<length/2; ++i) {
        int hi=local_hex(source[2*i]), lo=local_hex(source[2*i+1]);
        if (hi<0 || lo<0) return false;
        unsigned byte=(unsigned)(hi*16+lo);
        if (byte<32 || byte==127) return false;
        out[i]=(char)byte;
    }
    out[length/2]=0;
    return true;
}
static bool wifi_parse(const char *id, unsigned operation, char request_id[17], UartBridgeLocalWifi *result)
{
    UartBridgeLocalWifi parsed={0};
    bool valid=false;
    if (strlen(id)<17 || id[16]!=' ' || !local_id(id,request_id)) goto done;
    const char *p=id+17;
    if (!local_decimal(&p,&parsed.deadline_ms) || strlen(p)<8) goto done;
    for (unsigned i=0; i<8; ++i) if (local_hex(p[i])<0) goto done;
    memcpy(parsed.boot_prefix,p,8);
    p+=8;
    if (operation >= 2) { valid=(*p==0); goto done; }
    if (operation == 1) goto pairing;
    if (*p++!=' ') goto done;
    const char *space=strchr(p,' ');
    if (!space || !wifi_decode(p,(size_t)(space-p),parsed.ssid,sizeof(parsed.ssid))) goto done;
    p=space+1;
    space=strchr(p,' ');
    size_t password_length=space ? (size_t)(space-p) : strlen(p);
    if (!(password_length==1 && *p=='-')) {
        size_t length=password_length;
        if (!wifi_decode(p,length,parsed.password,sizeof(parsed.password))) goto done;
        length/=2;
        if (length<8) goto done;
        if (length==64) {
            for (size_t i=0; i<length; ++i) {
                char c=parsed.password[i];
                if (local_hex(c)<0 && !(c>='A' && c<='F')) goto done;
            }
        }
    }
    p+=password_length;
pairing:
    /* Legacy private builds keep the original syntax. A capable public
     * receiver may receive one key, never arbitrary trailing parameters. */
    if (*p) {
        if (*p++!=' ' || strlen(p)!=64) goto done;
        for (unsigned i=0; i<64; ++i) if (local_hex(p[i])<0) goto done;
        memcpy(parsed.api_token,p,64);
    }
    valid=true;
done:
    if (valid && result) *result=parsed;
    UartBridgeLocal_ClearSecret(&parsed,sizeof(parsed));
    return valid;
}
static UartBridgeLocalAction local_action(UartBridgeLocalState *state, char request_id[17], uint32_t *ttl_ms,
                                          UartBridgeLocalCharge *charge, UartBridgeLocalWifi *wifi, SoundCommand *sound)
{
    state->line[state->length] = 0;
    if (sound && SoundCommand_Parse(state->line,sound)) {
        memcpy(request_id,sound->id,17); return UART_BRIDGE_LOCAL_SOUND;
    }
    if (!strcmp(state->line, "ESP STATUS")) return UART_BRIDGE_LOCAL_STATUS;
#if defined(ROBOT_PUBLIC_TEST_BUILD) && ROBOT_PUBLIC_TEST_BUILD
    static const char snapshot[] = "ESP CAMERA_SNAPSHOT ";
    if (state->length == sizeof(snapshot) - 1 + 16 && !memcmp(state->line, snapshot, sizeof(snapshot) - 1)) {
        return local_id(state->line + sizeof(snapshot) - 1, request_id) ?
            UART_BRIDGE_LOCAL_CAMERA_SNAPSHOT : UART_BRIDGE_LOCAL_REJECTED;
    }
#endif
    static const struct { const char *prefix; UartBridgeLocalAction action; } wifi_commands[]={
        {"ESP WIFI_CONNECT ",UART_BRIDGE_LOCAL_WIFI_CONNECT},
        {"ESP WIFI_RECONNECT ",UART_BRIDGE_LOCAL_WIFI_RECONNECT},
        {"ESP WIFI_DISCONNECT ",UART_BRIDGE_LOCAL_WIFI_DISCONNECT},
        {"ESP WIFI_SCAN ",UART_BRIDGE_LOCAL_WIFI_SCAN}
    };
    for (unsigned i=0; i<sizeof(wifi_commands)/sizeof(wifi_commands[0]); ++i) {
        size_t prefix_length=strlen(wifi_commands[i].prefix);
        if (state->length>=prefix_length && !memcmp(state->line,wifi_commands[i].prefix,prefix_length))
            return wifi_parse(state->line+prefix_length, i,request_id,wifi) ?
                wifi_commands[i].action : UART_BRIDGE_LOCAL_REJECTED;
    }
    static const char camera[] = "ESP CAMERA_CHECK ";
    if (state->length == sizeof(camera)-1+16 && !memcmp(state->line,camera,sizeof(camera)-1)) {
        return local_id(state->line+sizeof(camera)-1,request_id) ?
            UART_BRIDGE_LOCAL_CAMERA_CHECK : UART_BRIDGE_LOCAL_REJECTED;
    }
    static const char current[] = "ESP CHARGE_CURRENT ";
    if (state->length >= sizeof(current)-1+16+1+1+1+1+1+8 &&
        !memcmp(state->line,current,sizeof(current)-1)) {
        const char *id=state->line+sizeof(current)-1;
        if (id[16]!=' ' || !local_id(id,request_id)) return UART_BRIDGE_LOCAL_REJECTED;
        const char *p=id+17;
        UartBridgeLocalCharge parsed={0};
        if (!local_decimal(&p,&parsed.ma) || !parsed.ma ||
            !local_decimal(&p,&parsed.deadline_ms) || strlen(p)!=8) return UART_BRIDGE_LOCAL_REJECTED;
        for (unsigned i=0; i<8; ++i) {
            if (!((p[i]>='0' && p[i]<='9') || (p[i]>='a' && p[i]<='f')))
                return UART_BRIDGE_LOCAL_REJECTED;
        }
        memcpy(parsed.boot_prefix,p,8);
        if (charge) *charge=parsed;
        /* The telemetry layer validates the gear, boot and time remaining. */
        return UART_BRIDGE_LOCAL_CHARGE_CURRENT;
    }
    static const char scan[] = "ESP I2C_SCAN ";
    if (state->length == sizeof(scan)-1+16 && !memcmp(state->line,scan,sizeof(scan)-1)) {
        return local_id(state->line+sizeof(scan)-1,request_id) ?
            UART_BRIDGE_LOCAL_SCAN : UART_BRIDGE_LOCAL_REJECTED;
    }
    static const char stop[] = "ESP AUDIO_STOP ";
    if (state->length == sizeof(stop)-1+16 && !memcmp(state->line,stop,sizeof(stop)-1)) {
        return local_id(state->line+sizeof(stop)-1,request_id) ?
            UART_BRIDGE_LOCAL_AUDIO_STOP : UART_BRIDGE_LOCAL_REJECTED;
    }
    static const char start[] = "ESP AUDIO_START ";
    if (state->length >= sizeof(start)-1+16+2 && !memcmp(state->line,start,sizeof(start)-1)) {
        const char *id = state->line+sizeof(start)-1;
        if (id[16]!=' ' || !local_id(id,request_id)) return UART_BRIDGE_LOCAL_REJECTED;
        uint32_t value=0;
        for (const char *p=id+17; *p; ++p) {
            if (*p<'0' || *p>'9') return UART_BRIDGE_LOCAL_REJECTED;
            value=value*10U+(uint32_t)(*p-'0');
            if (value>15000U) return UART_BRIDGE_LOCAL_REJECTED;
        }
        if (!value) return UART_BRIDGE_LOCAL_REJECTED;
        *ttl_ms=value;
        return UART_BRIDGE_LOCAL_AUDIO_START;
    }
    return UART_BRIDGE_LOCAL_REJECTED;
}
size_t UartBridgeLocal_FilterWithSound(UartBridgeLocalState *state, const uint8_t *data, size_t length,
                             uint8_t *out, UartBridgeLocalAction *action, char request_id[17], uint32_t *ttl_ms,
                             UartBridgeLocalCharge *charge, UartBridgeLocalWifi *wifi, SoundCommand *sound)
{
    static const char prefix[] = "ESP ";
    size_t written = 0;
    *action = UART_BRIDGE_LOCAL_NONE;
    request_id[0] = 0; *ttl_ms=0;
    if (charge) memset(charge,0,sizeof(*charge));
    if (wifi) UartBridgeLocal_ClearSecret(wifi,sizeof(*wifi));
    for (size_t i=0; i<length; ++i) {
        uint8_t c=data[i];
        bool end = c=='\r' || c=='\n';
        if (state->mode == LOCAL_PASS) {
            out[written++]=c;
            if (end) UartBridgeLocal_Reset(state);
        } else if (state->mode == LOCAL_DROP) {
            if (end) { *action=UART_BRIDGE_LOCAL_REJECTED; UartBridgeLocal_Reset(state); }
        } else if (end) {
            if (state->mode == LOCAL_CAPTURE) *action=local_action(state,request_id,ttl_ms,charge,wifi,sound);
            else if (state->length) *action=UART_BRIDGE_LOCAL_REJECTED;
            else out[written++]=c; /* Keep ordinary empty lines and the LF in CRLF. */
            UartBridgeLocal_Reset(state);
        } else if (state->mode == LOCAL_CAPTURE) {
            if (c<32 || c>126 || state->length>=sizeof(state->line)-1) state->mode=LOCAL_DROP;
            else state->line[state->length++]=(char)c;
        } else if (c == (uint8_t)prefix[state->length]) {
            state->line[state->length++]=(char)c;
            if (state->length==sizeof(prefix)-1) state->mode=LOCAL_CAPTURE;
        } else if (state->length==3) {
            /* An ESP... typo is local and must never become a STM command. */
            state->mode=LOCAL_DROP;
        } else {
            memcpy(out+written,state->line,state->length); written+=state->length;
            out[written++]=c; state->length=0; state->mode=LOCAL_PASS;
        }
    }
    return written;
}

size_t UartBridgeLocal_Filter(UartBridgeLocalState *state, const uint8_t *data, size_t length,
    uint8_t *out, UartBridgeLocalAction *action, char request_id[17], uint32_t *ttl_ms,
    UartBridgeLocalCharge *charge, UartBridgeLocalWifi *wifi)
{
    return UartBridgeLocal_FilterWithSound(state,data,length,out,action,request_id,ttl_ms,charge,wifi,NULL);
}
