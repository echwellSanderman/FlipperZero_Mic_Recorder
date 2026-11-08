# Troubleshooting

## Build problems (ufbt)

**`ld ... activate.csh: file format not recognized` / `ld returned 1 exit status`**
The build picked up stray files from the project folder (for example a `bin/` directory created by a Python virtual environment). `flipper_app/application.fam` therefore lists the source explicitly: `sources=["mic_recorder.c"]`. Keep that line, and keep the app in its own folder without virtual environments inside it.

**SDK download fails (403) or the app does not start ("API mismatch")**
`ufbt` downloads an SDK for a specific firmware. Use the SDK that matches the firmware installed on your Flipper (official, Momentum, ...), then rebuild. `ufbt status` shows the SDK in use; the firmware's release page tells which channel/version to use.

**`ufbt launch` says no Flipper is connected (Windows + WSL)**
WSL2 does not see USB devices by default. Either attach the Flipper to WSL with [usbipd-win](https://github.com/dorssel/usbipd-win), or just run `ufbt` in WSL and copy `dist/mic_recorder.fap` to the SD card's `apps/GPIO/` folder (with qFlipper, the Flipper Lab web app, or a card reader).

## Arduino IDE

**"Missing FQBN (Fully Qualified Board Name)"** - select a board first: Tools -> Board -> esp32 -> **ESP32 Dev Module**.

**Compile errors about `driver/i2s_std.h`** - you need the Arduino-ESP32 **3.x** core (Boards Manager -> "esp32 by Espressif Systems", version 3.0 or newer).

## At runtime

**"ESP32 not detected."**
No audio arrives on the UART. Check: Flipper TX (pin 13) goes to ESP32 **RX2 / GPIO16** and Flipper RX (pin 14) to **TX2 / GPIO17** (crossed), a common GND (any ESP32 GND pin works, they are all connected on the board), the sketch is uploaded, the ESP32 is powered (if powered from the Flipper: USB unplugged, `POWER_ESP32_FROM_FLIPPER 1`, pin 1 -> 5V/VIN). Open the ESP32's Serial Monitor (115200) to see whether it boots.

**"Microphone not detected."**
The ESP32 streams, but the signal is perfectly flat. Check the six microphone wires (SD/WS/SCK on GPIO33/25/26, VDD on 3V3, GND, and **L/R to GND**). In the ESP32 Serial Monitor the startup line `Microphone check: range=...` should show a range well above 8.

**Silent recording (`Lvl: 0`)**
Same causes as above. If the check passes but the recording is silent, raise the gain with Up, and check `L/R`: with L/R at 3.3 V the microphone uses the right slot while the sketch reads the left one.

**Recording is too quiet / distorted**
Use Up/Down. If the level meter regularly reaches ~32000 the signal is clipping: lower `GAIN` in the sketch (the app can only reduce what the ESP32 already clipped) and re-flash.

**`Lost:` or `UARTerr:` on the screen**
Bytes were lost on the UART: shorten the wires, check the common ground, and make sure the SD card is healthy (slow SD writes can overflow the buffer).

**The app closes right after starting**
That is the "device not found" behaviour (it shows the error for 5 s first). To use the app without the check, set `STARTUP_CHECK 0`.
