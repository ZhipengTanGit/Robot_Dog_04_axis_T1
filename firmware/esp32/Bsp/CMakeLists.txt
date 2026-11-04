idf_component_register(
    SRCS
        "Bsp_LED.c"
        "Bsp_UartBridge.c"
        "Bsp_UdpTelemetry.c"
        "Bsp_Wifi.c"
        "WifiControlPolicy.c"
        "TelemetryLine.c"
        "Bsp_Camera.c"
        "CameraCheckLogic.c"
        "Bsp_BoardBus.c"
        "Bsp_Peripherals.c"
        "Bsp_Display.c"
        "DisplaySyncLogic.c"
        "DisplayRecoveryLogic.c"
        "Bsp_Audio.c"
        "PeripheralMath.c"
        "ChargeCommand.c"
        "Bsp_AudioMonitor.c"
        "UartBridgePolicy.c"
        "SoundCommandLogic.c"
        "Bsp_I2cDiagnostics.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_uart esp_driver_i2c esp_driver_spi esp_driver_i2s esp_driver_ledc esp_lcd esp_psram esp_timer esp_wifi esp_event esp_netif nvs_flash lwip esp_http_client esp_http_server json
)

if(ROBOT_PUBLIC_TEST_BUILD)
    target_sources(${COMPONENT_LIB} PRIVATE "Bsp_CameraSerial.c" "CameraSerialLogic.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ROBOT_PUBLIC_TEST_BUILD=1)
endif()
