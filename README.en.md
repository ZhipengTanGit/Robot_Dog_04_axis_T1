# Four-axis Robot Dog T1

Repository for the user workbench, local debug terminal, ESP32/STM32 source code, and hardware design references.

- Workbench: https://110.42.234.162/robot-studio/
- Hardware tests: https://110.42.234.162/robot-studio/hardware.html

Install Node.js, run `cd user-web` then `npm start`, and open http://127.0.0.1:8093/ . Browser dependencies are included. See [README.md](README.md) for the five modules and their setup instructions.

`user-web/` retains the existing October 29 public firmware downloads. `debug-terminal/` contains the local 8081 platform. `firmware/esp32/` includes current ESP-IDF source, while `firmware/stm32/` uses the tested r13-180deg source snapshot with TTL servo control/readback and persistent nominal charge settings. `hardware/` contains V0.24 schematic exports, BOM, netlist, an EDA source archive, and deduplicated manuals. See `docs/source-manifest.json` for provenance and hashes.

Pushing to Gitee does not deploy the live server or flash hardware. Server credentials, private configuration, runtime logs, mechanical CAD originals, and full Flash backups are excluded. The existing repository license and third-party notices are retained.
