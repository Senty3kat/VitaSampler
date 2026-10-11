/*
 * Vita Sampler - версия 0.3
 *
 * Управление:
 *   X (крест)         - запись / стоп
 *   Квадрат           - воспроизведение / пауза
 *   Круг              - стоп и возврат в начало
 *   Треугольник       - сохранить запись в WAV (ux0:data/Sampler/)
 *   L / R             - масштаб волны (отдалить / приблизить к курсору)
 *   D-pad лево/право  - двигать курсор (тап = 1 сэмпл, удержание = быстрее)
 *   D-pad вверх/вниз  - менять значение выбранного параметра
 *   SELECT            - переключить параметр (BIT DEPTH / SAMPLE RATE)
 *   Касание волны     - мгновенный переход; дальше палец двигает курсор
 *                       с замедлением (медленно = точно, быстро = грубо)
 *   START             - выход
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

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
#define SAMPLE_RATE   16000
#define OUT_RATE      48000
#define MAX_SECONDS   300
#define MAX_SAMPLES   (SAMPLE_RATE * MAX_SECONDS)
#define IN_GRAIN      256
#define OUT_GRAIN     1024
#define MAX_BLOCKS    (MAX_SAMPLES / IN_GRAIN)
#define LIVE_SAMPLES  (8 * SAMPLE_RATE)        /* живая волна при записи: 8 секунд */
#define MIN_VIEW      256                      /* максимальное приближение */

#define PANEL_X 24
#define PANEL_Y 140
#define PANEL_W 912
#define PANEL_H 190
#define WAVE_X  40
#define WAVE_Y  150
#define WAVE_W  880
#define WAVE_H  150
#define N_WAVE_TEX 4

#define N_BITS 16                              /* 16 ... 1 бит */
#define N_RATE 40                              /* плавные шаги 16000 ... 500 Гц */

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
#define C_DARK     RGBA8(14, 16, 24, 255)
#define C_WAVE     RGBA8(120, 170, 230, 255)
#define C_GRID     RGBA8(48, 56, 78, 255)

enum { ST_IDLE = 0, ST_RECORDING = 1, ST_PLAYING = 2 };

/* ---------- Общие данные ---------- */
static int16_t *g_audio = NULL;
static int16_t g_blk_min[MAX_BLOCKS];
static int16_t g_blk_max[MAX_BLOCKS];
static int g_rate_tab[N_RATE];

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
static volatile int g_out_ch = 0;
static volatile int g_discard = 0;
static volatile int g_scrub = 0;
static volatile float g_scrub_target = 0.0f;

static vita2d_pgf *g_pgf = NULL;
static vita2d_texture *g_wave_tex[N_WAVE_TEX];

/* ---------- Эффект: уменьшение разрядности (16 ... 1 бит) ---------- */
static inline int quantize(int v, int bits) {
    if (bits >= 16) return v;
    if (v < -32768) v = -32768;
    if (v > 32767) v = 32767;
    int levels = 1 << bits;
    int step = 65536 / levels;
    int q = (v + 32768) / step;
    if (q > levels - 1) q = levels - 1;
    int out = q * step + step / 2 - 32768;
    int maxa = 32768 - step / 2;
    return out * 32767 / maxa;
}

/* ---------- Поток записи: микрофон открывается ОДИН раз ---------- */
static int in_thread(SceSize args, void *argp) {
    static int16_t in_buf[IN_GRAIN] __attribute__((aligned(64)));
    int in_port = -1;

    while (g_running) {
        if (in_port < 0) {
            if (g_state != ST_RECORDING) { sceKernelDelayThread(10000); continue; }
            in_port = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, IN_GRAIN,
                                         SAMPLE_RATE, SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
            if (in_port < 0) {
                g_error = in_port;
                __sync_bool_compare_and_swap(&g_state, ST_RECORDING, ST_IDLE);
                in_port = -1;
                continue;
            }
        }

        sceAudioInInput(in_port, in_buf);      /* ждёт, пока наберётся блок */

        if (g_state != ST_RECORDING) continue; /* вне записи просто сбрасываем данные */
        if (g_discard > 0) { g_discard--; continue; }

        int len = g_length;
        if (len + IN_GRAIN > MAX_SAMPLES) {
            __sync_bool_compare_and_swap(&g_state, ST_RECORDING, ST_IDLE);
            continue;
        }

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
    }

    if (in_port >= 0) sceAudioInReleasePort(in_port);
    return 0;
}

/* ---------- Открытие аудио-выхода ---------- */
static int open_out(int *channels) {
    int p = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_MONO);
    if (p >= 0) { *channels = 1; return p; }
    int p2 = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (p2 >= 0) { *channels = 2; return p2; }
    return p;
}

/* ---------- Поток воспроизведения ---------- */
static int out_thread(SceSize args, void *argp) {
    static int16_t out_buf[OUT_GRAIN * 2] __attribute__((aligned(64)));
    static int16_t mono[OUT_GRAIN];

    int out_port = -1, out_ch = 1;
    int was_playing = 0;
    double pos = 0.0;
    float held = 0.0f, hold_acc = 0.0f, scrub_gain = 0.0f;
    const double step = (double)SAMPLE_RATE / (double)OUT_RATE;

    while (g_running) {
        if (g_state != ST_PLAYING) {
            was_playing = 0;
            sceKernelDelayThread(3000);
            continue;
        }

        if (out_port < 0) {
            out_port = open_out(&out_ch);
            if (out_port < 0) {
                g_error = out_port;
                __sync_bool_compare_and_swap(&g_state, ST_PLAYING, ST_IDLE);
                out_port = -1;
                continue;
            }
            g_out_ch = out_ch;
        }

        if (!was_playing) {
            pos = g_play_pos;
            hold_acc = 0.0f;
            held = 0.0f;
            scrub_gain = 0.0f;
            was_playing = 1;
        }
        int seek = g_seek_to;
        if (seek >= 0) { pos = seek; g_seek_to = -1; }

        int len = g_length;
        if (len < 2) {
            __sync_bool_compare_and_swap(&g_state, ST_PLAYING, ST_IDLE);
            continue;
        }
        if (pos < 0.0) pos = 0.0;

        /* режим скраба: скорость воспроизведения следует за пальцем */
        int scrub = g_scrub;
        double rate_src = step;
        float g0 = 1.0f, g1 = 1.0f;
        if (scrub) {
            double r = ((double)g_scrub_target - pos) / (double)OUT_GRAIN;
            if (r > 1.0) r = 1.0;
            if (r < -1.0) r = -1.0;
            rate_src = r;
            float ar = (float)(r < 0.0 ? -r : r);
            g0 = scrub_gain;
            g1 = ar / 0.03f;
            if (g1 > 1.0f) g1 = 1.0f;
            scrub_gain = g1;
        } else {
            scrub_gain = 1.0f;
        }

        const int16_t *a = g_audio;
        int bits = 16 - g_bits_idx;
        int crush = g_rate_tab[g_rate_idx];
        int do_hold = crush < SAMPLE_RATE;
        float hold_inc = (float)crush / (float)OUT_RATE;
        int ended = 0, peak = 0;

        for (int i = 0; i < OUT_GRAIN; i++) {
            int v = 0;
            if (!ended) {
                int idx = (int)pos;
                if (!scrub && idx >= len - 1) {
                    ended = 1;
                } else {
                    if (idx > len - 2) idx = len - 2;
                    if (idx < 0) idx = 0;
                    float fr = (float)(pos - idx);
                    if (fr < 0.0f) fr = 0.0f;
                    if (fr > 1.0f) fr = 1.0f;
                    float s = (float)a[idx] + (float)(a[idx + 1] - a[idx]) * fr;
                    if (do_hold) {
                        hold_acc += hold_inc;
                        if (hold_acc >= 1.0f) { hold_acc -= 1.0f; held = s; }
                        s = held;
                    }
                    v = quantize((int)s, bits);
                    if (scrub) {
                        float gcur = g0 + (g1 - g0) * (float)i / (float)OUT_GRAIN;
                        v = (int)((float)v * gcur);
                    }
                    pos += rate_src;
                    if (pos < 0.0) pos = 0.0;
                    if (scrub && pos > (double)(len - 2)) pos = (double)(len - 2);
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
    }

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

/* ---------- Рисование: помощники ---------- */
static void rect(int x, int y, int w, int h, unsigned int c) {
    if (w <= 0 || h <= 0) return;
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

static void tri_up(int cx, int cy, int hw, unsigned int c) {
    for (int r = 0; r <= hw; r++) rect(cx - r, cy - hw + r, 2 * r + 1, 1, c);
}

static void tri_down(int cx, int cy, int hw, unsigned int c) {
    for (int r = 0; r <= hw; r++) rect(cx - r, cy + hw - r, 2 * r + 1, 1, c);
}

/* Значки кнопок: 0 крест, 1 квадрат, 2 круг, 3 треугольник (только прямоугольники и круги) */
static void draw_pad_button(int cx, int cy, int kind) {
    vita2d_draw_fill_circle(cx, cy, 13, C_PANEL2);
    switch (kind) {
    case 0: {
        unsigned int c = RGBA8(120, 170, 255, 255);
        for (int i = -5; i <= 5; i += 2) {
            rect(cx + i - 1, cy + i - 1, 3, 3, c);
            rect(cx + i - 1, cy - i - 1, 3, 3, c);
        }
        break; }
    case 1:
        rect_outline(cx - 5, cy - 5, 11, 11, 2, RGBA8(245, 150, 215, 255));
        break;
    case 2:
        vita2d_draw_fill_circle(cx, cy, 6, RGBA8(255, 105, 110, 255));
        vita2d_draw_fill_circle(cx, cy, 4, C_PANEL2);
        break;
    default:
        tri_up(cx, cy + 1, 6, RGBA8(110, 235, 175, 255));
        break;
    }
}

static void fmt_time(char *out, size_t cap, int samples) {
    int s = samples / SAMPLE_RATE;
    int tenth = (samples % SAMPLE_RATE) * 10 / SAMPLE_RATE;
    snprintf(out, cap, "%02d:%02d.%d", s / 60, s % 60, tenth);
}

static int level_percent(int peak) {
    if (peak <= 0) return 0;
    float db = 20.0f * log10f((float)peak / 32768.0f);   /* 0 dB = полная шкала */
    float p = (db + 60.0f) / 60.0f;                       /* шкала -60 ... 0 dB */
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return (int)(p * 100.0f);
}

/* ---------- Волна: рисуем процессором в текстуру (1 draw-вызов вместо сотен) ---------- */
static void render_wave(vita2d_texture *tex, int vs, int vl, int length, int live) {
    uint32_t *px = (uint32_t *)vita2d_texture_get_datap(tex);
    int stride = (int)vita2d_texture_get_stride(tex) / 4;
    memset(px, 0, (size_t)stride * WAVE_H * 4);

    int cy = WAVE_H / 2;
    int half = WAVE_H / 2 - 4;
    for (int x = 0; x < WAVE_W; x++) px[cy * stride + x] = C_GRID;
    if (length <= 0 || vl <= 0) return;

    int peak = g_peak_all;
    if (peak < 3000) peak = 3000;
    float gain = 32768.0f / (float)peak;
    if (gain > 8.0f) gain = 8.0f;

    uint32_t col = live ? C_REC : C_WAVE;
    int nb = length / IN_GRAIN;
    int use_blocks = (vl / WAVE_W) >= 512;

    for (int x = 0; x < WAVE_W; x++) {
        int64_t s0 = (int64_t)vs + (int64_t)x * vl / WAVE_W;
        int64_t s1 = (int64_t)vs + (int64_t)(x + 1) * vl / WAVE_W;
        if (s1 <= s0) s1 = s0 + 1;
        if (s1 <= 0 || s0 >= length) continue;
        if (s0 < 0) s0 = 0;

        int mn = 32767, mx = -32768;
        if (use_blocks && nb > 0) {
            int b0 = (int)(s0 / IN_GRAIN);
            int b1 = (int)((s1 + IN_GRAIN - 1) / IN_GRAIN);
            if (b1 > nb) b1 = nb;
            if (b0 >= b1) b0 = b1 - 1;
            if (b0 < 0) continue;
            for (int b = b0; b < b1; b++) {
                if (g_blk_min[b] < mn) mn = g_blk_min[b];
                if (g_blk_max[b] > mx) mx = g_blk_max[b];
            }
        } else {
            int64_t e = s1;                       /* включительно: соединяет соседние точки */
            if (e > length - 1) e = length - 1;
            for (int64_t s = s0; s <= e; s++) {
                int v = g_audio[s];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
        }
        if (mx < mn) continue;

        float fmx = (float)mx * gain / 32768.0f; if (fmx > 1.0f) fmx = 1.0f;
        float fmn = (float)mn * gain / 32768.0f; if (fmn < -1.0f) fmn = -1.0f;
        int ytop = cy - (int)(fmx * half);
        int ybot = cy - (int)(fmn * half);
        if (ybot - ytop < 1) { ytop = cy - 1; ybot = cy + 1; }
        if (ytop < 0) ytop = 0;
        if (ybot > WAVE_H - 1) ybot = WAVE_H - 1;
        for (int y = ytop; y <= ybot; y++) px[y * stride + x] = col;
    }
}

static void draw_ticks(int vs, int vl, int length) {
    if (length <= 0 || vl <= 0) return;
    static const int steps[] = { 16, 32, 80, 160, 400, 800, 1600, 3200, 8000, 16000,
                                 32000, 80000, 160000, 320000, 800000, 1600000, 3200000 };
    const int n = (int)(sizeof(steps) / sizeof(steps[0]));
    int step = steps[n - 1];
    for (int i = 0; i < n; i++) {
        if (vl / steps[i] <= 24) { step = steps[i]; break; }
    }
    int y = WAVE_Y + WAVE_H + 8;
    int64_t first = ((int64_t)vs + step - 1) / step * step;
    int count = 0;
    for (int64_t s = first; s <= (int64_t)vs + vl && s <= length && count < 40; s += step, count++) {
        int x = WAVE_X + (int)((s - vs) * WAVE_W / vl);
        rect(x, y, 1, ((s / step) % 5 == 0) ? 9 : 5, C_DIM);
    }
}

/* ---------- Карточка параметра ---------- */
static void draw_card(int x, int y, int w, int h, const char *label, const char *value,
                      float quality, int selected) {
    rect(x, y, w, h, C_PANEL);
    if (selected) rect_outline(x, y, w, h, 2, C_ACCENT);

    text(x + 18, y + 26, selected ? C_ACCENT : C_DIM, 0.9f, label);
    text(x + 18, y + 64, C_TEXT, 1.6f, value);

    /* шкала качества: заполнена = ближе к оригиналу */
    int bw = 150;
    rect(x + w - 24 - bw - 36, y + 18, bw, 8, C_PANEL2);
    rect(x + w - 24 - bw - 36, y + 18, (int)(bw * quality), 8, selected ? C_ACCENT : C_DIM);

    if (selected) {
        tri_up(x + w - 30, y + 48, 7, C_TEXT);
        tri_down(x + w - 30, y + 68, 7, C_TEXT);
    }
}

/* ---------- Вид волны (масштаб) ---------- */
static int view_start = 0;
static int view_len = LIVE_SAMPLES;

static void clamp_view(int length) {
    int minv = length < MIN_VIEW ? (length > 0 ? length : 1) : MIN_VIEW;
    int maxv = length > 0 ? length : MIN_VIEW;
    if (view_len < minv) view_len = minv;
    if (view_len > maxv) view_len = maxv;
    if (view_start > length - view_len) view_start = length - view_len;
    if (view_start < 0) view_start = 0;
}

static void zoom_view(int dir, int cursor, int length) {
    int64_t nl = dir > 0 ? view_len / 2 : (int64_t)view_len * 2;
    if (nl > 100000000) nl = 100000000;
    float a = (cursor >= view_start && cursor <= view_start + view_len)
              ? (float)(cursor - view_start) / (float)view_len : 0.5f;
    view_len = (int)nl;
    clamp_view(length);
    view_start = cursor - (int)(a * (float)view_len);
    clamp_view(length);
}

static void follow_cursor(int cursor, int length) {
    if (view_len >= length) { view_start = 0; return; }
    if (cursor > view_start + view_len) view_start = cursor - view_len / 10;
    else if (cursor < view_start) view_start = cursor - view_len * 9 / 10;
    clamp_view(length);
}

static float ballistic(float s) {
    const float lo = 1.5f, hi = 24.0f, fmin_ = 0.08f;
    if (s <= lo) return fmin_;
    if (s >= hi) return 1.0f;
    return fmin_ + (1.0f - fmin_) * (s - lo) / (hi - lo);
}

static void set_cursor(int p, int length) {
    if (p > length - 1) p = length - 1;
    if (p < 0) p = 0;
    if (g_state == ST_PLAYING) g_seek_to = p;
    g_play_pos = p;
}

/* ---------- Главная программа ---------- */
int main(void) {
    g_audio = (int16_t *)malloc(MAX_SAMPLES * sizeof(int16_t));
    if (!g_audio) sceKernelExitProcess(0);

    for (int i = 0; i < N_RATE; i++) {
        double f = pow(500.0 / 16000.0, (double)i / (double)(N_RATE - 1));
        g_rate_tab[i] = (int)(16000.0 * f + 0.5);
    }

    vita2d_init();
    vita2d_set_clear_color(C_BG);
    g_pgf = vita2d_load_default_pgf();
    for (int i = 0; i < N_WAVE_TEX; i++) g_wave_tex[i] = vita2d_create_empty_texture(WAVE_W, WAVE_H);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    SceUID th_in = sceKernelCreateThread("audio_in", in_thread, 0x10000100, 0x10000, 0, 0, NULL);
    SceUID th_out = sceKernelCreateThread("audio_out", out_thread, 0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(th_in, 0, NULL);
    sceKernelStartThread(th_out, 0, NULL);

    SceCtrlData pad;
    SceTouchData touch;
    unsigned int old_buttons = 0;
    char msg[160] = "Press X to start recording";
    char buf[160], t1[32], t2[32];
    int shown_level = 0, clip_hold = 0;
    int sel = 0;
    int dirty = 0, confirm = 0;
    unsigned int frame = 0;

    int scrubbing = 0, resume_state = ST_IDLE, last_raw_x = 0;
    double scrub_pos = 0.0;
    float speed_f = 0.0f, scrub_factor = 1.0f;
    int nudge_frames = 0, ud_frames = 0, zm_frames = 0;

    int wave_cur = -1, wave_next = 0;
    int l_vs = -1, l_vl = -1, l_len = -1, l_live = -1, l_peak = -1;

    while (1) {
        frame++;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old_buttons;
        old_buttons = pad.buttons;
        if (pressed & SCE_CTRL_START) break;

        int state = g_state;
        int length = g_length;

        /* X: запись / стоп */
        if (pressed & SCE_CTRL_CROSS) {
            if (state == ST_RECORDING) {
                g_state = ST_IDLE;
                sceKernelDelayThread(40000);
                length = g_length;
                dirty = (length > 0);
                view_start = 0;
                view_len = length > 0 ? length : MIN_VIEW;
                clamp_view(length);
                snprintf(msg, sizeof(msg), "Recording stopped. SQUARE: play   TRIANGLE: save");
            } else if (dirty && !confirm) {
                confirm = 1;
                snprintf(msg, sizeof(msg), "Unsaved recording! Press X again to erase it, or TRIANGLE to save");
            } else {
                g_state = ST_IDLE;
                g_scrub = 0;
                scrubbing = 0;
                sceKernelDelayThread(60000);
                g_length = 0;
                g_play_pos = 0;
                g_seek_to = -1;
                g_peak_all = 0;
                g_error = 0;
                g_discard = 3;
                dirty = 0;
                confirm = 0;
                view_start = 0;
                view_len = LIVE_SAMPLES;
                __sync_synchronize();
                g_state = ST_RECORDING;
                snprintf(msg, sizeof(msg), "Recording... press X to stop");
            }
            state = g_state;
            length = g_length;
        }

        /* Квадрат: play / pause */
        if ((pressed & SCE_CTRL_SQUARE) && state != ST_RECORDING) {
            if (state == ST_PLAYING) {
                g_state = ST_IDLE;
                snprintf(msg, sizeof(msg), "Paused");
            } else if (length > 1) {
                g_error = 0;
                g_state = ST_PLAYING;
                snprintf(msg, sizeof(msg), "Playing");
            }
        }

        /* Круг: стоп и в начало */
        if ((pressed & SCE_CTRL_CIRCLE) && state != ST_RECORDING) {
            g_state = ST_IDLE;
            sceKernelDelayThread(40000);
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

        /* SELECT: переключить параметр */
        if (pressed & SCE_CTRL_SELECT) sel = 1 - sel;

        /* D-pad вверх/вниз: значение параметра (с повтором при удержании) */
        {
            int ud = ((pad.buttons & SCE_CTRL_DOWN) ? 1 : 0) - ((pad.buttons & SCE_CTRL_UP) ? 1 : 0);
            if (ud) {
                ud_frames++;
                if (ud_frames == 1 || (ud_frames > 18 && ud_frames % 3 == 0)) {
                    if (sel == 0) {
                        int v = g_bits_idx + ud;
                        if (v < 0) v = 0;
                        if (v > N_BITS - 1) v = N_BITS - 1;
                        g_bits_idx = v;
                        snprintf(msg, sizeof(msg), "Bit depth: %d bit", 16 - v);
                    } else {
                        int v = g_rate_idx + ud;
                        if (v < 0) v = 0;
                        if (v > N_RATE - 1) v = N_RATE - 1;
                        g_rate_idx = v;
                        snprintf(msg, sizeof(msg), "Sample rate: %d Hz", g_rate_tab[v]);
                    }
                }
            } else ud_frames = 0;
        }

        /* L / R: масштаб */
        if (state != ST_RECORDING && length > 0) {
            int zm = ((pad.buttons & SCE_CTRL_RTRIGGER) ? 1 : 0) - ((pad.buttons & SCE_CTRL_LTRIGGER) ? 1 : 0);
            if (zm) {
                zm_frames++;
                if (zm_frames == 1 || (zm_frames > 20 && zm_frames % 8 == 0)) {
                    int cur = scrubbing ? (int)scrub_pos : g_play_pos;
                    zoom_view(zm, cur, length);
                }
            } else zm_frames = 0;

            /* D-pad лево/право: двигать курсор */
            int dir = ((pad.buttons & SCE_CTRL_RIGHT) ? 1 : 0) - ((pad.buttons & SCE_CTRL_LEFT) ? 1 : 0);
            if (dir && !scrubbing) {
                nudge_frames++;
                int move = 0;
                if (nudge_frames == 1) move = 1;
                else if (nudge_frames > 15) {
                    int px = view_len / WAVE_W;
                    if (px < 1) px = 1;
                    move = px * (1 + (nudge_frames - 15) / 15);
                }
                if (move) set_cursor(g_play_pos + dir * move, length);
            } else nudge_frames = 0;
        }

        /* Касание волны: прыжок, затем точный скраб */
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        int touch_in = 0, raw_x = 0;
        if (touch.reportNum > 0 && g_state != ST_RECORDING && length > 1) {
            raw_x = touch.report[0].x;
            int tx = raw_x / 2;
            int ty = touch.report[0].y / 2;
            if (tx >= WAVE_X && tx <= WAVE_X + WAVE_W && ty >= PANEL_Y && ty <= PANEL_Y + PANEL_H)
                touch_in = 1;
        }
        if (touch_in && !scrubbing) {
            scrubbing = 1;
            resume_state = g_state;
            double p = (double)view_start + (double)(raw_x / 2 - WAVE_X) * (double)view_len / (double)WAVE_W;
            if (p < 0.0) p = 0.0;
            if (p > (double)(length - 1)) p = (double)(length - 1);
            scrub_pos = p;
            last_raw_x = raw_x;
            speed_f = 0.0f;
            scrub_factor = 1.0f;
            g_scrub_target = (float)p;
            g_seek_to = (int)p;
            g_play_pos = (int)p;
            g_error = 0;
            g_scrub = 1;
            g_state = ST_PLAYING;
        } else if (touch_in && scrubbing) {
            int dx = raw_x - last_raw_x;
            last_raw_x = raw_x;
            float sp = (float)(dx < 0 ? -dx : dx);
            speed_f = 0.6f * speed_f + 0.4f * sp;
            scrub_factor = ballistic(speed_f);
            double spp = (double)view_len / (double)WAVE_W;
            scrub_pos += (double)dx * 0.5 * spp * (double)scrub_factor;
            if (scrub_pos < 0.0) scrub_pos = 0.0;
            if (scrub_pos > (double)(length - 1)) scrub_pos = (double)(length - 1);
            g_scrub_target = (float)scrub_pos;
        } else if (!touch_in && scrubbing) {
            scrubbing = 0;
            g_scrub = 0;
            int endpos = (int)scrub_pos;
            if (resume_state == ST_PLAYING) {
                g_seek_to = endpos;
            } else {
                g_state = ST_IDLE;
                sceKernelDelayThread(40000);
                g_play_pos = endpos;
                g_seek_to = endpos;
            }
        }

        if (pressed & ~SCE_CTRL_CROSS) confirm = 0;

        /* ---------- Подготовка к рисованию ---------- */
        state = g_state;
        length = g_length;
        int cursor = scrubbing ? (int)scrub_pos : g_play_pos;
        if (state == ST_RECORDING) cursor = length;

        if (state != ST_RECORDING) {
            clamp_view(length);
            follow_cursor(cursor, length);
        }

        /* текстура волны обновляется только при изменениях */
        {
            int live = (state == ST_RECORDING);
            int vs = live ? length - LIVE_SAMPLES : view_start;
            int vl = live ? LIVE_SAMPLES : view_len;
            int peak = g_peak_all;
            if (live || vs != l_vs || vl != l_vl || length != l_len || live != l_live || peak != l_peak) {
                wave_cur = wave_next;
                wave_next = (wave_next + 1) % N_WAVE_TEX;
                if (g_wave_tex[wave_cur]) render_wave(g_wave_tex[wave_cur], vs, vl, length, live);
                l_vs = vs; l_vl = vl; l_len = length; l_live = live; l_peak = peak;
            }
        }

        /* ---------- Рисуем ---------- */
        vita2d_start_drawing();
        vita2d_clear_screen();

        /* шапка */
        rect(24, 16, 6, 34, C_ACCENT);
        text(42, 46, C_TEXT, 1.5f, "VITA SAMPLER");
        text(260, 46, C_DIM, 0.9f, "v0.3");

        const char *label = "READY";
        unsigned int pill = C_PANEL2, pill_text = C_DIM;
        if (state == ST_RECORDING) { label = "REC";  pill = C_REC;  pill_text = C_DARK; }
        if (state == ST_PLAYING)   { label = scrubbing ? "SCRUB" : "PLAY"; pill = C_PLAY; pill_text = C_DARK; }
        rect(800, 14, 136, 38, pill);
        text(scrubbing ? 828 : 842, 41, pill_text, 1.2f, label);

        /* время и номер сэмпла */
        fmt_time(t1, sizeof(t1), cursor);
        text(30, 120, C_TEXT, 2.0f, t1);
        snprintf(buf, sizeof(buf), "sample %d / %d", cursor, length);
        text(240, 98, C_TEXT, 1.0f, buf);
        fmt_time(t2, sizeof(t2), length);
        {
            int tenths = (int)((int64_t)view_len * 10 / WAVE_W);
            int zoomx = view_len > 0 ? length / view_len : 1;
            if (zoomx < 1) zoomx = 1;
            if (state == ST_RECORDING)
                snprintf(buf, sizeof(buf), "total %s   (max 05:00)", t1);
            else
                snprintf(buf, sizeof(buf), "total %s   zoom x%d   1 px = %d.%d smp", t2, zoomx, tenths / 10, tenths % 10);
            text(240, 122, C_DIM, 0.8f, buf);
        }

        /* индикатор уровня (шкала в децибелах) */
        int lvl = (state == ST_IDLE) ? 0 : level_percent(g_level);
        if (g_level >= 32000 && state != ST_IDLE) clip_hold = 60;
        if (clip_hold > 0) clip_hold--;
        if (lvl > shown_level) shown_level = lvl;
        else shown_level = shown_level * 88 / 100;
        text(580, 101, C_DIM, 0.8f, "LEVEL");
        rect(640, 90, 296, 12, C_PANEL2);
        rect(640, 90, 296 * shown_level / 100, 12,
             shown_level > 90 ? C_REC : (shown_level > 75 ? C_AMBER : C_PLAY));
        if (clip_hold > 0) text(640, 124, C_REC, 0.9f, "CLIP!");

        /* панель волны */
        rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, C_PANEL);
        if (wave_cur >= 0 && g_wave_tex[wave_cur])
            vita2d_draw_texture(g_wave_tex[wave_cur], WAVE_X, WAVE_Y);
        if (length == 0 && state != ST_RECORDING)
            text(WAVE_X + 290, WAVE_Y + 80, C_DIM, 1.0f, "no recording yet - press X");

        if (state != ST_RECORDING && length > 0) {
            int px = WAVE_X + (int)((int64_t)(cursor - view_start) * WAVE_W / view_len);
            if (px >= WAVE_X && px <= WAVE_X + WAVE_W) {
                rect(WAVE_X, WAVE_Y, px - WAVE_X, WAVE_H, RGBA8(56, 200, 255, 36));
                rect(px - 1, WAVE_Y - 2, 3, WAVE_H + 4, C_TEXT);
            } else if (px < WAVE_X) {
                /* курсор левее окна - ничего не закрашиваем */
            } else {
                rect(WAVE_X, WAVE_Y, WAVE_W, WAVE_H, RGBA8(56, 200, 255, 36));
            }
            draw_ticks(view_start, view_len, length);
        }
        if (state == ST_RECORDING)
            text(WAVE_X, WAVE_Y + WAVE_H + 22, C_DIM, 0.8f, "live view: last 8 seconds");
        if (scrubbing) {
            snprintf(buf, sizeof(buf), "scrub speed %d%%", (int)(scrub_factor * 100.0f));
            text(WAVE_X + WAVE_W - 150, WAVE_Y + WAVE_H + 22, C_ACCENT, 0.8f, buf);
        }

        /* карточки параметров */
        snprintf(buf, sizeof(buf), "%d bit%s", 16 - g_bits_idx, g_bits_idx == 0 ? "  (original)" : "");
        draw_card(24, 344, 444, 84, "BIT DEPTH  (playback)", buf,
                  (float)(N_BITS - 1 - g_bits_idx) / (float)(N_BITS - 1), sel == 0);
        snprintf(buf, sizeof(buf), "%d Hz%s", g_rate_tab[g_rate_idx], g_rate_idx == 0 ? "  (original)" : "");
        draw_card(492, 344, 444, 84, "SAMPLE RATE  (playback)", buf,
                  (float)(N_RATE - 1 - g_rate_idx) / (float)(N_RATE - 1), sel == 1);

        /* сообщения */
        text(30, 456, confirm ? C_AMBER : C_TEXT, 1.0f, msg);
        if (g_error != 0) {
            snprintf(buf, sizeof(buf), "Audio error: 0x%08X", (unsigned int)g_error);
            text(30, 476, C_AMBER, 0.85f, buf);
        } else if (g_out_ch != 0) {
            snprintf(buf, sizeof(buf), "audio out: %d Hz, %s", OUT_RATE, g_out_ch == 1 ? "mono" : "stereo");
            text(30, 476, C_DIM, 0.8f, buf);
        }

        /* подсказки */
        draw_pad_button(44, 504, 0);  text(66, 510, C_DIM, 0.85f, "rec / stop");
        draw_pad_button(212, 504, 1); text(234, 510, C_DIM, 0.85f, "play / pause");
        draw_pad_button(402, 504, 2); text(424, 510, C_DIM, 0.85f, "stop + rewind");
        draw_pad_button(600, 504, 3); text(622, 510, C_DIM, 0.85f, "save WAV");
        text(30, 536, C_DIM, 0.8f,
             "L/R zoom   D-pad left/right move cursor   up/down change value   SELECT switch param   START exit");

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    g_running = 0;
    g_scrub = 0;
    g_state = ST_IDLE;
    sceKernelWaitThreadEnd(th_out, NULL, NULL);
    sceKernelWaitThreadEnd(th_in, NULL, NULL);

    for (int i = 0; i < N_WAVE_TEX; i++) if (g_wave_tex[i]) vita2d_free_texture(g_wave_tex[i]);
    vita2d_free_pgf(g_pgf);
    vita2d_fini();
    free(g_audio);
    sceKernelExitProcess(0);
    return 0;
}
