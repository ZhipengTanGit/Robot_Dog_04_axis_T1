idf_component_register(
    SRCS "Bsp_BoardBus.c" "Bsp_Display.c" "DisplaySyncLogic.c"
         "DisplayRecoveryLogic.c" "FaceLogic.c" "FaceRender.c" "PeripheralMath.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd esp_timer esp_psram
)
