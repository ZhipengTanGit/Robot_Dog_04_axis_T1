#include "main.h"
#include "Battery.h"
#include "Battery_Config.h"
#include "BSP_Battery.h"
#include "Peripherals.h"

#if BATTERY_UART_DIAGNOSTICS
static const char *Battery_IoErrorName(int error)
{
    switch (error) {
    case BSP_BATTERY_IO_OK: return "NONE";
    case BSP_BATTERY_IO_ARGUMENT: return "ARG";
    case BSP_BATTERY_IO_ASLEEP: return "ASLEEP";
    case BSP_BATTERY_IO_NACK: return "NACK";
    case BSP_BATTERY_IO_SCL_TIMEOUT: return "SCL_TIMEOUT";
    case BSP_BATTERY_IO_SDA_STUCK: return "SDA_STUCK";
    case BSP_BATTERY_IO_STOP: return "STOP";
    default: return "UNKNOWN";
    }
}

static const char *Battery_IoStageName(uint8_t stage)
{
    switch (stage) {
    case BSP_BATTERY_STAGE_IDLE: return "IDLE";
    case BSP_BATTERY_STAGE_START: return "START";
    case BSP_BATTERY_STAGE_ADDRESS_WRITE: return "ADDR_W";
    case BSP_BATTERY_STAGE_REGISTER: return "REG";
    case BSP_BATTERY_STAGE_RESTART: return "RESTART";
    case BSP_BATTERY_STAGE_ADDRESS_READ: return "ADDR_R";
    case BSP_BATTERY_STAGE_DATA_READ: return "DATA_R";
    case BSP_BATTERY_STAGE_DATA_WRITE: return "DATA_W";
    case BSP_BATTERY_STAGE_STOP: return "STOP";
    case BSP_BATTERY_STAGE_RECOVERY: return "RECOVER";
    default: return "UNKNOWN";
    }
}

static const char *Battery_LinkName(Battery_LinkState state)
{
    switch (state) {
    case BATTERY_LINK_STARTING: return "STARTING";
    case BATTERY_LINK_STANDBY: return "STANDBY";
    case BATTERY_LINK_READY: return "READY";
    case BATTERY_LINK_BUS_ERROR: return "BUS_ERROR";
    case BATTERY_LINK_DATA_ERROR: return "DATA_ERROR";
    default: return "STALE";
    }
}

static void Battery_PrintStatus(void)
{
    static uint8_t printed_initial_config, printed_initial_power;
    Battery_Status battery;
    BSP_Battery_IoDiagnostics io;
    Battery_GetStatus(&battery);
    BSP_Battery_GetIoDiagnostics(&io);
    if (battery.power_config_before_valid && !printed_initial_power) {
        const IP5310_PowerConfiguration *p = &battery.power_config_before;
        printf("BAT_PWR_BEFORE 00:%02X,01:%02X,02:%02X,0E:%02X\r\n", p->raw00, p->raw01, p->raw02, p->raw0e);
        printed_initial_power = 1U;
    }
    if (battery.charge_config_before_valid && !printed_initial_config) {
        const IP5310_ChargeConfiguration *c = &battery.charge_config_before;
        printf("BAT_CFG_BEFORE 00:%02X,0A:%02X,22:%02X,23:%02X,24:%02X,25:%02X,26:%02X\r\n",
            c->raw00, c->raw0a, c->raw22, c->raw23, c->raw24, c->raw25, c->raw26);
        printed_initial_config = 1;
    }
    printf("BAT t=%lu link=%s awake=%u valid=%u faults=0x%02lX",
        (unsigned long)BSP_Millis(), Battery_LinkName(battery.link),
        (unsigned)battery.awake, (unsigned)battery.snapshot_valid,
        (unsigned long)battery.faults);
    if (battery.snapshot_valid) {
        uint8_t enabled = battery.charge_config_valid ?
            (uint8_t)((battery.charge_config.raw00 >> 4) & 1U) : battery.chip.charge_enabled;
        printf(" soc=%c%u,%u]%% chg_flag=%u full=%u usb_sink=%u ntc=%u%u%u",
            battery.chip.soc_min_inclusive ? '[' : '(',
            (unsigned)battery.chip.soc_min_pct, (unsigned)battery.chip.soc_max_pct,
            (unsigned)battery.chip.charge_active, (unsigned)battery.chip.charge_full,
            (unsigned)battery.chip.typec_sink, (unsigned)battery.chip.ntc_high,
            (unsigned)battery.chip.ntc_mid, (unsigned)battery.chip.ntc_low);
        printf(" chg_en=%u charge=%s soc_source=IP5310_BUCKET",
            (unsigned)enabled,
            battery.chip.charge_full ? "FULL_FLAG" :
            (!enabled ? "DISABLED" :
             (battery.chip.charge_active ? "ENABLED_NOT_FULL" : "IDLE_FLAG")));
        printf(" raw=00:%02X,70:%02X,71:%02X,74:%02X,76:%02X",
            battery.chip.raw00, battery.chip.raw70, battery.chip.raw71,
            battery.chip.raw74, battery.chip.raw76);
        printf(" light_load=%u", (unsigned)battery.chip.light_load);
    } else {
        printf(" soc=NA chg_flag=NA chg_en=NA charge=UNKNOWN light_load=NA");
    }
    if (battery.temperature_valid) {
        printf(" temp_dC=%d", (int)battery.temperature_dc);
    } else {
        printf(" temp=NA");
    }
    /* V0.24 has no battery current/voltage measurement path. Never substitute
     * the programmed current for a physical measurement. */
    printf(" actual_mA=NA vbat_mV=NA mode=%s set_mA=%u verified=%u stop_bit=%u motion=%u",
        battery.monitor_only ? "MONITOR" : (battery.vbus_limit_only ? "VBUS_LIMIT" : "CONTROL"),
        (unsigned)battery.applied_nominal_ma, (unsigned)battery.control_verified,
        (unsigned)battery.charge_stop_verified, (unsigned)battery.motion_request);
    printf(" request_mA=%u current_req=%u limit_mA=%u", (unsigned)battery.requested_nominal_ma,
        (unsigned)battery.current_request_state, (unsigned)BATTERY_BOARD_MAX_NOMINAL_MA);
    printf(" saved_mA=%u save_rc=%d", (unsigned)battery.saved_nominal_ma, battery.settings_result);
    printf(" keep5v=%u pwr_valid=%u pwr_result=%d", (unsigned)battery.keep_5v_verified,
        (unsigned)battery.power_config_valid, battery.power_setup_result);
    if (battery.power_config_valid) {
        printf(" pwr=00:%02X,01:%02X,02:%02X,0E:%02X", battery.power_config.raw00,
            battery.power_config.raw01, battery.power_config.raw02, battery.power_config.raw0e);
    }
    if (battery.charge_config_valid) {
        printf(" cfg=00:%02X,0A:%02X,22:%02X,23:%02X,24:%02X,25:%02X,26:%02X",
            battery.charge_config.raw00, battery.charge_config.raw0a,
            battery.charge_config.raw22, battery.charge_config.raw23,
            battery.charge_config.raw24, battery.charge_config.raw25,
            battery.charge_config.raw26);
    }
    printf(" bus_err=%lu io_fail=%lu io_retry=%lu io_recovered=%lu",
        (unsigned long)battery.bus_error_count, (unsigned long)io.failures,
        (unsigned long)io.retries, (unsigned long)io.recovered);
    /* Error evidence survives successful polls. Format is
     * reason:stage:register:R/W:physical-pin-bits:timestamp-ms. */
    printf(" io_first=%s:%s:%02X:%c:%u:%lu io_last=%s:%s:%02X:%c:%u:%lu",
        Battery_IoErrorName(io.first_error), Battery_IoStageName(io.first_stage),
        (unsigned)io.first_reg, io.first_write ? 'W' : 'R', (unsigned)io.first_pins,
        (unsigned long)io.first_error_ms,
        Battery_IoErrorName(io.last_error), Battery_IoStageName(io.last_stage),
        (unsigned)io.last_reg, io.last_write ? 'W' : 'R', (unsigned)io.last_pins,
        (unsigned long)io.last_error_ms);
    printf(" bus_cause=%s:%s:%02X:%c:%u:%lu\r\n",
        Battery_IoErrorName(battery.last_io_failure.error),
        Battery_IoStageName(battery.last_io_failure.stage),
        (unsigned)battery.last_io_failure.reg, battery.last_io_failure.write ? 'W' : 'R',
        (unsigned)battery.last_io_failure.pins, (unsigned long)battery.last_io_failure.at_ms);
}
#endif

int main(void)
{
#if BATTERY_UART_DIAGNOSTICS
    uint32_t last_log;
#endif
    Main_Config();
#if BATTERY_UART_DIAGNOSTICS
    last_log = BSP_Millis() - 1000U;
    printf("\r\nRobot battery V0.24 STM32F103RB 1S2P 1400mAh fw=TTL_READBACK_stm32f103rb-test-ttl-readback-20261103-r13-180deg\r\n");
    printf("Diagnostics USART%u 115200 8N1\r\n", (unsigned)BATTERY_DIAGNOSTIC_UART);
    printf("I2C PB10/PB11 addr=0x75; L3 PA15; temp_connected=%u; control=%u; vbus_setup=%u; map_confirmed=%u\r\n",
        (unsigned)BATTERY_TEMP_ADC_CONNECTED, (unsigned)BATTERY_CONTROL_ENABLED,
        (unsigned)BATTERY_VBUS_CURRENT_SETUP_ENABLED,
        (unsigned)BATTERY_REGISTER_MAP_CONFIRMED);
#endif
    while (1) {
        Battery_Task();
        Peripherals_Task();
#if BATTERY_UART_DIAGNOSTICS
        if ((uint32_t)(BSP_Millis() - last_log) >= 1000U) {
            last_log = BSP_Millis();
            Battery_PrintStatus();
            Peripherals_PrintStatus();
        }
#endif
        /* Motion code should consume Battery_GetStatus().motion_request.
         * There is no separate servo power switch in this schematic.
         */
        __WFI();
    }
}

void Main_Config(void)
{
    SysTick_Init();
#if BATTERY_UART_DIAGNOSTICS
#if BATTERY_DIAGNOSTIC_UART == 3
    UART_3_Config();
#else
    UART_1_Config();
#endif
#endif
    Battery_Init();
    Peripherals_Init();
}

