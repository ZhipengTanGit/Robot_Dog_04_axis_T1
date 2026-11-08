Ported from E:/Project/ESP32_robot_dog/Bsp on 2026-11-08.
Retains the verified CO5300 native QSPI / landscape rotation / dual-pixel alignment,
TE window policy, DMA drain ownership and CST9217 stopped-read touch protocol.
Voice changes: independent of debug audio/camera; face visible on boot; thread-safe
emotion request. STM32 telemetry continues to feed head touch and sensor diagnostics.
BoardBus owns I2C0 for every client; codec XL9555 updates use the same atomic lock.
No recovery task power-cycles peripherals during audio.
