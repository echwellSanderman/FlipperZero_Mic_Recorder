# Changelog

## 1.0.0

- Flipper Zero app: records 16 kHz / 16-bit mono WAV files from an ESP32 + INMP441 to `/ext/audio_rec/`.
- Startup check of ESP32 and microphone with automatic close and error screen.
- Gain control (-12 ... +12 dB in 3 dB steps) and live level meter.
- 5 V power for the ESP32 from Flipper pin 1 (enabled by default).
- ESP32 sketch with a one-time microphone check at power-up.
- Breadboard wiring diagram.
