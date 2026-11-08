/*
 * ESP32 + INMP441 -> UART -> Flipper Zero
 *
 * Reads the INMP441 I2S microphone and, when the Flipper sends the command 'S',
 * streams 16-bit little-endian PCM samples (16 kHz, mono) over UART2.
 * The command 'X' stops the stream.
 * The command 'P' answers "#K" (microphone OK) or "#N" (microphone missing).
 *
 * The microphone is checked ONCE at power-up, before any command. After the
 * check the I2S channel is destroyed and created again from scratch, so a
 * recording starts exactly as it would without any check.
 * 'P' replies immediately with the stored result and does not touch the I2S.
 * Only if the stored result is "missing", 'P' repeats the check (this lets you
 * plug the microphone in after power-up).
 * (The current Flipper app does not send 'P': it detects the microphone from
 * the audio stream itself. 'P' is kept for other clients.)
 *
 * Requires: Arduino-ESP32 core 3.x (ESP-IDF 5.x), board "ESP32 Dev Module".
 *
 * Wiring:
 *   INMP441 SCK -> GPIO26      INMP441 WS  -> GPIO25
 *   INMP441 SD  -> GPIO33      INMP441 L/R -> GND (left channel)
 *   INMP441 VDD -> 3V3         INMP441 GND -> GND
 *   ESP32 GPIO16 (RX2) <- Flipper pin 13 (TX)
 *   ESP32 GPIO17 (TX2) -> Flipper pin 14 (RX)
 *   ESP32 GND          -- Flipper pin 8/11/18 (GND)
 */

#include <Arduino.h>
#include <driver/i2s_std.h>

// ---- Pins ----
#define PIN_I2S_BCLK 26   // INMP441 "SCK"
#define PIN_I2S_WS   25   // INMP441 "WS"
#define PIN_I2S_DIN  33   // INMP441 "SD"
#define PIN_UART_RX  16   // <- Flipper TX (pin 13)
#define PIN_UART_TX  17   // -> Flipper RX (pin 14)
#define PIN_LED      2    // onboard LED (on many dev boards)

// ---- Parameters (must match the Flipper app) ----
#define UART_BAUD         921600
#define SAMPLE_RATE       16000
#define SAMPLES_PER_BLOCK 256
#define WARMUP_SAMPLES    3200     // ~200 ms discarded when the microphone starts
#define GAIN              2.0f     // the INMP441 is fairly quiet; the Flipper app adds -12..+12 dB on top

// ---- Microphone check ----
#define MIC_MIN_RANGE         8     // minimum swing (24-bit units) for a "live" microphone
#define PROBE_WARMUP_SAMPLES  1600  // ~100 ms discarded before measuring
#define PROBE_MEASURE_SAMPLES 1024  // ~64 ms of measurement

static i2s_chan_handle_t rx_chan = nullptr;
static bool recording = false;
static bool mic_ok = false;          // result of the last check
static uint32_t warmup = 0;

static int32_t i2s_buf[SAMPLES_PER_BLOCK];
static int16_t pcm_buf[SAMPLES_PER_BLOCK];

// High-pass filter (DC blocker): removes the INMP441 DC offset
static float dc_prev_x = 0.0f;
static float dc_prev_y = 0.0f;

static bool i2s_init() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 256;
  if (i2s_new_channel(&chan_cfg, nullptr, &rx_chan) != ESP_OK) return false;

  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)PIN_I2S_BCLK,
      .ws   = (gpio_num_t)PIN_I2S_WS,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)PIN_I2S_DIN,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv   = false,
      },
    },
  };
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // L/R tied to GND = left channel

  return i2s_channel_init_std_mode(rx_chan, &std_cfg) == ESP_OK;
}

static void i2s_release() {
  if (rx_chan) {
    i2s_channel_disable(rx_chan);  // error ignored if already stopped
    i2s_del_channel(rx_chan);
    rx_chan = nullptr;
  }
}

// Checks that the microphone is connected and produces a live signal.
// A real INMP441 always has a little noise: if all samples are identical
// (all zero or stuck), the microphone is missing or wired wrongly.
// A temporary pull-down is used on the data pin (with the microphone
// unplugged the line stays at 0 instead of reading random noise). At the end
// the pull-down is removed and the I2S channel is destroyed and created again,
// so the recording starts as if the check had never happened.
// Do not call while streaming.
static bool check_mic() {
  bool ok = false;

  i2s_release();
  if (i2s_init()) {
    gpio_set_pull_mode((gpio_num_t)PIN_I2S_DIN, GPIO_PULLDOWN_ONLY);
    if (i2s_channel_enable(rx_chan) == ESP_OK) {
      size_t got = 0, discarded = 0, measured = 0;
      int fails = 0;

      while (discarded < PROBE_WARMUP_SAMPLES && fails < 5) {  // the microphone is starting up
        if (i2s_channel_read(rx_chan, i2s_buf, sizeof(i2s_buf), &got, 200) == ESP_OK && got > 0) {
          discarded += got / sizeof(int32_t);
        } else {
          fails++;
        }
      }

      int32_t vmin = INT32_MAX, vmax = INT32_MIN;
      while (measured < PROBE_MEASURE_SAMPLES && fails < 5) {
        if (i2s_channel_read(rx_chan, i2s_buf, sizeof(i2s_buf), &got, 200) == ESP_OK && got > 0) {
          size_t n = got / sizeof(int32_t);
          for (size_t i = 0; i < n; i++) {
            int32_t v = i2s_buf[i] >> 8;  // 24-bit sample
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
          }
          measured += n;
        } else {
          fails++;
        }
      }

      ok = (measured > 0) && ((vmax - vmin) >= MIC_MIN_RANGE);
      Serial.printf("Microphone check: range=%ld -> %s\n", (long)(vmax - vmin),
                    ok ? "OK" : "NOT DETECTED");
    }
  }

  // Back to the original state: no pull, new and stopped channel
  i2s_release();
  gpio_set_pull_mode((gpio_num_t)PIN_I2S_DIN, GPIO_FLOATING);
  if (!i2s_init()) {
    Serial.println("ERROR: I2S init failed");
    ok = false;
  }
  return ok;
}

static void start_recording() {
  if (recording) return;
  dc_prev_x = dc_prev_y = 0.0f;
  warmup = WARMUP_SAMPLES;
  i2s_channel_enable(rx_chan);
  recording = true;
  digitalWrite(PIN_LED, HIGH);
  Serial.println("REC start");
}

static void stop_recording() {
  if (!recording) return;
  recording = false;
  i2s_channel_disable(rx_chan);
  Serial2.flush();
  digitalWrite(PIN_LED, LOW);
  Serial.println("REC stop");
}

void setup() {
  Serial.begin(115200);  // debug over USB
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Serial2.setTxBufferSize(8192);  // must be called before begin()
  Serial2.begin(UART_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);

  mic_ok = check_mic();  // always leaves a new, stopped I2S channel

  if (rx_chan == nullptr) {
    Serial.println("ERROR: I2S init failed");
    while (true) {  // fast blinking = error
      digitalWrite(PIN_LED, !digitalRead(PIN_LED));
      delay(100);
    }
  }
  Serial.println("Ready. Waiting for command 'S' from the Flipper.");
}

void loop() {
  while (Serial2.available()) {
    int c = Serial2.read();
    if (c == 'S') {
      start_recording();
    } else if (c == 'X') {
      stop_recording();
    } else if (c == 'P' && !recording) {
      // Probe. If the microphone was missing, try again now.
      if (!mic_ok) mic_ok = check_mic();
      Serial2.write((const uint8_t*)(mic_ok ? "#K" : "#N"), 2);
      Serial2.flush();
    }
  }

  if (!recording) {
    delay(2);
    return;
  }

  size_t bytes_read = 0;
  if (i2s_channel_read(rx_chan, i2s_buf, sizeof(i2s_buf), &bytes_read, 100) != ESP_OK ||
      bytes_read == 0) {
    return;
  }

  size_t n = bytes_read / sizeof(int32_t);

  if (warmup > 0) {  // discard the first samples (microphone starting up)
    warmup = (warmup > n) ? (warmup - n) : 0;
    return;
  }

  for (size_t i = 0; i < n; i++) {
    float x = (float)(i2s_buf[i] >> 8);                    // 24-bit sample
    float y = x - dc_prev_x + 0.995f * dc_prev_y;          // DC blocker
    dc_prev_x = x;
    dc_prev_y = y;

    float s = (y / 256.0f) * GAIN;                         // 24 bit -> 16 bit with gain
    if (s > 32767.0f) s = 32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    pcm_buf[i] = (int16_t)s;
  }

  Serial2.write((const uint8_t*)pcm_buf, n * sizeof(int16_t));
}
