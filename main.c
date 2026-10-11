/*
 * Vita Sampler - версия 0.2
 *
 * Управление:
 *   X (крест)        - запись / стоп (если запись не сохранена - спросит подтверждение)
 *   Квадрат          - воспроизведение / пауза
 *   Круг             - стоп и возврат в начало
 *   Треугольник      - сохранить запись в WAV (ux0:data/Sampler/)
 *   Касание волны    - мгновенный переход и воспроизведение с этого места
 *   D-pad лево/право - выбрать параметр (BIT DEPTH / SAMPLE RATE)
 *   D-pad вверх/вниз - изменить значение (работает и во время воспроизведения)
 *   START            - выход
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/audioin.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/io/stat.h>
#include <vita2d.h>

unsigned int _newlib_heap_size_user = 64 * 1024 * 1024;

/* ---------- Настройки ---------- */
#define SAMPLE_RATE   16000              /* микрофон Vita пишет 16 кГц моно */
#define OUT_RATE      48000              /* вывод всегда 48 кГц (самый совместимый режим) */
#define MAX_SECONDS   300
#define MAX_SAMPLES   (SAMPLE_RATE * MAX_SECONDS)
#define IN_GRAIN      256
#define OUT_GRAIN     1024
#define MAX_BLOCKS    (MAX_SAMPLES / IN_GRAIN)
#define LIVE_SECONDS  8                  /* окно живой волны при записи */
#define WIN_BLOCKS    (LIVE_SECONDS * SAMPLE_RATE / IN_GRAIN)

/* Раскладка экрана 960x544 */
#define PANEL_X 24
#define PANEL_Y 140
#define PANEL_W 912
#define PANEL_H 190
#define WAVE_X  40
#define WAVE_Y  150
#define WAVE_W  880
#define WAVE_H  150

/* Цвета */
#define C_BG       RGBA8(13, 15, 23, 255)
#define C_PANEL    RGBA8(22, 26, 38, 255)
#define C_PANEL2   RGBA8(34, 40, 58, 255)
#define C_TEXT     RGBA8(236, 239, 246, 255)
#define C_DIM      RGBA8(122, 132, 158, 255)
#define C_ACCENT   RGBA8(56, 200, 255, 255)
#define C_REC      RGBA8(255, 84, 100, 255)
#define C_PLAY     RGBA8(76, 224, 146, 255)
#define C_AMBER    RGBA8(255, 190, 80, 255)
#define C_WAVE_DIM RGBA8(70, 92, 130, 255)
#define C_DARK     RGBA8(14, 16, 24, 255)

enum { ST_IDLE = 0, ST_RECORDING = 1, ST_PLAYING = 2 };

/* Параметры эффекта воспроизведения */
static const int BITS_OPTS[] = { 16, 12, 10, 8, 6, 4, 3, 2 };
static const int RATE_OPTS[] = { 16000, 12000, 8000, 6000, 4000, 3000, 2000, 1000 };
#define N_BITS ((int)(sizeof(BITS_OPTS) / sizeof(BITS_OPTS[0])))
#define N_RATE ((int)(sizeof(RATE_OPTS) / sizeof(RATE_OPTS[0])))

/* ---------- Общие данные ---------- */
static int16_t *g_audio = NULL;
static int16_t g_blk_min[MAX_BLOCKS];    /* мин/макс каждого блока - для волны */
static int16_t g_blk_max[MAX_BLOCKS];
static volatile int g_peak_all = 0;
static volatile int g_length = 0;
static volatile int g_play_pos = 0;
static volatile int g_seek_to = -1;
static volatile int g_state = ST_IDLE;
static volatile int g_running = 1;
static volatile int g_level = 0;
static volatile int g_error = 0;
static volatile int g_bits_idx = 0;
static volatile int g_rate_idx = 0;
static volatile int g_out_port = -1;
static volatile int g_out_ch = 0;

static vita2d_pgf *g_pgf = NULL;

/* ---------- Эффект: уменьшение разрядности ---------- */
static inline int quantize(int v, int bits) {
    if (bits >= 16) return v;
    int shift = 16 - bits;
    int q = ((v + (1 << (shift - 1))) >> shift) << shift;
    if (q > 32767) q = 32767;
    return q;
}

/* ---------- Открытие аудио-выхода (с запасными режимами) ---------- */
static int open_out(int *channels) {
    int p = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_MONO);
    if (p >= 0) { *channels = 1; return p; }
    int p2 = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (p2 >= 0) { *channels = 2; return p2; }
    return p;   /* код первой ошибки */
}

/* ---------- Аудио-поток ---------- */
static int audio_thread(SceSize args, void *argp) {
    static int16_t in_buf[IN_GRAIN] __attribute__((aligned(64)));
    static int16_t out_buf[OUT_GRAIN * 2] __attribute__((aligned(64)));
    static int16_t mono[OUT_GRAIN];

    int in_port = -1, out_port = -1, out_ch = 1;
    int was_playing = 0;
    double pos = 0.0;
    float held = 0.0f, hold_acc = 0.0f;
    const double step = (double)SAMPLE_RATE / (double)OUT_RATE;

    while (g_running) {
        int st = g_state;
        if (st != ST_PLAYING) was_playing = 0;

        if (st == ST_RECORDING) {
            int len = g_length;
            if (len + IN_GRAIN > MAX_SAMPLES) {
                __sync_bool_compare_and_swap(&g_state, ST_RECORDING, ST_IDLE);
                continue;
            }
            if (in_port < 0) {
                in_port = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, IN_GRAIN,
                                             SAMPLE_RATE, SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
                if (in_port < 0) {
                    g_error = in_port;
                    __sync_bool_compare_and_swap(&g_state, ST_RECORDING, ST_IDLE);
                    in_port = -1;
                    continue;
                }
            }

            sceAudioInInput(in_port, in_buf);

            int mn = 32767, mx = -32768, peak = 0;
            for (int i = 0; i < IN_GRAIN; i++) {
                int v = in_buf[i];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
                int a = v < 0 ? -v : v;
                if (a > peak) peak = a;
                g_audio[len + i] = in_buf[i];
            }
            int bi = len / IN_GRAIN;
            g_blk_min[bi] = (int16_t)mn;
            g_blk_max[bi] = (int16_t)mx;
            if (peak > g_peak_all) g_peak_all = peak;
            g_level = peak;
            g_length = len + IN_GRAIN;

        } else if (st == ST_PLAYING) {
            if (in_port >= 0) { sceAudioInReleasePort(in_port); in_port = -1; }

            if (out_port < 0) {
                out_port = open_out(&out_ch);
                if (out_port < 0) {
                    g_error = out_port;
                    g_out_port = -1;
                    __sync_bool_compare_and_swap(&g_state, ST_PLAYING, ST_IDLE);
                    out_port = -1;
                    continue;
                }
                g_out_port = out_port;
                g_out_ch = out_ch;
            }

            if (!was_playing) {
                pos = g_play_pos;
                hold_acc = 0.0f;
                held = 0.0f;
                was_playing = 1;
            }
            int seek = g_seek_to;
            if (seek >= 0) {
                pos = seek;
                g_seek_to = -1;
            }

            int len = g_length;
            if (pos < 0) pos = 0;

            const int16_t *a = g_audio;
            int bits = BITS_OPTS[g_bits_idx];
            int crush = RATE_OPTS[g_rate_idx];
            int do_hold = crush < SAMPLE_RATE;
            float hold_inc = (float)crush / (float)OUT_RATE;
            int ended = 0, peak = 0;

            for (int i = 0; i < OUT_GRAIN; i++) {
                int v = 0;
                if (!ended) {
                    int idx = (int)pos;
                    if (idx >= len - 1) {
                        ended = 1;
                    } else {
                        float fr = (float)(pos - idx);
                        float s = (float)a[idx] + (float)(a[idx + 1] - a[idx]) * fr;
                        if (do_hold) {
                            hold_acc += hold_inc;
                            if (hold_acc >= 1.0f) { hold_acc -= 1.0f; held = s; }
                            s = held;
                        }
                        v = quantize((int)s, bits);
                        pos += step;
                    }
                }
                mono[i] = (int16_t)v;
                int av = v < 0 ? -v : v;
                if (av > peak) peak = av;
            }

            if (out_ch == 1) {
                memcpy(out_buf, mono, sizeof(mono));
            } else {
                for (int i = 0; i < OUT_GRAIN; i++) {
                    out_buf[2 * i] = mono[i];
                    out_buf[2 * i + 1] = mono[i];
                }
            }

            sceAudioOutOutput(out_port, out_buf);
            g_level = peak;

            if (ended) {
                pos = 0.0;
                g_play_pos = 0;
                was_playing = 0;
                __sync_bool_compare_and_swap(&g_state, ST_PLAYING, ST_IDLE);
            } else if (g_state == ST_PLAYING) {
                g_play_pos = (int)pos;
            }

        } else {
            if (in_port >= 0) { sceAudioInReleasePort(in_port); in_port = -1; }
            g_level = 0;
            sceKernelDelayThread(5000);
        }
    }

    if (in_port >= 0) sceAudioInReleasePort(in_port);
    if (out_port >= 0) sceAudioOutReleasePort(out_port);
    return 0;
}

/* ---------- Сохранение в WAV ---------- */
typedef struct __attribute__((packed)) {
    char     riff[4];
    uint32_t size;
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_size;
    uint16_t format;
    uint16_t channels;
    uint32_t rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
    char     data[4];
    uint32_t data_size;
} WavHeader;

static int save_wav(char *path, size_t path_cap) {
    int len = g_length;
    if (len <= 0) return -1;

    sceIoMkdir("ux0:data/Sampler", 0777);

    for (int i = 1; i < 1000; i++) {
        snprintf(path, path_cap, "ux0:data/Sampler/rec_%03d.wav", i);
        FILE *t = fopen(path, "rb");
        if (!t) break;
        fclose(t);
    }

    FILE *f = fopen(path, "wb");
    if (!f) return -2;

    uint32_t bytes = (uint32_t)len * 2;
    WavHeader h;
    memcpy(h.riff, "RIFF", 4);
    h.size = 36 + bytes;
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt, "fmt ", 4);
    h.fmt_size = 16;
    h.format = 1;
    h.channels = 1;
    h.rate = SAMPLE_RATE;
    h.byte_rate = SAMPLE_RATE * 2;
    h.block_align = 2;
    h.bits = 16;
    memcpy(h.data, "data", 4);
    h.data_size = bytes;

    fwrite(&h, sizeof(h), 1, f);
    fwrite(g_audio, 2, len, f);
    fclose(f);
    return 0;
}

/* ---------- Рисование: мелкие помощники ---------- */
static void rect(int x, int y, int w, int h, unsigned int c) {
    vita2d_draw_rectangle(x, y, w, h, c);
}

static void rect_outline(int x, int y, int w, int h, int t, unsigned int c) {
    rect(x, y, w, t, c);
    rect(x, y + h - t, w, t, c);
    rect(x, y, t, h, c);
    rect(x + w - t, y, t, h, c);
}

static void text(int x, int y, unsigned int c, float s, const char *t) {
    vita2d_pgf_draw_text(g_pgf, x, y, c, s, t);
}

static void line2(int x0, int y0, int x1, int y1, unsigned int c) {
    vita2d_draw_line(x0, y0, x1, y1, c);
    vita2d_draw_line(x0 + 1, y0, x1 + 1, y1, c);
}

static void tri_up(int cx, int cy, int hw, unsigned int c) {
    for (int r = 0; r <= hw; r++) rect(cx - r, cy - hw + r, 2 * r + 1, 1, c);
}

static void tri_down(int cx, int cy, int hw, unsigned int c) {
    for (int r = 0; r <= hw; r++) rect(cx - r, cy + hw - r, 2 * r + 1, 1, c);
}

/* Кнопка геймпада: 0 крест, 1 квадрат, 2 круг, 3 треугольник */
static void draw_pad_button(int cx, int cy, int kind) {
    vita2d_draw_fill_circle(cx, cy, 13, C_PANEL2);
    switch (kind) {
    case 0: {
        unsigned int c = RGBA8(120, 170, 255, 255);
        line2(cx - 5, cy - 5, cx + 5, cy + 5, c);
        line2(cx + 5, cy - 5, cx - 5, cy + 5, c);
        break; }
    case 1: {
        unsigned int c = RGBA8(245, 150, 215, 255);
        rect_outline(cx - 5, cy - 5, 11, 11, 2, c);
        break; }
    case 2: {
        unsigned int c = RGBA8(255, 105, 110, 255);
        vita2d_draw_fill_circle(cx, cy, 6, c);
        vita2d_draw_fill_circle(cx, cy, 4, C_PANEL2);
        break; }
    default: {
        unsigned int c = RGBA8(110, 235, 175, 255);
        line2(cx, cy - 6, cx - 6, cy + 5, c);
        line2(cx, cy - 6, cx + 6, cy + 5, c);
        rect(cx - 6, cy + 4, 13, 2, c);
        break; }
    }
}

static void fmt_time(char *out, size_t cap, int samples) {
    int s = samples / SAMPLE_RATE;
    int tenth = (samples % SAMPLE_RATE) * 10 / SAMPLE_RATE;
    snprintf(out, cap, "%02d:%02d.%d", s / 60, s % 60, tenth);
}

/* ---------- Волна ---------- */
static void draw_wave(int state, int length, int play_pos) {
    int x0 = WAVE_X, y0 = WAVE_Y, w = WAVE_W, h = WAVE_H;
    const int COL_W = 2, COL_STEP = 3;
    int ncols = w / COL_STEP;
    int cy = y0 + h / 2;
    int half = h / 2 - 4;
    int nb = length / IN_GRAIN;

    rect(x0, cy, w, 1, C_PANEL2);
    if (nb <= 0) return;

    int peak = g_peak_all;
    if (peak < 3000) peak = 3000;
    float gain = 32768.0f / (float)peak;
    if (gain > 8.0f) gain = 8.0f;

    int play_col = (int)((int64_t)play_pos * ncols / (length > 0 ? length : 1));

    for (int c = 0; c < ncols; c++) {
        int b0, b1;
        if (state == ST_RECORDING) {
            b0 = nb - WIN_BLOCKS + (int)((int64_t)c * WIN_BLOCKS / ncols);
            b1 = nb - WIN_BLOCKS + (int)((int64_t)(c + 1) * WIN_BLOCKS / ncols);
            if (b1 <= b0) b1 = b0 + 1;
            if (b1 <= 0) continue;
            if (b0 < 0) b0 = 0;
        } else {
            b0 = (int)((int64_t)c * nb / ncols);
            b1 = (int)((int64_t)(c + 1) * nb / ncols);
            if (b1 <= b0) b1 = b0 + 1;
            if (b1 > nb) b1 = nb;
            if (b0 >= b1) b0 = b1 - 1;
        }

        int mn = 32767, mx = -32768;
        for (int b = b0; b < b1; b++) {
            if (g_blk_min[b] < mn) mn = g_blk_min[b];
            if (g_blk_max[b] > mx) mx = g_blk_max[b];
        }
        if (mx < mn) continue;

        float fmx = (float)mx * gain / 32768.0f; if (fmx > 1.0f) fmx = 1.0f;
        float fmn = (float)mn * gain / 32768.0f; if (fmn < -1.0f) fmn = -1.0f;
        int ytop = cy - (int)(fmx * half);
        int ybot = cy - (int)(fmn * half);
        if (ybot - ytop < 2) { ytop = cy - 1; ybot = cy + 1; }

        unsigned int col;
        if (state == ST_RECORDING) col = C_REC;
        else if (c <= play_col && (state == ST_PLAYING || play_pos > 0)) col = C_ACCENT;
        else col = C_WAVE_DIM;

        rect(x0 + c * COL_STEP, ytop, COL_W, ybot - ytop, col);
    }

    if (state != ST_RECORDING) {
        int px = x0 + (int)((int64_t)play_pos * w / (length > 0 ? length : 1));
        rect(px - 1, y0 - 2, 3, h + 4, C_TEXT);
    }
}

static void draw_ticks(int length) {
    if (length <= 0) return;
    float total = (float)length / (float)SAMPLE_RATE;
    static const int steps[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300 };
    int step = 300;
    for (int i = 0; i < 9; i++) {
        if (total / (float)steps[i] <= 24.0f) { step = steps[i]; break; }
    }
    int y = WAVE_Y + WAVE_H + 8;
    for (int t = 0; t <= (int)total; t += step) {
        int x = WAVE_X + (int)((int64_t)t * SAMPLE_RATE * WAVE_W / length);
        rect(x, y, 1, (t % (step * 5) == 0) ? 9 : 5, C_DIM);
    }
}

/* ---------- Карточка параметра ---------- */
static void draw_card(int x, int y, int w, int h, const char *label, const char *value,
                      int idx, int n, int selected) {
    rect(x, y, w, h, C_PANEL);
    if (selected) rect_outline(x, y, w, h, 2, C_ACCENT);

    text(x + 18, y + 26, selected ? C_ACCENT : C_DIM, 0.9f, label);
    text(x + 18, y + 62, C_TEXT, 1.6f, value);

    /* шкала качества: чем больше заполнено, тем ближе к оригиналу */
    int filled = n - idx;
    for (int i = 0; i < n; i++) {
        rect(x + w - 24 - (n - i) * 14 - 40, y + 20, 10, 10, i < filled ? C_ACCENT : C_PANEL2);
    }

    if (selected) {
        tri_up(x + w - 30, y + 48, 7, idx > 0 ? C_TEXT : C_PANEL2);
        tri_down(x + w - 30, y + 66, 7, idx < n - 1 ? C_TEXT : C_PANEL2);
    }
}

/* ---------- Главная программа ---------- */
int main(void) {
    g_audio = (int16_t *)malloc(MAX_SAMPLES * sizeof(int16_t));
    if (!g_audio) sceKernelExitProcess(0);

    vita2d_init();
    vita2d_set_clear_color(C_BG);
    g_pgf = vita2d_load_default_pgf();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    SceUID thid = sceKernelCreateThread("audio", audio_thread, 0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(thid, 0, NULL);

    SceCtrlData pad;
    SceTouchData touch;
    unsigned int old_buttons = 0;
    char msg[160] = "Press X to start recording";
    char buf[160], t1[32], t2[32];
    int shown_level = 0;
    int sel = 0;                 /* 0 = bit depth, 1 = sample rate */
    int dirty = 0;               /* есть несохранённая запись */
    int confirm = 0;             /* ждём подтверждения перезаписи */
    int touching = 0, last_tx = -1000;
    unsigned int frame = 0;

    while (1) {
        frame++;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old_buttons;
        old_buttons = pad.buttons;

        if (pressed & SCE_CTRL_START) break;

        int state = g_state;

        /* X: запись / стоп */
        if (pressed & SCE_CTRL_CROSS) {
            if (state == ST_RECORDING) {
                g_state = ST_IDLE;
                dirty = (g_length > 0);
                snprintf(msg, sizeof(msg), "Recording stopped. Press SQUARE to play, TRIANGLE to save");
            } else if (dirty && !confirm) {
                confirm = 1;
                snprintf(msg, sizeof(msg), "Unsaved recording! Press X again to erase it, or TRIANGLE to save");
            } else {
                g_state = ST_IDLE;
                sceKernelDelayThread(60000);
                g_length = 0;
                g_play_pos = 0;
                g_seek_to = -1;
                g_peak_all = 0;
                g_error = 0;
                dirty = 0;
                confirm = 0;
                __sync_synchronize();
                g_state = ST_RECORDING;
                snprintf(msg, sizeof(msg), "Recording... press X to stop");
            }
        }

        /* Квадрат: play / pause */
        if ((pressed & SCE_CTRL_SQUARE) && state != ST_RECORDING) {
            if (state == ST_PLAYING) {
                g_state = ST_IDLE;
                snprintf(msg, sizeof(msg), "Paused");
            } else if (g_length > 0) {
                g_error = 0;
                g_state = ST_PLAYING;
                snprintf(msg, sizeof(msg), "Playing");
            }
        }

        /* Круг: стоп и в начало */
        if ((pressed & SCE_CTRL_CIRCLE) && state != ST_RECORDING) {
            g_state = ST_IDLE;
            sceKernelDelayThread(60000);
            g_play_pos = 0;
            g_seek_to = 0;
            snprintf(msg, sizeof(msg), "Stopped");
        }

        /* Треугольник: сохранить */
        if ((pressed & SCE_CTRL_TRIANGLE) && state != ST_RECORDING) {
            char path[128];
            int r = save_wav(path, sizeof(path));
            if (r == 0) { snprintf(msg, sizeof(msg), "Saved: %s", path); dirty = 0; }
            else if (r == -1) snprintf(msg, sizeof(msg), "Nothing to save - record something first");
            else snprintf(msg, sizeof(msg), "Could not write the file");
        }

        /* D-pad: выбор и изменение параметров */
        if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) sel = 1 - sel;
        if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
            int d = (pressed & SCE_CTRL_UP) ? -1 : 1;     /* вверх = ближе к оригиналу */
            if (sel == 0) {
                int v = g_bits_idx + d;
                if (v < 0) v = 0;
                if (v > N_BITS - 1) v = N_BITS - 1;
                g_bits_idx = v;
                snprintf(msg, sizeof(msg), "Bit depth: %d bit", BITS_OPTS[v]);
            } else {
                int v = g_rate_idx + d;
                if (v < 0) v = 0;
                if (v > N_RATE - 1) v = N_RATE - 1;
                g_rate_idx = v;
                snprintf(msg, sizeof(msg), "Sample rate: %d Hz", RATE_OPTS[v]);
            }
        }

        if (pressed & ~SCE_CTRL_CROSS) confirm = 0;

        /* Касание волны: мгновенный переход */
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        if (touch.reportNum > 0 && g_state != ST_RECORDING && g_length > 0) {
            int tx = touch.report[0].x / 2;
            int ty = touch.report[0].y / 2;
            if (tx >= WAVE_X && tx <= WAVE_X + WAVE_W &&
                ty >= PANEL_Y && ty <= PANEL_Y + PANEL_H) {
                if (!touching || (tx - last_tx > 3) || (last_tx - tx > 3)) {
                    int target = (int)((int64_t)(tx - WAVE_X) * g_length / WAVE_W);
                    g_seek_to = target;
                    g_error = 0;
                    g_state = ST_PLAYING;
                    last_tx = tx;
                }
                touching = 1;
            } else {
                touching = 0;
            }
        } else {
            touching = 0;
        }

        /* ---------- Рисуем ---------- */
        state = g_state;
        int length = g_length;
        int pos = g_play_pos;

        vita2d_start_drawing();
        vita2d_clear_screen();

        /* шапка */
        rect(24, 16, 6, 34, C_ACCENT);
        text(42, 46, C_TEXT, 1.5f, "VITA SAMPLER");
        text(260, 46, C_DIM, 0.9f, "v0.2");

        const char *label = "READY";
        unsigned int pill = C_PANEL2, pill_text = C_DIM;
        if (state == ST_RECORDING) { label = "REC";  pill = C_REC;  pill_text = C_DARK; }
        if (state == ST_PLAYING)   { label = "PLAY"; pill = C_PLAY; pill_text = C_DARK; }
        rect(800, 14, 136, 38, pill);
        text(842, 41, pill_text, 1.2f, label);
        if (state == ST_RECORDING && ((frame / 30) % 2 == 0))
            vita2d_draw_fill_circle(820, 33, 6, C_DARK);

        /* время */
        fmt_time(t1, sizeof(t1), state == ST_RECORDING ? length : pos);
        text(30, 122, C_TEXT, 2.0f, t1);
        if (state != ST_RECORDING) {
            fmt_time(t2, sizeof(t2), length);
            snprintf(buf, sizeof(buf), "/ %s", t2);
            text(230, 122, C_DIM, 1.1f, buf);
        }

        /* индикатор уровня */
        int lvl = g_level * 100 / 32768;
        if (lvl > 100) lvl = 100;
        if (lvl > shown_level) shown_level = lvl;
        else shown_level = shown_level * 85 / 100;
        text(520, 100, C_DIM, 0.8f, "LEVEL");
        rect(580, 90, 356, 12, C_PANEL2);
        rect(580, 90, 356 * shown_level / 100, 12,
             shown_level > 85 ? C_REC : (shown_level > 60 ? C_AMBER : C_PLAY));

        /* панель волны */
        rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, C_PANEL);
        if (length == 0) {
            text(WAVE_X + 260, WAVE_Y + 80, C_DIM, 1.0f, "no recording yet - press X");
        }
        draw_wave(state, length, pos);
        if (state != ST_RECORDING) draw_ticks(length);
        else text(WAVE_X, WAVE_Y + WAVE_H + 22, C_DIM, 0.8f, "live view: last 8 seconds");

        /* карточки параметров */
        snprintf(buf, sizeof(buf), "%d bit%s", BITS_OPTS[g_bits_idx], g_bits_idx == 0 ? "  (original)" : "");
        draw_card(24, 344, 444, 84, "BIT DEPTH  (playback)", buf, g_bits_idx, N_BITS, sel == 0);
        snprintf(buf, sizeof(buf), "%d Hz%s", RATE_OPTS[g_rate_idx], g_rate_idx == 0 ? "  (original)" : "");
        draw_card(492, 344, 444, 84, "SAMPLE RATE  (playback)", buf, g_rate_idx, N_RATE, sel == 1);

        /* сообщения */
        text(30, 456, confirm ? C_AMBER : C_TEXT, 1.0f, msg);
        if (g_error != 0) {
            snprintf(buf, sizeof(buf), "Audio error: 0x%08X", (unsigned int)g_error);
            text(30, 478, C_AMBER, 0.85f, buf);
        } else if (g_out_port >= 0) {
            snprintf(buf, sizeof(buf), "audio out: %d Hz, %s", OUT_RATE, g_out_ch == 1 ? "mono" : "stereo");
            text(30, 478, C_DIM, 0.8f, buf);
        }

        /* подсказки */
        draw_pad_button(44, 508, 0);  text(66, 514, C_DIM, 0.85f, "rec / stop");
        draw_pad_button(212, 508, 1); text(234, 514, C_DIM, 0.85f, "play / pause");
        draw_pad_button(402, 508, 2); text(424, 514, C_DIM, 0.85f, "stop");
        draw_pad_button(512, 508, 3); text(534, 514, C_DIM, 0.85f, "save WAV");
        text(670, 514, C_DIM, 0.85f, "D-pad: select / change");
        text(868, 514, C_DIM, 0.85f, "START: exit");

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    g_running = 0;
    sceKernelWaitThreadEnd(thid, NULL, NULL);

    vita2d_free_pgf(g_pgf);
    vita2d_fini();
    free(g_audio);
    sceKernelExitProcess(0);
    return 0;
}
