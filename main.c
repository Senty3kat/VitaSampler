/*
 * Vita Sampler - версия 0.4
 *
 * Управление:
 *   R                 - запись / стоп записи
 *   L                 - воспроизведение / пауза;  двойное нажатие = стоп
 *   Квадрат           - петля между курсорами вкл/выкл
 *   Круг              - меню эффектов
 *   SELECT            - сохранить WAV (ux0:data/Sampler/)
 *   Левый стик        - курсор A,  правый стик - курсор B
 *   D-pad лево/право  - двигать последний выбранный курсор
 *   D-pad вверх/вниз  - масштаб волны (приблизить / отдалить)
 *   Касание волны     - мгновенный переход и скраб (макс. скорость x3)
 *   START             - выход
 *
 * В меню эффектов: D-pad вверх/вниз - выбор ползунка, лево/право - значение,
 *   левый стик - значение, касание - ползунок, X - сброс, треугольник - сброс всех.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/audioin.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
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
#define LIVE_SAMPLES  (8 * SAMPLE_RATE)
#define MIN_VIEW      256

/* WSOLA (растяжение времени без смены тона), работает на частоте источника 16 кГц */
#define W_N           640                      /* длина кадра, 40 мс */
#define W_HS          (W_N / 4)                /* шаг синтеза, перекрытие 75% */
#define W_D           128                      /* радиус поиска лучшего совпадения */
#define W_L           160                      /* длина окна сравнения */
#define Y1_SIZE       4096                     /* кольцевой буфер (степень двойки) */
#define Y1_MASK       (Y1_SIZE - 1)
#define MAX_SCRUB_X   3.0                      /* максимум скраба: x3 от обычной скорости */

#define PANEL_X 24
#define PANEL_Y 140
#define PANEL_W 912
#define PANEL_H 190
#define WAVE_X  40
#define WAVE_Y  150
#define WAVE_W  880
#define WAVE_H  150
#define SEG_W   432                            /* ширина мини-дисплея в режиме сплита */
#define N_WAVE_TEX 4

#define MENU_X 24
#define MENU_Y 336
#define MENU_W 912
#define MENU_H 154
#define SL_X   250
#define SL_W   440

#define LOG_PATH "ux0:data/Sampler/log.txt"

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
#define C_CUR_A    RGBA8(255, 170, 60, 255)
#define C_CUR_B    RGBA8(240, 110, 200, 255)

enum { ST_IDLE = 0, ST_RECORDING = 1, ST_PLAYING = 2 };

/* ---------- Эффекты: все ползунки идут слева направо по возрастанию ---------- */
typedef struct { const char *name; int n; int def; } FxParam;
static const FxParam FX[4] = {
    { "BIT DEPTH",   16,  15 },   /* 1 ... 16 бит */
    { "SAMPLE RATE", 101, 100 },  /* 500 ... 16000 Гц (логарифмически) */
    { "SPEED",       276, 75 },   /* 0.25x ... 3.00x */
    { "PITCH",       241, 120 },  /* -12.0 ... +12.0 полутонов */
};

/* ---------- Общие данные ---------- */
static int16_t *g_audio = NULL;
static int16_t g_blk_min[MAX_BLOCKS];
static int16_t g_blk_max[MAX_BLOCKS];
static float g_hann[W_N];

static volatile int g_fx[4] = { 15, 100, 75, 120 };
static volatile int g_peak_all = 0;
static volatile int g_length = 0;
static volatile int g_play_pos = 0;
static volatile int g_seek_to = -1;
static volatile int g_state = ST_IDLE;
static volatile int g_running = 1;
static volatile int g_level = 0;
static volatile int g_error = 0;
static volatile int g_out_ch = 0;
static volatile int g_discard = 0;
static volatile int g_scrub = 0;
static volatile int g_loop = 0;
static volatile int g_lo = 0;
static volatile int g_hi = 0;
static volatile float g_scrub_target = 0.0f;
static volatile float g_scrub_vel = 0.0f;       /* сэмплов в секунду */

static vita2d_pgf *g_pgf = NULL;
static vita2d_texture *g_wave_tex[N_WAVE_TEX];

/* ---------- Лог (чтобы после сбоя было видно, что происходило) ---------- */
static void dbg_log(const char *fmt, ...) {
    char b[220];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(b) - 2) n = (int)sizeof(b) - 2;
    b[n++] = '\n';
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, b, n);
        sceIoClose(fd);
    }
}

/* ---------- Область воспроизведения между курсорами ---------- */
static void region_sanitize(int *lo, int *hi, int len) {
    if (*lo < 0) *lo = 0;
    if (*hi > len) *hi = len;
    if (*hi - *lo < 64) {
        *hi = len;
        if (*hi - *lo < 64) *lo = 0;
    }
}

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

/* ====================================================================
 *  Движок воспроизведения
 *   режим 0 - скраб (скорость следует за пальцем, не быстрее x3)
 *   режим 1 - обычный (скорость 1.0, тон 0): точное воспроизведение, петля без размытия
 *   режим 2 - WSOLA: скорость и высота тона независимо
 *             этап 1: растяжение времени с подбором кадров по совпадению формы волны
 *             этап 2: передискретизация результата в 48 кГц с коэффициентом высоты тона
 * ==================================================================== */
typedef struct {
    double pos;                 /* позиция (режимы 0 и 1) */
    /* WSOLA */
    double wpos;                /* номинальная позиция следующего кадра в источнике */
    double ppos;                /* оценка слышимой позиции (для курсора на экране) */
    int have_prev;
    int prev_start;
    int ending;
    int was_gran;
    long long frames;
    double r1;                  /* позиция чтения в кольцевом буфере y1 */
    float y1[Y1_SIZE];
    /* эффекты и скраб */
    float held, hold_acc;
    double scrub_rate;
    float scrub_gain;
} Engine;

typedef struct {
    int lo, hi, loop;
    int bits;
    float crush_hz;
    float speed, pratio;
    int neutral;
    int scrub;
    double scrub_target, scrub_vel;
} Params;

static void engine_reset(Engine *e, double pos) {
    memset(e, 0, sizeof(*e));
    e->pos = pos;
    e->wpos = pos;
    e->ppos = pos;
}

static inline float rd_clamped(const int16_t *a, int len, double pos) {
    if (pos < 0.0) pos = 0.0;
    int idx = (int)pos;
    if (idx >= len - 1) return (float)a[len - 1];
    float fr = (float)(pos - (double)idx);
    return (float)a[idx] + (float)(a[idx + 1] - a[idx]) * fr;
}

static inline int clamp16(int v) {
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

/* сэмпл источника с учётом области и петли (петля читается по кругу, без "утечки" за границу) */
static inline float fetch(const int16_t *a, int len, int lo, int hi, int loop, int idx) {
    if (loop && hi > lo) {
        if (idx >= hi || idx < lo) {
            int span = hi - lo;
            int r = (idx - lo) % span;
            if (r < 0) r += span;
            idx = lo + r;
        }
    } else if (idx >= hi) {
        return 0.0f;
    }
    if (idx < 0 || idx >= len) return 0.0f;
    return (float)a[idx];
}

/* Один кадр WSOLA: ищем смещение, при котором форма волны лучше всего продолжает предыдущий кадр */
static void wsola_frame(Engine *e, const int16_t *a, int len, const Params *p) {
    float seg[2 * W_D + W_N];
    float tgt[W_L];
    const int lo = p->lo, hi = p->hi, span = p->hi - p->lo;

    int base = (int)floor(e->wpos);
    for (int i = 0; i < 2 * W_D + W_N; i++)
        seg[i] = fetch(a, len, lo, hi, p->loop, base - W_D + i);

    int best_d = 0;
    if (e->have_prev) {
        int tstart = e->prev_start + W_HS;
        for (int i = 0; i < W_L; i++) tgt[i] = fetch(a, len, lo, hi, p->loop, tstart + i);
        float best = -1e30f;
        for (int d = -W_D; d <= W_D; d++) {
            const float *c = &seg[W_D + d];
            float dot = 0.0f, en = 0.0f;
            for (int i = 0; i < W_L; i++) { dot += c[i] * tgt[i]; en += c[i] * c[i]; }
            float score = dot / sqrtf(en + 1.0f);
            if (score > best) { best = score; best_d = d; }
        }
    }

    const float *c = &seg[W_D + best_d];
    long long m0 = e->frames * (long long)W_HS;
    for (int i = W_N - W_HS; i < W_N; i++) e->y1[(m0 + i) & Y1_MASK] = 0.0f;
    for (int i = 0; i < W_N; i++) e->y1[(m0 + i) & Y1_MASK] += 0.5f * g_hann[i] * c[i];

    e->prev_start = base + best_d;
    e->have_prev = 1;
    e->frames++;

    /* шаг анализа: Ha = Hs * speed / pitch_ratio */
    e->wpos += (double)W_HS * (double)p->speed / (double)p->pratio;
    if (e->wpos >= (double)hi) {
        if (p->loop && span > 0) {
            e->wpos = (double)lo + fmod(e->wpos - (double)hi, (double)span);
            e->have_prev = 0;                  /* разрыв: кадры плавно перекрываются окном */
        } else {
            e->ending = 1;
        }
    }
}

/* Заполняет mono[OUT_GRAIN]. Возвращает 1, если воспроизведение дошло до конца. */
static int engine_render(Engine *e, const int16_t *a, int len, const Params *p,
                         int16_t *mono, int *peak_out) {
    const double step = (double)SAMPLE_RATE / (double)OUT_RATE;
    const int lo = p->lo, hi = p->hi, span = p->hi - p->lo;
    const int do_hold = p->crush_hz < (float)SAMPLE_RATE - 0.5f;
    const float hold_inc = p->crush_hz / (float)OUT_RATE;
    int mode = p->scrub ? 0 : (p->neutral ? 1 : 2);
    int ended = 0, peak = 0;

    if (mode != 2 && e->was_gran) e->pos = e->ppos;
    if (mode == 2 && !e->was_gran) {
        e->wpos = e->pos;
        e->have_prev = 0;
        e->ending = 0;
        e->frames = 0;
        e->r1 = 0.0;
        memset(e->y1, 0, sizeof(e->y1));
    }
    e->was_gran = (mode == 2);

    double rate0 = e->scrub_rate, rate1 = e->scrub_rate;
    float g0 = e->scrub_gain, g1 = e->scrub_gain;
    if (mode == 0) {
        /* скорость = скорость пальца + мягкая подтяжка к позиции пальца */
        double r = p->scrub_vel / (double)OUT_RATE
                 + (p->scrub_target - e->pos) * (6.0 / (double)OUT_RATE);
        double maxr = MAX_SCRUB_X * step;
        if (r > maxr) r = maxr;
        if (r < -maxr) r = -maxr;
        rate1 = r;
        float ar = (float)(r < 0.0 ? -r : r);
        g1 = ar / 0.02f;                       /* почти стоим - тишина, без щелчков */
        if (g1 > 1.0f) g1 = 1.0f;
    }

    for (int i = 0; i < OUT_GRAIN; i++) {
        float s = 0.0f;

        if (ended) { mono[i] = 0; continue; }

        if (mode == 0) {
            double rate = rate0 + (rate1 - rate0) * (double)i / (double)OUT_GRAIN;
            s = rd_clamped(a, len, e->pos);
            e->pos += rate;
            if (e->pos < 0.0) e->pos = 0.0;
            if (e->pos > (double)(len - 2)) e->pos = (double)(len - 2);

        } else if (mode == 1) {
            if (e->pos >= (double)hi) {
                if (p->loop) {
                    e->pos = (double)lo + fmod(e->pos - (double)hi, (double)span);
                } else {
                    ended = 1;
                    mono[i] = 0;
                    continue;
                }
            }
            s = rd_clamped(a, len, e->pos);
            e->pos += step;

        } else {
            /* дослать кадры, пока в буфере не накопится достаточно готового звука */
            while (!e->ending && (double)(e->frames * W_HS) < e->r1 + 4.0)
                wsola_frame(e, a, len, p);
            double fin = (double)(e->frames * W_HS);
            if (e->r1 + 2.0 >= fin) {
                if (e->ending) { ended = 1; mono[i] = 0; continue; }
            }
            long long i0 = (long long)floor(e->r1);
            float fr = (float)(e->r1 - (double)i0);
            float y0 = e->y1[i0 & Y1_MASK], y1v = e->y1[(i0 + 1) & Y1_MASK];
            s = y0 + (y1v - y0) * fr;
            e->r1 += step * (double)p->pratio;
        }

        /* эффекты: понижение частоты дискретизации и разрядности */
        if (do_hold) {
            e->hold_acc += hold_inc;
            if (e->hold_acc >= 1.0f) { e->hold_acc -= 1.0f; e->held = s; }
            s = e->held;
        }
        int v = quantize(clamp16((int)s), p->bits);
        if (mode == 0) {
            float gcur = g0 + (g1 - g0) * (float)i / (float)OUT_GRAIN;
            v = (int)((float)v * gcur);
        }
        mono[i] = (int16_t)v;
        int av = v < 0 ? -v : v;
        if (av > peak) peak = av;
    }

    if (mode == 0) { e->scrub_rate = rate1; e->scrub_gain = g1; }
    if (mode == 2) {
        double fin = (double)(e->frames * W_HS);
        double pp = e->wpos - (fin - e->r1) * ((double)p->speed / (double)p->pratio);
        if (e->ending) pp = (double)hi;
        if (pp < 0.0) pp = 0.0;
        e->ppos = pp;
    }
    *peak_out = peak;
    return ended;
}

static inline double engine_pos(const Engine *e) {
    return e->was_gran ? e->ppos : e->pos;
}

/* ---------- Параметры эффектов -> физические величины ---------- */
static void load_params(Params *p, int lo, int hi) {
    int fb = g_fx[0], fr = g_fx[1], fs = g_fx[2], fp = g_fx[3];
    p->bits = 1 + fb;
    p->crush_hz = 500.0f * powf(32.0f, (float)fr / 100.0f);
    p->speed = 0.25f + 0.01f * (float)fs;
    p->pratio = powf(2.0f, ((float)(fp - 120) * 0.1f) / 12.0f);
    p->neutral = (fs == 75 && fp == 120);
    p->lo = lo;
    p->hi = hi;
    p->loop = g_loop;
    p->scrub = g_scrub;
    p->scrub_target = (double)g_scrub_target;
    p->scrub_vel = (double)g_scrub_vel;
}

/* ---------- Поток записи: микрофон открывается ОДИН раз ---------- */
static int in_thread(SceSize args, void *argp) {
    static int16_t in_buf[IN_GRAIN] __attribute__((aligned(64)));
    int in_port = -1, in_err = 0;

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
            in_err = 0;
        }

        int r = sceAudioInInput(in_port, in_buf);
        if (r < 0) {                           /* ошибка: не крутимся впустую */
            sceKernelDelayThread(5000);
            if (++in_err > 40) {
                g_error = r;
                sceAudioInReleasePort(in_port);
                in_port = -1;
                __sync_bool_compare_and_swap(&g_state, ST_RECORDING, ST_IDLE);
            }
            continue;
        }
        in_err = 0;

        if (g_state != ST_RECORDING) continue;
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
            int av = v < 0 ? -v : v;
            if (av > peak) peak = av;
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
    static Engine eng;

    int out_port = -1, out_ch = 1, out_err = 0;
    int was_playing = 0;

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
            out_err = 0;
        }

        int len = g_length;
        if (len < 2) {
            __sync_bool_compare_and_swap(&g_state, ST_PLAYING, ST_IDLE);
            continue;
        }

        int lo = g_lo, hi = g_hi;
        region_sanitize(&lo, &hi, len);

        if (!was_playing) {
            double p0 = (double)g_play_pos;
            if (p0 < (double)lo || p0 >= (double)hi) p0 = (double)lo;
            engine_reset(&eng, p0);
            was_playing = 1;
        }
        int seek = g_seek_to;
        if (seek >= 0) {
            if (seek > len - 1) seek = len - 1;
            double keep_rate = eng.scrub_rate;
            float keep_gain = eng.scrub_gain;
            engine_reset(&eng, (double)seek);
            eng.scrub_rate = keep_rate;
            eng.scrub_gain = keep_gain;
            g_seek_to = -1;
        }

        Params prm;
        load_params(&prm, lo, hi);
        int peak = 0;
        int ended = engine_render(&eng, g_audio, len, &prm, mono, &peak);

        if (out_ch == 1) {
            memcpy(out_buf, mono, sizeof(mono));
        } else {
            for (int i = 0; i < OUT_GRAIN; i++) {
                out_buf[2 * i] = mono[i];
                out_buf[2 * i + 1] = mono[i];
            }
        }

        int r = sceAudioOutOutput(out_port, out_buf);
        if (r < 0) {
            sceKernelDelayThread(5000);
            if (++out_err > 40) {
                g_error = r;
                sceAudioOutReleasePort(out_port);
                out_port = -1;
                __sync_bool_compare_and_swap(&g_state, ST_PLAYING, ST_IDLE);
            }
        } else {
            out_err = 0;
        }
        g_level = peak;

        if (ended) {
            g_play_pos = lo;
            was_playing = 0;
            __sync_bool_compare_and_swap(&g_state, ST_PLAYING, ST_IDLE);
        } else if (g_state == ST_PLAYING) {
            g_play_pos = (int)engine_pos(&eng);
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

/* ---------- Рисование: помощники (только прямоугольники и текст) ---------- */
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

/* Значки кнопок: 1 квадрат, 2 круг */
static void draw_pad_icon(int cx, int cy, int kind) {
    rect(cx - 12, cy - 10, 25, 21, C_PANEL2);
    rect(cx - 10, cy - 12, 21, 25, C_PANEL2);
    if (kind == 1) {
        rect_outline(cx - 5, cy - 5, 11, 11, 2, RGBA8(245, 150, 215, 255));
    } else {
        unsigned int c = RGBA8(255, 105, 110, 255);
        rect(cx - 3, cy - 6, 7, 2, c);
        rect(cx - 3, cy + 5, 7, 2, c);
        rect(cx - 6, cy - 3, 2, 7, c);
        rect(cx + 5, cy - 3, 2, 7, c);
        rect(cx - 5, cy - 5, 2, 2, c);
        rect(cx + 4, cy - 5, 2, 2, c);
        rect(cx - 5, cy + 4, 2, 2, c);
        rect(cx + 4, cy + 4, 2, 2, c);
    }
}

static void draw_chip(int x, int cy, int w, const char *label) {
    rect(x, cy - 11, w, 23, C_PANEL2);
    text(x + 7, cy + 6, C_TEXT, 0.85f, label);
}

static void fmt_time(char *out, size_t cap, int samples) {
    int s = samples / SAMPLE_RATE;
    int tenth = (samples % SAMPLE_RATE) * 10 / SAMPLE_RATE;
    snprintf(out, cap, "%02d:%02d.%d", s / 60, s % 60, tenth);
}

static int level_percent(int peak) {
    if (peak <= 0) return 0;
    float db = 20.0f * log10f((float)peak / 32768.0f);
    float p = (db + 60.0f) / 60.0f;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return (int)(p * 100.0f);
}

/* ---------- Вид волны: один экран или сплит из двух мини-дисплеев ---------- */
typedef struct {
    int nseg;
    int x0[2];      /* относительно WAVE_X */
    int w[2];
    int vs[2];
    int vl[2];
} Layout;

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

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void compute_layout(Layout *L, int length, int live, int split, double c1, double c2) {
    memset(L, 0, sizeof(*L));
    if (live) {
        L->nseg = 1; L->x0[0] = 0; L->w[0] = WAVE_W;
        L->vs[0] = length - LIVE_SAMPLES; L->vl[0] = LIVE_SAMPLES;
    } else if (!split) {
        L->nseg = 1; L->x0[0] = 0; L->w[0] = WAVE_W;
        L->vs[0] = view_start; L->vl[0] = view_len;
    } else {
        int segvl = (int)((int64_t)view_len * SEG_W / WAVE_W);
        if (segvl < 64) segvl = 64;
        int maxvs = length - segvl; if (maxvs < 0) maxvs = 0;
        double lo = c1 < c2 ? c1 : c2, hi = c1 < c2 ? c2 : c1;
        L->nseg = 2;
        L->x0[0] = 0;               L->w[0] = SEG_W;
        L->x0[1] = WAVE_W - SEG_W;  L->w[1] = SEG_W;
        L->vl[0] = L->vl[1] = segvl;
        L->vs[0] = clampi((int)lo - segvl / 2, 0, maxvs);
        L->vs[1] = clampi((int)hi - segvl / 2, 0, maxvs);
    }
}

/* позиция сэмпла -> экранный x в мини-дисплее seg (или -1000000, если вне окна) */
static int sample_to_x(const Layout *L, int seg, double s) {
    double fx = (s - (double)L->vs[seg]) * (double)L->w[seg] / (double)L->vl[seg];
    if (fx < 0.0 || fx > (double)L->w[seg]) return -1000000;
    return WAVE_X + L->x0[seg] + (int)fx;
}

/* экранный x -> позиция сэмпла (по ближайшему мини-дисплею) */
static double x_to_sample(const Layout *L, int tx, int *seg_out) {
    int seg = 0;
    if (L->nseg == 2 && tx >= WAVE_X + L->x0[1] - 8) seg = 1;
    double fx = (double)(tx - WAVE_X - L->x0[seg]);
    if (fx < 0.0) fx = 0.0;
    if (fx > (double)L->w[seg]) fx = (double)L->w[seg];
    if (seg_out) *seg_out = seg;
    return (double)L->vs[seg] + fx * (double)L->vl[seg] / (double)L->w[seg];
}

/* ---------- Волна: рисуем процессором в текстуру (1 draw-вызов) ---------- */
static void render_wave(vita2d_texture *tex, const Layout *L, int length, int live) {
    uint32_t *px = (uint32_t *)vita2d_texture_get_datap(tex);
    int stride = (int)vita2d_texture_get_stride(tex) / 4;
    memset(px, 0, (size_t)stride * WAVE_H * 4);

    int cy = WAVE_H / 2;
    int half = WAVE_H / 2 - 4;
    for (int sg = 0; sg < L->nseg; sg++)
        for (int x = 0; x < L->w[sg]; x++) px[cy * stride + L->x0[sg] + x] = C_GRID;
    if (length <= 0) return;

    int peak = g_peak_all;
    if (peak < 3000) peak = 3000;
    float gain = 32768.0f / (float)peak;
    if (gain > 8.0f) gain = 8.0f;

    uint32_t col = live ? C_REC : C_WAVE;
    int nb = length / IN_GRAIN;

    for (int sg = 0; sg < L->nseg; sg++) {
        int vs = L->vs[sg], vl = L->vl[sg], w = L->w[sg], x0 = L->x0[sg];
        if (vl <= 0) continue;
        int use_blocks = (vl / w) >= 512;

        for (int x = 0; x < w; x++) {
            int64_t s0 = (int64_t)vs + (int64_t)x * vl / w;
            int64_t s1 = (int64_t)vs + (int64_t)(x + 1) * vl / w;
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
                int64_t e = s1;
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
            for (int y = ytop; y <= ybot; y++) px[y * stride + x0 + x] = col;
        }
    }
}

static void draw_ticks(const Layout *L, int length) {
    if (length <= 0) return;
    static const int steps[] = { 16, 32, 80, 160, 400, 800, 1600, 3200, 8000, 16000,
                                 32000, 80000, 160000, 320000, 800000, 1600000, 3200000 };
    const int n = (int)(sizeof(steps) / sizeof(steps[0]));
    for (int sg = 0; sg < L->nseg; sg++) {
        int vs = L->vs[sg], vl = L->vl[sg], w = L->w[sg];
        if (vl <= 0) continue;
        int step = steps[n - 1];
        for (int i = 0; i < n; i++) {
            if ((int64_t)vl * 880 / w / steps[i] <= 24) { step = steps[i]; break; }
        }
        int y = WAVE_Y + WAVE_H + 3;
        int64_t first = ((int64_t)vs + step - 1) / step * step;
        int count = 0;
        for (int64_t s = first; s <= (int64_t)vs + vl && s <= length && count < 30; s += step, count++) {
            int x = WAVE_X + L->x0[sg] + (int)((s - vs) * w / vl);
            rect(x, y, 1, ((s / step) % 5 == 0) ? 8 : 4, C_DIM);
        }
    }
}

/* Курсоры, область между ними, головка воспроизведения */
static void draw_overlays(const Layout *L, double c1, double c2, int active,
                          int playhead, int show_play, int finger_on, double finger) {
    double lo = c1 < c2 ? c1 : c2, hi = c1 < c2 ? c2 : c1;
    for (int sg = 0; sg < L->nseg; sg++) {
        int xa = sample_to_x(L, sg, lo), xb = sample_to_x(L, sg, hi);
        int segx = WAVE_X + L->x0[sg], segw = L->w[sg];
        /* область между курсорами */
        double vs = (double)L->vs[sg], ve = vs + (double)L->vl[sg];
        if (hi > vs && lo < ve) {
            int sx = xa == -1000000 ? segx : xa;
            int ex = xb == -1000000 ? segx + segw : xb;
            unsigned int shade = g_loop ? RGBA8(76, 224, 146, 42) : RGBA8(56, 200, 255, 30);
            rect(sx, WAVE_Y, ex - sx, WAVE_H, shade);
        }
        /* курсоры */
        for (int k = 0; k < 2; k++) {
            double cs = (k == 0) ? c1 : c2;
            int x = sample_to_x(L, sg, cs);
            if (x == -1000000) continue;
            unsigned int c = (k == 0) ? C_CUR_A : C_CUR_B;
            int th = (active == k) ? 3 : 2;
            rect(x - th / 2, WAVE_Y - 4, th, WAVE_H + 8, c);
            rect(x - 1, WAVE_Y - 4, 11, 13, c);
            text(x + 1, WAVE_Y + 7, C_DARK, 0.7f, k == 0 ? "A" : "B");
        }
        if (finger_on) {
            int x = sample_to_x(L, sg, finger);
            if (x != -1000000) rect(x, WAVE_Y, 1, WAVE_H, C_DIM);
        }
        if (show_play) {
            int x = sample_to_x(L, sg, (double)playhead);
            if (x != -1000000) rect(x - 1, WAVE_Y - 2, 3, WAVE_H + 4, C_TEXT);
        }
    }
}

/* ---------- Меню эффектов ---------- */
static void fx_text(char *out, size_t cap, int param, int idx) {
    switch (param) {
    case 0: snprintf(out, cap, "%d bit", 1 + idx); break;
    case 1: snprintf(out, cap, "%d Hz", (int)(500.0f * powf(32.0f, (float)idx / 100.0f) + 0.5f)); break;
    case 2: snprintf(out, cap, "%d.%02dx", (25 + idx) / 100, (25 + idx) % 100); break;
    default: {
        int t = idx - 120;
        int at = t < 0 ? -t : t;
        snprintf(out, cap, "%c%d.%d st", t < 0 ? '-' : '+', at / 10, at % 10);
        break; }
    }
}

static void draw_menu(int sel) {
    char buf[48];
    rect(MENU_X, MENU_Y, MENU_W, MENU_H, C_PANEL);
    rect_outline(MENU_X, MENU_Y, MENU_W, MENU_H, 2, C_PANEL2);
    for (int i = 0; i < 4; i++) {
        int y0 = MENU_Y + 4 + i * 37;
        int idx = g_fx[i];
        int sel_row = (i == sel);
        if (sel_row) rect_outline(MENU_X + 8, y0, MENU_W - 16, 35, 2, C_ACCENT);
        text(MENU_X + 24, y0 + 25, sel_row ? C_ACCENT : C_DIM, 1.0f, FX[i].name);

        float f = (float)idx / (float)(FX[i].n - 1);
        float fd = (float)FX[i].def / (float)(FX[i].n - 1);
        int kx = SL_X + (int)(f * SL_W);
        rect(SL_X, y0 + 13, SL_W, 10, C_PANEL2);
        rect(SL_X, y0 + 13, kx - SL_X, 10, sel_row ? C_ACCENT : C_DIM);
        rect(SL_X + (int)(fd * SL_W) - 1, y0 + 7, 2, 22, C_DIM);   /* метка "по умолчанию" */
        rect(kx - 5, y0 + 6, 10, 24, C_TEXT);

        fx_text(buf, sizeof(buf), i, idx);
        text(SL_X + SL_W + 36, y0 + 26, idx == FX[i].def ? C_TEXT : C_AMBER, 1.2f, buf);
    }
}

/* ---------- Ввод: стики и ползунки ---------- */
static float stick_axis(unsigned char v) {
    float a = ((float)v - 128.0f) / 128.0f;
    float m = fabsf(a);
    const float dz = 0.14f;
    if (m < dz) return 0.0f;
    m = (m - dz) / (1.0f - dz);
    if (m > 1.0f) m = 1.0f;
    m = m * m;
    return a < 0.0f ? -m : m;
}

static void fx_set(int param, int idx) {
    if (idx < 0) idx = 0;
    if (idx > FX[param].n - 1) idx = FX[param].n - 1;
    g_fx[param] = idx;
}

/* ---------- Масштаб и положение окна ---------- */
static void zoom_view(double factor, double anchor, int length) {
    double a = (anchor >= view_start && anchor <= view_start + view_len)
               ? (anchor - view_start) / (double)view_len : 0.5;
    double nl = (double)view_len * factor;
    if (nl > 100000000.0) nl = 100000000.0;
    view_len = (int)nl;
    if (view_len < 1) view_len = 1;
    clamp_view(length);
    view_start = (int)(anchor - a * (double)view_len);
    clamp_view(length);
}

/* ---------- Главная программа ---------- */
#ifndef SAMPLER_NO_MAIN
int main(void) {
    sceIoMkdir("ux0:data/Sampler", 0777);
    {   /* новый лог при каждом запуске */
        SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) sceIoClose(fd);
    }
    dbg_log("start v0.4");

    g_audio = (int16_t *)malloc(MAX_SAMPLES * sizeof(int16_t));
    if (!g_audio) { dbg_log("malloc failed"); sceKernelExitProcess(0); }

    for (int i = 0; i < W_N; i++)
        g_hann[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * (float)i / (float)W_N));

    vita2d_init_advanced(4 * 1024 * 1024);
    vita2d_set_clear_color(C_BG);
    g_pgf = vita2d_load_default_pgf();
    for (int i = 0; i < N_WAVE_TEX; i++) g_wave_tex[i] = vita2d_create_empty_texture(WAVE_W, WAVE_H);
    dbg_log("gfx ready");

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    SceUID th_in = sceKernelCreateThread("audio_in", in_thread, 0x10000100, 0x10000, 0, 0, NULL);
    SceUID th_out = sceKernelCreateThread("audio_out", out_thread, 0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(th_in, 0, NULL);
    sceKernelStartThread(th_out, 0, NULL);

    SceCtrlData pad;
    SceTouchData touch;
    unsigned int old_buttons = 0;
    char msg[160] = "Press R to start recording";
    char buf[200], t1[32], t2[32];
    int shown_level = 0, clip_hold = 0;
    int dirty = 0, confirm = 0;
    unsigned int frame = 0, last_l_frame = 0;
    int last_err = 0;

    /* курсоры */
    double c1 = 0.0, c2 = 0.0;
    int active = 0, split = 0;
    int nudge_frames = 0, zoom_frames = 0;

    /* меню эффектов */
    int menu_open = 0, menu_sel = 0, menu_touch_row = -1, fx_hold = 0;
    float fx_acc = 0.0f;

    /* скраб */
    int scrubbing = 0, resume_state = ST_IDLE, scrub_seg = 0;
    double scrub_pos = 0.0, prev_target = 0.0;
    float vel_f = 0.0f;
    uint64_t prev_touch_t = 0;

    int wave_cur = -1, wave_next = 0;
    int key[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    Layout lay_last;
    memset(&lay_last, 0xff, sizeof(lay_last));

    uint64_t last_t = sceKernelGetProcessTimeWide();

    while (1) {
        frame++;
        uint64_t now_t = sceKernelGetProcessTimeWide();
        float dt = (float)(now_t - last_t) * 1e-6f;
        last_t = now_t;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.1f) dt = 0.1f;

        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old_buttons;
        old_buttons = pad.buttons;
        if (pressed & SCE_CTRL_START) break;

        int state = g_state;
        int length = g_length;

        /* ---- R: запись / стоп записи ---- */
        if (pressed & SCE_CTRL_RTRIGGER) {
            if (state == ST_RECORDING) {
                g_state = ST_IDLE;
                sceKernelDelayThread(40000);
                length = g_length;
                dirty = (length > 0);
                view_start = 0;
                view_len = length > 0 ? length : MIN_VIEW;
                clamp_view(length);
                c1 = 0.0; c2 = (double)length; active = 0; split = 0;
                g_lo = 0; g_hi = length;
                snprintf(msg, sizeof(msg), "Recording stopped. L: play   SELECT: save");
                dbg_log("rec stop len=%d", length);
            } else if (dirty && !confirm) {
                confirm = 1;
                snprintf(msg, sizeof(msg), "Unsaved recording! Press R again to erase it, or SELECT to save");
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
                c1 = c2 = 0.0; split = 0;
                g_lo = 0; g_hi = 0;
                __sync_synchronize();
                g_state = ST_RECORDING;
                snprintf(msg, sizeof(msg), "Recording... press R to stop");
                dbg_log("rec start");
            }
            state = g_state;
            length = g_length;
        }

        /* ---- L: play/pause, двойное нажатие = стоп ---- */
        if ((pressed & SCE_CTRL_LTRIGGER) && state != ST_RECORDING && length > 1) {
            int lo = (int)(c1 < c2 ? c1 : c2), hi = (int)(c1 < c2 ? c2 : c1);
            region_sanitize(&lo, &hi, length);
            if (last_l_frame != 0 && frame - last_l_frame <= 20) {
                g_state = ST_IDLE;
                sceKernelDelayThread(40000);
                g_play_pos = lo;
                g_seek_to = -1;
                last_l_frame = 0;
                snprintf(msg, sizeof(msg), "Stopped");
                dbg_log("stop");
            } else {
                last_l_frame = frame;
                if (state == ST_PLAYING) {
                    g_state = ST_IDLE;
                    snprintf(msg, sizeof(msg), "Paused");
                    dbg_log("pause");
                } else {
                    g_error = 0;
                    g_state = ST_PLAYING;
                    snprintf(msg, sizeof(msg), g_loop ? "Playing (loop)" : "Playing");
                    dbg_log("play lo=%d hi=%d", lo, hi);
                }
            }
        }

        /* ---- Квадрат: петля ---- */
        if (pressed & SCE_CTRL_SQUARE) {
            g_loop = !g_loop;
            snprintf(msg, sizeof(msg), g_loop ? "Loop ON" : "Loop off");
            dbg_log("loop %d", g_loop);
        }

        /* ---- SELECT: сохранить WAV ---- */
        if ((pressed & SCE_CTRL_SELECT) && state != ST_RECORDING) {
            char path[128];
            int r = save_wav(path, sizeof(path));
            if (r == 0) { snprintf(msg, sizeof(msg), "Saved: %s", path); dirty = 0; dbg_log("saved %s", path); }
            else if (r == -1) snprintf(msg, sizeof(msg), "Nothing to save - record something first");
            else snprintf(msg, sizeof(msg), "Could not write the file");
        }

        /* ---- Круг: меню эффектов ---- */
        if (pressed & SCE_CTRL_CIRCLE) {
            menu_open = !menu_open;
            menu_touch_row = -1;
            fx_acc = 0.0f;
        }

        /* ---- меню: управление ползунками ---- */
        if (menu_open) {
            if (pressed & SCE_CTRL_UP)   menu_sel = (menu_sel + 3) % 4;
            if (pressed & SCE_CTRL_DOWN) menu_sel = (menu_sel + 1) % 4;
            if (pressed & SCE_CTRL_CROSS) fx_set(menu_sel, FX[menu_sel].def);
            if (pressed & SCE_CTRL_TRIANGLE) for (int i = 0; i < 4; i++) fx_set(i, FX[i].def);

            int dir = ((pad.buttons & SCE_CTRL_RIGHT) ? 1 : 0) - ((pad.buttons & SCE_CTRL_LEFT) ? 1 : 0);
            if (dir) {
                fx_hold++;
                int d = 0;
                if (fx_hold == 1) d = 1;
                else if (fx_hold > 14) d = 1 + (fx_hold - 14) / 6;
                if (d) fx_set(menu_sel, g_fx[menu_sel] + dir * d);
            } else fx_hold = 0;

            float a = stick_axis(pad.lx);
            if (a != 0.0f) {
                fx_acc += a * (float)(FX[menu_sel].n - 1) * 0.45f * dt;
                int whole = (int)fx_acc;
                if (whole) { fx_set(menu_sel, g_fx[menu_sel] + whole); fx_acc -= (float)whole; }
            } else fx_acc = 0.0f;
        }

        /* ---- курсоры и масштаб ---- */
        if (state != ST_RECORDING && length > 1) {
            double spp = (double)view_len / (double)WAVE_W;
            if (spp < 0.05) spp = 0.05;

            float a1 = menu_open ? 0.0f : stick_axis(pad.lx);
            float a2 = stick_axis(pad.rx);
            if (a1 != 0.0f) { c1 += (double)a1 * 420.0 * spp * (double)dt; active = 0; }
            if (a2 != 0.0f) { c2 += (double)a2 * 420.0 * spp * (double)dt; active = 1; }

            if (!menu_open) {
                int dir = ((pad.buttons & SCE_CTRL_RIGHT) ? 1 : 0) - ((pad.buttons & SCE_CTRL_LEFT) ? 1 : 0);
                if (dir) {
                    nudge_frames++;
                    double move = 0.0;
                    if (nudge_frames == 1) move = 1.0;
                    else if (nudge_frames > 15) {
                        double px = spp < 1.0 ? 1.0 : spp;
                        move = px * (double)(1 + (nudge_frames - 15) / 15);
                    }
                    if (move != 0.0) { if (active == 0) c1 += dir * move; else c2 += dir * move; }
                } else nudge_frames = 0;

                int zd = ((pad.buttons & SCE_CTRL_UP) ? 1 : 0) - ((pad.buttons & SCE_CTRL_DOWN) ? 1 : 0);
                if (zd) {
                    zoom_frames++;
                    double anchor = active == 0 ? c1 : c2;
                    if (zoom_frames == 1) zoom_view(zd > 0 ? 0.5 : 2.0, anchor, length);
                    else if (zoom_frames > 18) zoom_view(zd > 0 ? 0.97 : 1.03, anchor, length);
                } else zoom_frames = 0;
            }

            if (c1 < 0.0) c1 = 0.0;
            if (c1 > (double)length) c1 = (double)length;
            if (c2 < 0.0) c2 = 0.0;
            if (c2 > (double)length) c2 = (double)length;
            g_lo = (int)(c1 < c2 ? c1 : c2);
            g_hi = (int)(c1 < c2 ? c2 : c1);
        }

        /* ---- сплит / положение окна ---- */
        if (state != ST_RECORDING && length > 1) {
            clamp_view(length);
            double lo = c1 < c2 ? c1 : c2, hi = c1 < c2 ? c2 : c1;
            double span = hi - lo;
            if (!split) { if (span > (double)view_len * 0.88) split = 1; }
            else if (span <= (double)view_len * 0.75) {
                split = 0;
                view_start = (int)((lo + hi) * 0.5 - (double)view_len * 0.5);
                clamp_view(length);
            }
            if (!split && view_len < length) {
                double margin = (double)view_len / 12.0;
                double vmin = hi + margin - (double)view_len;
                double vmax = lo - margin;
                if (vmin <= vmax) {
                    double v = (double)view_start;
                    if (v < vmin) v = vmin;
                    if (v > vmax) v = vmax;
                    view_start = (int)v;
                }
                clamp_view(length);
            }
            if (view_len >= length) view_start = 0;
        }

        /* ---- касание: ползунки меню или скраб волны ---- */
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        int have_touch = touch.reportNum > 0;
        int raw_x = have_touch ? touch.report[0].x : 0;
        int tx = raw_x / 2;
        int ty = have_touch ? touch.report[0].y / 2 : 0;

        if (menu_open) {
            if (!have_touch) menu_touch_row = -1;
            else {
                if (menu_touch_row < 0 && ty >= MENU_Y && ty < MENU_Y + MENU_H && tx >= MENU_X) {
                    int row = (ty - MENU_Y - 4) / 37;
                    if (row < 0) row = 0;
                    if (row > 3) row = 3;
                    menu_touch_row = row;
                    menu_sel = row;
                }
                if (menu_touch_row >= 0) {
                    int row = menu_touch_row;
                    float f = (float)(tx - SL_X) / (float)SL_W;
                    if (f < 0.0f) f = 0.0f;
                    if (f > 1.0f) f = 1.0f;
                    if (tx >= SL_X - 20) fx_set(row, (int)(f * (float)(FX[row].n - 1) + 0.5f));
                }
            }
        }

        Layout lay_now;
        compute_layout(&lay_now, length, state == ST_RECORDING, split, c1, c2);

        int touch_in = 0;
        if (have_touch && !menu_open && g_state != ST_RECORDING && length > 1 &&
            tx >= WAVE_X && tx <= WAVE_X + WAVE_W && ty >= PANEL_Y && ty <= PANEL_Y + PANEL_H)
            touch_in = 1;

        if (touch_in) {
            int seg = 0;
            double target = x_to_sample(&lay_now, tx, &seg);
            if (target < 0.0) target = 0.0;
            if (target > (double)(length - 1)) target = (double)(length - 1);

            if (!scrubbing) {
                scrubbing = 1;
                resume_state = g_state;
                scrub_seg = seg;
                scrub_pos = target;
                prev_target = target;
                prev_touch_t = now_t;
                vel_f = 0.0f;
                g_scrub_target = (float)target;
                g_scrub_vel = 0.0f;
                g_seek_to = (int)target;
                g_play_pos = (int)target;
                g_error = 0;
                g_scrub = 1;
                g_state = ST_PLAYING;
                dbg_log("scrub start %d", (int)target);
            } else {
                float tdt = (float)(now_t - prev_touch_t) * 1e-6f;
                if (tdt < 0.004f) tdt = 0.004f;
                if (seg != scrub_seg) {                  /* перешли на другой мини-дисплей: просто прыжок */
                    vel_f = 0.0f;
                    scrub_seg = seg;
                    g_seek_to = (int)target;
                } else {
                    float v = (float)((target - prev_target) / (double)tdt);
                    vel_f = 0.5f * vel_f + 0.5f * v;
                }
                prev_target = target;
                prev_touch_t = now_t;
                scrub_pos = target;
                g_scrub_target = (float)target;
                g_scrub_vel = vel_f;
            }
        } else if (scrubbing) {
            scrubbing = 0;
            g_scrub = 0;
            g_scrub_vel = 0.0f;
            int endpos = (int)scrub_pos;
            if (resume_state == ST_PLAYING) {
                g_seek_to = endpos;
            } else {
                g_state = ST_IDLE;
                sceKernelDelayThread(40000);
                g_play_pos = endpos;
                g_seek_to = -1;
            }
            dbg_log("scrub end %d", endpos);
        }

        if (pressed & ~SCE_CTRL_RTRIGGER) confirm = 0;

        /* ---- подготовка к рисованию ---- */
        state = g_state;
        length = g_length;
        int live = (state == ST_RECORDING);
        int playhead = g_play_pos;
        compute_layout(&lay_now, length, live, split, c1, c2);

        /* текстура волны обновляется только при изменениях */
        {
            int peak = g_peak_all;
            int newkey[8] = { length, live, peak, lay_now.nseg, 0, 0, 0, 0 };
            int changed = live || memcmp(key, newkey, sizeof(key)) != 0 ||
                          memcmp(&lay_last, &lay_now, sizeof(Layout)) != 0;
            if (changed) {
                wave_cur = wave_next;
                wave_next = (wave_next + 1) % N_WAVE_TEX;
                if (g_wave_tex[wave_cur]) {
                    vita2d_wait_rendering_done();       /* GPU не должен читать то, что пишет CPU */
                    render_wave(g_wave_tex[wave_cur], &lay_now, length, live);
                }
                memcpy(key, newkey, sizeof(key));
                lay_last = lay_now;
            }
        }

        /* ---- лог: пульс раз в ~1.5 с и ошибки ---- */
        if (frame % 90 == 0)
            dbg_log("hb f=%u st=%d len=%d pos=%d lvl=%d split=%d menu=%d scrub=%d",
                    frame, state, length, playhead, g_level, split, menu_open, scrubbing);
        if (g_error != last_err) { last_err = g_error; dbg_log("audio error 0x%08X", (unsigned)g_error); }

        /* ---------- Рисуем ---------- */
        vita2d_start_drawing();
        vita2d_clear_screen();

        /* шапка */
        rect(24, 16, 6, 34, C_ACCENT);
        text(42, 46, C_TEXT, 1.5f, "VITA SAMPLER");
        text(260, 46, C_DIM, 0.9f, "v0.4");
        text(560, 42, g_loop ? C_PLAY : C_DIM, 1.0f, g_loop ? "LOOP ON" : "loop off");

        const char *label = "READY";
        unsigned int pill = C_PANEL2, pill_text = C_DIM;
        if (state == ST_RECORDING) { label = "REC";  pill = C_REC;  pill_text = C_DARK; }
        if (state == ST_PLAYING)   { label = scrubbing ? "SCRUB" : "PLAY"; pill = C_PLAY; pill_text = C_DARK; }
        rect(800, 14, 136, 38, pill);
        text(scrubbing ? 828 : 842, 41, pill_text, 1.2f, label);

        /* время и номер сэмпла */
        int shown_pos = live ? length : playhead;
        fmt_time(t1, sizeof(t1), shown_pos);
        text(30, 120, C_TEXT, 2.0f, t1);
        snprintf(buf, sizeof(buf), "sample %d / %d", shown_pos, length);
        text(240, 98, C_TEXT, 1.0f, buf);
        fmt_time(t2, sizeof(t2), length);
        {
            int tenths = (int)((int64_t)view_len * 10 / WAVE_W);
            int zoomx = view_len > 0 ? length / view_len : 1;
            if (zoomx < 1) zoomx = 1;
            if (live) snprintf(buf, sizeof(buf), "max 05:00");
            else snprintf(buf, sizeof(buf), "total %s   zoom x%d   1 px = %d.%d smp%s",
                          t2, zoomx, tenths / 10, tenths % 10, split ? "   SPLIT" : "");
            text(240, 122, C_DIM, 0.8f, buf);
        }

        /* индикатор уровня */
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
        if (length == 0 && !live)
            text(WAVE_X + 290, WAVE_Y + 80, C_DIM, 1.0f, "no recording yet - press R");

        if (!live && length > 1) {
            draw_overlays(&lay_now, c1, c2, active, playhead, 1, scrubbing, scrub_pos);
            draw_ticks(&lay_now, length);
            snprintf(buf, sizeof(buf), "A %d    B %d    region %d smp    active: %c",
                     (int)c1, (int)c2, (int)(c1 < c2 ? c2 - c1 : c1 - c2), active == 0 ? 'A' : 'B');
            text(WAVE_X, PANEL_Y + PANEL_H - 6, C_DIM, 0.8f, buf);
        }
        if (live) text(WAVE_X, PANEL_Y + PANEL_H - 6, C_DIM, 0.8f, "live view: last 8 seconds");
        if (scrubbing) {
            int sp = (int)(fabsf(vel_f) / (float)SAMPLE_RATE * 100.0f);
            if (sp > (int)(MAX_SCRUB_X * 100.0)) sp = (int)(MAX_SCRUB_X * 100.0);
            snprintf(buf, sizeof(buf), "scrub speed %d%%", sp);
            text(WAVE_X + WAVE_W - 150, PANEL_Y + PANEL_H - 6, C_ACCENT, 0.8f, buf);
        }

        /* нижняя часть: меню эффектов или сообщения */
        if (menu_open) {
            draw_menu(menu_sel);
        } else {
            text(30, 376, confirm ? C_AMBER : C_TEXT, 1.1f, msg);
            char v0[32], v1[32], v2[32], v3[32];
            fx_text(v0, sizeof(v0), 0, g_fx[0]); fx_text(v1, sizeof(v1), 1, g_fx[1]);
            fx_text(v2, sizeof(v2), 2, g_fx[2]); fx_text(v3, sizeof(v3), 3, g_fx[3]);
            int modified = 0;
            for (int i = 0; i < 4; i++) if (g_fx[i] != FX[i].def) modified = 1;
            snprintf(buf, sizeof(buf), "FX:  %s  |  %s  |  speed %s  |  pitch %s    (O: edit)", v0, v1, v2, v3);
            text(30, 412, modified ? C_AMBER : C_DIM, 0.9f, buf);
            if (g_error != 0) {
                snprintf(buf, sizeof(buf), "Audio error: 0x%08X", (unsigned int)g_error);
                text(30, 448, C_AMBER, 0.9f, buf);
            } else if (g_out_ch != 0) {
                snprintf(buf, sizeof(buf), "audio out: %d Hz, %s", OUT_RATE, g_out_ch == 1 ? "mono" : "stereo");
                text(30, 448, C_DIM, 0.8f, buf);
            }
        }

        /* подсказки (легенда) */
        draw_chip(30, 504, 26, "R");   text(62, 510, C_DIM, 0.85f, "rec/stop");
        draw_chip(150, 504, 26, "L");  text(182, 510, C_DIM, 0.85f, "play/pause (2x = stop)");
        draw_pad_icon(372, 504, 1);    text(392, 510, C_DIM, 0.85f, "loop");
        draw_pad_icon(470, 504, 2);    text(490, 510, C_DIM, 0.85f, "effects");
        draw_chip(580, 504, 44, "SEL"); text(632, 510, C_DIM, 0.85f, "save WAV");
        text(800, 510, C_DIM, 0.85f, "START exit");
        if (menu_open)
            text(30, 536, C_ACCENT, 0.8f,
                 "MENU: D-pad up/down row   left/right value   left stick value   touch slider   X reset   TRIANGLE reset all   O close");
        else
            text(30, 536, C_DIM, 0.8f,
                 "left/right stick: cursors A/B   D-pad left/right: active cursor   up/down: zoom   touch wave: scrub");

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    dbg_log("exit");
    g_running = 0;
    g_scrub = 0;
    g_state = ST_IDLE;
    sceKernelWaitThreadEnd(th_out, NULL, NULL);
    sceKernelWaitThreadEnd(th_in, NULL, NULL);

    vita2d_wait_rendering_done();
    for (int i = 0; i < N_WAVE_TEX; i++) if (g_wave_tex[i]) vita2d_free_texture(g_wave_tex[i]);
    vita2d_free_pgf(g_pgf);
    vita2d_fini();
    free(g_audio);
    sceKernelExitProcess(0);
    return 0;
}
#endif
