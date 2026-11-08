# Serial protocol

UART, 921600 baud, 8N1, 3.3 V logic. Flipper pin 13 (TX) <-> ESP32 GPIO16 (RX2), Flipper pin 14 (RX) <-> ESP32 GPIO17 (TX2).

## Commands (Flipper -> ESP32), one ASCII byte each

| Byte | Meaning |
|---|---|
| `S` | start streaming (ignored if already streaming) |
| `X` | stop streaming |
| `P` | probe (not sent by the current app): replies `#K` if the microphone is OK, `#N` if it is missing. Ignored while streaming |

## Audio (ESP32 -> Flipper)

After `S` the ESP32 discards about 200 ms (microphone start-up) and then sends a continuous stream of **signed 16-bit little-endian samples, 16 kHz, mono**, with no framing, header or checksum. The stream ends shortly after `X`.

The ESP32 converts the INMP441's 24-bit samples to 16 bit with a DC-blocking high-pass filter (`y[n] = x[n] - x[n-1] + 0.995 y[n-1]`) and a gain (`GAIN` in the sketch), with clipping.

The Flipper app applies an additional gain in 3 dB steps (Q8 fixed point), writes the samples to the WAV file and discards a lone trailing byte so the file always holds whole samples.

## Startup device check

The app sends `S` (repeating it every 500 ms until data arrives, in case the ESP32 is still booting), skips about 250 ms of data (filter start-up transient), measures the peak-to-peak range of the next 250 ms, then sends `X` and drains the link.

| Result | Meaning |
|---|---|
| fewer than ~16 KB received within 4 s | ESP32 missing |
| range < 3 (16-bit units) | microphone missing or not wired (flat signal) |
| otherwise | OK |
