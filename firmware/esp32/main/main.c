#include "esp_err.h"
#include "esp_log.h"
#include "Bsp_UartBridge.h"
#include "Bsp_UdpTelemetry.h"
#include "Bsp_Camera.h"
#include "Bsp_Peripherals.h"

void app_main(void)
{
    /* GPIO4 is connected to STM32 PC11: do not run the copied LED demo. */
    ESP_ERROR_CHECK(UartBridge_Start());
    /* UART0 has one writer after bridge startup. Camera/audio diagnostics are
     * included in ESP_STATUS; asynchronous ESP_LOG output would corrupt it. */
    /* Shared I2C/expander setup is owned by BoardBus; all failures are reported
     * individually. Actuators remain inactive until an explicit command. */
    (void)Peripherals_Start();
    /* Camera probing has its own task; a missing sensor must not stop telemetry. */
    (void)Camera_Start();
    /* Failure remains visible in [NET] status; USB forwarding stays alive. */
    (void)UdpTelemetry_Start();
}
