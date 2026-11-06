/* Exercise the real peripheral task, command parser and battery status gates. */
#define main existing_peripheral_test_main
#include "test_peripherals.c"
#undef main

static void charge_tick(uint32_t t)
{
    battery.last_good_ms = t;
    tick(t);
}

static void start_charging(void)
{
    battery.chip.typec_sink = battery.chip.charge_enabled = battery.chip.charge_active = 1U;
    battery.chip.charge_full = 0U;
}

static void test_charge_breath_and_complete(void)
{
    uint16_t rising;
    reset();
    battery.chip.charge_enabled = battery.chip.charge_active = 1U;
    charge_tick(0U); assert(state().charge_led_mode == CHARGE_LED_MANUAL && !charge_led_level);
    start_charging(); charge_tick(10U);
    assert(state().charge_led_mode == CHARGE_LED_BREATH && !charge_led_level);
    charge_tick(310U); rising = charge_led_level; assert(rising > 0U && rising < 250U);
    charge_tick(610U); assert(charge_led_level == 250U);
    charge_tick(1210U); assert(charge_led_level == 1000U);
    charge_tick(1810U); assert(charge_led_level == 250U);
    charge_tick(2410U); assert(!charge_led_level);
    battery.chip.charge_full = 1U; charge_tick(2420U);
    assert(state().charge_led_mode == CHARGE_LED_FULL && charge_led_level == 1000U);
    battery.chip.typec_sink = 0U; charge_tick(2430U);
    assert(state().charge_led_mode == CHARGE_LED_MANUAL && !charge_led_level);
    assert(!pulses[0] && !pulses[1] && !pulses[2] && !pulses[3] && !buzzer);
}

static void test_disabled_invalid_and_stale(void)
{
    reset(); start_charging(); charge_tick(0U); charge_tick(600U); assert(charge_led_level);
    battery.charge_config_valid = 1U; battery.charge_config.raw00 = 0U; charge_tick(610U);
    assert(!charge_led_level && state().charge_led_mode == CHARGE_LED_MANUAL);
    battery.charge_config.raw00 = 0x10U; charge_tick(620U); charge_tick(1220U); assert(charge_led_level);
    battery.snapshot_valid = 0U; charge_tick(1230U); assert(!charge_led_level);
    battery.snapshot_valid = 1U; battery.link = BATTERY_LINK_BUS_ERROR; charge_tick(1240U); assert(!charge_led_level);
    battery.link = BATTERY_LINK_READY; charge_tick(1250U); charge_tick(1850U); assert(charge_led_level);
    tick(1850U + BATTERY_STALE_MS + 1U); assert(!charge_led_level);
    battery.awake = 0U; charge_tick(5000U); assert(!charge_led_level);
}

static void test_manual_led_ownership_and_time_wrap(void)
{
    reset(); feed("PERIPH LED 0 1\n"); assert(charge_led_level == 1000U);
    start_charging(); charge_tick(0U); charge_tick(600U); assert(charge_led_level == 250U);
    feed("PERIPH LED 0 0\nPERIPH LED 1 1\n");
    assert(strstr(ack_output, "ok=0 reason=CHARGE_INDICATOR") && led[1] && charge_led_level == 250U);
    battery.chip.typec_sink = 0U; charge_tick(610U); assert(charge_led_level == 1000U);
    feed("PERIPH LED 0 0\n"); assert(!charge_led_level && led[1]);
    start_charging(); charge_tick(UINT32_MAX - 1000U); charge_tick(199U);
    assert(state().charge_led_mode == CHARGE_LED_BREATH && charge_led_level == 1000U);
    charge_tick(1399U); assert(!charge_led_level);
    Peripherals_PrintStatus();
    assert(strstr(ack_output, "led0_mode=CHARGING led0_level=0"));
}

int main(void)
{
    existing_peripheral_test_main();
    test_charge_breath_and_complete();
    test_disabled_invalid_and_stale();
    test_manual_led_ownership_and_time_wrap();
    puts("Charge LED passed: smooth breathing, charge/USB/freshness gates, full, manual ownership and tick wrap.");
    return 0;
}
