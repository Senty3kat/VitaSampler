/*
 * Vita Sampler - версия 0.6
 *
 * Управление:
 *   R                 - запись / стоп записи
 *   L                 - воспроизведение / пауза
 *   Треугольник       - стоп: короткое нажатие - следующее воспроизведение с точки A,
 *                       удержание - позиция сохраняется
 *   Квадрат           - петля между курсорами вкл/выкл
 *   Круг              - меню эффектов
 *   SELECT            - сохранить WAV (ux0:data/Sampler/)
 *   Левый стик        - курсор A,  правый стик - курсор B (A всегда левее B)
 *   D-pad лево/право  - двигать последний выбранный курсор
 *   D-pad вверх/вниз  - масштаб волны (приблизить / отдалить)
 *   Касание волны     - мгновенный переход и скраб (макс. скорость x2); работает и при открытом меню
 *   START             - выход
 *
 * В меню эффектов: D-pad вверх/вниз - выбор ползунка, лево/право - значение,
 *   левый стик - значение, касание - ползунок, X - сброс (удержание - сброс всех).
 *
 * Запись автоматически сохраняется в ux0:data/Sampler/session.* и восстанавливается при запуске.
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
#define MAX_SCRUB_X   2.0                      /* максимум скраба: x2 от обычной скорости */

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
#define MENU_ROW 25
#define SL_X   250
#define SL_W   440

#define N_FX   6
#define LOG_PATH      "ux0:data/Sampler/log.txt"
#define LOG_PREV      "ux0:data/Sampler/log_prev.txt"
#define SESS_RAW      "ux0:data/Sampler/session.raw"
#define SESS_RAW_TMP  "ux0:data/Sampler/session.raw.tmp"
#define SESS_META     "ux0:data/Sampler/session.meta"
#define SESS_META_TMP "ux0:data/Sampler/session.meta.tmp"

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
static const FxParam FX[N_FX] = {
    { "BIT DEPTH",   16,  15 },   /* 1 ... 16 бит */
    { "SAMPLE RATE", 101, 100 },  /* 500 ... 16000 Гц (логарифмически) */
    { "SPEED",       311, 200 },  /* 0.01x ... 3.00x (до 1.0x - логарифмически, выше - плавно) */
    { "PITCH  semitones", 49, 24 },   /* -24 ... +24 полутонов, шаг 1 */
    { "FINE   cents",    101, 50 },   /* -50 ... +50 центов, шаг 1 */
    { "VOLUME",      151, 75 },   /* 0 ... 150 %, по умолчанию 75 % - запас против клиппинга */
};
#define FX_SPEED_DEF 200

static float speed_from_idx(int idx) {
    if (idx <= FX_SPEED_DEF) return powf(0.01f, (float)(FX_SPEED_DEF - idx) / (float)FX_SPEED_DEF);
    return powf(3.0f, (float)(idx - FX_SPEED_DEF) / 110.0f);
}

/* ---------- Общие данные ---------- */
static int16_t *g_audio = NULL;
static int16_t g_blk_min[MAX_BLOCKS];
static int16_t g_blk_max[MAX_BLOCKS];
static float g_hann[W_N];

static volatile int g_fx[N_FX] = { 15, 100, 200, 24, 50, 75 };
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
static volatile int g_reset_in = 0;             /* после "сна" переоткрыть аудио-порты */
static volatile int g_reset_out = 0;
static volatile int g_in_late = 0;              /* диагностика микрофона: опоздавшие блоки */
static volatile int g_in_odd = 0;               /* диагностика микрофона: необычные коды возврата */
static volatile int g_in_r0 = -999;
static volatile int g_out_late = 0;             /* диагностика вывода: опоздавшие блоки */

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
    if (*hi - *lo < 2) {
        *hi = len;
        if (*hi - *lo < 2) *lo = 0;
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
 *   режим 0 - скраб (скорость плавно следует за пальцем, не быстрее x2)
 *   режим 1 - обычный (скорость 1.0, тон 0): точное воспроизведение,
 *             петля со склейкой (кроссфейд) на шве
 *   режим 2 - WSOLA: скорость и высота тона независимо
 *             этап 1: растяжение времени с подбором кадров по совпадению формы волны
 *             этап 2: передискретизация результата в 48 кГц с коэффициентом высоты тона
 *   Во всех режимах: плавное нарастание/затухание громкости (5 мс) на старте, стопе и конце.
 * ==================================================================== */
#define FADE_SAMPLES   240.0f           /* 5 мс при 48 кГц */
#define SCRUB_KP       4.0              /* подтяжка позиции к пальцу, 1/с */
#define SCRUB_GATE     0.006            /* ниже этой скорости (ист./вых. сэмпл) звук затухает */

/* Интерполяция оконным sinc (16 точек, 128 фаз) вместо линейной: чище звук, меньше "цифровых" зеркал */
#define SK 8
#define SP 128
static float g_ker[SP][2 * SK];

static void ker_init(void) {
    const float PI = 3.14159265f;
    for (int p = 0; p < SP; p++) {
        float fr = (float)p / (float)SP, sum = 0.0f;
        for (int j = 0; j < 2 * SK; j++) {
            float d = (float)(j - (SK - 1)) - fr, w;
            if (fabsf(d) < 1e-6f) w = 1.0f; else { float px = PI * d; w = sinf(px) / px; }
            float ad = fabsf(d) / (float)SK;
            float win = ad >= 1.0f ? 0.0f : (0.42f + 0.5f * cosf(PI * ad) + 0.08f * cosf(2.0f * PI * ad));
            g_ker[p][j] = w * win;
            sum += g_ker[p][j];
        }
        for (int j = 0; j < 2 * SK; j++) g_ker[p][j] /= sum;
    }
}

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
    /* эффекты, скраб, огибающая */
    float held, hold_acc;
    double scrub_rate;
    float scrub_gain;
    float gain;                 /* текущая громкость огибающей */
    int force_fade;             /* 1 = плавно затухнуть (стоп / пауза) */
} Engine;

typedef struct {
    int lo, hi, loop;
    int bits;
    float crush_hz;
    float speed, pratio;
    int neutral;
    int scrub;
    float volume;
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
    if (pos > (double)(len - 1)) pos = (double)(len - 1);
    int i = (int)pos;
    int ph = (int)((pos - (double)i) * (double)SP);
    if (ph > SP - 1) ph = SP - 1;
    const float *k = g_ker[ph];
    int base = i - (SK - 1);
    float acc = 0.0f;
    if (base >= 0 && base + 2 * SK <= len) {
        const int16_t *x = a + base;
        for (int j = 0; j < 2 * SK; j++) acc += k[j] * (float)x[j];
    } else {
        for (int j = 0; j < 2 * SK; j++) {
            int idx = base + j;
            if (idx < 0) idx = 0;
            if (idx >= len) idx = len - 1;
            acc += k[j] * (float)a[idx];
        }
    }
    return acc;
}

/* то же для кольцевого буфера WSOLA (там уже готовый звук) */
static inline float rd_ring(const float *y, double pos) {
    long long i = (long long)floor(pos);
    int ph = (int)((pos - (double)i) * (double)SP);
    if (ph > SP - 1) ph = SP - 1;
    const float *k = g_ker[ph];
    float acc = 0.0f;
    for (int j = 0; j < 2 * SK; j++) acc += k[j] * y[(i - (SK - 1) + j) & Y1_MASK];
    return acc;
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
    const int mode = p->scrub ? 0 : (p->neutral ? 1 : 2);
    int ended = 0, peak = 0;

    /* склейка петли в обычном режиме */
    int xf = 0;
    if (p->loop && span >= 16) { xf = span / 4; if (xf > 320) xf = 320; }

    /* сглаживание скраба */
    const double a_rate = 1.0 - exp(-1.0 / (0.010 * (double)OUT_RATE));
    const float  a_gate = (float)(1.0 - exp(-1.0 / (0.004 * (double)OUT_RATE)));
    const double maxr = MAX_SCRUB_X * step;

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

    for (int i = 0; i < OUT_GRAIN; i++) {
        float s = 0.0f;
        float ft = e->force_fade ? 0.0f : 1.0f;     /* цель огибающей */

        if (ended) { mono[i] = 0; continue; }

        if (mode == 0) {
            /* скорость = скорость пальца + мягкая подтяжка к позиции пальца, всё сглажено */
            double desired = p->scrub_vel / (double)OUT_RATE
                           + (p->scrub_target - e->pos) * (SCRUB_KP / (double)OUT_RATE);
            if (desired > maxr) desired = maxr;
            if (desired < -maxr) desired = -maxr;
            e->scrub_rate += (desired - e->scrub_rate) * a_rate;
            s = rd_clamped(a, len, e->pos);
            e->pos += e->scrub_rate;
            if (e->pos < 0.0) e->pos = 0.0;
            if (e->pos > (double)(len - 2)) e->pos = (double)(len - 2);
            float gt = (float)(fabs(e->scrub_rate) / SCRUB_GATE);
            if (gt > 1.0f) gt = 1.0f;
            e->scrub_gain += (gt - e->scrub_gain) * a_gate;

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
            double rem = (double)hi - e->pos;               /* сколько осталось до конца области */
            if (xf > 0 && rem < (double)xf) {
                /* кроссфейд на шве петли: подмешиваем материал, предшествующий началу петли */
                float t = (float)(1.0 - rem / (double)xf);
                double pre = (double)lo - rem;
                if (pre < 0.0) pre += (double)span;
                s = (1.0f - t) * s + t * rd_clamped(a, len, pre);
            }
            if (!p->loop && rem / step < (double)FADE_SAMPLES) ft = 0.0f;   /* затухание перед концом */
            e->pos += step;

        } else {
            /* дослать кадры, пока в буфере не накопится достаточно готового звука */
            while (!e->ending && (double)(e->frames * W_HS) < e->r1 + (double)SK + 3.0)
                wsola_frame(e, a, len, p);
            double fin = (double)(e->frames * W_HS);
            if (e->r1 + (double)SK + 1.0 >= fin) {
                if (e->ending) { ended = 1; mono[i] = 0; continue; }
            }
            s = rd_ring(e->y1, e->r1);
            e->r1 += step * (double)p->pratio;
        }

        /* эффекты: понижение частоты дискретизации и разрядности */
        if (do_hold) {
            e->hold_acc += hold_inc;
            if (e->hold_acc >= 1.0f) { e->hold_acc -= 1.0f; e->held = s; }
            s = e->held;
        }
        int v = quantize(clamp16((int)s), p->bits);

        /* огибающая: плавный вход/выход, чтобы не было щелчков */
        if (e->gain < ft) { e->gain += 1.0f / FADE_SAMPLES; if (e->gain > ft) e->gain = ft; }
        else if (e->gain > ft) { e->gain -= 1.0f / FADE_SAMPLES; if (e->gain < ft) e->gain = ft; }
        float g = e->gain * p->volume;
        if (mode == 0) g *= e->scrub_gain;
        v = clamp16((int)((float)v * g));

        mono[i] = (int16_t)v;
        int av = v < 0 ? -v : v;
        if (av > peak) peak = av;
    }

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
    int fb = g_fx[0], fr = g_fx[1], fs = g_fx[2], fp = g_fx[3], fc = g_fx[4];
    p->bits = 1 + fb;
    p->crush_hz = 500.0f * powf(32.0f, (float)fr / 100.0f);
    p->speed = speed_from_idx(fs);
    p->pratio = powf(2.0f, ((float)(fp - 24) + (float)(fc - 50) / 100.0f) / 12.0f);
    p->neutral = (fs == FX_SPEED_DEF && fp == 24 && fc == 50);
    p->volume = (float)g_fx[5] / 100.0f;
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
    uint64_t t_prev = 0;

    while (g_running) {
        if (g_reset_in) {                      /* после сна: закрыть порт, откроем заново при записи */
            if (in_port >= 0) { sceAudioInReleasePort(in_port); in_port = -1; }
            g_reset_in = 0;
        }
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
            t_prev = 0;
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

        /* диагностика: приходят ли блоки вовремя и с одинаковым кодом возврата */
        uint64_t tn = sceKernelGetProcessTimeWide();
        if (t_prev && tn - t_prev > 26000) g_in_late++;
        t_prev = tn;
        if (g_in_r0 == -999) g_in_r0 = r;
        else if (r != g_in_r0) g_in_odd++;

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
/* Три буфера по кругу: драйвер читает отданный буфер асинхронно, поэтому нельзя
 * тут же переписывать его следующим блоком (это давало треск). Так сделано и в VitaTTS. */
#define N_OUT_BUFS 3

static int out_thread(SceSize args, void *argp) {
    static int16_t out_bufs[N_OUT_BUFS][OUT_GRAIN * 2] __attribute__((aligned(64)));
    static int16_t mono[OUT_GRAIN];
    static Engine eng;

    int out_port = -1, out_ch = 1, out_err = 0;
    int was_playing = 0, bi = 0;
    uint64_t t_last = 0;

    while (g_running) {
        if (g_reset_out) {                     /* после сна: закрыть порт, откроем заново */
            if (out_port >= 0) { sceAudioOutReleasePort(out_port); out_port = -1; }
            was_playing = 0;
            t_last = 0;
            g_reset_out = 0;
        }

        if (g_state != ST_PLAYING) {
            /* плавное затухание при паузе/стопе, чтобы не было щелчка */
            if (was_playing && out_port >= 0 && g_length > 1) {
                int len = g_length, lo = g_lo, hi = g_hi, peak = 0;
                region_sanitize(&lo, &hi, len);
                Params prm;
                load_params(&prm, lo, hi);
                eng.force_fade = 1;
                engine_render(&eng, g_audio, len, &prm, mono, &peak);
                int16_t *ob = out_bufs[bi];
                if (out_ch == 1) memcpy(ob, mono, sizeof(mono));
                else for (int i = 0; i < OUT_GRAIN; i++) { ob[2 * i] = mono[i]; ob[2 * i + 1] = mono[i]; }
                sceAudioOutOutput(out_port, ob);
                bi = (bi + 1) % N_OUT_BUFS;
            }
            was_playing = 0;
            t_last = 0;
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
            t_last = 0;
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
            engine_reset(&eng, (double)seek);
            g_seek_to = -1;
        }

        Params prm;
        load_params(&prm, lo, hi);
        int peak = 0;
        int ended = engine_render(&eng, g_audio, len, &prm, mono, &peak);

        int16_t *ob = out_bufs[bi];
        if (out_ch == 1) {
            memcpy(ob, mono, sizeof(mono));
        } else {
            for (int i = 0; i < OUT_GRAIN; i++) {
                ob[2 * i] = mono[i];
                ob[2 * i + 1] = mono[i];
            }
        }

        int r = sceAudioOutOutput(out_port, ob);
        bi = (bi + 1) % N_OUT_BUFS;

        /* диагностика недогруза: блок 21 мс, интервал заметно больше - значит, звук прерывался */
        uint64_t tn = sceKernelGetProcessTimeWide();
        if (t_last && tn - t_last > 32000) g_out_late++;
        t_last = tn;

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

/* ---------- Автосохранение сессии: запись переживёт сворачивание и перезапуск ---------- */
typedef struct {
    uint32_t magic, version;
    int32_t length, c1, c2, loop, dirty;
    int32_t fx[N_FX];
    int32_t reserved[3];
} SessionMeta;
#define SESS_MAGIC 0x504D5356u

static volatile int g_sess_c1 = 0, g_sess_c2 = 0, g_sess_dirty = 0;
static volatile int g_save_len = 0, g_save_req = 0, g_save_busy = 0;

static int write_atomic(const char *tmp, const char *dst, const void *data, size_t n) {
    SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return -1;
    const uint8_t *p = (const uint8_t *)data;
    size_t left = n;
    while (left > 0) {
        size_t c = left > 32768 ? 32768 : left;
        int w = sceIoWrite(fd, p, (unsigned)c);
        if (w < 0 || (size_t)w != c) { sceIoClose(fd); return -2; }
        p += c;
        left -= c;
        if (n > 65536) sceKernelDelayThread(500);     /* не мешаем звуку и интерфейсу */
    }
    sceIoClose(fd);
    sceIoRemove(dst);
    return sceIoRename(tmp, dst) < 0 ? -3 : 0;
}

static void session_write_meta(int length) {
    SessionMeta m;
    memset(&m, 0, sizeof(m));
    m.magic = SESS_MAGIC;
    m.version = 2;
    m.length = length;
    m.c1 = g_sess_c1;
    m.c2 = g_sess_c2;
    m.loop = g_loop;
    m.dirty = g_sess_dirty;
    for (int i = 0; i < N_FX; i++) m.fx[i] = g_fx[i];
    write_atomic(SESS_META_TMP, SESS_META, &m, sizeof(m));
}

static void session_write_audio(int length) {
    if (length > 1 && length <= MAX_SAMPLES)
        write_atomic(SESS_RAW_TMP, SESS_RAW, g_audio, (size_t)length * 2);
}

/* Возвращает 1, если сессия восстановлена (g_audio, блоки волны и пик заполнены) */
static int session_restore(SessionMeta *m) {
    SceUID fd = sceIoOpen(SESS_META, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    int r = sceIoRead(fd, m, sizeof(*m));
    sceIoClose(fd);
    if (r != (int)sizeof(*m) || m->magic != SESS_MAGIC || (m->version != 1 && m->version != 2)) return 0;
    if (m->version == 1) m->fx[5] = FX[5].def;       /* сессия из версии 0.5: громкости там ещё не было */
    if (m->length < 2 || m->length > MAX_SAMPLES) return 0;

    fd = sceIoOpen(SESS_RAW, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    size_t need = (size_t)m->length * 2, got = 0;
    uint8_t *dst = (uint8_t *)g_audio;
    while (got < need) {
        size_t c = need - got > 65536 ? 65536 : need - got;
        int n = sceIoRead(fd, dst + got, (unsigned)c);
        if (n <= 0) break;
        got += (size_t)n;
    }
    sceIoClose(fd);
    if (got != need) return 0;

    int peak_all = 0;
    for (int b = 0; b < m->length / IN_GRAIN; b++) {
        int mn = 32767, mx = -32768;
        for (int i = 0; i < IN_GRAIN; i++) {
            int v = g_audio[b * IN_GRAIN + i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        g_blk_min[b] = (int16_t)mn;
        g_blk_max[b] = (int16_t)mx;
        int a1 = mx < 0 ? -mx : mx, a2 = mn < 0 ? -mn : mn;
        if (a1 > peak_all) peak_all = a1;
        if (a2 > peak_all) peak_all = a2;
    }
    g_peak_all = peak_all;
    return 1;
}

static int save_thread(SceSize args, void *argp) {
    while (g_running) {
        if (!g_save_req) { sceKernelDelayThread(50000); continue; }
        g_save_busy = 1;
        int req = __sync_lock_test_and_set(&g_save_req, 0);
        if (req) {
            int len = g_save_len;
            if (req & 1) session_write_audio(len);
            session_write_meta(len);
        }
        g_save_busy = 0;
    }
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
    } else if (kind == 3) {
        tri_up(cx, cy + 1, 6, RGBA8(110, 235, 175, 255));
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
static double x_to_sample(const Layout *L, double tx, int *seg_out) {
    int seg = 0;
    if (L->nseg == 2 && tx >= WAVE_X + L->x0[1] - 8) seg = 1;
    double fx = tx - (double)(WAVE_X + L->x0[seg]);
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
    case 2: {
        float sp = speed_from_idx(idx);
        if (sp < 0.1f) { int v = (int)(sp * 1000.0f + 0.5f); snprintf(out, cap, "0.%03dx", v); }
        else { int v = (int)(sp * 100.0f + 0.5f); snprintf(out, cap, "%d.%02dx", v / 100, v % 100); }
        break; }
    case 3: { int t = idx - 24; snprintf(out, cap, "%c%d st", t < 0 ? '-' : '+', t < 0 ? -t : t); break; }
    case 4: { int t = idx - 50; snprintf(out, cap, "%c%d ct", t < 0 ? '-' : '+', t < 0 ? -t : t); break; }
    default: snprintf(out, cap, "%d %%", idx); break;
    }
}

static void draw_menu(int sel) {
    char buf[48];
    rect(MENU_X, MENU_Y, MENU_W, MENU_H, C_PANEL);
    rect_outline(MENU_X, MENU_Y, MENU_W, MENU_H, 2, C_PANEL2);
    for (int i = 0; i < N_FX; i++) {
        int y0 = MENU_Y + 3 + i * MENU_ROW;
        int idx = g_fx[i];
        int sel_row = (i == sel);
        if (sel_row) rect_outline(MENU_X + 8, y0, MENU_W - 16, MENU_ROW - 2, 2, C_ACCENT);
        text(MENU_X + 24, y0 + 17, sel_row ? C_ACCENT : C_DIM, 0.85f, FX[i].name);

        float f = (float)idx / (float)(FX[i].n - 1);
        float fd = (float)FX[i].def / (float)(FX[i].n - 1);
        int kx = SL_X + (int)(f * SL_W);
        rect(SL_X, y0 + 8, SL_W, 7, C_PANEL2);
        rect(SL_X, y0 + 8, kx - SL_X, 7, sel_row ? C_ACCENT : C_DIM);
        rect(SL_X + (int)(fd * SL_W) - 1, y0 + 4, 2, 15, C_DIM);   /* метка "по умолчанию" */
        rect(kx - 4, y0 + 3, 8, 17, C_TEXT);

        fx_text(buf, sizeof(buf), i, idx);
        text(SL_X + SL_W + 36, y0 + 18, idx == FX[i].def ? C_TEXT : C_AMBER, 1.0f, buf);
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

/* ---------- Состояние интерфейса (файловый уровень, чтобы им пользовались вспомогательные функции) ---------- */
static double c1 = 0.0, c2 = 0.0;      /* курсоры A и B (в сэмплах), всегда c1 < c2 */
static int active = 0;                 /* последний выбранный курсор: 0 = A, 1 = B */
static int split = 0;
static int dirty = 0;                  /* есть запись, не выгруженная в WAV */
static char msg[160] = "Press R to start recording";

typedef struct { uint64_t t; double v; } TouchSample;

/* Скорость пальца (сэмплов/с): разность по окну ~90 мс - сглаживает дрожание и дискретность датчика */
static double vel_estimate(const TouchSample *h, int n, uint64_t now_t) {
    if (n < 2) return 0.0;
    int oldest = -1;
    for (int i = 0; i < n; i++) {
        if (now_t - h[i].t <= 90000ULL) { oldest = i; break; }
    }
    if (oldest < 0 || oldest >= n - 1) return 0.0;
    uint64_t dt = h[n - 1].t - h[oldest].t;
    if (dt < 25000ULL) return 0.0;
    return (h[n - 1].v - h[oldest].v) / ((double)dt * 1e-6);
}

/* Курсор A никогда не заходит за B и наоборот (минимум 2 сэмпла между ними) */
static void enforce_cursors(int length) {
    if (length < 4) { c1 = 0.0; c2 = (double)length; return; }
    if (c1 < 0.0) c1 = 0.0;
    if (c2 > (double)length) c2 = (double)length;
    if (c2 < 2.0) c2 = 2.0;
    if (c1 > c2 - 2.0) { if (active == 0) c1 = c2 - 2.0; else c2 = c1 + 2.0; }
    if (c1 < 0.0) c1 = 0.0;
    if (c2 > (double)length) c2 = (double)length;
}

static void request_autosave(int length, int with_audio) {
    g_sess_c1 = (int)c1;
    g_sess_c2 = (int)c2;
    g_sess_dirty = dirty;
    g_save_len = length;
    __sync_synchronize();
    g_save_req |= (with_audio ? 3 : 2);
}

static void finish_recording(void) {
    int length = g_length;
    dirty = (length > 0);
    view_start = 0;
    view_len = length > 0 ? length : MIN_VIEW;
    clamp_view(length);
    c1 = 0.0; c2 = (double)length; active = 0; split = 0;
    g_lo = 0; g_hi = length; g_play_pos = 0; g_seek_to = -1;
    snprintf(msg, sizeof(msg), "Recording stopped. L: play   SELECT: save WAV");
    dbg_log("rec stop len=%d late=%d odd=%d", length, g_in_late, g_in_odd);
    if (length > 1) request_autosave(length, 1);
}

#ifndef SAMPLER_NO_MAIN
int main(void) {
    sceIoMkdir("ux0:data/Sampler", 0777);
    sceIoRemove(LOG_PREV);
    sceIoRename(LOG_PATH, LOG_PREV);          /* лог прошлого запуска сохраняем: пригодится после сбоя */
    {
        SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) sceIoClose(fd);
    }
    dbg_log("start v0.6");

    g_audio = (int16_t *)malloc(MAX_SAMPLES * sizeof(int16_t));
    if (!g_audio) { dbg_log("malloc failed"); sceKernelExitProcess(0); }

    for (int i = 0; i < W_N; i++)
        g_hann[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * (float)i / (float)W_N));
    ker_init();

    vita2d_init_advanced(4 * 1024 * 1024);
    vita2d_set_clear_color(C_BG);
    g_pgf = vita2d_load_default_pgf();
    for (int i = 0; i < N_WAVE_TEX; i++) g_wave_tex[i] = vita2d_create_empty_texture(WAVE_W, WAVE_H);
    dbg_log("gfx ready");

    /* восстановить прошлую сессию, если она есть */
    {
        SessionMeta sm;
        if (session_restore(&sm)) {
            int len = sm.length;
            g_length = len;
            c1 = (double)sm.c1; c2 = (double)sm.c2; active = 0;
            enforce_cursors(len);
            for (int i = 0; i < N_FX; i++)
                if (sm.fx[i] >= 0 && sm.fx[i] < FX[i].n) g_fx[i] = sm.fx[i];
            g_loop = sm.loop ? 1 : 0;
            dirty = sm.dirty ? 1 : 0;
            g_lo = (int)c1; g_hi = (int)c2;
            view_start = 0; view_len = len; clamp_view(len);
            int t10 = (int)((int64_t)len * 10 / SAMPLE_RATE);
            snprintf(msg, sizeof(msg), "Restored last session (%d.%d s). SELECT: export WAV", t10 / 10, t10 % 10);
            dbg_log("restored len=%d dirty=%d", len, dirty);
        } else {
            dbg_log("no session to restore");
        }
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    /* аудио-потоки: высокий приоритет и отдельные ядра процессора, чтобы рисование их не вытесняло */
    SceUID th_out = sceKernelCreateThread("audio_out", out_thread, 96, 0x10000, 0, 0x40000, NULL);
    if (th_out < 0) th_out = sceKernelCreateThread("audio_out", out_thread, 96, 0x10000, 0, 0, NULL);
    if (th_out < 0) th_out = sceKernelCreateThread("audio_out", out_thread, 0x10000100, 0x10000, 0, 0, NULL);
    SceUID th_in = sceKernelCreateThread("audio_in", in_thread, 96, 0x10000, 0, 0x20000, NULL);
    if (th_in < 0) th_in = sceKernelCreateThread("audio_in", in_thread, 96, 0x10000, 0, 0, NULL);
    if (th_in < 0) th_in = sceKernelCreateThread("audio_in", in_thread, 0x10000100, 0x10000, 0, 0, NULL);
    SceUID th_save = sceKernelCreateThread("autosave", save_thread, 0x10000100, 0x10000, 0, 0x20000, NULL);
    if (th_save < 0) th_save = sceKernelCreateThread("autosave", save_thread, 0x10000100, 0x10000, 0, 0, NULL);
    dbg_log("threads: out=%d in=%d save=%d", (int)th_out, (int)th_in, (int)th_save);
    sceKernelStartThread(th_in, 0, NULL);
    sceKernelStartThread(th_out, 0, NULL);
    sceKernelStartThread(th_save, 0, NULL);

    SceCtrlData pad;
    SceTouchData touch;
    unsigned int old_buttons = 0;
    char buf[200], t1[32], t2[32];
    int shown_level = 0, clip_hold = 0;
    int confirm = 0;
    unsigned int frame = 0;
    int last_err = 0, prev_state = ST_IDLE;

    int nudge_frames = 0, zoom_frames = 0;

    /* стоп на треугольнике: короткое нажатие / удержание */
    int tri_active = 0, tri_frames = 0, tri_pos = 0;

    /* меню эффектов */
    int menu_open = 0, menu_sel = 0, menu_touch_row = -1, menu_touch_id = -1, fx_hold = 0, x_hold = 0;
    float fx_acc = 0.0f;

    /* скраб */
    int scrubbing = 0, scrub_id = -1, resume_state = ST_IDLE, scrub_seg = 0;
    double scrub_pos = 0.0;
    float vel_f = 0.0f;
    TouchSample hist[8];
    int hn = 0;

    int wave_cur = -1, wave_next = 0;
    int key[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    Layout lay_last;
    memset(&lay_last, 0xff, sizeof(lay_last));

    /* автосохранение настроек курсоров/эффектов */
    int last_sig = 0, sig_pending = 0;
    unsigned int last_sig_frame = 0;

    uint64_t last_t = sceKernelGetProcessTimeWide();
    uint64_t last_sys = sceKernelGetSystemTimeWide();

    while (1) {
        frame++;
        uint64_t now_t = sceKernelGetProcessTimeWide();
        uint64_t sys_now = sceKernelGetSystemTimeWide();
        float dt = (float)(now_t - last_t) * 1e-6f;
        last_t = now_t;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.1f) dt = 0.1f;

        /* ---- возврат из "сна" (свернули на главный экран и вернулись) ---- */
        if (sys_now - last_sys > 1500000ULL) {
            dbg_log("RESUME gap=%d ms state=%d", (int)((sys_now - last_sys) / 1000), (int)g_state);
            if (g_state == ST_RECORDING) {
                g_state = ST_IDLE;
                sceKernelDelayThread(40000);
                finish_recording();
                prev_state = ST_IDLE;
                snprintf(msg, sizeof(msg), "Recording was interrupted by suspend - kept what was recorded");
            } else if (g_state == ST_PLAYING) {
                g_state = ST_IDLE;
            }
            g_scrub = 0; scrubbing = 0; scrub_id = -1; menu_touch_row = -1; hn = 0;
            g_reset_in = 1; g_reset_out = 1;
            vita2d_wait_rendering_done();
            for (int i = 0; i < N_WAVE_TEX; i++) {
                if (g_wave_tex[i]) vita2d_free_texture(g_wave_tex[i]);
                g_wave_tex[i] = vita2d_create_empty_texture(WAVE_W, WAVE_H);
            }
            vita2d_free_pgf(g_pgf);
            g_pgf = vita2d_load_default_pgf();
            key[0] = -1;
            wave_cur = -1;
            dbg_log("resume: gpu resources recreated");
        }
        last_sys = sys_now;

        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old_buttons;
        old_buttons = pad.buttons;
        if (pressed & SCE_CTRL_START) break;

        int state = g_state;
        int length = g_length;

        /* запись остановилась сама (лимит 5 минут или ошибка) */
        if (prev_state == ST_RECORDING && state != ST_RECORDING) {
            finish_recording();
            state = g_state;
            length = g_length;
        }

        /* ---- R: запись / стоп записи ---- */
        if (pressed & SCE_CTRL_RTRIGGER) {
            if (state == ST_RECORDING) {
                g_state = ST_IDLE;
                sceKernelDelayThread(40000);
                finish_recording();
                prev_state = ST_IDLE;
            } else if (dirty && !confirm) {
                confirm = 1;
                snprintf(msg, sizeof(msg), "Unsaved recording! Press R again to erase it, or SELECT to save");
            } else {
                g_state = ST_IDLE;
                g_scrub = 0;
                scrubbing = 0;
                scrub_id = -1;
                sceKernelDelayThread(60000);
                for (int w = 0; w < 300 && (g_save_req || g_save_busy); w++) sceKernelDelayThread(10000);
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
                c1 = c2 = 0.0; split = 0; active = 0;
                g_lo = 0; g_hi = 0;
                __sync_synchronize();
                g_state = ST_RECORDING;
                snprintf(msg, sizeof(msg), "Recording... press R to stop");
                dbg_log("rec start");
            }
            state = g_state;
            length = g_length;
        }

        /* ---- L: воспроизведение / пауза ---- */
        if ((pressed & SCE_CTRL_LTRIGGER) && state != ST_RECORDING && length > 1) {
            if (state == ST_PLAYING) {
                g_state = ST_IDLE;
                snprintf(msg, sizeof(msg), "Paused");
                dbg_log("pause");
            } else {
                g_error = 0;
                g_state = ST_PLAYING;
                snprintf(msg, sizeof(msg), g_loop ? "Playing (loop)" : "Playing");
                dbg_log("play");
            }
        }

        /* ---- Треугольник: стоп. Короткое нажатие - с точки A, удержание - позиция сохраняется ---- */
        if ((pressed & SCE_CTRL_TRIANGLE) && state != ST_RECORDING && length > 1) {
            g_state = ST_IDLE;
            g_scrub = 0;
            sceKernelDelayThread(40000);
            tri_pos = g_play_pos;
            int lo = (int)c1, hi = (int)c2;
            region_sanitize(&lo, &hi, length);
            g_play_pos = lo;
            g_seek_to = -1;
            tri_active = 1;
            tri_frames = 0;
            snprintf(msg, sizeof(msg), "Stopped - next play starts from A   (hold TRIANGLE to keep the position)");
            dbg_log("stop");
        }
        if (tri_active) {
            if (pad.buttons & SCE_CTRL_TRIANGLE) {
                if (++tri_frames == 24) {
                    g_play_pos = tri_pos;
                    snprintf(msg, sizeof(msg), "Stopped - position kept");
                }
            } else tri_active = 0;
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
            if (r == 0) {
                snprintf(msg, sizeof(msg), "Saved: %s", path);
                dirty = 0;
                request_autosave(length, 0);
                dbg_log("saved %s", path);
            }
            else if (r == -1) snprintf(msg, sizeof(msg), "Nothing to save - record something first");
            else snprintf(msg, sizeof(msg), "Could not write the file");
            last_sys = sceKernelGetSystemTimeWide();     /* запись файла - не "сон" */
        }

        /* ---- Круг: меню эффектов ---- */
        if (pressed & SCE_CTRL_CIRCLE) {
            menu_open = !menu_open;
            menu_touch_row = -1;
            fx_acc = 0.0f;
        }

        /* ---- меню: управление ползунками ---- */
        if (menu_open) {
            if (pressed & SCE_CTRL_UP)   menu_sel = (menu_sel + N_FX - 1) % N_FX;
            if (pressed & SCE_CTRL_DOWN) menu_sel = (menu_sel + 1) % N_FX;

            if (pad.buttons & SCE_CTRL_CROSS) {
                x_hold++;
                if (x_hold == 1) fx_set(menu_sel, FX[menu_sel].def);
                if (x_hold == 30) {
                    for (int i = 0; i < N_FX; i++) fx_set(i, FX[i].def);
                    snprintf(msg, sizeof(msg), "All effects reset");
                }
            } else x_hold = 0;

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
            if (a1 != 0.0f) { c1 += (double)a1 * 420.0 * spp * (double)dt; active = 0; enforce_cursors(length); }
            if (a2 != 0.0f) { c2 += (double)a2 * 420.0 * spp * (double)dt; active = 1; enforce_cursors(length); }

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
                    if (move != 0.0) {
                        if (active == 0) c1 += dir * move; else c2 += dir * move;
                        enforce_cursors(length);
                    }
                } else nudge_frames = 0;

                int zd = ((pad.buttons & SCE_CTRL_UP) ? 1 : 0) - ((pad.buttons & SCE_CTRL_DOWN) ? 1 : 0);
                if (zd) {
                    zoom_frames++;
                    double anchor = active == 0 ? c1 : c2;
                    if (zoom_frames == 1) zoom_view(zd > 0 ? 0.5 : 2.0, anchor, length);
                    else if (zoom_frames > 18) zoom_view(zd > 0 ? 0.97 : 1.03, anchor, length);
                } else zoom_frames = 0;
            }

            enforce_cursors(length);
            g_lo = (int)c1;
            g_hi = (int)c2;
        }

        /* ---- сплит / положение окна ---- */
        if (state != ST_RECORDING && length > 1) {
            clamp_view(length);
            double lo = c1, hi = c2;
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

        /* ---- касание: два пальца независимо - ползунок меню и скраб волны ---- */
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        int nrep = (int)touch.reportNum;
        if (nrep > 6) nrep = 6;

        if (menu_open) {
            int mk = -1;
            if (menu_touch_row >= 0) {
                for (int k = 0; k < nrep; k++) if (touch.report[k].id == menu_touch_id) mk = k;
                if (mk < 0) menu_touch_row = -1;
            } else {
                for (int k = 0; k < nrep; k++) {
                    if (scrubbing && touch.report[k].id == scrub_id) continue;
                    double fx = touch.report[k].x / 2.0, fy = touch.report[k].y / 2.0;
                    if (fx >= MENU_X && fy >= MENU_Y && fy < MENU_Y + MENU_H) {
                        int row = (int)((fy - MENU_Y - 3.0) / MENU_ROW);
                        if (row < 0) row = 0;
                        if (row > N_FX - 1) row = N_FX - 1;
                        menu_touch_row = row;
                        menu_touch_id = touch.report[k].id;
                        menu_sel = row;
                        mk = k;
                        break;
                    }
                }
            }
            if (mk >= 0) {
                double fx = touch.report[mk].x / 2.0;
                float f = (float)((fx - SL_X) / SL_W);
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
                if (fx >= SL_X - 20)
                    fx_set(menu_touch_row, (int)(f * (float)(FX[menu_touch_row].n - 1) + 0.5f));
            }
        } else {
            menu_touch_row = -1;
        }

        Layout lay_now;
        compute_layout(&lay_now, length, state == ST_RECORDING, split, c1, c2);

        int sk = -1;
        if (scrubbing) {
            for (int k = 0; k < nrep; k++) if (touch.report[k].id == scrub_id) sk = k;
        } else if (g_state != ST_RECORDING && length > 1) {
            for (int k = 0; k < nrep; k++) {
                if (menu_touch_row >= 0 && touch.report[k].id == menu_touch_id) continue;
                double fx = touch.report[k].x / 2.0, fy = touch.report[k].y / 2.0;
                if (fx >= WAVE_X && fx <= WAVE_X + WAVE_W && fy >= PANEL_Y && fy <= PANEL_Y + PANEL_H) { sk = k; break; }
            }
        }

        if (sk >= 0) {
            int seg = 0;
            double target = x_to_sample(&lay_now, touch.report[sk].x / 2.0, &seg);
            if (target < 0.0) target = 0.0;
            if (target > (double)(length - 1)) target = (double)(length - 1);

            if (!scrubbing) {
                scrubbing = 1;
                scrub_id = touch.report[sk].id;
                resume_state = g_state;
                scrub_seg = seg;
                scrub_pos = target;
                hn = 0;
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
                if (seg != scrub_seg) {                     /* перешли на другой мини-дисплей: прыжок */
                    scrub_seg = seg;
                    hn = 0;
                    g_seek_to = (int)target;
                }
                scrub_pos = target;
                g_scrub_target = (float)target;
            }
            if (hn == 8) { memmove(hist, hist + 1, 7 * sizeof(TouchSample)); hn = 7; }
            hist[hn].t = now_t; hist[hn].v = target; hn++;
            vel_f = (float)vel_estimate(hist, hn, now_t);
            g_scrub_vel = vel_f;
        } else if (scrubbing) {
            scrubbing = 0;
            scrub_id = -1;
            g_scrub = 0;
            g_scrub_vel = 0.0f;
            hn = 0;
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

        /* ---- автосохранение курсоров/эффектов (через 1.5 с после последнего изменения) ---- */
        if (length > 1 && state != ST_RECORDING) {
            int sig = (int)c1 * 31 + (int)c2 * 17 + g_loop * 7 + dirty * 3;
            for (int i = 0; i < N_FX; i++) sig = sig * 131 + g_fx[i];
            if (sig != last_sig) { last_sig = sig; last_sig_frame = frame; sig_pending = 1; }
            if (sig_pending && frame - last_sig_frame > 90) {
                request_autosave(length, 0);
                sig_pending = 0;
            }
        }

        /* ---- лог: пульс раз в ~1.5 с и ошибки ---- */
        if (frame % 90 == 0)
            dbg_log("hb f=%u st=%d len=%d pos=%d lvl=%d split=%d menu=%d scrub=%d mic_late=%d mic_odd=%d r0=%d out_late=%d",
                    frame, state, length, playhead, g_level, split, menu_open, scrubbing,
                    g_in_late, g_in_odd, g_in_r0, g_out_late);
        if (g_error != last_err) { last_err = g_error; dbg_log("audio error 0x%08X", (unsigned)g_error); }
        prev_state = state;

        /* ---------- Рисуем ---------- */
        vita2d_start_drawing();
        vita2d_clear_screen();

        /* шапка */
        rect(24, 16, 6, 34, C_ACCENT);
        text(42, 46, C_TEXT, 1.5f, "VITA SAMPLER");
        text(260, 46, C_DIM, 0.9f, "v0.6");
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
                     (int)c1, (int)c2, (int)(c2 - c1), active == 0 ? 'A' : 'B');
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
            char v0[32], v1[32], v2[32], v3[32], v4[32], v5[32];
            fx_text(v0, sizeof(v0), 0, g_fx[0]); fx_text(v1, sizeof(v1), 1, g_fx[1]);
            fx_text(v2, sizeof(v2), 2, g_fx[2]); fx_text(v3, sizeof(v3), 3, g_fx[3]);
            fx_text(v4, sizeof(v4), 4, g_fx[4]); fx_text(v5, sizeof(v5), 5, g_fx[5]);
            int modified = 0;
            for (int i = 0; i < N_FX; i++) if (g_fx[i] != FX[i].def) modified = 1;
            snprintf(buf, sizeof(buf), "FX:  %s | %s | speed %s | pitch %s %s | vol %s    (O: edit)", v0, v1, v2, v3, v4, v5);
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
        draw_chip(30, 504, 26, "R");    text(62, 510, C_DIM, 0.85f, "rec/stop");
        draw_chip(140, 504, 26, "L");   text(172, 510, C_DIM, 0.85f, "play/pause");
        draw_pad_icon(272, 504, 3);     text(292, 510, C_DIM, 0.85f, "stop (hold = keep pos)");
        draw_pad_icon(480, 504, 1);     text(500, 510, C_DIM, 0.85f, "loop");
        draw_pad_icon(560, 504, 2);     text(580, 510, C_DIM, 0.85f, "effects");
        draw_chip(660, 504, 44, "SEL"); text(712, 510, C_DIM, 0.85f, "save WAV");
        text(830, 510, C_DIM, 0.85f, "START exit");
        if (menu_open)
            text(30, 536, C_ACCENT, 0.8f,
                 "MENU: D-pad up/down row  left/right value  left stick value  touch sliders  X reset (hold = all)  O close");
        else
            text(30, 536, C_DIM, 0.8f,
                 "left/right stick: cursors A/B   D-pad left/right: active cursor   up/down: zoom   touch wave: scrub");

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    dbg_log("exit");
    if (g_state == ST_RECORDING) {              /* вышли во время записи: сохраняем то, что успели */
        g_state = ST_IDLE;
        sceKernelDelayThread(40000);
        finish_recording();
    } else if (g_length > 1) {
        request_autosave(g_length, 0);
    }
    for (int w = 0; w < 300 && (g_save_req || g_save_busy); w++) sceKernelDelayThread(10000);
    g_running = 0;
    g_scrub = 0;
    g_state = ST_IDLE;
    sceKernelWaitThreadEnd(th_out, NULL, NULL);
    sceKernelWaitThreadEnd(th_in, NULL, NULL);
    sceKernelWaitThreadEnd(th_save, NULL, NULL);

    vita2d_wait_rendering_done();
    for (int i = 0; i < N_WAVE_TEX; i++) if (g_wave_tex[i]) vita2d_free_texture(g_wave_tex[i]);
    vita2d_free_pgf(g_pgf);
    vita2d_fini();
    free(g_audio);
    sceKernelExitProcess(0);
    return 0;
}
#endif
