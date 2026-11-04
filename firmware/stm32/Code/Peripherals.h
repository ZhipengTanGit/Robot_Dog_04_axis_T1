#ifndef PERIPHERALS_H
#define PERIPHERALS_H
#include <stdint.h>
#include "AHT20.h"
#define PERIPHERALS_SERVO_LEASE_MS 1500UL
#define PERIPHERALS_SERVO_STAMP_MAX_AGE_MS 2500UL
#define PERIPHERALS_SERVO_TEST_MS 10000UL
#define PERIPHERALS_SERVO_TEST_RAMP_MS 700UL
#define PERIPHERALS_SERVO_TEST_HOLD_MS 300UL
#define PERIPHERALS_SERVO_TEST_PERIOD_MS (2UL * (PERIPHERALS_SERVO_TEST_RAMP_MS + PERIPHERALS_SERVO_TEST_HOLD_MS))
typedef enum { SERVO_MODE_IDLE, SERVO_MODE_LIVE, SERVO_MODE_TEST } Peripherals_ServoMode;
typedef enum { SERVO_TEST_IDLE, SERVO_TEST_WAIT, SERVO_TEST_RUN, SERVO_TEST_DONE,
    SERVO_TEST_STOPPED, SERVO_TEST_FAILED } Peripherals_TestState;
typedef enum { SERVO_RESULT_NONE, SERVO_RESULT_RUNNING, SERVO_RESULT_OK,
    SERVO_RESULT_STOP, SERVO_RESULT_BATTERY } Peripherals_TestResult;
typedef struct {
    uint8_t key, touch, led[2], buzzer, servo_allowed, servo_mask;
    uint32_t key_press, key_release, touch_press, touch_release;
    uint32_t cmd_ok, cmd_err, rx_drop;
    uint16_t servo_us[4];
    Peripherals_ServoMode servo_mode;
    Peripherals_TestState test_state;
    Peripherals_TestResult test_result;
    uint32_t last_id, test_id;
    uint16_t live_left_ms, test_delay_ms, test_left_ms, test_lo, test_hi;
    uint8_t test_mask;
    AHT20_Status aht;
} Peripherals_Status;
void Peripherals_Init(void);
void Peripherals_Task(void);
void Peripherals_GetStatus(Peripherals_Status *out);
void Peripherals_PrintStatus(void);
#endif
