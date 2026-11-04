#include "UartBridgePolicy.h"
#include "TelemetryLine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_boundaries_and_fragmentation(void)
{
    UartBridgeFrameState pc, stm;
    UartBridgeFrame_Init(&pc, true);
    UartBridgeFrame_Init(&stm, false);
    const uint8_t first[] = "BAT CUR";
    UartBridgeFrame_Forwarded(&pc, first, sizeof(first)-1);
    assert(!pc.at_boundary);
    const uint8_t rest[] = "RENT 850 abc\rPERIPH STATUS\n";
    size_t n = UartBridgeFrame_Segment(&pc, rest, sizeof(rest)-1);
    assert(n == strlen("RENT 850 abc\r"));
    UartBridgeFrame_Forwarded(&pc, rest, n);
    assert(pc.at_boundary); /* A CR-only command permits web injection now. */
    n = UartBridgeFrame_Segment(&stm, rest, sizeof(rest)-1);
    assert(n == sizeof(rest)-1); /* Log records still require LF. */
    UartBridgeFrame_Forwarded(&stm, (const uint8_t *)"BAT t=1\r", 8);
    assert(!stm.at_boundary);
    UartBridgeFrame_Forwarded(&stm, (const uint8_t *)"\n", 1);
    assert(stm.at_boundary);

    /* No boundary means no command may be injected in this binary payload. */
    const uint8_t binary[] = {'a', 0, 'b'};
    UartBridgeFrame_Forwarded(&pc, binary, sizeof(binary));
    assert(!pc.at_boundary);
}

/* Minimal peer with STM32's control-byte rejection and CR/LF termination.
 * This checks the on-wire recovery sequence, rather than just state flags. */
static char peer_line[80], peer_executed[80];
static size_t peer_length;
static unsigned peer_commands;
static bool peer_discard;
static void peer_feed(const uint8_t *data, size_t length)
{
    for (size_t i=0; i<length; ++i) {
        uint8_t c=data[i];
        if (c=='\r' || c=='\n') {
            if (!peer_discard && peer_length) {
                peer_line[peer_length]=0;
                strcpy(peer_executed, peer_line); ++peer_commands;
            }
            peer_discard=false; peer_length=0;
        } else if (!peer_discard) {
            if (c<32 || c>126 || peer_length>=sizeof(peer_line)-1) {
                peer_discard=true; peer_length=0;
            } else peer_line[peer_length++]=(char)c;
        }
    }
}
static void test_lost_pc_frame_is_never_executed(void)
{
    UartBridgeFrameState pc;
    UartBridgeFrame_Init(&pc,true);
    /* A syntactically valid command prefix was already sent before loss. */
    const char *prefix="BAT CURRENT 850 0123456789abcdef";
    peer_feed((const uint8_t *)prefix,strlen(prefix));
    UartBridgeFrame_Invalidate(&pc);
    const uint8_t poison[]={0,'\n'};
    peer_feed(poison,sizeof(poison));
    assert(peer_commands==0);
    const uint8_t tail[]="truncated tail";
    assert(UartBridgeFrame_Discard(&pc,tail,sizeof(tail)-1)==sizeof(tail)-1);
    assert(pc.discarding && !pc.at_boundary);
    const uint8_t recovered[]="still tail\rPERIPH STATUS\n";
    size_t skip=UartBridgeFrame_Discard(&pc,recovered,sizeof(recovered)-1);
    assert(skip==strlen("still tail\r"));
    assert(!pc.discarding && pc.at_boundary);
    peer_feed(recovered+skip,sizeof(recovered)-1-skip);
    assert(peer_commands==1 && !strcmp(peer_executed,"PERIPH STATUS"));
}

static unsigned emitted;
static char last_line[128];
static void got_line(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    assert(length < sizeof(last_line));
    memcpy(last_line,data,length); last_line[length]=0; ++emitted;
}
static void test_telemetry_loss_cannot_join_ack(void)
{
    TelemetryLine line;
    memset(&line,0,sizeof(line));
    UartBridgeFrameState stm;
    UartBridgeFrame_Init(&stm,false);
    const char *prefix="BAT_CURRENT_ACK id=0123456789abcdef ";
    TelemetryLine_Feed(&line,(const uint8_t *)prefix,strlen(prefix),got_line,NULL);
    TelemetryLine_Reset(&line,1); /* UdpTelemetry_InvalidateStream contract. */
    UartBridgeFrame_Invalidate(&stm);
    const uint8_t a[]="status=APPLIED\r";
    size_t skip=UartBridgeFrame_Discard(&stm,a,sizeof(a)-1);
    TelemetryLine_Feed(&line,a,skip,got_line,NULL);
    assert(stm.discarding && emitted==0); /* CR alone is not a log boundary. */
    const uint8_t b[]="\nBAT t=99 valid=1\r\n";
    skip=UartBridgeFrame_Discard(&stm,b,sizeof(b)-1);
    assert(skip==1 && !stm.discarding);
    TelemetryLine_Feed(&line,b,skip,got_line,NULL);
    TelemetryLine_Feed(&line,b+skip,sizeof(b)-1-skip,got_line,NULL);
    assert(emitted==1 && !strcmp(last_line,"BAT t=99 valid=1\r\n"));
}

static void test_deadline_includes_drain_and_wire_time(void)
{
    uint32_t reserve=UartBridgeCommand_ReserveTicks(35,115200,1);
    assert(reserve==24); /* 3.039 ms on wire, rounded up, plus 20 ms. */
    assert(!UartBridgeCommand_CanSend(1000,999,reserve));
    assert(!UartBridgeCommand_CanSend(1000,1000,reserve));
    assert(!UartBridgeCommand_CanSend(1000,1001,reserve));
    assert(UartBridgeCommand_CanSend(1000,800,reserve));
    /* An initially valid command expires while older UART data drains. */
    assert(!UartBridgeCommand_CanSend(1000,980,reserve));
    assert(UartBridgeCommand_CanSend(1000,970,reserve));
    assert(!UartBridgeCommand_CanSend(1000,976,reserve));
    /* Tick wrap must not turn an expired deadline into a huge allowance. */
    assert(UartBridgeCommand_CanSend(20,UINT32_MAX-20,10));
    assert(!UartBridgeCommand_CanSend(UINT32_MAX-20,20,10));
    assert(UartBridgeCommand_ReserveTicks(35,115200,10)==3);
    assert(UartBridgeCommand_ReserveTicks(80,115200,10)==3);
    assert(!UartBridgeCommand_CanSend(50,1,UartBridgeCommand_ReserveTicks(81,115200,1)));
}

static void test_local_commands_never_reach_stm(void)
{
    UartBridgeLocalState local;
    UartBridgeLocal_Reset(&local);
    uint8_t out[300]; char id[17];
    UartBridgeLocalAction action; uint32_t ttl;
    size_t count;
    const char *parts[]={"E", "SP ST", "ATUS\r"};
    for (unsigned i=0; i<3; ++i) {
        count=UartBridgeLocal_Filter(&local,(const uint8_t *)parts[i],strlen(parts[i]),out,&action,id,&ttl,NULL,NULL);
        assert(count==0);
        assert(action==(i==2 ? UART_BRIDGE_LOCAL_STATUS : UART_BRIDGE_LOCAL_NONE));
    }
    /* CRLF's second byte is an ordinary empty line, never leaked ESP text. */
    count=UartBridgeLocal_Filter(&local,(const uint8_t *)"\n",1,out,&action,id,&ttl,NULL,NULL);
    assert(count==1 && out[0]=='\n' && action==UART_BRIDGE_LOCAL_NONE);
    const char *scan="ESP I2C_SCAN 0123456789abcdef\n";
    count=UartBridgeLocal_Filter(&local,(const uint8_t *)scan,strlen(scan),out,&action,id,&ttl,NULL,NULL);
    assert(count==0 && action==UART_BRIDGE_LOCAL_SCAN && !strcmp(id,"0123456789abcdef"));
    const char *invalid[]={"E\n","ES\r","ESP\n","ESP STATUS extra\n","ESP I2C_SCAN bad\n",
        "ESP I2C_SCAN 0123456789abcdeg\n","ESP I2C_SCAN 0123456789abcdef EXTRA\r","ESPbad\n"};
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
        count=UartBridgeLocal_Filter(&local,(const uint8_t *)invalid[i],strlen(invalid[i]),out,&action,id,&ttl,NULL,NULL);
        assert(count==0 && action==UART_BRIDGE_LOCAL_REJECTED);
    }
    uint8_t oversized[200]; memset(oversized,'x',sizeof(oversized));
    memcpy(oversized,"ESP ",4); oversized[sizeof(oversized)-1]='\n';
    count=UartBridgeLocal_Filter(&local,oversized,sizeof(oversized),out,&action,id,&ttl,NULL,NULL);
    assert(count==0 && action==UART_BRIDGE_LOCAL_REJECTED);
    const uint8_t binary[]={'E','S','P',' ',0,'\n'};
    count=UartBridgeLocal_Filter(&local,binary,sizeof(binary),out,&action,id,&ttl,NULL,NULL);
    assert(count==0 && action==UART_BRIDGE_LOCAL_REJECTED);
}

static void test_unrelated_pc_data_remains_transparent(void)
{
    UartBridgeLocalState local;
    UartBridgeLocal_Reset(&local);
    uint8_t out[300]; char id[17]; UartBridgeLocalAction action; uint32_t ttl;
    const char *command="BAT CURRENT 850 0123456789abcdef\r";
    size_t count=UartBridgeLocal_Filter(&local,(const uint8_t *)command,strlen(command),out,&action,id,&ttl,NULL,NULL);
    assert(count==strlen(command) && !memcmp(out,command,count) && action==UART_BRIDGE_LOCAL_NONE);
    /* A mismatching prefix is released byte-for-byte, even across calls. */
    count=UartBridgeLocal_Filter(&local,(const uint8_t *)"ES",2,out,&action,id,&ttl,NULL,NULL);
    assert(count==0);
    const uint8_t mismatch[]={'X',0,'Y','\n'};
    const uint8_t expected[]={'E','S','X',0,'Y','\n'};
    count=UartBridgeLocal_Filter(&local,mismatch,sizeof(mismatch),out,&action,id,&ttl,NULL,NULL);
    assert(count==sizeof(expected) && !memcmp(out,expected,count));
    /* ESP text in the middle of a normal PC command has no local meaning. */
    const char *ordinary="PERIPH ESP STATUS\n";
    count=UartBridgeLocal_Filter(&local,(const uint8_t *)ordinary,strlen(ordinary),out,&action,id,&ttl,NULL,NULL);
    assert(count==strlen(ordinary) && !memcmp(out,ordinary,count) && action==UART_BRIDGE_LOCAL_NONE);
    /* RX loss also drops a locally buffered prefix; it must not leak later. */
    count=UartBridgeLocal_Filter(&local,(const uint8_t *)"ESP ST",6,out,&action,id,&ttl,NULL,NULL);
    assert(count==0);
    UartBridgeLocal_Reset(&local);
    count=UartBridgeLocal_Filter(&local,(const uint8_t *)command,strlen(command),out,&action,id,&ttl,NULL,NULL);
    assert(count==strlen(command) && !memcmp(out,command,count));
}
static void test_audio_control_is_strict_and_fragment_safe(void)
{
    UartBridgeLocalState local;
    UartBridgeLocal_Reset(&local);
    uint8_t out[300]; char id[17]; uint32_t ttl;
    UartBridgeLocalAction action;
    const char *parts[]={"ESP AUDIO_STA", "RT 0123456789ab", "cdef 150", "00\r"};
    for (unsigned i=0; i<4; ++i) {
        size_t count=UartBridgeLocal_Filter(&local,(const uint8_t *)parts[i],strlen(parts[i]),out,&action,id,&ttl,NULL,NULL);
        assert(count==0);
        if (i<3) assert(action==UART_BRIDGE_LOCAL_NONE && ttl==0);
        else assert(action==UART_BRIDGE_LOCAL_AUDIO_START && ttl==15000 && !strcmp(id,"0123456789abcdef"));
    }
    const char *minimum="ESP AUDIO_START 0123456789abcdef 1\n";
    assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)minimum,strlen(minimum),out,&action,id,&ttl,NULL,NULL));
    assert(action==UART_BRIDGE_LOCAL_AUDIO_START && ttl==1);
    const char *stop="ESP AUDIO_STOP 0123456789abcdef\n";
    assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)stop,strlen(stop),out,&action,id,&ttl,NULL,NULL));
    assert(action==UART_BRIDGE_LOCAL_AUDIO_STOP && ttl==0 && !strcmp(id,"0123456789abcdef"));
    const char *invalid[]={
        "ESP AUDIO_START 0123456789abcdef 0\n",
        "ESP AUDIO_START 0123456789abcdef 15001\n",
        "ESP AUDIO_START 0123456789abcdef 4294967297\n",
        "ESP AUDIO_START 0123456789abcdef -1\n",
        "ESP AUDIO_START 0123456789abcdef +1\n",
        "ESP AUDIO_START 0123456789abcdef 1.5\n",
        "ESP AUDIO_START 0123456789abcdef 1 2\n",
        "ESP AUDIO_START 0123456789abcdef \n",
        "ESP AUDIO_START 0123456789abcdef\n",
        "ESP AUDIO_START 0123456789abcde 1000\n",
        "ESP AUDIO_START 0123456789abcdeg 1000\n",
        "ESP AUDIO_STOP 0123456789abcdef 1000\n",
        "ESP AUDIO_STOP\n"
    };
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)invalid[i],strlen(invalid[i]),out,&action,id,&ttl,NULL,NULL));
        assert(action==UART_BRIDGE_LOCAL_REJECTED);
    }
    /* Changing PC speed must not shorten the reservation on the STM link. */
    assert(UartBridgeCommand_ReserveTicks(80,115200,1)==27);
    assert(UartBridgeCommand_ReserveTicks(80,921600,1)==21);
}
static void test_absolute_charge_command_is_strict_and_fragment_safe(void)
{
    UartBridgeLocalState local;
    uint8_t out[300]; char id[17]; uint32_t ttl;
    UartBridgeLocalAction action;
    UartBridgeLocalCharge charge;
    const char *command="ESP CHARGE_CURRENT 0123456789abcdef 1850 4294967295 89abcdef\n";
    size_t length=strlen(command);
    /* Every possible USB read split must yield exactly one complete action. */
    for (size_t split=0; split<length; ++split) {
        UartBridgeLocal_Reset(&local);
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)command,split,out,&action,id,&ttl,&charge,NULL));
        assert(action==UART_BRIDGE_LOCAL_NONE && charge.ma==0);
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)command+split,length-split,out,&action,id,&ttl,&charge,NULL));
        assert(action==UART_BRIDGE_LOCAL_CHARGE_CURRENT && ttl==0);
        assert(!strcmp(id,"0123456789abcdef") && charge.ma==1850 && charge.deadline_ms==UINT32_MAX);
        assert(!strcmp(charge.boot_prefix,"89abcdef"));
    }
    /* Zero is a valid absolute uint32 deadline at uptime wrap, not a TTL. */
    const char *wrap="ESP CHARGE_CURRENT 0123456789abcdef 850 0 01234567\r";
    assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)wrap,strlen(wrap),out,&action,id,&ttl,&charge,NULL));
    assert(action==UART_BRIDGE_LOCAL_CHARGE_CURRENT && charge.deadline_ms==0 && charge.ma==850);
    const char *invalid[]={
        "ESP CHARGE_CURRENT 0123456789abcde 850 100 01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdeg 850 100 01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 0 100 01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdef -850 100 01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdef +850 100 01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 850.0 100 01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 4294967296 100 01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 850 4294967296 01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 850 -1 01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 850 1.0 01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 850 100 0123456\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 850 100 012345678\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 850 100 0123456G\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 850 100 01234567 EXTRA\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 850 100 01234567 \n",
        "ESP CHARGE_CURRENT 0123456789abcdef  850 100 01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 850  100 01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 850 100  01234567\n",
        "ESP CHARGE_CURRENT 0123456789abcdef 850 100\n",
        "ESP CHARGE_CURRENT\n"
    };
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)invalid[i],strlen(invalid[i]),out,&action,id,&ttl,&charge,NULL));
        assert(action==UART_BRIDGE_LOCAL_REJECTED && charge.ma==0 && charge.boot_prefix[0]==0);
    }
    /* Following ordinary commands still reach STM without local prefixes. */
    const char *ordinary="PERIPH STATUS\n";
    size_t count=UartBridgeLocal_Filter(&local,(const uint8_t *)ordinary,strlen(ordinary),out,&action,id,&ttl,&charge,NULL);
    assert(count==strlen(ordinary) && !memcmp(out,ordinary,count) && action==UART_BRIDGE_LOCAL_NONE);
}
static void test_camera_check_local_frames(void)
{
    const char *command="ESP CAMERA_CHECK 0123456789abcdef\n";
    size_t length=strlen(command);
    uint8_t out[128]; char id[17]; uint32_t ttl;
    UartBridgeLocalState local; UartBridgeLocalAction action;
    for (size_t split=1; split<length; ++split) {
        UartBridgeLocal_Reset(&local);
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)command,split,out,&action,id,&ttl,NULL,NULL));
        assert(action==UART_BRIDGE_LOCAL_NONE);
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)command+split,length-split,out,&action,id,&ttl,NULL,NULL));
        assert(action==UART_BRIDGE_LOCAL_CAMERA_CHECK && !strcmp(id,"0123456789abcdef"));
    }
    const char *invalid[]={"ESP CAMERA_CHECK\n","ESP CAMERA_CHECK bad\n",
        "ESP CAMERA_CHECK 0123456789abcdeg\n","ESP CAMERA_CHECK 0123456789abcdef extra\n"};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        UartBridgeLocal_Reset(&local);
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)invalid[i],strlen(invalid[i]),out,&action,id,&ttl,NULL,NULL));
        assert(action==UART_BRIDGE_LOCAL_REJECTED);
    }
}
static void test_wifi_credentials_are_local_and_strict(void)
{
    UartBridgeLocalState local;
    UartBridgeLocalWifi wifi;
    UartBridgeLocalAction action;
    uint8_t out[400]; char id[17]; uint32_t ttl;
    /* UTF-8 SSID and a password containing a space never reach the STM UART. */
    const char *command="ESP WIFI_CONNECT 0123456789abcdef 4294967295 89abcdef e58d8ee789a7 616263642065666768\n";
    size_t length=strlen(command);
    for (size_t split=0; split<length; ++split) {
        UartBridgeLocal_Reset(&local);
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)command,split,out,&action,id,&ttl,NULL,&wifi));
        assert(action==UART_BRIDGE_LOCAL_NONE && !wifi.password[0]);
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)command+split,length-split,out,&action,id,&ttl,NULL,&wifi));
        assert(action==UART_BRIDGE_LOCAL_WIFI_CONNECT && wifi.deadline_ms==UINT32_MAX);
        assert(!strcmp(wifi.boot_prefix,"89abcdef") && !strcmp(wifi.ssid,"\xe5\x8d\x8e\xe7\x89\xa7"));
        assert(!strcmp(wifi.password,"abcd efgh"));
        for (size_t i=0; i<sizeof(local.line); ++i) assert(!local.line[i]);
    }
    const char *commands[]={
        "ESP WIFI_CONNECT 0123456789abcdef 0 89abcdef 41 -\n",
        "ESP WIFI_RECONNECT 0123456789abcdef 123 89abcdef\n",
        "ESP WIFI_DISCONNECT 0123456789abcdef 123 89abcdef\r"
    };
    for (unsigned i=0; i<3; ++i) {
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)commands[i],strlen(commands[i]),out,&action,id,&ttl,NULL,&wifi));
        assert(action==(UartBridgeLocalAction)(UART_BRIDGE_LOCAL_WIFI_CONNECT+i) && !wifi.password[0]);
    }
    const char *invalid[]={
        "ESP WIFI_CONNECT\n",
        "ESP WIFI_CONNECT 0123456789abcdeg 10 89abcdef 41 -\n",
        "ESP WIFI_CONNECT 0123456789abcdef 4294967296 89abcdef 41 -\n",
        "ESP WIFI_CONNECT 0123456789abcdef 10 89abcdeg 41 -\n",
        "ESP WIFI_CONNECT 0123456789abcdef 10 89abcdef 41\n",
        "ESP WIFI_CONNECT 0123456789abcdef 10 89abcdef  -\n",
        "ESP WIFI_CONNECT 0123456789abcdef 10 89abcdef 4 -\n",
        "ESP WIFI_CONNECT 0123456789abcdef 10 89abcdef 4g -\n",
        "ESP WIFI_CONNECT 0123456789abcdef 10 89abcdef 00 -\n",
        "ESP WIFI_CONNECT 0123456789abcdef 10 89abcdef 410a42 -\n",
        "ESP WIFI_CONNECT 0123456789abcdef 10 89abcdef 41 616263\n",
        "ESP WIFI_CONNECT 0123456789abcdef 10 89abcdef 41 - extra\n",
        "ESP WIFI_RECONNECT 0123456789abcdef 10 89abcdef extra\n",
        "ESP WIFI_DISCONNECT 0123456789abcdef 10 89abcde\n",
        "ESP WIFI_DISCONNECT 0123456789abcdef  10 89abcdef\n"
    };
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)invalid[i],strlen(invalid[i]),out,&action,id,&ttl,NULL,&wifi));
        assert(action==UART_BRIDGE_LOCAL_REJECTED && !wifi.ssid[0] && !wifi.password[0]);
    }
    /* Largest valid SSID+PSK must survive all serial split positions. */
    char longest[400];
    strcpy(longest,"ESP WIFI_CONNECT 0123456789abcdef 4294967295 89abcdef ");
    for (unsigned i=0; i<32; ++i) strcat(longest,"41");
    strcat(longest," ");
    for (unsigned i=0; i<64; ++i) strcat(longest,"61");
    strcat(longest,"\n");
    length=strlen(longest);
    for (size_t split=0; split<length; ++split) {
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)longest,split,out,&action,id,&ttl,NULL,&wifi));
        assert(action==UART_BRIDGE_LOCAL_NONE);
        assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)longest+split,length-split,out,&action,id,&ttl,NULL,&wifi));
        assert(action==UART_BRIDGE_LOCAL_WIFI_CONNECT && strlen(wifi.ssid)==32 && strlen(wifi.password)==64);
    }
    /* A 64-byte password is only valid as a hex PSK. */
    char *password=strrchr(longest,' ')+1;
    password[0]='7'; password[1]='a';
    assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)longest,length,out,&action,id,&ttl,NULL,&wifi));
    assert(action==UART_BRIDGE_LOCAL_REJECTED && !wifi.password[0]);
    memset(longest,'x',sizeof(longest)); memcpy(longest,"ESP ",4); longest[sizeof(longest)-1]='\n';
    assert(!UartBridgeLocal_Filter(&local,(const uint8_t *)longest,sizeof(longest),out,&action,id,&ttl,NULL,&wifi));
    assert(action==UART_BRIDGE_LOCAL_REJECTED);
}
int main(void)
{
    test_boundaries_and_fragmentation();
    test_lost_pc_frame_is_never_executed();
    test_telemetry_loss_cannot_join_ack();
    test_deadline_includes_drain_and_wire_time();
    test_local_commands_never_reach_stm();
    test_unrelated_pc_data_remains_transparent();
    test_audio_control_is_strict_and_fragment_safe();
    test_absolute_charge_command_is_strict_and_fragment_safe();
    test_camera_check_local_frames();
    test_wifi_credentials_are_local_and_strict();
    puts("UART bridge framing, loss recovery, deadline, local command, audio, and absolute charge tests passed");
    return 0;
}
