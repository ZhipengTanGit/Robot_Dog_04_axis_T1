#include "Peripherals.h"
#include "BSP_Peripherals.h"
#include "BSP_Battery.h"
#include "BSP_SysTick.h"
#include "BSP_UART_3.h"
#include "Battery.h"
#include "Battery_Config.h"
#include "Servo_Config.h"
#if SERVO_TTL_ENABLED
#include "TTLServo.h"
#include "TTLServo_Motion.h"
#endif
#include <stdio.h>
#include <string.h>

#define COMMAND_CAPACITY 80U
#define COMMAND_BUDGET 128U
#define DEBOUNCE_MS 20UL
#define BATTERY_MOTION_FAULTS (BATTERY_FAULT_BUS | BATTERY_FAULT_DATA | BATTERY_FAULT_TEMP_SENSOR | \
    BATTERY_FAULT_NTC | BATTERY_FAULT_CONTROL | BATTERY_FAULT_STALE)

typedef struct { uint8_t stable, candidate; uint32_t changed_at, presses, releases; } Debounce;
static Peripherals_Status status;
static Debounce key, touch;
static uint32_t boot_at, buzzer_until, uart_dropped, uart_errors;
static char buzzer_id[17];
static uint32_t live_until, test_accepted_at;
static uint16_t test_delay;
static char command[COMMAND_CAPACITY];
static uint8_t command_length, discarding;
static uint8_t manual_led0;
static uint32_t charge_led_started;
static struct {
    char id[33];
    uint16_t target_ma, applied_ma;
    const char *state, *reason;
    uint8_t valid, pending;
} current_command;

static void charge_led_task(const Battery_Status *battery, uint32_t now)
{
    Peripherals_ChargeLedMode mode = CHARGE_LED_MANUAL;
    uint16_t level;
    uint8_t enabled;
    if (battery->link == BATTERY_LINK_READY && battery->snapshot_valid && battery->awake &&
        battery->chip.typec_sink && (uint32_t)(now - battery->last_good_ms) <= BATTERY_STALE_MS) {
        enabled = battery->charge_config_valid ? (uint8_t)((battery->charge_config.raw00 >> 4) & 1U) :
            battery->chip.charge_enabled;
        if (battery->chip.charge_full) { mode = CHARGE_LED_FULL; }
        else if (enabled && battery->chip.charge_active) { mode = CHARGE_LED_BREATH; }
    }
    if (mode != status.charge_led_mode) { charge_led_started = now; }
    if (mode == CHARGE_LED_BREATH) {
        uint32_t phase = (uint32_t)(now - charge_led_started) % 2400UL;
        uint32_t ramp = phase <= 1200UL ? phase : 2400UL - phase;
        uint32_t smooth = ramp * 1000UL / 1200UL;
        /* Smooth endpoints, then perceptual brightness correction. All terms
         * fit uint32_t; no floats, delays or software-PWM interrupts. */
        smooth = smooth * smooth * (3000UL - 2UL * smooth) / 1000000UL;
        level = (uint16_t)(smooth * smooth / 1000UL);
    } else { level = mode == CHARGE_LED_FULL || manual_led0 ? 1000U : 0U; }
    if (level != status.charge_led_level) { BSP_Peripherals_ChargeLedLevel(level); }
    status.charge_led_mode = mode;
    status.charge_led_level = level;
    status.led[0] = level != 0U;
}

static void debounce(Debounce *d, uint8_t raw, uint32_t now)
{
    if (raw != d->candidate) { d->candidate = raw; d->changed_at = now; }
    if (d->candidate != d->stable && (uint32_t)(now - d->changed_at) >= DEBOUNCE_MS) {
        d->stable = d->candidate;
        if (d->stable) { ++d->presses; } else { ++d->releases; }
    }
}

static void buzzer_ack(const char *id, uint8_t ok, const char *reason)
{
    /* Only an exactly validated 16-character lowercase hexadecimal ID is kept. */
    printf("PERIPH_ACK cmd=BEEP id=%s ok=%u reason=%s\r\n", id, (unsigned)ok, reason);
}

static void buzzer_end(const char *reason, uint8_t ok)
{
    if (buzzer_id[0]) {
        buzzer_ack(buzzer_id, ok, reason);
        buzzer_id[0] = 0;
    }
}

static void stop_outputs(void)
{
    uint8_t channel;
    for (channel = 0U; channel < 4U; ++channel) {
        BSP_Peripherals_Servo(channel, 0U);
        status.servo_us[channel] = 0U;
    }
#if SERVO_TTL_ENABLED
    TTLServo_StopAll();
#endif
    status.servo_mask = 0U;
    BSP_Peripherals_Buzzer(0U);
    status.buzzer = 0U;
    buzzer_end("CANCELLED", 0U);
    status.servo_mode = SERVO_MODE_IDLE;
    status.live_left_ms = 0U;
}

static uint8_t test_active(void)
{
    return status.test_state == SERVO_TEST_WAIT || status.test_state == SERVO_TEST_RUN;
}

static void finish_test(Peripherals_TestState state, Peripherals_TestResult result)
{
    stop_outputs();
    status.test_state = state;
    status.test_result = result;
    status.test_delay_ms = status.test_left_ms = 0U;
}

static void set_servo(uint8_t channel, uint16_t pulse)
{
#if SERVO_TTL_ENABLED
    if(TTLServo_SetTarget(channel,pulse,BSP_Millis())!=0)return;
#else
    if (status.servo_us[channel] != pulse) { BSP_Peripherals_Servo(channel, pulse); }
#endif
    status.servo_us[channel] = pulse;
    status.servo_mask |= (uint8_t)(1U << channel);
}

static void motion_task(uint32_t now)
{
    uint32_t elapsed, phase, ramp, delta;
    uint16_t pulse;
    uint8_t channel;
    if (!status.servo_allowed) {
        if (test_active()) { finish_test(SERVO_TEST_FAILED, SERVO_RESULT_BATTERY); }
        else if (status.servo_mask) { stop_outputs(); }
        return;
    }
    if (status.servo_mode == SERVO_MODE_LIVE) {
        if ((int32_t)(now - live_until) >= 0) { stop_outputs(); }
        else { status.live_left_ms = (uint16_t)(live_until - now); }
    }
    if (!test_active()) { return; }
    elapsed = (uint32_t)(now - test_accepted_at);
    if (elapsed < test_delay) {
        status.test_delay_ms = (uint16_t)(test_delay - elapsed);
        return;
    }
    elapsed -= test_delay;
    status.test_delay_ms = 0U;
    if (elapsed >= PERIPHERALS_SERVO_TEST_MS) {
        finish_test(SERVO_TEST_DONE, SERVO_RESULT_OK);
        return;
    }
    status.test_state = SERVO_TEST_RUN;
    status.test_left_ms = (uint16_t)(PERIPHERALS_SERVO_TEST_MS - elapsed);
    phase = elapsed % PERIPHERALS_SERVO_TEST_PERIOD_MS;
    ramp = phase % (PERIPHERALS_SERVO_TEST_PERIOD_MS / 2U);
    if (ramp > PERIPHERALS_SERVO_TEST_RAMP_MS) { ramp = PERIPHERALS_SERVO_TEST_RAMP_MS; }
    delta = (uint32_t)(status.test_hi - status.test_lo) * ramp / PERIPHERALS_SERVO_TEST_RAMP_MS;
    /* Each one-second leg reaches its endpoint after 700 ms, then holds for
     * 300 ms. The 20 ms PWM frame can latch the endpoint even when main-loop
     * calls miss the exact transition. The last leg holds low before STOP. */
    pulse = phase < PERIPHERALS_SERVO_TEST_PERIOD_MS / 2U ?
        (uint16_t)(status.test_lo + delta) : (uint16_t)(status.test_hi - delta);
    for (channel = 0U; channel < 4U; ++channel) {
        if (status.test_mask & (uint8_t)(1U << channel)) { set_servo(channel, pulse); }
    }
}

void Peripherals_Init(void)
{
    memset(&status, 0, sizeof(status));
    memset(&key, 0, sizeof(key));
    memset(&touch, 0, sizeof(touch));
    boot_at = BSP_Millis();
    manual_led0 = 0U;
    charge_led_started = boot_at;
    buzzer_until = boot_at;
    buzzer_id[0] = 0;
    live_until = test_accepted_at = boot_at;
    test_delay = 0U;
    command_length = discarding = 0U;
    memset(&current_command, 0, sizeof(current_command));
    uart_dropped = UART_3_Get_Rx_Dropped();
    uart_errors = UART_3_Get_Rx_Errors();
    BSP_Peripherals_Init();
    stop_outputs();
    AHT20_Init(boot_at);
#if SERVO_TTL_ENABLED
    TTLServo_Init();
#endif
}

static int number(const char *text, uint32_t *out)
{
    uint32_t value = 0U;
    if (!*text) { return 0; }
    while (*text) {
        if (*text < '0' || *text > '9' || value > 6553U) { return 0; }
        value = value * 10U + (uint32_t)(*text++ - '0');
        if (value > 65535U) { return 0; }
    }
    *out = value;
    return 1;
}

static int number32(const char *text, uint32_t *out)
{
    uint32_t value = 0U, digit;
    if (!*text) { return 0; }
    while (*text) {
        if (*text < '0' || *text > '9') { return 0; }
        digit = (uint32_t)(*text++ - '0');
        if (value > (0xFFFFFFFFUL - digit) / 10UL) { return 0; }
        value = value * 10UL + digit;
    }
    *out = value;
    return 1;
}

static void reply(const char *name, uint8_t ok, const char *reason)
{
    if (ok) { ++status.cmd_ok; } else { ++status.cmd_err; }
    /* Only compile-time tokens are emitted; never echo arbitrary RX input. */
    printf("PERIPH_ACK cmd=%s ok=%u reason=%s\r\n", name, (unsigned)ok, reason);
}

static void motion_reply(const char *name, uint32_t id, uint8_t ok, const char *reason)
{
    if (ok) { ++status.cmd_ok; } else { ++status.cmd_err; }
    printf("PERIPH_ACK cmd=%s id=%lu ok=%u reason=%s\r\n", name, (unsigned long)id,
        (unsigned)ok, reason);
}

static uint8_t valid_buzzer_id(const char *id)
{
    unsigned i;
    if (strlen(id) != 16U) { return 0U; }
    for (i = 0U; i < 16U; ++i) {
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) { return 0U; }
    }
    return 1U;
}

static void execute_beep(char **tokens, uint8_t count, uint32_t now)
{
    uint32_t duration;
    uint8_t identified = count == 4U && valid_buzzer_id(tokens[3]);
    if ((count != 3U && !identified) || !number(tokens[2], &duration) || duration > 1000U) {
        if (identified) {
            ++status.cmd_err;
            buzzer_ack(tokens[3], 0U, "SYNTAX");
        } else { reply("INVALID", 0U, "SYNTAX"); }
        return;
    }
    /* A new command cancels the old identified request before taking ownership.
     * A zero-duration BEEP only stops the buzzer; it does not alter servo PWM. */
    buzzer_end("CANCELLED", 0U);
    status.buzzer = duration != 0U;
    buzzer_until = now + duration;
    BSP_Peripherals_Buzzer(status.buzzer);
    if (identified) {
        ++status.cmd_ok;
        if (duration) { memcpy(buzzer_id, tokens[3], sizeof(buzzer_id)); }
        buzzer_ack(tokens[3], 1U, duration ? "STARTED" : "STOPPED");
    } else { reply("BEEP", 1U, "OK"); }
}

static void execute_motion(char **tokens, uint8_t count, uint32_t now)
{
    uint32_t id = 0U, stamp = 0U, channel = 0U, pulse = 0U, high = 0U, delay = 0U, speed = 100U;
    const char *name = tokens[1];
    uint8_t live = !strcmp(name, "LIVE"), keep = !strcmp(name, "KEEP");
    uint8_t valid = count >= 4U && number32(tokens[2], &id) && number32(tokens[3], &stamp);
    if (live) {
        valid = valid && (count == 6U || count == 7U) && number(tokens[4], &channel) && channel < 4U &&
            number(tokens[5], &pulse) && pulse >= BSP_SERVO_MIN_PULSE_US && pulse <= BSP_SERVO_MAX_PULSE_US;
        if (count == 7U) { valid = valid && number(tokens[6], &speed) && speed >= 1U && speed <= 1023U; }
    } else if (keep) { valid = valid && count == 4U; }
    else {
        valid = valid && count == 8U && number(tokens[4], &channel) && channel >= 1U && channel <= 15U &&
            number(tokens[5], &pulse) && pulse >= BSP_SERVO_MIN_PULSE_US &&
            number(tokens[6], &high) && high <= BSP_SERVO_MAX_PULSE_US && pulse < high &&
            number(tokens[7], &delay) && delay <= 10000U;
    }
    if (!valid) { motion_reply(name, id, 0U, "SYNTAX"); return; }
    if (id <= status.last_id) { motion_reply(name, id, 0U, "OLD_ID"); return; }
    if ((uint32_t)(now - stamp) > PERIPHERALS_SERVO_STAMP_MAX_AGE_MS) {
        motion_reply(name, id, 0U, "STALE"); return;
    }
    /* Consume valid fresh IDs even when an interlock rejects the action. An
     * old retry must not start motion after the interlock later recovers. */
    status.last_id = id;
#if SERVO_TTL_ENABLED
    if(!live&&!keep){motion_reply(name,id,0U,"UNSUPPORTED");return;}
    if(TTLServo_IsTeaching()){motion_reply(name,id,0U,"TEACH_LOCK");return;}
    if(live){TTLServo_Status feedback;TTLServo_GetStatus((uint8_t)channel,now,&feedback);
        if(!feedback.valid||feedback.status||feedback.baud!=115200UL){motion_reply(name,id,0U,"TTL_NOT_READY");return;}}
#endif
    if (test_active()) { motion_reply(name, id, 0U, "BUSY"); return; }
    if (!status.servo_allowed) { motion_reply(name, id, 0U, "BATTERY"); return; }
    if (keep && status.servo_mode != SERVO_MODE_LIVE) {
        motion_reply(name, id, 0U, "NOT_LIVE"); return;
    }
    if (live || keep) {
        if (live) {
#if SERVO_TTL_ENABLED
            if (TTLServo_SetTargetSpeed((uint8_t)channel,(uint16_t)pulse,(uint16_t)speed,now) != 0) {
                motion_reply(name,id,0U,"TTL_NOT_READY"); return;
            }
            status.servo_us[channel] = (uint16_t)pulse;
            status.servo_mask |= (uint8_t)(1U << channel);
#else
            if (count == 7U) { motion_reply(name,id,0U,"UNSUPPORTED"); return; }
            set_servo((uint8_t)channel, (uint16_t)pulse);
#endif
        }
        status.servo_mode = SERVO_MODE_LIVE;
        live_until = now + PERIPHERALS_SERVO_LEASE_MS;
        status.live_left_ms = (uint16_t)PERIPHERALS_SERVO_LEASE_MS;
    } else {
        stop_outputs();
        status.servo_mode = SERVO_MODE_TEST;
        status.test_id = id;
        status.test_state = delay ? SERVO_TEST_WAIT : SERVO_TEST_RUN;
        status.test_result = SERVO_RESULT_RUNNING;
        status.test_mask = (uint8_t)channel;
        status.test_lo = (uint16_t)pulse;
        status.test_hi = (uint16_t)high;
        status.test_delay_ms = test_delay = (uint16_t)delay;
        status.test_left_ms = (uint16_t)PERIPHERALS_SERVO_TEST_MS;
        test_accepted_at = now;
        motion_task(now);
    }
    motion_reply(name, id, 1U, "OK");
}

/* POSE queues all four targets in one task iteration. It never starts from a
 * partial set: validate the entire pose and every feedback channel first. */
static void execute_teaching(char **tokens, uint8_t count, uint32_t now)
{
    uint32_t id=0U,stamp=0U,pulse[4];
    uint8_t pose=!strcmp(tokens[1],"POSE"),i;
    const char *name=tokens[1];
    if(count!=(pose?8U:4U)||!number32(tokens[2],&id)||!number32(tokens[3],&stamp)||!id){
        motion_reply(name,id,0U,"SYNTAX");return;}
    if(pose){for(i=0;i<4U;++i){if(!number(tokens[4U+i],&pulse[i])||pulse[i]<500U||pulse[i]>2500U){
        motion_reply(name,id,0U,"SYNTAX");return;}}}
    if(id<=status.last_id){motion_reply(name,id,0U,"OLD_ID");return;}
    if((uint32_t)(now-stamp)>PERIPHERALS_SERVO_STAMP_MAX_AGE_MS){motion_reply(name,id,0U,"STALE");return;}
    status.last_id=id;
#if SERVO_TTL_ENABLED
    if(!pose){
        if(!strcmp(name,"TEACH")){for(i=0;i<4U;++i){TTLServo_Status s;TTLServo_GetStatus(i,now,&s);
            if(!s.valid||s.status||s.baud!=115200UL){motion_reply(name,id,0U,"TTL_NOT_READY");return;}}}
        stop_outputs();
        if(!strcmp(name,"TEACH"))TTLServo_BeginTeach();else TTLServo_EndTeach();
        motion_reply(name,id,1U,"OK");return;
    }
    if(test_active()){motion_reply(name,id,0U,"BUSY");return;}
    if(!status.servo_allowed){motion_reply(name,id,0U,"BATTERY");return;}
    for(i=0;i<4U;++i){TTLServo_Status s;TTLServo_GetStatus(i,now,&s);
        if(!s.valid||s.status||s.baud!=115200UL){motion_reply(name,id,0U,"TTL_NOT_READY");return;}}
    {uint16_t targets[4];for(i=0;i<4U;++i)targets[i]=(uint16_t)pulse[i];
        if(TTLServo_SetPose(targets,now)!=0){motion_reply(name,id,0U,"TTL_NOT_READY");return;}}
    for(i=0;i<4U;++i)status.servo_us[i]=(uint16_t)pulse[i];
    status.servo_mask=15U;
    status.servo_mode=SERVO_MODE_LIVE;
    live_until=now+PERIPHERALS_SERVO_LEASE_MS;
    status.live_left_ms=(uint16_t)PERIPHERALS_SERVO_LEASE_MS;
    motion_reply(name,id,1U,"OK");
#else
    motion_reply(name,id,0U,"UNSUPPORTED");
#endif
}

static uint8_t valid_command_id(const char *id)
{
    uint8_t length = 0U;
    while (*id) {
        char c = *id++;
        if (++length > 32U || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')) { return 0U; }
    }
    return length != 0U;
}

static void current_reply(const char *id, uint16_t target, const char *state_name,
                          const char *reason, uint16_t applied)
{
    /* ID has been restricted to 1..32 ASCII letters/digits/-/_ first. */
    printf("BAT_CURRENT_ACK id=%s target_mA=%u status=%s reason=%s applied_mA=%u\r\n",
        id, (unsigned)target, state_name, reason, (unsigned)applied);
}

static void current_cached_reply(void)
{
    current_reply(current_command.id, current_command.target_ma, current_command.state,
        current_command.reason, current_command.applied_ma);
}

static void finish_current_command(const Battery_Status *battery)
{
    if (!current_command.pending || battery->current_request_state == BATTERY_CURRENT_PENDING) { return; }
    current_command.pending = 0U;
    current_command.applied_ma = battery->control_verified ? battery->applied_nominal_ma : 0U;
    if (battery->current_request_state == BATTERY_CURRENT_APPLIED && battery->control_verified &&
        battery->charge_config_valid && battery->applied_nominal_ma == current_command.target_ma &&
        battery->requested_nominal_ma == current_command.target_ma) {
        current_command.state = "APPLIED";
        current_command.reason = "OK";
    } else {
        current_command.state = "FAILED";
        current_command.reason = battery->current_request_result == IP5310_ERROR_VERIFY ? "VERIFY" : "IO";
    }
    current_cached_reply();
}

static void execute_current(char **tokens, uint8_t count)
{
    Battery_Status battery;
    uint32_t value = 0U;
    int result;
    const char *id = count >= 4U && valid_command_id(tokens[3]) ? tokens[3] : "INVALID";
    uint8_t valid = count == 4U && valid_command_id(tokens[3]) && number(tokens[2], &value);
    Battery_GetStatus(&battery);
    if (!valid) { current_reply(id, 0U, "REJECTED", "SYNTAX", battery.applied_nominal_ma); return; }
    if (current_command.valid && !strcmp(id, current_command.id)) {
        if (current_command.target_ma != value) {
            current_reply(id, (uint16_t)value, "REJECTED", "ID_CONFLICT", battery.applied_nominal_ma);
        } else { current_cached_reply(); }
        return;
    }
    if (current_command.pending) {
        current_reply(id, (uint16_t)value, "REJECTED", "BUSY", battery.applied_nominal_ma);
        return;
    }
    result = Battery_RequestNominalCurrent((uint16_t)value);
    memcpy(current_command.id, id, strlen(id) + 1U);
    current_command.target_ma = (uint16_t)value;
    current_command.applied_ma = battery.control_verified ? battery.applied_nominal_ma : 0U;
    current_command.valid = 1U;
    current_command.pending = result == BATTERY_CURRENT_ACCEPTED;
    current_command.state = current_command.pending ? "PENDING" : "REJECTED";
    switch (result) {
    case BATTERY_CURRENT_ACCEPTED: current_command.reason = "QUEUED"; break;
    case BATTERY_CURRENT_UNSUPPORTED: current_command.reason = "UNSUPPORTED"; break;
    case BATTERY_CURRENT_WRONG_MODE: current_command.reason = "MODE"; break;
    case BATTERY_CURRENT_BUSY: current_command.reason = "BUSY"; break;
    case BATTERY_CURRENT_BOARD_LIMIT: current_command.reason = "BOARD_LIMIT"; break;
    default: current_command.reason = "NOT_READY"; break;
    }
    current_cached_reply();
}

static void execute(uint32_t now)
{
    char *tokens[8], *cursor = command;
    uint8_t count = 0U;
    uint32_t index, value;
    while (*cursor) {
        while (*cursor == ' ') { ++cursor; }
        if (!*cursor) { break; }
        if (count == 8U) { reply("INVALID", 0U, "SYNTAX"); return; }
        tokens[count++] = cursor;
        while (*cursor && *cursor != ' ') { ++cursor; }
        if (*cursor) { *cursor++ = 0; }
    }
    if (count >= 2U && !strcmp(tokens[0], "BAT") && !strcmp(tokens[1], "CURRENT")) {
        execute_current(tokens, count);
        return;
    }
    if (count < 2U || strcmp(tokens[0], "PERIPH")) { reply("INVALID", 0U, "SYNTAX"); return; }
    if (!strcmp(tokens[1],"TEACH") || !strcmp(tokens[1],"UNTEACH") || !strcmp(tokens[1],"POSE")) {
        execute_teaching(tokens,count,now);
    } else if (!strcmp(tokens[1], "LIVE") || !strcmp(tokens[1], "KEEP") || !strcmp(tokens[1], "TEST")) {
        execute_motion(tokens, count, now);
    } else if (!strcmp(tokens[1], "STOP") &&
               (count == 2U || (count == 3U && number32(tokens[2], &value)))) {
        if (test_active()) { finish_test(SERVO_TEST_STOPPED, SERVO_RESULT_STOP); }
        else { stop_outputs(); }
#if SERVO_TTL_ENABLED
        TTLServo_EndTeach();
#endif
        if (count == 3U) {
            /* STOP always works, including a delayed old STOP. Its ID also
             * fences queued motion commands with lower IDs. */
            if (value > status.last_id) { status.last_id = value; }
            motion_reply("STOP", value, 1U, "OK");
        } else { reply("STOP", 1U, "OK"); }
    } else if (!strcmp(tokens[1], "STATUS") && count == 2U) {
        reply("STATUS", 1U, "OK");
        Peripherals_PrintStatus();
    } else if (!strcmp(tokens[1], "LED") && count == 4U &&
               number(tokens[2], &index) && number(tokens[3], &value) && index < 2U && value < 2U) {
        if (index == 0U && status.charge_led_mode != CHARGE_LED_MANUAL) {
            reply("LED", 0U, "CHARGE_INDICATOR"); return;
        }
        if (index == 0U) { manual_led0 = (uint8_t)value; status.charge_led_level = value ? 1000U : 0U; }
        status.led[index] = (uint8_t)value;
        BSP_Peripherals_Led((uint8_t)index, (uint8_t)value);
        reply("LED", 1U, "OK");
    } else if (!strcmp(tokens[1], "BEEP")) {
        execute_beep(tokens, count, now);
    } else if (!strcmp(tokens[1], "SERVO") && count == 4U &&
               number(tokens[2], &index) && number(tokens[3], &value) &&
               index < 4U && value >= BSP_SERVO_MIN_PULSE_US && value <= BSP_SERVO_MAX_PULSE_US) {
#if SERVO_TTL_ENABLED
        if(TTLServo_IsTeaching()){reply("SERVO",0U,"TEACH_LOCK");return;}
#endif
        if (test_active()) { reply("SERVO", 0U, "BUSY"); return; }
        if (!status.servo_allowed) { reply("SERVO", 0U, "BATTERY"); return; }
        set_servo((uint8_t)index, (uint16_t)value);
        status.servo_mode = SERVO_MODE_LIVE;
        live_until = now + PERIPHERALS_SERVO_LEASE_MS;
        status.live_left_ms = (uint16_t)PERIPHERALS_SERVO_LEASE_MS;
        reply("SERVO", 1U, "OK");
    } else { reply("INVALID", 0U, "SYNTAX"); }
}

static void receive(uint32_t now)
{
    uint16_t budget;
    uint8_t data;
    uint32_t dropped, errors;
    for (budget = 0U; budget < COMMAND_BUDGET; ++budget) {
        dropped = UART_3_Get_Rx_Dropped();
        errors = UART_3_Get_Rx_Errors();
        if (dropped != uart_dropped || errors != uart_errors) {
            uart_dropped = dropped;
            uart_errors = errors;
            status.rx_drop = dropped;
            UART_3_FlushRx();
            command_length = 0U;
            discarding = 1U;
            reply("INVALID", 0U, "RX_LOSS");
        }
        if (!UART_3_Read(&data, 1U)) { break; }
        /* RX IRQ may have reported a loss while the byte was consumed. Flush
         * before processing it, especially before accepting a final newline. */
        if (UART_3_Get_Rx_Dropped() != uart_dropped || UART_3_Get_Rx_Errors() != uart_errors) { continue; }
        if (data == '\r' || data == '\n') {
            if (!discarding && command_length) { command[command_length] = 0; execute(now); }
            discarding = command_length = 0U;
            continue;
        }
        if (discarding) { continue; }
        if (data < 32U || data > 126U || command_length >= COMMAND_CAPACITY - 1U) {
            command_length = 0U;
            discarding = 1U;
            reply("INVALID", 0U, "LINE");
            continue;
        }
        command[command_length++] = (char)data;
    }
}

void Peripherals_Task(void)
{
    Battery_Status battery;
    uint32_t now = BSP_Millis();
    uint8_t bus_allowed;
    Battery_GetStatus(&battery);
    charge_led_task(&battery, now);
    finish_current_command(&battery);
    status.servo_allowed = battery.link == BATTERY_LINK_READY && battery.snapshot_valid && battery.awake &&
        battery.motion_request != BATTERY_MOTION_STOP_REQUEST && !(battery.faults & BATTERY_MOTION_FAULTS);
#if SERVO_TTL_ENABLED
#endif
    motion_task(now);
#if SERVO_TTL_ENABLED
    TTLServo_Task(now);
#endif
    if (status.buzzer && (int32_t)(now - buzzer_until) >= 0) {
        status.buzzer = 0U;
        BSP_Peripherals_Buzzer(0U);
        buzzer_end("DONE", 1U);
    }
    debounce(&key, BSP_Peripherals_Key(), now);
    /* AI01C may calibrate for 300 ms at power-up. Its MD-floating output is
     * LOW when touched and high-Z when released; BSP applies this polarity. */
    if ((uint32_t)(now - boot_at) >= 300UL) { debounce(&touch, BSP_Peripherals_Touch(), now); }
    status.key = key.stable;
    status.key_press = key.presses;
    status.key_release = key.releases;
    status.touch = touch.stable;
    status.touch_press = touch.presses;
    status.touch_release = touch.releases;
    bus_allowed = battery.link == BATTERY_LINK_READY && battery.snapshot_valid && battery.awake;
    AHT20_Task(now, bus_allowed);
    receive(now);
}

void Peripherals_GetStatus(Peripherals_Status *out)
{
    if (!out) { return; }
    *out = status;
    AHT20_GetStatus(BSP_Millis(), &out->aht);
}

void Peripherals_PrintStatus(void)
{
    static const char *const modes[] = { "IDLE", "LIVE", "TEST" };
    static const char *const states[] = { "IDLE", "WAIT", "RUN", "DONE", "STOPPED", "FAILED" };
    static const char *const results[] = { "NONE", "RUNNING", "OK", "STOP", "BATTERY" };
    Peripherals_Status s;
    Peripherals_GetStatus(&s);
#if SERVO_TTL_ENABLED
    s.servo_mask=TTLServo_ActiveMask();
    {unsigned i;for(i=0;i<4;++i)if(!(s.servo_mask&(1U<<i)))s.servo_us[i]=0;}
#endif
    printf("PERIPH t=%lu aht_t_cC=", (unsigned long)BSP_Millis());
    if (s.aht.has_sample) { printf("%ld", (long)s.aht.temperature_cc); } else { printf("NA"); }
    printf(" aht_rh_cPct=");
    if (s.aht.has_sample) { printf("%lu", (unsigned long)s.aht.humidity_cpct); } else { printf("NA"); }
    printf(" aht_valid=%u aht_age_ms=%lu aht_state=%s aht_errors=%lu",
        (unsigned)s.aht.valid, (unsigned long)s.aht.age_ms, AHT20_StateName(s.aht.state), (unsigned long)s.aht.errors);
    printf(" key=%u key_press=%lu key_release=%lu touch=%u touch_press=%lu touch_release=%lu",
        (unsigned)s.key, (unsigned long)s.key_press, (unsigned long)s.key_release,
        (unsigned)s.touch, (unsigned long)s.touch_press, (unsigned long)s.touch_release);
    printf(" led0=%u led1=%u buzzer=%u buzzer_proto=1 servo0_us=%u servo1_us=%u servo2_us=%u servo3_us=%u servo_mask=%u servo_allowed=%u",
        (unsigned)s.led[0], (unsigned)s.led[1], (unsigned)s.buzzer,
        (unsigned)s.servo_us[0], (unsigned)s.servo_us[1], (unsigned)s.servo_us[2], (unsigned)s.servo_us[3],
        (unsigned)s.servo_mask, (unsigned)s.servo_allowed);
    printf(" led0_mode=%s led0_level=%u", s.charge_led_mode == CHARGE_LED_BREATH ? "CHARGING" :
        s.charge_led_mode == CHARGE_LED_FULL ? "FULL" : "MANUAL", (unsigned)s.charge_led_level);
    printf(" servo_proto=%u servo_mode=%s last_id=%lu live_left_ms=%u test_id=%lu test_state=%s test_result=%s",
        2U, modes[s.servo_mode], (unsigned long)s.last_id, (unsigned)s.live_left_ms,
        (unsigned long)s.test_id, states[s.test_state], results[s.test_result]);
    printf(" test_delay_ms=%u test_left_ms=%u test_mask=%u test_lo=%u test_hi=%u",
        (unsigned)s.test_delay_ms, (unsigned)s.test_left_ms, (unsigned)s.test_mask,
        (unsigned)s.test_lo, (unsigned)s.test_hi);
    printf(" test_ramp_ms=%u test_hold_ms=%u", (unsigned)PERIPHERALS_SERVO_TEST_RAMP_MS,
        (unsigned)PERIPHERALS_SERVO_TEST_HOLD_MS);
#if SERVO_TTL_ENABLED
    printf(" teach_proto=1 pose_proto=1 teach=%u",(unsigned)TTLServo_IsTeaching());
    TTLServo_PrintStatus(BSP_Millis());
#endif
    printf(" cmd_ok=%lu cmd_err=%lu rx_drop=%lu\r\n",
        (unsigned long)s.cmd_ok, (unsigned long)s.cmd_err, (unsigned long)s.rx_drop);
}
