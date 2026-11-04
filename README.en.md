# Four-axis Robot Dog T1 Workbench

Browser workbench for color design, assembly guidance, exploded views, choreography, colored 3MF export, and USB hardware tests.

- Workbench: https://110.42.234.162/robot-studio/
- Hardware tests: https://110.42.234.162/robot-studio/hardware.html

Install Node.js, run `npm start`, and open http://127.0.0.1:8093/ . Browser dependencies are included. See [README.md](README.md) for details.

This mirrors the existing public workbench and its October 29, 2026 public test firmware. The included STM32 firmware uses PWM servo tests and a 350mA nominal charging limit. Later local TTL servo and 1850mA firmware is not part of this synchronization.

Pushing to Gitee does not deploy the live server. Server credentials, local configuration, runtime logs, and original CAD projects are excluded. The existing repository license is retained; third-party notices are under `dist/vendor/`.
