# Public USB protocol v1

CH340 UART0: 921600 baud, 8N1, no flow control. ESP32 UART1 ↔ STM32 USART3: 115200 baud, 8N1. ESP TX GPIO4 → STM PC11; ESP RX GPIO5 ← STM PC10. Runtime logs and responses use LF/CRLF-delimited lines. Do not assume one USB read equals one line. Bound a line to 32KiB and discard overflow through the next LF.

Only `ESP ` commands are consumed by ESP32. Other bytes, including `PERIPH ...` and STM32 ACK/log lines, pass through unchanged. The bridge does not program STM32 or forward USB DTR/RTS to STM32. USB has a single UART writer; extra messages are inserted between complete STM32 lines.

## Status and diagnostics

```text
ESP STATUS
ESP I2C_SCAN <id16hex>
ESP CAMERA_CHECK <id16hex>
ESP CAMERA_SNAPSHOT <id16hex>
```

IDs contain exactly 16 lower-case hexadecimal characters. Create a fresh ID for each manual action. STATUS returns `ESP_STATUS {JSON}` including `device_id`, `boot_id`, `seq`, `uptime_ms`, raw BAT/PERIPH lines and their ages, `camera`, `camera_check`, `esp_peripherals`, `i2c_scan`, `audio_stream`, `sound_test`, `uart_bridge`. Public firmware additionally identifies `firmware_build=public-usb-20261029`, `idf_version=v5.4.4`, `public_test_build=true`, and `camera_snapshot_proto=1` when its worker is available. Poll STATUS about every 3 seconds (1 second while awaiting an explicit diagnostic).

I²C scan is ESP32 GPIO8 SDA / GPIO19 SCL, addresses 0x08–0x77. Ordinary addresses use probe; ES8388 0x10/0x11 uses a bounded low-speed register-pointer/read check, with evidence marked separately in `i2c_scan`. No register configuration or output state is changed by this scan. Address ACK is not chip identity verification.

## Single JPEG over USB

Request one frame with `ESP CAMERA_SNAPSHOT <id>`. Only public firmware advertising `camera_snapshot_proto=1` supports this operation. It acquires a real validated JPEG on a separate task, copies it into bounded PSRAM, and returns the sensor frame buffer before UART transfer. It does not reinitialize the camera or transmit continuously. At most one request is active; another ID gets ERROR/BUSY.

Every response is one independent line beginning `ESP_CAMERA_FRAME `, followed by JSON:

```text
ESP_CAMERA_FRAME {"id":"0123456789abcdef","phase":"BEGIN","bytes":50000,"width":1920,"height":1080,"chunks":49,"chunk_bytes":1024,"crc32":123456789,"seq":100,"timestamp_ms":2000}
ESP_CAMERA_FRAME {"id":"0123456789abcdef","phase":"DATA","index":0,"data":"...base64..."}
ESP_CAMERA_FRAME {"id":"0123456789abcdef","phase":"END","bytes":50000,"chunks":49,"crc32":123456789,"seq":100}
```

The example CRC is illustrative. The actual value is an unsigned JSON number holding standard reflected IEEE CRC32 (polynomial 0xEDB88320, initial/final XOR 0xFFFFFFFF; ASCII `123456789` → 0xCBF43926). Maximum JPEG size is 524288 bytes. DATA contains 1–1024 raw bytes encoded as strict base64; zero-based index increases by one. The final chunk may be shorter. BEGIN/END count and CRC refer to the full JPEG. `seq` is the camera frame acquisition counter, not the chunk index.

Ordinary complete telemetry/audio lines may appear between chunks. Validate the active ID, ordered indexes, declared byte/chunk limits, matching END metadata, CRC32, and JPEG SOI/EOI before constructing/displaying a Blob. Discard incomplete, stale, duplicate, oversized, out-of-order or timed-out transfers. Do not display partial JPEGs. Clear state on serial disconnect or ESP32 boot change. Host timeout should allow at least the 15-second board deadline plus a small serial scheduling margin. 921600 baud is required; this is a manual still-image test, not a 30fps stream.

Errors have `phase=ERROR`, numeric `error` and `detail` equal to `BUSY`, `TIMEOUT`, or `CAPTURE_FAILED`. They contain no image data. A repeated ID does not trigger another capture. The board owns the 15-second request deadline; an expired transfer never resumes as a fresh request.

## Microphone

```text
ESP AUDIO_START <session16hex> <ttl_ms 1..15000>
ESP AUDIO_STOP <session16hex>
```

Renew the current session about every 5 seconds while the user is listening. Stop on page hide/disconnect. Matching `ESP_AUDIO {JSON}` contains `session_id`, incrementing `seq`, `sample_rate:16000`, `channels:1`, `format:"s16le"`, and base64 `data`. Decode only the current session, monotonic sequence, even 2–6400-byte packets and initialized/valid codec status. Feed bounded PCM to Web Audio; do not put audio into ordinary logs or persistent storage. Lease expiration automatically stops transmission. No synthetic microphone samples are generated when the codec fails.

## Explicit sound and charging

The following commands require a fresh status snapshot (within 3 seconds). Calculate `deadline_ms` as an absolute uint32 ESP uptime deadline, subtracting transport age/margin. `boot8` is the first eight lower-case hex characters of the current `boot_id`; do not renew old requests after reconnect/reset.

```text
ESP SOUND <id16hex> <deadline_ms> <boot8> SPEAKER <hz 100..4000> <duration_ms 20..1000> <volume_percent 1..20>
ESP SOUND <id16hex> <deadline_ms> <boot8> SPEAKER_STOP
ESP SOUND <id16hex> <deadline_ms> <boot8> BUZZER <duration_ms 20..1000>
ESP SOUND <id16hex> <deadline_ms> <boot8> BUZZER_STOP
ESP CHARGE_CURRENT <id16hex> <ma> <deadline_ms> <boot8>
```

Speaker default test: 1000Hz, 500ms, 5% digital amplitude. `ESP_STATUS.sound_test.speaker/buzzer` carries matching IDs and QUEUED/STARTED/COMPLETED/STOPPED/FAILED/TIMED_OUT or compatibility states. For STM32 buzzer firmware advertising `buzzer_proto=1`, raw `PERIPH BEEP <ms0..1000> <id16hex>` also passes through, returning ID-matched `PERIPH_ACK cmd=BEEP ... reason=STARTED|DONE|STOPPED|CANCELLED`. Legacy unnumbered responses are not proof of completion for a new request.

Charging requests require the STM32 policy and final `BAT_CURRENT_ACK` plus a fresh matching verified BAT readback. Queue admission is not confirmation. Battery voltage and actual charging current still have no hardware sampling path in V0.24; never label nominal current as a measured value.

## STM32 passthrough

`PERIPH STATUS`, `PERIPH LED <0|1> <0|1>`, `PERIPH BEEP ...`, `PERIPH SERVO <0..3> <500..2500>`, `PERIPH STOP` and newer numbered LIVE/KEEP/TEST lines reach STM32 byte-for-byte. Follow the exact STM32 firmware's advertised `servo_proto`/`buzzer_proto` and command-ID/lease rules. No movement is started by opening the page. PWM values are commands, not angle feedback. Stop PWM does not cut the servo 5V supply.

Wi-Fi command grammar remains `ESP WIFI_CONNECT <id> <deadline> <boot8> <ssid_utf8_lowerhex> <password_lowerhex_or_dash>`, RECONNECT/DISCONNECT omit the credential fields. The public image has no compiled-in API token and does not expose its LAN camera/control endpoints; use USB for this release. Never echo, log or persist credential command lines in a webpage.
