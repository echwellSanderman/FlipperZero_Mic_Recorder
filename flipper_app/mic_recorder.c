/*
 * Mic Recorder for Flipper Zero
 *
 * Receives 16-bit / 16 kHz / mono PCM audio from an ESP32 (which reads an
 * INMP441 microphone over I2S) through the UART on the GPIO header
 * (pin 13 TX, pin 14 RX) and saves it as a WAV file in /ext/audio_rec/ on the
 * Flipper's SD card.
 *
 * Protocol (very simple):
 *   Flipper -> ESP32 : 'S' = start streaming, 'X' = stop streaming
 *   ESP32 -> Flipper : continuous stream of little-endian int16 samples
 *
 * Device detection: at startup the app makes a very short test recording with
 * the same 'S' / 'X' commands used for real recordings (nothing is saved).
 *   - no data at all            -> the ESP32 is missing
 *   - data, but a flat signal   -> the microphone is missing / not wired
 * A working INMP441 always produces a little noise, so a perfectly flat
 * signal means no microphone. In both failure cases the app shows an error
 * and closes by itself after a few seconds.
 * Set STARTUP_CHECK to 0 to skip the check entirely.
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <expansion/expansion.h>
#include <datetime/datetime.h>

#include <stdio.h>
#include <string.h>

#define TAG "MicRec"

/* ---- Parameters (must match the ESP32 sketch) ---- */
#define SERIAL_BAUD   921600
#define SAMPLE_RATE   16000
#define BYTES_PER_SEC (SAMPLE_RATE * 2) /* 16-bit mono */

/* If 1 (default), the app turns on the 5V rail on Flipper pin 1 to power the
 * ESP32 board. Set to 0 if the ESP32 is powered through USB / a power bank. */
#define POWER_ESP32_FROM_FLIPPER 1

#define REC_DIR            "/ext/audio_rec"
#define RX_STREAM_SIZE     (16 * 1024)
#define RX_TRIGGER_LEVEL   512
#define WRITE_CHUNK        1024
#define NO_DATA_TIMEOUT_MS 1500
#define STOP_DRAIN_MS      150

/* ---- Device detection at startup ---- */
#define STARTUP_CHECK       1    /* 1 = check ESP32 + microphone at startup */
#define DETECT_TIMEOUT_MS   4000 /* total time to wait for the ESP32 */
#define DETECT_RETRY_MS     500  /* how often 'S' is re-sent while no data arrives */
#define DETECT_SKIP_BYTES   8000 /* ~250 ms ignored (filter start-up transient) */
#define DETECT_ANALYZE_BYTES 8000 /* ~250 ms of signal examined */
#define DETECT_MIN_RANGE    3    /* min peak-to-peak (16-bit units) of a live mic */
#define NOT_FOUND_CLOSE_MS  5000 /* time the error is shown before auto-close */

#define CMD_START 'S'
#define CMD_STOP  'X'

/* Extra gain applied by the app on top of the ESP32's own gain, in 3 dB steps
 * (Q8 fixed point: 256 = x1). The ESP32 clips at its own full scale, so the
 * app can only reduce a signal that is already clipped there, not restore it. */
#define GAIN_STEPS       9
#define GAIN_DEFAULT_IDX 4 /* 0 dB */
static const uint16_t gain_table_q8[GAIN_STEPS] = {64, 91, 128, 181, 256, 362, 511, 724, 1024};
static const int8_t gain_table_db[GAIN_STEPS] = {-12, -9, -6, -3, 0, 3, 6, 9, 12};

typedef enum {
    StateChecking,
    StateNotFound,
    StateIdle,
    StateRecording,
    StateSaved,
    StateError,
} RecState;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;
    Storage* storage;
    FuriStreamBuffer* rx_stream;
    FuriHalSerialHandle* serial;
    FuriThread* worker;

    volatile bool accepting; /* the RX callback stores bytes only when true */
    volatile bool stop_req;

    RecState state;
    const char* error_msg;
    char filename[64];

    volatile uint32_t bytes_written;
    volatile uint32_t dropped;     /* bytes lost because the buffer was full */
    volatile uint32_t uart_errors; /* UART framing/overrun errors */
    volatile uint32_t close_at;    /* tick at which the app closes itself */
    volatile uint32_t peak;        /* recent peak sample amplitude (level meter) */
    volatile uint8_t gain_idx;     /* index into gain_table_q8 (Up/Down keys) */
} App;

/* ------------------------------------------------------------------ */
/* WAV                                                                 */
/* ------------------------------------------------------------------ */

static void put16(uint8_t* p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static void wav_header(uint8_t* h, uint32_t data_bytes) {
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16); /* fmt chunk size */
    put16(h + 20, 1); /* PCM */
    put16(h + 22, 1); /* mono */
    put32(h + 24, SAMPLE_RATE);
    put32(h + 28, BYTES_PER_SEC);
    put16(h + 32, 2); /* block align */
    put16(h + 34, 16); /* bits per sample */
    memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
}

/* ------------------------------------------------------------------ */
/* State / UI                                                          */
/* ------------------------------------------------------------------ */

static void set_state(App* app, RecState s, const char* err) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->state = s;
    app->error_msg = err;
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

static void draw_cb(Canvas* canvas, void* ctx) {
    App* app = ctx;
    char line[64];

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    RecState state = app->state;
    const char* err = app->error_msg;
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);

    /* Device-not-found screen */
    if(state == StateNotFound) {
        int32_t left_ms = (int32_t)(app->close_at - furi_get_tick());
        if(left_ms < 0) left_ms = 0;
        unsigned long secs_left = (unsigned long)((left_ms + 999) / 1000);

        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "Error");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 26, err ? err : "Device not detected.");
        canvas_draw_str(canvas, 2, 38, "Check the wiring.");
        snprintf(line, sizeof(line), "Closing in %lu s...", secs_left);
        canvas_draw_str(canvas, 2, 54, line);
        return;
    }

    /* Detection in progress */
    if(state == StateChecking) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "Mic Recorder -> SD");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 30, "Looking for device...");
        return;
    }

    uint32_t bytes = app->bytes_written;
    uint32_t secs = bytes / BYTES_PER_SEC;

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Mic Recorder -> SD");

    canvas_set_font(canvas, FontSecondary);

    const char* status = "Ready";
    switch(state) {
    case StateIdle:
        status = "Ready";
        break;
    case StateRecording:
        status = "RECORDING...";
        break;
    case StateSaved:
        status = "Saved!";
        break;
    case StateError:
        status = err ? err : "Error";
        break;
    default:
        break;
    }
    canvas_draw_str(canvas, 2, 22, status);

    if(state != StateIdle) {
        snprintf(
            line,
            sizeof(line),
            "%02lu:%02lu  %lu KB",
            (unsigned long)(secs / 60),
            (unsigned long)(secs % 60),
            (unsigned long)(bytes / 1024));
        canvas_draw_str(canvas, 2, 32, line);

        const char* base = strrchr(app->filename, '/');
        canvas_draw_str(canvas, 2, 42, base ? base + 1 : app->filename);

        if(app->dropped || app->uart_errors) {
            snprintf(
                line,
                sizeof(line),
                "Lost:%lu UARTerr:%lu",
                (unsigned long)app->dropped,
                (unsigned long)app->uart_errors);
            canvas_draw_str(canvas, 2, 52, line);
        } else if(state == StateRecording) {
            /* Level meter: a 0 here means the samples are silent */
            snprintf(
                line,
                sizeof(line),
                "Lvl:%lu Gain:%+d dB",
                (unsigned long)app->peak,
                (int)gain_table_db[app->gain_idx]);
            canvas_draw_str(canvas, 2, 52, line);
        }
    } else {
        canvas_draw_str(canvas, 2, 32, "16 kHz / 16 bit / mono");
        canvas_draw_str(canvas, 2, 42, "Files: /ext/audio_rec/");
        snprintf(
            line, sizeof(line), "Gain: %+d dB (Up/Down)", (int)gain_table_db[app->gain_idx]);
        canvas_draw_str(canvas, 2, 52, line);
    }

    canvas_draw_str(
        canvas, 2, 62, state == StateRecording ? "OK/Back: stop" : "OK: record  Back: exit");
}

static void input_cb(InputEvent* event, void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->input_queue, event, 0);
}

/* ------------------------------------------------------------------ */
/* UART                                                                */
/* ------------------------------------------------------------------ */

static void serial_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    App* app = ctx;

    if(event & FuriHalSerialRxEventData) {
        uint8_t b = furi_hal_serial_async_rx(handle);
        if(app->accepting) {
            if(furi_stream_buffer_send(app->rx_stream, &b, 1, 0) != 1) {
                app->dropped++;
            }
        }
    }
    if(event &
       (FuriHalSerialRxEventFrameError | FuriHalSerialRxEventNoiseError |
        FuriHalSerialRxEventOverrunError)) {
        app->uart_errors++;
    }
}

static void send_cmd(App* app, char c) {
    uint8_t b = (uint8_t)c;
    furi_hal_serial_tx(app->serial, &b, 1);
    furi_hal_serial_tx_wait_complete(app->serial);
}

/* ------------------------------------------------------------------ */
/* Device detection                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    DetectOk,
    DetectNoEsp32, /* no data on the UART */
    DetectNoMic, /* data arrives, but the signal is flat (no microphone) */
} DetectResult;

/* Short test recording using the normal 'S' / 'X' commands; nothing is saved.
 * Skips the first DETECT_SKIP_BYTES, then measures the peak-to-peak range of
 * the next DETECT_ANALYZE_BYTES. Always leaves the RX buffer empty. */
static DetectResult detect_device(App* app) {
    uint8_t tmp[256];
    uint32_t total = 0;    /* bytes received so far */
    uint32_t analysed = 0; /* bytes examined */
    int16_t vmin = 32767, vmax = -32768;
    uint8_t lo = 0;
    bool have_lo = false;

    furi_stream_buffer_reset(app->rx_stream);
    app->dropped = 0;
    app->uart_errors = 0;
    app->accepting = true;

    uint32_t start = furi_get_tick();
    uint32_t last_cmd = 0;
    bool first = true;

    while(furi_get_tick() - start < DETECT_TIMEOUT_MS && analysed < DETECT_ANALYZE_BYTES) {
        /* Re-send 'S' while no data arrives: the ESP32 may still be booting
         * (for example when it is powered by the Flipper). A repeated 'S'
         * is ignored by the ESP32 once it is streaming. */
        if(total == 0 && (first || furi_get_tick() - last_cmd >= DETECT_RETRY_MS)) {
            send_cmd(app, CMD_START);
            last_cmd = furi_get_tick();
            first = false;
        }
        size_t n = furi_stream_buffer_receive(app->rx_stream, tmp, sizeof(tmp), 50);
        for(size_t i = 0; i < n && analysed < DETECT_ANALYZE_BYTES; i++) {
            total++;
            if(total <= DETECT_SKIP_BYTES) continue;
            if(!have_lo) {
                lo = tmp[i];
                have_lo = true;
            } else {
                int16_t s = (int16_t)((uint16_t)lo | ((uint16_t)tmp[i] << 8));
                have_lo = false;
                if(s < vmin) vmin = s;
                if(s > vmax) vmax = s;
                analysed += 2;
            }
        }
    }

    /* Stop the test stream and discard everything still in transit */
    send_cmd(app, CMD_STOP);
    uint32_t t0 = furi_get_tick();
    while(furi_get_tick() - t0 < STOP_DRAIN_MS) {
        furi_stream_buffer_receive(app->rx_stream, tmp, sizeof(tmp), 20);
    }
    app->accepting = false;
    furi_stream_buffer_reset(app->rx_stream);
    app->dropped = 0;
    app->uart_errors = 0;

    DetectResult result;
    if(analysed < DETECT_ANALYZE_BYTES) {
        result = DetectNoEsp32; /* no (or too little) data */
    } else if((int32_t)vmax - (int32_t)vmin < DETECT_MIN_RANGE) {
        result = DetectNoMic; /* flat signal */
    } else {
        result = DetectOk;
    }

    FURI_LOG_I(
        TAG,
        "Detection: result=%d total=%lu range=%ld",
        (int)result,
        (unsigned long)total,
        (long)((int32_t)vmax - (int32_t)vmin));
    return result;
}

/* ------------------------------------------------------------------ */
/* Recording thread                                                    */
/* ------------------------------------------------------------------ */

/* State of the sample processor: a lone byte is held back until its pair
 * arrives, so gain is always applied to whole 16-bit samples. */
typedef struct {
    uint8_t lo;
    bool have_lo;
    uint32_t win_peak; /* peak of the current display window */
} Proc;

/* Applies the gain to 'n' input bytes and writes whole samples to 'out'
 * (which must hold n + 1 bytes). Returns the number of bytes written. */
static size_t proc_chunk(App* app, Proc* p, const uint8_t* in, size_t n, uint8_t* out) {
    int32_t g = (int32_t)gain_table_q8[app->gain_idx];
    size_t o = 0;
    for(size_t i = 0; i < n; i++) {
        if(!p->have_lo) {
            p->lo = in[i];
            p->have_lo = true;
            continue;
        }
        int32_t s = (int16_t)((uint16_t)p->lo | ((uint16_t)in[i] << 8));
        p->have_lo = false;
        if(g != 256) {
            s = (s * g) >> 8;
            if(s > 32767) s = 32767;
            if(s < -32768) s = -32768;
        }
        uint32_t a = (uint32_t)(s < 0 ? -s : s);
        if(a > p->win_peak) p->win_peak = a;
        out[o++] = (uint8_t)(s & 0xFF);
        out[o++] = (uint8_t)((s >> 8) & 0xFF);
    }
    return o;
}

static int32_t worker_cb(void* ctx) {
    App* app = ctx;
    uint8_t header[44];
    uint8_t* buf = malloc(WRITE_CHUNK);
    uint8_t* out = malloc(WRITE_CHUNK + 2);
    Proc proc = {0};
    File* file = storage_file_alloc(app->storage);

    bool file_open = false;
    bool started = false;
    bool ok = true;
    const char* err = NULL;
    uint32_t total = 0;

    if(!storage_file_open(file, app->filename, FSAM_READ_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = false;
        err = "SD file open error";
    } else {
        file_open = true;
        wav_header(header, 0);
        if(storage_file_write(file, header, sizeof(header)) != sizeof(header)) {
            ok = false;
            err = "SD write error";
        }
    }

    if(ok) {
        furi_stream_buffer_reset(app->rx_stream);
        app->dropped = 0;
        app->uart_errors = 0;
        app->bytes_written = 0;
        app->accepting = true;
        send_cmd(app, CMD_START);
        started = true;

        uint32_t last_data = furi_get_tick();
        uint32_t last_ui = furi_get_tick();
        app->peak = 0;

        while(!app->stop_req) {
            size_t n = furi_stream_buffer_receive(app->rx_stream, buf, WRITE_CHUNK, 50);
            if(n > 0) {
                size_t m = proc_chunk(app, &proc, buf, n, out);
                if(m > 0 && storage_file_write(file, out, m) != m) {
                    ok = false;
                    err = "SD write error";
                    break;
                }
                total += m;
                app->bytes_written = total;
                last_data = furi_get_tick();
            } else if(furi_get_tick() - last_data > NO_DATA_TIMEOUT_MS) {
                ok = false;
                err = total ? "ESP32 stream lost" : "No data from ESP32";
                break;
            }

            if(furi_get_tick() - last_ui > 250) {
                last_ui = furi_get_tick();
                app->peak = proc.win_peak;
                proc.win_peak = 0;
                view_port_update(app->view_port);
            }
        }
    }

    if(started) {
        /* Stop the ESP32 and collect the bytes still in transit */
        send_cmd(app, CMD_STOP);
        uint32_t t0 = furi_get_tick();
        while(file_open && furi_get_tick() - t0 < STOP_DRAIN_MS) {
            size_t n = furi_stream_buffer_receive(app->rx_stream, buf, WRITE_CHUNK, 20);
            if(n > 0) {
                size_t m = proc_chunk(app, &proc, buf, n, out);
                if(m > 0 && storage_file_write(file, out, m) == m) {
                    total += m;
                }
            }
        }
        app->accepting = false;
        size_t n;
        while(file_open && (n = furi_stream_buffer_receive(app->rx_stream, buf, WRITE_CHUNK, 0)) >
                               0) {
            size_t m = proc_chunk(app, &proc, buf, n, out);
            if(m > 0 && storage_file_write(file, out, m) != m) break;
            total += m;
        }
        app->bytes_written = total;
    }

    if(file_open) {
        total &= ~1u; /* even number of bytes (16-bit samples) */
        wav_header(header, total);
        storage_file_seek(file, 0, true);
        storage_file_write(file, header, sizeof(header));
        storage_file_close(file);
    }
    storage_file_free(file);
    free(buf);
    free(out);

    FURI_LOG_I(TAG, "Recording finished: %lu bytes", (unsigned long)total);
    set_state(app, ok ? StateSaved : StateError, err);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Recording control                                                   */
/* ------------------------------------------------------------------ */

static void start_recording(App* app) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(
        app->filename,
        sizeof(app->filename),
        REC_DIR "/rec_%04u%02u%02u_%02u%02u%02u.wav",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second);

    app->stop_req = false;
    app->bytes_written = 0;
    app->dropped = 0;
    app->uart_errors = 0;
    set_state(app, StateRecording, NULL);

    app->worker = furi_thread_alloc_ex("MicRecWorker", 4 * 1024, worker_cb, app);
    furi_thread_start(app->worker);
}

static void reap_worker(App* app) {
    if(app->worker && furi_thread_get_state(app->worker) == FuriThreadStateStopped) {
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int32_t mic_recorder_app(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->state = StateChecking;
    app->gain_idx = GAIN_DEFAULT_IDX;

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->rx_stream = furi_stream_buffer_alloc(RX_STREAM_SIZE, RX_TRIGGER_LEVEL);

    app->storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(app->storage, REC_DIR);

    /* Free the header UART (pins 13/14) from the expansion service */
    Expansion* expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(expansion);

#if POWER_ESP32_FROM_FLIPPER
    bool otg_was_on = furi_hal_power_is_otg_enabled();
    if(!otg_was_on) furi_hal_power_enable_otg();
#endif

    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(app->serial) {
        furi_hal_serial_init(app->serial, SERIAL_BAUD);
        furi_hal_serial_async_rx_start(app->serial, serial_rx_cb, app, false);
    } else {
        app->state = StateError;
        app->error_msg = "UART busy";
    }

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_cb, app);
    view_port_input_callback_set(app->view_port, input_cb, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    bool running = true;
    InputEvent ev;

    /* ---- Startup check: is the ESP32 + microphone connected? ---- */
#if STARTUP_CHECK
    if(app->serial) {
        set_state(app, StateChecking, NULL);

        DetectResult det = detect_device(app);
        if(det == DetectOk) {
            set_state(app, StateIdle, NULL);
        } else {
            /* Not found: show the error and close after NOT_FOUND_CLOSE_MS
             * (Back closes it immediately). */
            app->close_at = furi_get_tick() + NOT_FOUND_CLOSE_MS;
            set_state(
                app,
                StateNotFound,
                det == DetectNoMic ? "Microphone not detected." : "ESP32 not detected.");

            while(furi_get_tick() < app->close_at) {
                if(furi_message_queue_get(app->input_queue, &ev, 100) == FuriStatusOk &&
                   ev.type == InputTypeShort && ev.key == InputKeyBack) {
                    break;
                }
                view_port_update(app->view_port); /* refresh the countdown */
            }
            running = false;
        }
    }
#else
    (void)detect_device; /* unused when the check is disabled */
    if(app->serial) set_state(app, StateIdle, NULL);
#endif

    while(running) {
        bool got_event = furi_message_queue_get(app->input_queue, &ev, 100) == FuriStatusOk;
        if(got_event && (ev.type == InputTypeShort || ev.type == InputTypeRepeat) &&
           (ev.key == InputKeyUp || ev.key == InputKeyDown)) {
            /* Gain: Up = louder, Down = quieter (also while recording) */
            if(ev.key == InputKeyUp && app->gain_idx < GAIN_STEPS - 1) app->gain_idx++;
            if(ev.key == InputKeyDown && app->gain_idx > 0) app->gain_idx--;
            view_port_update(app->view_port);
        } else if(got_event && ev.type == InputTypeShort) {
            if(ev.key == InputKeyOk) {
                if(app->worker) {
                    app->stop_req = true;
                } else if(app->serial) {
                    start_recording(app);
                }
            } else if(ev.key == InputKeyBack) {
                if(app->worker) {
                    app->stop_req = true; /* stop and save first */
                } else {
                    running = false;
                }
            }
        }
        reap_worker(app);
    }

    /* Cleanup */
    if(app->worker) {
        app->stop_req = true;
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    if(app->serial) {
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }

#if POWER_ESP32_FROM_FLIPPER
    if(!otg_was_on) furi_hal_power_disable_otg();
#endif

    expansion_enable(expansion);
    furi_record_close(RECORD_EXPANSION);

    furi_record_close(RECORD_STORAGE);
    furi_stream_buffer_free(app->rx_stream);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
