# Four-axis Robot Dog T1

The repository is organized into seven modules:

- `ESP32-IDF-Debug`: ESP-IDF debug firmware, public build r17.
- `ESP32-IDF-Xiaozhi`: Xiaozhi v2.1.0 voice firmware and the V0.24 board port.
- `STM32-LSP-Debug`: STM32F103RB standard peripheral library (SPL), deployed r18 source.
- `STM32-HAL-Debug`: reserved; no valid robot-dog HAL project was found locally.
- `HTML-WorkBench`: local debug terminal and user-facing web workbench.
- `Datasheet`: component and module documentation.
- `原理图`: editable EDA projects and schematic/BOM/netlist exports.

See [README.md](README.md) for build commands, module documentation and migration details. Local debug: `HTML-WorkBench/Debug-Terminal`, Python 3.10+, port 8081. User web: `HTML-WorkBench/User-Web`, `npm start`, port 8093.

The voice and debug ESP32 applications use different partition layouts. Keep personal credentials and device backups local. Preserve the STM32 settings area 0x0801F800–0x0801FFFF when flashing. The web firmware downloads dated 2026-10-29 are historical PWM builds and are not the current TTL servo firmware. A Gitee push does not deploy the public website.
