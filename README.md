# Lumino Smart Room Controller

An ESP32-based, locally hosted smart-room controller for lights and a fan. It combines physical touch controls with a mobile-friendly web dashboard, persistent device states, Wi-Fi setup portal, daily fan schedules, an OLED interface, and browser-based firmware updates (OTA).

> **Project status:** Community-ready starting point based on the author's working V1.6-B build. The repository package has not yet been compiled or tested on another user's hardware. Please test safely and report issues before relying on it for unattended operation.

## Features

- Four independent relay outputs: three lights and one fan.
- Four TTP223 touch inputs.
- SSD1306 128×64 I²C OLED with boot animation, device spotlight, and idle sleep.
- Responsive web dashboard hosted directly on the ESP32.
- WebSocket updates for multiple connected clients.
- Wi-Fi configuration portal with network scanning and fallback access point.
- Device state persistence using ESP32 Preferences/NVS.
- Up to eight daily recurring fan ON/OFF events.
- NTP time synchronization with a configurable local time zone.
- Browser-based OTA firmware upload protected by HTTP Basic Authentication.

## Hardware used by the reference build

| Component | Reference hardware |
|---|---|
| MCU | ESP32 DevKit V1 / ESP32-WROOM-32, 30-pin |
| Relay board | 4-channel 5V relay module (active LOW in this firmware) |
| Touch inputs | 4 × TTP223 modules |
| Display | 0.96-inch SSD1306 I²C OLED, 128×64 |
| Power | Regulated 5V supply; OLED powered from ESP32 3.3V |

### Default GPIO map

| Function | GPIO |
|---|---:|
| Relay 1 – Light 1 | 16 |
| Relay 2 – Light 2 | 17 |
| Relay 3 – Light 3 | 18 |
| Relay 4 – Fan | 19 |
| Touch 1 – Light 1 | 32 |
| Touch 2 – Light 2 | 33 |
| Touch 3 – Light 3 | 25 |
| Touch 4 – Fan | 26 |
| OLED SDA | 21 |
| OLED SCL | 22 |

Pins and relay polarity are defined near the top of `src/main.cpp`; adapt them to your board before powering the system.

## Getting started

1. Install [Visual Studio Code](https://code.visualstudio.com/) and the [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode).
2. Clone or download this repository.
3. Copy `secrets.h.example` to `secrets.h` in the project root.
4. Edit `secrets.h` with your Wi-Fi and fallback AP settings. `secrets.h` is ignored by Git so credentials are not committed.
5. Edit `include/project_config.h` and set your time zone and OTA credentials.
6. Connect the ESP32 by USB, select the `esp32dev` PlatformIO environment, build, and upload.
7. Open the Serial Monitor at 115200 baud. If configured Wi-Fi is unavailable, connect your phone to the fallback AP and open `http://192.168.4.1`.

PlatformIO installs the libraries listed in `platformio.ini` automatically.

## Time-zone configuration

Set `TIME_ZONE` in `include/project_config.h`. The ESP32 Arduino `configTzTime()` API expects a **POSIX TZ string**, not an IANA name such as `Asia/Dhaka`.

| Location | POSIX TZ value |
|---|---|
| Bangladesh | `"<+06>-6"` |
| UTC | `"UTC0"` |
| India | `"IST-5:30"` |
| Nepal | `"<+05:45>-5:45"` |
| Pakistan | `"PKT-5"` |
| Singapore | `"SGT-8"` |
| United Kingdom | `"GMT0BST,M3.5.0/1,M10.5.0/2"` |
| US Eastern | `"EST5EDT,M3.2.0/2,M11.1.0/2"` |
| US Pacific | `"PST8PDT,M3.2.0/2,M11.1.0/2"` |

After changing `TIME_ZONE`, rebuild and upload the firmware. The OLED clock, dashboard clock, and fan scheduler use the same local system time. For other regions, look up the correct POSIX TZ format and verify daylight-saving rules for your location.

## OTA firmware updates

1. Build the project in PlatformIO. The firmware binary is normally at `.pio/build/esp32dev/firmware.bin`.
2. Open the controller dashboard on the same local network.
3. Choose **Update** in the dashboard header (or open `/firmware`).
4. Authenticate with `OTA_USERNAME` and `OTA_PASSWORD` from `include/project_config.h`.
5. Select `firmware.bin` and start the upload. Keep the ESP32 powered and connected until it restarts.

**OTA security:** Change the default OTA username/password before use. This feature uses HTTP Basic Authentication over plain HTTP; it is intended only for a trusted local network. Do not expose the controller or OTA endpoint directly to the public internet. Anyone with network access and the credentials may replace the firmware. Use a unique, strong password.

OTA requires an ESP32 partition layout with two OTA application slots. If your selected partition table has no OTA slots or the application is too large, OTA will fail; select an OTA-capable partition layout and ensure the compiled image fits in one app slot. Keep USB serial access available for recovery during initial testing.

## Wi-Fi configuration

The firmware first tries the credentials in `secrets.h`, then stored credentials in NVS. It can also start a fallback setup AP. The setup portal allows network scanning and connection testing before saving replacement credentials. The sample configuration uses a static IP only for the configured original SSID; change the sample addresses to match your router or disable static IP.

Never commit your real `secrets.h`, Wi-Fi password, or OTA password.

## Daily fan schedule

The controller supports up to eight daily recurring fan events. Schedule data is stored in NVS and executed locally when NTP time is valid. Manual fan control takes priority until the next scheduled event. If time has not synchronized after boot, scheduled actions wait rather than using an invalid clock.

## Safety

- This project controls electrical loads through relays. Mains voltage can cause fatal electric shock or fire.
- Have mains wiring, enclosure, fusing, earthing, strain relief, and isolation reviewed and installed by a qualified electrician.
- Never work on live mains wiring. Disconnect power before wiring or servicing.
- Use a properly rated relay/contactor and enclosure for the actual load and inrush current. A relay module's advertised rating is not a substitute for checking the load type and local electrical code.
- Use a regulated supply and common logic ground where required by the relay module. Confirm that the relay inputs reliably recognize 3.3V ESP32 logic.
- Do not power motors, fans, or relay coils from the ESP32 3.3V pin.
- Test the firmware with low-voltage loads before connecting mains equipment.

## Repository layout

```text
├── include/
│   └── project_config.h     # Time zone and OTA login configuration
├── src/
│   └── main.cpp             # Controller firmware
├── secrets.h.example        # Safe template; copy to secrets.h
├── platformio.ini           # PlatformIO build configuration
├── docs/
├── LICENSE
└── README.md
```

## Contributing

Issues and pull requests are welcome. When reporting a problem, include:

- ESP32 board/module and relay-board model;
- PlatformIO and ESP32 Arduino platform versions;
- build output or Serial Monitor logs (remove Wi-Fi credentials);
- wiring changes from the reference pin map;
- steps to reproduce the issue.

Please do not include passwords, private IP details you do not wish to share, or other secrets in issue reports.

## License

MIT. See [LICENSE](LICENSE).
