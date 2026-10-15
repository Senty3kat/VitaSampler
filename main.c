/*
 * Vita Sampler - версия 0.7
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
#define PANEL_Y 54
#define PANEL_W 912
#define PANEL_H 266
#define WAVE_X  40
#define WAVE_Y  124
#define WAVE_W  880
#define WAVE_H  150
#define SEG_W   432                            /* ширина мини-дисплея в режиме сплита */
#define N_WAVE_TEX 4

#define MENU_X 24
#define MENU_Y 332
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

/* палитра нового интерфейса */
#define K_CREAM_HI RGBA8(248, 243, 229, 255)
#define K_CREAM    RGBA8(237, 229, 209, 255)
#define K_BEIGE    RGBA8(224, 213, 188, 255)
#define K_BEIGE_LO RGBA8(205, 192, 164, 255)
#define K_EDGE     RGBA8(160, 145, 116, 255)
#define K_BROWN    RGBA8(72, 56, 44, 255)
#define K_BROWN_LO RGBA8(128, 108, 88, 255)
#define K_ORANGE   RGBA8(233, 106, 38, 255)
#define K_ORANGE_HI RGBA8(247, 152, 72, 255)
#define K_ORANGE_LO RGBA8(190, 76, 20, 255)
#define K_YELLOW   RGBA8(244, 190, 62, 255)
#define K_RED      RGBA8(214, 62, 50, 255)
#define K_GREEN    RGBA8(76, 178, 96, 255)
#define K_LCD      RGBA8(27, 24, 21, 255)
#define K_LCD_HI   RGBA8(52, 44, 36, 255)
#define K_AMBER    RGBA8(255, 178, 62, 255)
#define K_AMBER_LT RGBA8(255, 216, 150, 255)
#define K_AMBER_DIM RGBA8(176, 122, 52, 255)
#define K_WAVE     RGBA8(255, 150, 46, 255)
#define K_WAVE_REC RGBA8(255, 96, 78, 255)

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

/* режимы: 0 = EDIT (курсоры A/B), 1 = SLICE (авто-нарезка на регионы) */
#define SL_MAX 32
#define SL_MIN 4
static int g_mode = 0;
static int g_sl_n = 8;                 /* сколько регионов получилось */
static int g_sl_req = 8;               /* сколько регионов запрошено (4..32) */
static int g_sl_sel = 0;               /* выбранный регион */
static int g_sl_lo = 0, g_sl_hi = 0;   /* область, внутри которой построены регионы (выбрана в EDIT) */
static int g_sl_pos[SL_MAX + 1];       /* начала регионов; g_sl_pos[g_sl_n] = длина записи */
static volatile int g_audio_ver = 0;   /* растёт при каждом изменении звука (запись, обрезка, отмена) */
static volatile unsigned g_rg_seq = 0; /* счётчик для согласованной передачи области + перемотки */

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
static double g_scrub_tau_base = 0.008;    /* базовая инерция скраба, с */
static double g_scrub_tau_extra = 0.035;   /* добавочная инерция скраба на малой скорости, с */
static double g_scrub_min_x = 0.20;        /* ниже 20% обычной скорости при скрабе звук глушится */

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

    /* сглаживание скраба: на медленном движении сглаживаем сильнее (иначе звук "вибрирует") */
    double rel = fabs(e->scrub_rate) / step;              /* текущая скорость относительно обычной */
    double slow = 1.0 - rel / 0.5;
    if (slow < 0.0) slow = 0.0;
    if (slow > 1.0) slow = 1.0;
    slow = slow * slow * (3.0 - 2.0 * slow);
    const double a_rate = 1.0 - exp(-1.0 / ((g_scrub_tau_base + g_scrub_tau_extra * slow) * (double)OUT_RATE));
    const float  a_gate = (float)(1.0 - exp(-1.0 / (0.012 * (double)OUT_RATE)));
    const double knee_lo = 0.75 * g_scrub_min_x, knee_w = 0.5 * g_scrub_min_x;
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
            {   /* "магнит" к обычной скорости 1.0x: рука почти не попадает точно, поэтому +-12% прижимаем к 1.0x */
                double rd = fabs(desired) / step, dd = fabs(rd - 1.0) / 0.12;
                if (dd < 1.0) {
                    double w = 1.0 - dd; w = w * w * (3.0 - 2.0 * w);
                    double tgt = (desired < 0.0 ? -step : step);
                    desired += (tgt - desired) * w;
                }
            }
            e->scrub_rate += (desired - e->scrub_rate) * a_rate;
            s = rd_clamped(a, len, e->pos);
            e->pos += e->scrub_rate;
            if (e->pos < 0.0) e->pos = 0.0;
            if (e->pos > (double)(len - 2)) e->pos = (double)(len - 2);
            double kr = (fabs(e->scrub_rate) / step - knee_lo) / knee_w;
            if (kr < 0.0) kr = 0.0;
            if (kr > 1.0) kr = 1.0;
            float gt = (float)(kr * kr * (3.0 - 2.0 * kr));
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
    static Engine eng, eng_old;
    static int16_t mono2[OUT_GRAIN];
    Params prm_prev, prm_old;
    int have_prm = 0, old_active = 0;

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
            have_prm = 0;
            old_active = 0;
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

        /* область и перемотка читаются согласованно (пэды меняют обе величины одновременно) */
        int lo, hi, seek;
        for (;;) {
            unsigned q1 = g_rg_seq;
            __sync_synchronize();
            lo = g_lo; hi = g_hi; seek = g_seek_to;
            __sync_synchronize();
            if (q1 == g_rg_seq && !(q1 & 1u)) break;
        }
        region_sanitize(&lo, &hi, len);

        if (!was_playing) {
            double p0 = (double)g_play_pos;
            if (p0 < (double)lo || p0 >= (double)hi) p0 = (double)lo;
            engine_reset(&eng, p0);
            was_playing = 1;
        }
        if (seek >= 0) {
            if (seek > len - 1) seek = len - 1;
            /* перезапуск (пэд, касание волны): старый звук плавно гаснет, новый плавно нарастает - без щелчка */
            if (was_playing && have_prm) {
                memcpy(&eng_old, &eng, sizeof(eng));
                eng_old.force_fade = 1;
                prm_old = prm_prev;
                old_active = 2;
            }
            engine_reset(&eng, (double)seek);
            __sync_bool_compare_and_swap(&g_seek_to, seek, -1);   /* если пришла новая перемотка - не затираем */
        }

        Params prm;
        load_params(&prm, lo, hi);
        int peak = 0;
        int ended = engine_render(&eng, g_audio, len, &prm, mono, &peak);
        prm_prev = prm;
        have_prm = 1;
        if (old_active) {
            int pk2 = 0;
            engine_render(&eng_old, g_audio, len, &prm_old, mono2, &pk2);
            for (int i = 0; i < OUT_GRAIN; i++) mono[i] = (int16_t)clamp16((int)mono[i] + (int)mono2[i]);
            old_active--;
            if (pk2 > peak) peak = pk2;
        }

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

static char g_wav_path[128] = "";
static int g_wav_ver = -1;             /* версия звука, которая уже выгружена в g_wav_path */
static int g_wav_overwrote = 0;

static int save_wav(char *path, size_t path_cap) {
    int len = g_length;
    if (len <= 0) return -1;

    sceIoMkdir("ux0:data/Sampler", 0777);

    /* звук не менялся с прошлой выгрузки - перезаписываем тот же файл, а не плодим копии */
    g_wav_overwrote = 0;
    if (g_wav_path[0] && g_wav_ver == g_audio_ver) {
        snprintf(path, path_cap, "%s", g_wav_path);
        g_wav_overwrote = 1;
    } else {
        for (int i = 1; i < 1000; i++) {
            snprintf(path, path_cap, "ux0:data/Sampler/rec_%03d.wav", i);
            FILE *t = fopen(path, "rb");
            if (!t) break;
            fclose(t);
        }
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
    snprintf(g_wav_path, sizeof(g_wav_path), "%s", path);
    g_wav_ver = g_audio_ver;
    return 0;
}

/* ---------- Автосохранение сессии: запись переживёт сворачивание и перезапуск ---------- */
typedef struct {
    uint32_t magic, version;
    int32_t length, c1, c2, loop, dirty;
    int32_t fx[N_FX];
    int32_t mode, sl_n, sl_req, sl_sel;
    int32_t sl_pos[SL_MAX + 1];
    int32_t reserved[4];
} SessionMeta;
/* сессии версий 1 и 2 занимали 64 байта: 7 полей + 5 или 6 эффектов + резерв */
typedef struct {
    uint32_t magic, version;
    int32_t length, c1, c2, loop, dirty;
    int32_t rest[9];
} SessionMetaOld;
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
    m.version = 3;
    m.length = length;
    m.c1 = g_sess_c1;
    m.c2 = g_sess_c2;
    m.loop = g_loop;
    m.dirty = g_sess_dirty;
    for (int i = 0; i < N_FX; i++) m.fx[i] = g_fx[i];
    m.mode = g_mode;
    m.sl_n = g_sl_n;
    m.sl_req = g_sl_req;
    m.sl_sel = g_sl_sel;
    for (int i = 0; i <= SL_MAX; i++) m.sl_pos[i] = g_sl_pos[i];
    write_atomic(SESS_META_TMP, SESS_META, &m, sizeof(m));
}

static void session_write_audio(int length) {
    if (length > 1 && length <= MAX_SAMPLES)
        write_atomic(SESS_RAW_TMP, SESS_RAW, g_audio, (size_t)length * 2);
}

/* Возвращает 1, если сессия восстановлена (g_audio, блоки волны и пик заполнены) */
static int session_restore(SessionMeta *m) {
    uint8_t raw[sizeof(SessionMeta) + 16];
    SceUID fd = sceIoOpen(SESS_META, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    int r = sceIoRead(fd, raw, sizeof(raw));
    sceIoClose(fd);
    memset(m, 0, sizeof(*m));
    if (r == (int)sizeof(SessionMeta)) {
        memcpy(m, raw, sizeof(*m));
        if (m->magic != SESS_MAGIC || m->version != 3) return 0;
    } else if (r == (int)sizeof(SessionMetaOld)) {
        SessionMetaOld o;
        memcpy(&o, raw, sizeof(o));
        if (o.magic != SESS_MAGIC || (o.version != 1 && o.version != 2)) return 0;
        m->magic = o.magic; m->version = 3;
        m->length = o.length; m->c1 = o.c1; m->c2 = o.c2; m->loop = o.loop; m->dirty = o.dirty;
        int nfx_old = (o.version == 1) ? 5 : 6;
        for (int i = 0; i < N_FX; i++) m->fx[i] = (i < nfx_old) ? o.rest[i] : FX[i].def;
        m->sl_req = 8;
    } else {
        return 0;
    }
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

/* ====================================================================
 *  Скраб: сглаживание пальца (фильтр "One Euro")
 *  Датчик касаний даёт позицию ступеньками по 0.5 пикселя. Если считать по ним скорость,
 *  при медленном движении звук "вибрирует". Фильтр сильно сглаживает медленные движения и
 *  почти не запаздывает на быстрых.
 * ==================================================================== */
typedef struct { double x, dx; int init; } OneEuro;

static double oe_alpha(double cutoff, double dt) {
    double tau = 1.0 / (2.0 * 3.14159265358979 * cutoff);
    return 1.0 / (1.0 + tau / dt);
}

static double oe_step(OneEuro *f, double x, double dt, double mincut, double beta, double dcut) {
    if (!f->init) { f->init = 1; f->x = x; f->dx = 0.0; return x; }
    double dx = (x - f->x) / dt;
    double ad = oe_alpha(dcut, dt);
    f->dx = ad * dx + (1.0 - ad) * f->dx;
    double cutoff = mincut + beta * fabs(f->dx);
    double a = oe_alpha(cutoff, dt);
    f->x = a * x + (1.0 - a) * f->x;
    return f->x;
}

typedef struct { OneEuro oe; double xf_prev; double vel_px; } ScrubTrack;

static void scrub_track_reset(ScrubTrack *s, double x_px) {
    memset(s, 0, sizeof(*s));
    s->oe.init = 1; s->oe.x = x_px; s->oe.dx = 0.0;
    s->xf_prev = x_px;
}

/* x_px - сырая позиция пальца в пикселях экрана; возвращает сглаженную позицию и скорость (пикс/с) */
static void scrub_track_step(ScrubTrack *s, double x_px, double dt, double *xf_out, double *vel_px_out) {
    if (dt < 0.004) dt = 0.004;
    if (dt > 0.1) dt = 0.1;
    double xf = oe_step(&s->oe, x_px, dt, 1.5, 0.05, 1.5);
    double v = (xf - s->xf_prev) / dt;
    s->vel_px = 0.5 * s->vel_px + 0.5 * v;
    s->xf_prev = xf;
    *xf_out = xf;
    *vel_px_out = s->vel_px;
}

/* ====================================================================
 *  Блок изменений звука: пересчёт волны, обрезка, отмена
 * ==================================================================== */
static int16_t *g_undo = NULL;
static int g_undo_len = 0, g_undo_valid = 0, g_undo_c1 = 0, g_undo_c2 = 0;

static void recompute_blocks(int len) {
    int peak_all = 0;
    for (int b = 0; b < len / IN_GRAIN; b++) {
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
}

/* Оставить только [lo, hi). Воспроизведение должно быть остановлено. Возвращает новую длину или -1. */
static int audio_crop(int lo, int hi, int c1, int c2) {
    int len = g_length;
    if (lo < 0) lo = 0;
    if (hi > len) hi = len;
    if (hi - lo < 256) return -1;
    if (g_undo) {
        memcpy(g_undo, g_audio, (size_t)len * sizeof(int16_t));
        g_undo_len = len; g_undo_c1 = c1; g_undo_c2 = c2; g_undo_valid = 1;
    }
    memmove(g_audio, g_audio + lo, (size_t)(hi - lo) * sizeof(int16_t));
    g_length = hi - lo;
    recompute_blocks(g_length);
    g_audio_ver++;
    return g_length;
}

/* Вернуть звук, каким он был до обрезки. Возвращает длину или -1. */
static int audio_undo(int *c1, int *c2) {
    if (!g_undo || !g_undo_valid) return -1;
    memcpy(g_audio, g_undo, (size_t)g_undo_len * sizeof(int16_t));
    g_length = g_undo_len;
    *c1 = g_undo_c1; *c2 = g_undo_c2;
    g_undo_valid = 0;
    recompute_blocks(g_length);
    g_audio_ver++;
    return g_length;
}

/* ====================================================================
 *  Нарезка на регионы по началам звуков (транзиентам)
 *  1) считаем "новизну": рост логарифма громкости и громкости разностного сигнала
 *     (разность соседних сэмплов подчёркивает резкие атаки);
 *  2) берём N-1 самых сильных пиков, не ближе 100 мс друг к другу;
 *  3) каждую границу уточняем до точки начала звука и сдвигаем к переходу через ноль.
 * ==================================================================== */
#define ON_HOP   128
#define ON_MAXF  (MAX_SAMPLES / ON_HOP + 4)
#define SL_MIN_LEN 1600                       /* минимальная длина региона: 100 мс */

static float g_nov[ON_MAXF];
static int g_nov_frames = 0;
static int g_nov_ver = -1;                    /* для какой версии звука посчитана новизна */

static void onset_analyze(const int16_t *a, int len) {
    static float lf[ON_MAXF], lh[ON_MAXF];
    int nf = len / ON_HOP;
    if (nf > ON_MAXF) nf = ON_MAXF;
    for (int f = 0; f < nf; f++) {
        const int16_t *x = a + f * ON_HOP;
        double e = 0.0, d = 0.0;
        int prev = f > 0 ? x[-1] : x[0];
        for (int i = 0; i < ON_HOP; i++) {
            int v = x[i];
            e += (double)v * v;
            int dd = v - prev;
            d += (double)dd * dd;
            prev = v;
        }
        lf[f] = logf(sqrtf((float)(e / ON_HOP)) + 30.0f);
        lh[f] = logf(sqrtf((float)(d / ON_HOP)) + 20.0f);
    }
    for (int f = 0; f < nf; f++) {
        int n = f < 3 ? f : 3;
        float rf = lf[f], rh = lh[f];
        if (n > 0) {
            rf = 0.0f; rh = 0.0f;
            for (int k = 1; k <= n; k++) { rf += lf[f - k]; rh += lh[f - k]; }
            rf /= (float)n; rh /= (float)n;
        }
        float nv = lf[f] - rf; if (nv < 0.0f) nv = 0.0f;
        float nh = lh[f] - rh; if (nh < 0.0f) nh = 0.0f;
        g_nov[f] = nv + 0.6f * nh;
    }
    g_nov_frames = nf;
}

typedef struct { int frame; float s; } OnsetCand;
static int cand_cmp(const void *x, const void *y) {
    float d = ((const OnsetCand *)y)->s - ((const OnsetCand *)x)->s;
    return d > 0.0f ? 1 : (d < 0.0f ? -1 : 0);
}
static int int_cmp(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return a < b ? -1 : (a > b);
}

/* Точное начало звука около кадра frame: первая точка, где сглаженная амплитуда достигает 15% пика */
static int refine_onset(const int16_t *a, int len, int frame) {
    int s0 = frame * ON_HOP - 3 * ON_HOP;
    int s1 = frame * ON_HOP + 3 * ON_HOP;
    if (s0 < 8) s0 = 8;
    if (s1 > len - 9) s1 = len - 9;
    if (s1 <= s0) return frame * ON_HOP;
    float pk = 1.0f;
    for (int i = s0; i < s1; i += 4) {
        float e = 0.0f;
        for (int k = -4; k < 4; k++) e += (float)abs(a[i + k]);
        e /= 8.0f;
        if (e > pk) pk = e;
    }
    int on = frame * ON_HOP;
    for (int i = s0; i < s1; i++) {
        float e = 0.0f;
        for (int k = -4; k < 4; k++) e += (float)abs(a[i + k]);
        e /= 8.0f;
        if (e > 0.15f * pk) { on = i; break; }
    }
    /* ближайший переход через ноль (снизу вверх), чтобы начало региона не щёлкало */
    int best = on, bestd = 1 << 30;
    for (int d = -40; d <= 40; d++) {
        int i = on + d;
        if (i < 1 || i >= len - 1) continue;
        if (a[i - 1] <= 0 && a[i] > 0 && abs(d) < bestd) { bestd = abs(d); best = i; }
    }
    return best;
}

/* Расставить n регионов (n = SL_MIN..SL_MAX). Заполняет g_sl_pos / g_sl_n. */
static void slices_pick(int n, const int16_t *a, int rlo, int rhi) {
    static OnsetCand cand[ON_MAXF];
    if (n < SL_MIN) n = SL_MIN;
    if (n > SL_MAX) n = SL_MAX;
    int nf = g_nov_frames, nc = 0;
    int f0 = rlo / ON_HOP + 3, f1 = rhi / ON_HOP - 3;
    if (f0 < 3) f0 = 3;
    if (f1 > nf - 3) f1 = nf - 3;
    float nmax = 0.0f;
    for (int f = f0; f < f1; f++) if (g_nov[f] > nmax) nmax = g_nov[f];
    float thr = 0.06f * nmax; if (thr < 0.10f) thr = 0.10f;
    int min_sp = SL_MIN_LEN, sp2 = (rhi - rlo) / (3 * n);
    if (sp2 > min_sp) min_sp = sp2 > 6400 ? 6400 : sp2;
    for (int f = f0; f < f1; f++) {
        float v = g_nov[f];
        if (v < thr) continue;
        int is_peak = 1;
        for (int k = -3; k <= 3 && is_peak; k++) if (k && g_nov[f + k] > v) is_peak = 0;
        if (is_peak) { cand[nc].frame = f; cand[nc].s = v; nc++; }
    }
    qsort(cand, (size_t)nc, sizeof(OnsetCand), cand_cmp);

    int pos[SL_MAX + 2], np = 0;
    for (int c = 0; c < nc && np < n - 1; c++) {
        int x = refine_onset(a, g_length, cand[c].frame);
        if (x < rlo + SL_MIN_LEN || x > rhi - SL_MIN_LEN) continue;
        int ok = 1;
        for (int j = 0; j < np; j++) if (abs(x - pos[j]) < min_sp) { ok = 0; break; }
        if (ok) pos[np++] = x;
    }
    qsort(pos, (size_t)np, sizeof(int), int_cmp);

    /* если звуков меньше, чем запрошено регионов, делим самые длинные регионы пополам */
    while (np < n - 1) {
        int bestgap = 0, bestat = -1, prev = rlo;
        for (int j = 0; j <= np; j++) {
            int cur = (j < np) ? pos[j] : rhi;
            if (cur - prev > bestgap) { bestgap = cur - prev; bestat = j; }
            prev = cur;
        }
        if (bestgap < 2 * SL_MIN_LEN) break;
        int start = (bestat == 0) ? rlo : pos[bestat - 1];
        int mid = start + bestgap / 2;
        for (int j = np; j > bestat; j--) pos[j] = pos[j - 1];
        pos[bestat] = mid;
        np++;
    }

    g_sl_pos[0] = rlo;
    for (int i = 0; i < np; i++) g_sl_pos[i + 1] = pos[i];
    g_sl_n = np + 1;
    g_sl_pos[g_sl_n] = rhi;
    if (g_sl_sel >= g_sl_n) g_sl_sel = g_sl_n - 1;
}

static int g_nov_ready = 0;                 /* новизна посчитана для g_nov_ver */
static int g_slices_valid_ver = -1;         /* для какой версии звука корректны g_sl_pos */

/* Подготовить нарезку для текущего звука. force = пересчитать регионы заново. */
static void slices_prepare(int req, int force) {
    int len = g_length;
    if (req < SL_MIN) req = SL_MIN;
    if (req > SL_MAX) req = SL_MAX;
    if (g_sl_lo < 0) g_sl_lo = 0;
    if (g_sl_hi > len) g_sl_hi = len;
    if (g_sl_hi - g_sl_lo < 4 * SL_MIN_LEN) { g_sl_lo = 0; g_sl_hi = len; }
    if (g_sl_hi - g_sl_lo < 4 * SL_MIN_LEN) { g_sl_n = 1; g_sl_pos[0] = g_sl_lo; g_sl_pos[1] = g_sl_hi; g_sl_sel = 0; g_sl_req = req; return; }
    if (g_slices_valid_ver != g_audio_ver) force = 1;     /* звук изменился - старые границы не годятся */
    g_sl_req = req;
    if (!force) return;                                   /* оставляем восстановленные/подвинутые границы */
    if (!g_nov_ready || g_nov_ver != g_audio_ver) {
        onset_analyze(g_audio, len);
        g_nov_ver = g_audio_ver;
        g_nov_ready = 1;
    }
    slices_pick(req, g_audio, g_sl_lo, g_sl_hi);
    g_slices_valid_ver = g_audio_ver;
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


/* Значки кнопок: 1 квадрат, 2 круг */


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
static inline float wv_at(int length, double p) {
    if (p < 0.0) p = 0.0;
    if (p > (double)(length - 1)) p = (double)(length - 1);
    int i = (int)p;
    float f = (float)(p - (double)i);
    int j = i + 1 < length ? i + 1 : i;
    return (float)g_audio[i] * (1.0f - f) + (float)g_audio[j] * f;
}

static void render_wave(vita2d_texture *tex, const Layout *L, int length, int live) {
    uint32_t *px = (uint32_t *)vita2d_texture_get_datap(tex);
    int stride = (int)vita2d_texture_get_stride(tex) / 4;
    memset(px, 0, (size_t)stride * WAVE_H * 4);

    int cy = WAVE_H / 2;
    int half = WAVE_H / 2 - 4;
    for (int sg = 0; sg < L->nseg; sg++)
        for (int x = 0; x < L->w[sg]; x++) px[cy * stride + L->x0[sg] + x] = RGBA8(190, 120, 50, 150);
    if (length <= 0) return;

    int peak = g_peak_all;
    if (peak < 3000) peak = 3000;
    float gain = 32768.0f / (float)peak;
    if (gain > 8.0f) gain = 8.0f;

    uint32_t col = live ? K_WAVE_REC : K_WAVE;
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
                /* близкий масштаб: непрерывная линия - значения на границах столбца берём интерполяцией,
                 * поэтому соседние столбцы всегда соединены */
                double p0 = (double)vs + (double)x * (double)vl / (double)w;
                double p1 = (double)vs + (double)(x + 1) * (double)vl / (double)w;
                float v0 = wv_at(length, p0), v1 = wv_at(length, p1);
                float fmn_ = v0 < v1 ? v0 : v1, fmx_ = v0 < v1 ? v1 : v0;
                int k0 = (int)ceil(p0), k1 = (int)floor(p1);
                if (k0 < 0) k0 = 0;
                if (k1 > length - 1) k1 = length - 1;
                for (int k = k0; k <= k1; k++) {
                    float v = (float)g_audio[k];
                    if (v < fmn_) fmn_ = v;
                    if (v > fmx_) fmx_ = v;
                }
                mn = (int)floorf(fmn_); mx = (int)ceilf(fmx_);
            }
            if (mx < mn) continue;

            float fmx = (float)mx * gain / 32768.0f; if (fmx > 1.0f) fmx = 1.0f;
            float fmn = (float)mn * gain / 32768.0f; if (fmn < -1.0f) fmn = -1.0f;
            int ytop = cy - (int)(fmx * half);
            int ybot = cy - (int)(fmn * half);
            if (!use_blocks && ybot - ytop < 2) { int m = (ytop + ybot) / 2; ytop = m - 1; ybot = m + 1; }
            if (ybot - ytop < 1) { ytop = cy - 1; ybot = cy + 1; }
            if (ytop < 0) ytop = 0;
            if (ybot > WAVE_H - 1) ybot = WAVE_H - 1;
            for (int y = ytop; y <= ybot; y++) px[y * stride + x0 + x] = col;
        }

        /* очень близко: отдельные сэмплы видны точками */
        if ((double)vl / (double)w < 0.25) {
            int k0 = (int)ceil((double)vs), k1 = (int)floor((double)vs + (double)vl);
            if (k0 < 0) k0 = 0;
            if (k1 > length - 1) k1 = length - 1;
            for (int k = k0; k <= k1; k++) {
                int xx = (int)(((double)k - (double)vs) * (double)w / (double)vl);
                float fv = (float)g_audio[k] * gain / 32768.0f;
                if (fv > 1.0f) fv = 1.0f;
                if (fv < -1.0f) fv = -1.0f;
                int yy = cy - (int)(fv * half);
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++) {
                        int X = xx + dx, Y = yy + dy;
                        if (X >= 0 && X < w && Y >= 0 && Y < WAVE_H) px[Y * stride + x0 + X] = RGBA8(255, 236, 190, 255);
                    }
            }
        }
    }
}


/* Курсоры, область между ними, головка воспроизведения */

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


/* ====================================================================
 *  ИНТЕРФЕЙС. Светлый ретро-стиль 70-х: кремовый пластик, оранжевые акценты, тёмный "экран".
 *  Панели, кнопки, пэды и ползунки рисуются ПРОЦЕССОРОМ в текстуры (картинка собирается только
 *  при изменениях), поэтому на кадр остаётся несколько вызовов отрисовки, а не сотни.
 * ==================================================================== */
#define SCR_W   960
#define SCR_H   544
#define TOP_H   50
#define CTL_TY  326
#define CTL_TH  168

/* палитра */

typedef struct { uint32_t *px; int w, h; } Canvas;

static inline float fclampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint32_t lerp_col(uint32_t a, uint32_t b, float t) {
    int ar = a & 255, ag = (a >> 8) & 255, ab = (a >> 16) & 255, aa = a >> 24;
    int br = b & 255, bg = (b >> 8) & 255, bb = (b >> 16) & 255, ba = b >> 24;
    return RGBA8((int)(ar + (br - ar) * t + 0.5f), (int)(ag + (bg - ag) * t + 0.5f),
                 (int)(ab + (bb - ab) * t + 0.5f), (int)(aa + (ba - aa) * t + 0.5f));
}

/* смешивание "поверх" (цвета не предумножены - так же смешивает и GPU) */
static inline void cv_put(Canvas *c, int x, int y, uint32_t src, float cov) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h || cov <= 0.0f) return;
    float sa = (float)(src >> 24) * (1.0f / 255.0f) * cov;
    if (sa <= 0.0f) return;
    uint32_t *d = &c->px[y * c->w + x];
    if (sa >= 0.998f) { *d = (src & 0x00FFFFFFu) | 0xFF000000u; return; }
    uint32_t dv = *d;
    float da = (float)(dv >> 24) * (1.0f / 255.0f);
    float oa = sa + da * (1.0f - sa);
    float k = da * (1.0f - sa);
    float r = ((float)(src & 255) * sa + (float)(dv & 255) * k) / oa;
    float g = ((float)((src >> 8) & 255) * sa + (float)((dv >> 8) & 255) * k) / oa;
    float b = ((float)((src >> 16) & 255) * sa + (float)((dv >> 16) & 255) * k) / oa;
    *d = RGBA8((int)(r + 0.5f), (int)(g + 0.5f), (int)(b + 0.5f), (int)(oa * 255.0f + 0.5f));
}

static void cv_clear(Canvas *c, uint32_t col) {
    for (int i = 0; i < c->w * c->h; i++) c->px[i] = col;
}

static void cv_rect(Canvas *c, int x, int y, int w, int h, uint32_t col) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) cv_put(c, i, j, col, 1.0f);
}

/* скруглённый прямоугольник через "расстояние до контура" - сглаженные края.
 * Середина (где покрытие заведомо 100%) закрашивается быстро, математика - только у краёв. */
static void cv_rrect_ex(Canvas *c, float x, float y, float w, float h, float r,
                        uint32_t ctop, uint32_t cbot, float stroke) {
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f - r, hh = h * 0.5f - r;
    int x0 = (int)floorf(x) - 1, x1 = (int)ceilf(x + w) + 1;
    int y0 = (int)floorf(y) - 1, y1 = (int)ceilf(y + h) + 1;
    const int fast = (r >= 1.0f) && (stroke <= 0.0f || r > stroke + 0.5f);
    for (int py = y0; py < y1; py++) {
        float t = fclampf(((float)py + 0.5f - y) / h, 0.0f, 1.0f);
        uint32_t col = (ctop == cbot) ? ctop : lerp_col(ctop, cbot, t);
        float qy = fabsf((float)py + 0.5f - cy) - hh;
        int sk0 = x1, sk1 = x1;                          /* быстрый участок [sk0, sk1) */
        if (fast && qy < 0.0f) {
            sk0 = (int)ceilf(cx - hw); sk1 = (int)floorf(cx + hw);
            if (sk0 < x0) sk0 = x0;
            if (sk1 > x1) sk1 = x1;
            if (sk1 <= sk0) { sk0 = sk1 = x1; }
            else if (stroke <= 0.0f && py >= 0 && py < c->h) {
                int a0 = sk0 < 0 ? 0 : sk0, a1 = sk1 > c->w ? c->w : sk1;
                if ((col >> 24) == 255) { uint32_t *row = &c->px[py * c->w]; for (int px = a0; px < a1; px++) row[px] = col; }
                else for (int px = a0; px < a1; px++) cv_put(c, px, py, col, 1.0f);
            }
        }
        for (int px = x0; px < x1; px++) {
            if (px >= sk0 && px < sk1) { px = sk1 - 1; continue; }
            float qx = fabsf((float)px + 0.5f - cx) - hw;
            float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
            float sd = (ox > 0.0f && oy > 0.0f ? sqrtf(ox * ox + oy * oy) : (ox > oy ? ox : oy))
                     + fminf(fmaxf(qx, qy), 0.0f) - r;
            float cov = fclampf(0.5f - sd, 0.0f, 1.0f);
            if (stroke > 0.0f) cov *= fclampf(sd + stroke + 0.5f, 0.0f, 1.0f);
            if (cov > 0.0f) cv_put(c, px, py, col, cov);
        }
    }
}
static void cv_rrect(Canvas *c, float x, float y, float w, float h, float r, uint32_t col) {
    cv_rrect_ex(c, x, y, w, h, r, col, col, 0.0f);
}
static void cv_rrect_grad(Canvas *c, float x, float y, float w, float h, float r, uint32_t top, uint32_t bot) {
    cv_rrect_ex(c, x, y, w, h, r, top, bot, 0.0f);
}
static void cv_rrect_stroke(Canvas *c, float x, float y, float w, float h, float r, float t, uint32_t col) {
    cv_rrect_ex(c, x, y, w, h, r, col, col, t);
}
static void cv_circle(Canvas *c, float cx, float cy, float r, uint32_t col) {
    cv_rrect_ex(c, cx - r, cy - r, 2 * r, 2 * r, r, col, col, 0.0f);
}
static void cv_ring(Canvas *c, float cx, float cy, float r, float t, uint32_t col) {
    cv_rrect_ex(c, cx - r, cy - r, 2 * r, 2 * r, r, col, col, t);
}

/* мягкая тень под деталью: три тонких контура вокруг (середину всё равно закрывает сама деталь) */
static void cv_shadow(Canvas *c, float x, float y, float w, float h, float r, float dy) {
    cv_rrect_stroke(c, x - 2, y + dy - 1, w + 4, h + 4, r + 2, 3.0f, RGBA8(60, 42, 24, 16));
    cv_rrect_stroke(c, x - 1, y + dy, w + 2, h + 2, r + 1, 3.0f, RGBA8(60, 42, 24, 24));
    cv_rrect_stroke(c, x, y + dy + 1, w, h, r, 3.0f, RGBA8(60, 42, 24, 34));
}

/* треугольник со сглаживанием (супервыборка 4x4) */
static void cv_tri(Canvas *c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col) {
    int bx0 = (int)floorf(fminf(x0, fminf(x1, x2))) - 1, bx1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2))) + 1;
    int by0 = (int)floorf(fminf(y0, fminf(y1, y2))) - 1, by1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2))) + 1;
    float den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (fabsf(den) < 1e-6f) return;
    for (int py = by0; py < by1; py++)
        for (int px = bx0; px < bx1; px++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float fx = (float)px + (sx + 0.5f) * 0.25f, fy = (float)py + (sy + 0.5f) * 0.25f;
                    float a = ((y1 - y2) * (fx - x2) + (x2 - x1) * (fy - y2)) / den;
                    float b = ((y2 - y0) * (fx - x2) + (x0 - x2) * (fy - y2)) / den;
                    if (a >= 0.0f && b >= 0.0f && 1.0f - a - b >= 0.0f) hit++;
                }
            if (hit) cv_put(c, px, py, col, (float)hit / 16.0f);
        }
}

/* лёгкая "фактура пластика": случайный шум по яркости */
static void cv_noise(Canvas *c, int x, int y, int w, int h, int amp) {
    uint32_t seed = 12345u;
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) {
            seed = seed * 1664525u + 1013904223u;
            if (i < 0 || j < 0 || i >= c->w || j >= c->h) continue;
            int n = (int)((seed >> 24) % (uint32_t)(2 * amp + 1)) - amp;
            uint32_t *d = &c->px[j * c->w + i];
            int r = (int)(*d & 255) + n, g = (int)((*d >> 8) & 255) + n, b = (int)((*d >> 16) & 255) + n;
            *d = RGBA8(r < 0 ? 0 : (r > 255 ? 255 : r), g < 0 ? 0 : (g > 255 ? 255 : g),
                       b < 0 ? 0 : (b > 255 ? 255 : b), (int)(*d >> 24));
        }
}

/* ---------- значки (рисуются фигурами) ---------- */
enum { IC_REC, IC_PLAY, IC_PAUSE, IC_STOP, IC_LOOP, IC_FX, IC_CROP, IC_UNDO, IC_SAVE, IC_ZOUT, IC_ZIN, IC_PLUS, IC_MINUS };

static void cv_icon(Canvas *c, int kind, float cx, float cy, uint32_t col) {
    switch (kind) {
    case IC_REC:   cv_circle(c, cx, cy, 8.5f, col); break;
    case IC_PLAY:  cv_tri(c, cx - 6, cy - 9.5f, cx - 6, cy + 9.5f, cx + 10, cy, col); break;
    case IC_PAUSE: cv_rrect(c, cx - 8, cy - 9, 6, 18, 1.5f, col); cv_rrect(c, cx + 2, cy - 9, 6, 18, 1.5f, col); break;
    case IC_STOP:  cv_rrect(c, cx - 8, cy - 8, 16, 16, 2.5f, col); break;
    case IC_LOOP:                                      /* значок "повтор": рамка со стрелками */
        cv_rrect_stroke(c, cx - 11, cy - 7, 22, 15, 6.0f, 2.4f, col);
        cv_tri(c, cx + 2, cy - 12.5f, cx + 2, cy - 1.5f, cx + 10, cy - 7, col);
        cv_tri(c, cx - 2, cy + 13.5f, cx - 2, cy + 2.5f, cx - 10, cy + 8, col);
        break;
    case IC_FX:
        for (int i = 0; i < 3; i++) {
            float yy = cy - 7 + i * 7;
            cv_rrect(c, cx - 10, yy - 1, 20, 2.2f, 1.0f, col);
            cv_circle(c, cx + (i == 0 ? -4 : (i == 1 ? 4 : -1)), yy, 3.2f, col);
        }
        break;
    case IC_CROP:
        cv_rect(c, (int)cx - 6, (int)cy - 11, 3, 17, col); cv_rect(c, (int)cx - 6, (int)cy + 3, 17, 3, col);
        cv_rect(c, (int)cx + 3, (int)cy - 6, 3, 17, col);  cv_rect(c, (int)cx - 11, (int)cy - 6, 17, 3, col);
        break;
    case IC_UNDO:                                      /* "назад": кольцо со стрелкой */
        cv_ring(c, cx + 1, cy + 1, 8.0f, 2.8f, col);
        cv_tri(c, cx - 11, cy - 4, cx - 1, cy - 5, cx - 6, cy + 4, col);
        break;
    case IC_SAVE:
        cv_rect(c, (int)cx - 2, (int)cy - 10, 4, 10, col);
        cv_tri(c, cx - 9, cy - 1, cx + 9, cy - 1, cx, cy + 8, col);
        cv_rect(c, (int)cx - 11, (int)cy + 8, 22, 3, col);
        break;
    case IC_ZOUT: case IC_ZIN:
        cv_ring(c, cx - 2, cy - 2, 8.0f, 2.6f, col);
        for (int k = 0; k < 6; k++) cv_circle(c, cx + 4.5f + k * 1.4f, cy + 4.5f + k * 1.4f, 2.0f, col);
        cv_rect(c, (int)cx - 7, (int)cy - 3, 9, 2, col);
        if (kind == IC_ZIN) cv_rect(c, (int)cx - 4, (int)cy - 6, 2, 9, col);
        break;
    case IC_PLUS:  cv_rect(c, (int)cx - 10, (int)cy - 2, 20, 4, col); cv_rect(c, (int)cx - 2, (int)cy - 10, 4, 20, col); break;
    case IC_MINUS: cv_rect(c, (int)cx - 10, (int)cy - 2, 20, 4, col); break;
    }
}

/* ---------- кнопка: 0 обычная, 1 оранжевая (включена), 2 красная, 3 зелёная ---------- */
static void cv_button(Canvas *c, int x, int y, int w, int h, int style, int pressed) {
    uint32_t top, bot, edge;
    switch (style) {
    case 1:  top = K_ORANGE_HI; bot = K_ORANGE;   edge = K_ORANGE_LO; break;
    case 2:  top = RGBA8(240, 98, 82, 255); bot = K_RED; edge = RGBA8(150, 40, 30, 255); break;
    case 3:  top = RGBA8(120, 206, 130, 255); bot = K_GREEN; edge = RGBA8(48, 120, 62, 255); break;
    default: top = K_CREAM_HI; bot = K_BEIGE; edge = K_EDGE; break;
    }
    int off = pressed ? 2 : 0;
    if (!pressed) cv_shadow(c, (float)x, (float)y, (float)w, (float)h, 11.0f, 3.0f);
    if (pressed) { uint32_t t = top; top = bot; bot = t; }
    cv_rrect_grad(c, (float)x, (float)(y + off), (float)w, (float)h, 11.0f, top, bot);
    cv_rrect_stroke(c, (float)x, (float)(y + off), (float)w, (float)h, 11.0f, 1.3f, edge);
    if (!pressed) cv_rrect(c, (float)(x + 8), (float)(y + 2), (float)(w - 16), 1.6f, 0.8f, RGBA8(255, 255, 255, style ? 70 : 150));
}

/* ====================================================================
 *  Раскладка кнопок (одна и та же для рисования и для нажатий пальцем)
 * ==================================================================== */
enum {
    B_NONE = -1,
    B_REC, B_PLAY, B_STOP, B_LOOP, B_FX,
    B_CROP, B_UNDO, B_SAVE, B_ZOUT, B_ZIN,
    B_TAB_EDIT, B_TAB_SLICE, B_SL_MINUS, B_SL_PLUS, B_FX_CLOSE,
    B_PAD0 = 100
};

#define PADS_X 36
#define PADS_Y 340
#define PADS_W 724
#define PADS_H 138

static void pad_layout(int n, int *cols, int *rows) {
    int r = n <= 4 ? 1 : (n <= 16 ? 2 : 4);
    *rows = r;
    *cols = (n + r - 1) / r;
}

static void pad_geom(int i, int n, int *x, int *y, int *w, int *h) {
    int cols, rows;
    pad_layout(n, &cols, &rows);
    int gap = 8;
    int pw = (PADS_W - (cols - 1) * gap) / cols;
    int ph = (PADS_H - (rows - 1) * gap) / rows;
    *x = PADS_X + (i % cols) * (pw + gap);
    *y = PADS_Y + (i / cols) * (ph + gap);
    *w = pw; *h = ph;
}

/* возвращает 1, если кнопка id существует в текущем режиме/странице */
static int btn_geom(int id, int mode, int fx_page, int sl_n, int *x, int *y, int *w, int *h) {
    if (id == B_TAB_EDIT)  { *x = 384; *y = 8; *w = 92; *h = 34; return 1; }
    if (id == B_TAB_SLICE) { *x = 484; *y = 8; *w = 92; *h = 34; return 1; }
    if (fx_page) {
        if (id == B_FX_CLOSE) { *x = 848; *y = 334; *w = 76; *h = 22; return 1; }
        return 0;
    }
    if (mode == 0) {
        if (id >= B_REC && id <= B_ZIN) {
            int k = id - B_REC;
            *x = 36 + (k % 5) * 180; *y = 340 + (k / 5) * 74; *w = 168; *h = 64;
            return 1;
        }
        return 0;
    }
    /* режим SLICE */
    if (id >= B_PAD0 && id < B_PAD0 + sl_n) { pad_geom(id - B_PAD0, sl_n, x, y, w, h); return 1; }
    switch (id) {
    case B_SL_MINUS: *x = 776; *y = 362; *w = 70; *h = 32; return 1;
    case B_SL_PLUS:  *x = 854; *y = 362; *w = 70; *h = 32; return 1;
    case B_LOOP:     *x = 776; *y = 410; *w = 70; *h = 68; return 1;
    case B_FX:       *x = 854; *y = 410; *w = 70; *h = 68; return 1;
    }
    return 0;
}

static int hit_test(int tx, int ty, int mode, int fx_page, int sl_n) {
    static const int order_edit[] = { B_TAB_EDIT, B_TAB_SLICE, B_REC, B_PLAY, B_STOP, B_LOOP, B_FX,
                                      B_CROP, B_UNDO, B_SAVE, B_ZOUT, B_ZIN };
    static const int order_slice[] = { B_TAB_EDIT, B_TAB_SLICE, B_SL_MINUS, B_SL_PLUS, B_LOOP, B_FX };
    static const int order_fx[] = { B_TAB_EDIT, B_TAB_SLICE, B_FX_CLOSE };
    const int *ord = fx_page ? order_fx : (mode == 0 ? order_edit : order_slice);
    int n = fx_page ? 3 : (mode == 0 ? 12 : 6);
    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        if (btn_geom(ord[i], mode, fx_page, sl_n, &x, &y, &w, &h) && tx >= x && tx < x + w && ty >= y && ty < y + h)
            return ord[i];
    }
    if (!fx_page && mode == 1) {
        for (int i = 0; i < sl_n; i++) {
            int x, y, w, h;
            pad_geom(i, sl_n, &x, &y, &w, &h);
            if (tx >= x && tx < x + w && ty >= y && ty < y + h) return B_PAD0 + i;
        }
    }
    return B_NONE;
}

/* ====================================================================
 *  Данные кадра для рисования
 * ==================================================================== */
typedef struct {
    int mode, fx_page, state, scrubbing, loop;
    int length, playhead, live;
    int split, active;
    double c1, c2;
    int menu_sel;
    int level_pct, clip, confirm;
    char msg[160];
    int sl_n, sl_sel, sl_flash, sl_playing;
    int btn_down, can_undo;
    int crop_armed, legend;
    double scrub_pos, scrub_speed;
    unsigned frame;
    Layout lay;
    int view_len;
    vita2d_texture *wave_tex;
} UiState;

/* ====================================================================
 *  Статичный фон (рисуется один раз)
 * ==================================================================== */
static void skin_render_static(Canvas *c) {
    /* основа: кремовый пластик с лёгким вертикальным градиентом и шумом */
    for (int y = 0; y < c->h; y++) {
        uint32_t col = lerp_col(K_CREAM_HI, K_CREAM, (float)y / (float)c->h);
        for (int x = 0; x < c->w; x++) c->px[y * c->w + x] = col;
    }
    cv_noise(c, 0, 0, c->w, c->h, 3);

    /* верхняя панель */
    cv_rect(c, 0, 0, SCR_W, TOP_H, K_BEIGE);
    for (int y = 0; y < TOP_H; y++) cv_rect(c, 0, y, SCR_W, 1, lerp_col(K_CREAM_HI, K_BEIGE, (float)y / (float)TOP_H));
    cv_rect(c, 0, TOP_H - 1, SCR_W, 1, K_EDGE);
    cv_rect(c, 0, TOP_H, SCR_W, 2, RGBA8(60, 42, 24, 40));
    /* фирменные полосы 70-х */
    cv_rect(c, 0, 0, SCR_W, 3, K_ORANGE);
    cv_rect(c, 0, 3, SCR_W, 3, K_YELLOW);
    cv_rect(c, 0, 6, SCR_W, 3, K_BROWN);
    /* логотип: оранжевая плашка с "волной" */
    cv_shadow(c, 18, 13, 32, 30, 7, 2);
    cv_rrect_grad(c, 18, 13, 32, 30, 7, K_ORANGE_HI, K_ORANGE);
    cv_rrect_stroke(c, 18, 13, 32, 30, 7, 1.2f, K_ORANGE_LO);
    static const int bh[] = { 6, 12, 20, 9, 15, 5 };
    for (int i = 0; i < 6; i++) cv_rrect(c, 23.0f + i * 4.2f, 28.0f - bh[i] * 0.5f, 2.4f, (float)bh[i], 1.2f, K_CREAM_HI);

    /* экран (LCD) в тёмной рамке */
    cv_shadow(c, 16, 52, 928, 276, 16, 4);
    cv_rrect_grad(c, 16, 50, 928, 280, 16, RGBA8(98, 80, 64, 255), RGBA8(58, 46, 36, 255));
    cv_rrect_stroke(c, 16, 50, 928, 280, 16, 1.2f, RGBA8(40, 30, 22, 255));
    cv_rrect(c, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 10, K_LCD);
    cv_rrect_stroke(c, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 10, 1.2f, K_LCD_HI);
    /* окно волны */
    cv_rrect(c, WAVE_X - 6, WAVE_Y - 6, WAVE_W + 12, WAVE_H + 12, 6, RGBA8(18, 16, 14, 255));
    for (int i = 1; i < 4; i++) {                              /* сетка */
        int yy = WAVE_Y + i * WAVE_H / 4;
        for (int x = WAVE_X; x < WAVE_X + WAVE_W; x += 8) cv_rect(c, x, yy, 4, 1, RGBA8(110, 80, 40, 60));
    }
    for (int i = 1; i < 11; i++) cv_rect(c, WAVE_X + i * WAVE_W / 11, WAVE_Y, 1, WAVE_H, RGBA8(110, 80, 40, 36));
    /* строки развёртки и блик стекла */
    for (int y = PANEL_Y + 2; y < PANEL_Y + PANEL_H - 2; y += 3) cv_rect(c, PANEL_X + 2, y, PANEL_W - 4, 1, RGBA8(0, 0, 0, 34));
    cv_rrect(c, PANEL_X + 4, PANEL_Y + 3, PANEL_W - 8, 26, 8, RGBA8(255, 235, 200, 9));

    /* панель управления */
    cv_shadow(c, 16, 326, 928, 164, 14, 3);
    cv_rrect_grad(c, 16, 326, 928, 164, 14, K_CREAM_HI, K_BEIGE);
    cv_rrect_stroke(c, 16, 326, 928, 164, 14, 1.3f, K_EDGE);
    cv_rrect(c, 24, 328, 912, 1.6f, 0.8f, RGBA8(255, 255, 255, 140));
    /* винты по углам */
    static const int sx[] = { 30, 930, 30, 930 }, sy[] = { 338, 338, 478, 478 };
    for (int i = 0; i < 4; i++) {
        cv_circle(c, (float)sx[i], (float)sy[i], 4.2f, K_EDGE);
        cv_circle(c, (float)sx[i] - 0.5f, (float)sy[i] - 0.5f, 3.2f, K_BEIGE_LO);
        cv_rect(c, sx[i] - 2, sy[i], 4, 1, K_EDGE);
    }

    /* подсказки: врезанная планка */
    cv_rrect_grad(c, 16, 496, 928, 44, 10, K_BEIGE_LO, K_BEIGE);
    cv_rrect_stroke(c, 16, 496, 928, 44, 10, 1.2f, K_EDGE);
    for (int r = 0; r < 3; r++)                                /* решётка динамика */
        for (int q = 0; q < 6; q++) cv_circle(c, 884.0f + q * 9.0f, 509.0f + r * 10.0f, 2.2f, RGBA8(110, 94, 72, 255));
}

/* ====================================================================
 *  Верхняя панель: вкладки, состояние, лампа "петля"
 * ==================================================================== */
static void top_render(Canvas *c, const UiState *u) {
    cv_clear(c, 0);
    for (int t = 0; t < 2; t++) {
        int x, y, w, h;
        btn_geom(t == 0 ? B_TAB_EDIT : B_TAB_SLICE, 0, 0, 0, &x, &y, &w, &h);
        int sel = (u->mode == t);
        int pressed = (u->btn_down == (t == 0 ? B_TAB_EDIT : B_TAB_SLICE));
        if (sel) {
            cv_rrect_grad(c, (float)x, (float)y, (float)w, (float)h, 10, K_ORANGE_HI, K_ORANGE);
            cv_rrect_stroke(c, (float)x, (float)y, (float)w, (float)h, 10, 1.3f, K_ORANGE_LO);
        } else {
            if (!pressed) cv_shadow(c, (float)x, (float)y, (float)w, (float)h, 10, 2);
            cv_rrect_grad(c, (float)x, (float)y, (float)w, (float)h, 10, K_CREAM_HI, K_BEIGE);
            cv_rrect_stroke(c, (float)x, (float)y, (float)w, (float)h, 10, 1.3f, K_EDGE);
        }
    }
    /* лампа петли */
    cv_rrect(c, 596, 10, 100, 30, 8, K_LCD);
    cv_rrect_stroke(c, 596, 10, 100, 30, 8, 1.2f, K_LCD_HI);
    cv_circle(c, 612, 25, 6.0f, u->loop ? K_GREEN : RGBA8(60, 52, 44, 255));
    if (u->loop) cv_circle(c, 612, 25, 10.0f, RGBA8(90, 220, 110, 40));
    /* состояние */
    cv_rrect(c, 710, 10, 226, 30, 8, K_LCD);
    cv_rrect_stroke(c, 710, 10, 226, 30, 8, 1.2f, K_LCD_HI);
    uint32_t lamp = RGBA8(60, 52, 44, 255);
    if (u->state == ST_RECORDING) lamp = ((u->frame / 24) % 2 == 0) ? K_RED : RGBA8(120, 40, 34, 255);
    else if (u->state == ST_PLAYING) lamp = K_GREEN;
    cv_circle(c, 728, 25, 6.0f, lamp);
}

/* ====================================================================
 *  Органы управления: каждая деталь перерисовывается ТОЛЬКО когда изменилось её состояние
 *  (одна кнопка - доли миллисекунды, а не вся страница)
 * ==================================================================== */
#define W_EDIT0   0            /* 0..9   кнопки EDIT */
#define W_PAD0    10           /* 10..41 пэды */
#define W_SIDE0   42           /* 42..47 правая колонка SLICE */
#define W_FXROW0  48           /* 48..53 строки эффектов */
#define W_FXCLOSE 54
#define W_COUNT   55
static int g_wcode[W_COUNT];
static int g_page_key = -1;

static void cv_clear_rect(Canvas *c, int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= c->h) continue;
        for (int i = x; i < x + w; i++) if (i >= 0 && i < c->w) c->px[j * c->w + i] = 0;
    }
}
/* очистить место детали вместе с её тенью */
static void cv_clear_part(Canvas *c, int x, int y, int w, int h) { cv_clear_rect(c, x - 6, y - 6, w + 12, h + 12); }

static void draw_fx_row(Canvas *c, const UiState *u, int i, int cy0) {
    int y0 = MENU_Y + 3 + i * MENU_ROW - cy0;
    int sel = (i == u->menu_sel);
    if (sel) {
        cv_rrect(c, 28, (float)y0, 904, MENU_ROW - 2, 7, RGBA8(233, 106, 38, 36));
        cv_rrect_stroke(c, 28, (float)y0, 904, MENU_ROW - 2, 7, 1.4f, K_ORANGE);
    }
    float f = (float)g_fx[i] / (float)(FX[i].n - 1);
    float fd = (float)FX[i].def / (float)(FX[i].n - 1);
    int kx = SL_X + (int)(f * SL_W);
    cv_rrect(c, (float)SL_X, (float)(y0 + 8), (float)SL_W, 8, 4, K_BEIGE_LO);
    cv_rrect_stroke(c, (float)SL_X, (float)(y0 + 8), (float)SL_W, 8, 4, 1.0f, K_EDGE);
    if (kx > SL_X + 2) cv_rrect_grad(c, (float)SL_X, (float)(y0 + 8), (float)(kx - SL_X), 8, 4, sel ? K_ORANGE_HI : K_BROWN_LO, sel ? K_ORANGE : K_BROWN);
    cv_rect(c, SL_X + (int)(fd * SL_W) - 1, y0 + 3, 2, 18, K_BROWN_LO);          /* метка "по умолчанию" */
    cv_shadow(c, (float)(kx - 9), (float)(y0 + 3), 18, 18, 9, 2);
    cv_circle(c, (float)kx, (float)(y0 + 12), 9.0f, K_CREAM_HI);
    cv_ring(c, (float)kx, (float)(y0 + 12), 9.0f, 1.3f, K_EDGE);
    cv_circle(c, (float)kx, (float)(y0 + 12), 3.4f, sel ? K_ORANGE : K_BROWN_LO);
}

static int edit_style(const UiState *u, int id) {
    int style = 0;
    if (id == B_REC && u->state == ST_RECORDING) style = 2;
    if (id == B_PLAY && u->state == ST_PLAYING) style = 3;
    if (id == B_LOOP && u->loop) style = 1;
    if (id == B_FX && u->fx_page) style = 1;
    if (id == B_CROP && u->crop_armed) style = 1;
    return style;
}

static void draw_edit_button(Canvas *c, const UiState *u, int id, int cy0) {
    static const int icons[10] = { IC_REC, IC_PLAY, IC_STOP, IC_LOOP, IC_FX, IC_CROP, IC_UNDO, IC_SAVE, IC_ZOUT, IC_ZIN };
    int x, y, w, h;
    btn_geom(id, 0, 0, 0, &x, &y, &w, &h);
    y -= cy0;
    int style = edit_style(u, id);
    int dim = (id == B_UNDO && !u->can_undo);
    int pressed = (u->btn_down == id);
    cv_button(c, x, y, w, h, style, pressed);
    uint32_t ic = style ? K_CREAM_HI : (dim ? K_BROWN_LO : K_BROWN);
    int kind = icons[id - B_REC];
    if (id == B_PLAY && u->state == ST_PLAYING) kind = IC_PAUSE;
    cv_icon(c, kind, (float)(x + 32), (float)(y + h / 2 + (pressed ? 2 : 0)), ic);
}

static void draw_pad(Canvas *c, const UiState *u, int i, int cy0) {
    int x, y, w, h;
    pad_geom(i, u->sl_n, &x, &y, &w, &h);
    y -= cy0;
    int sel = (i == u->sl_sel), playing = (i == u->sl_playing);
    int flash = sel && u->sl_flash > 0;
    int pressed = (u->btn_down == B_PAD0 + i) || flash;
    cv_button(c, x, y, w, h, playing ? 3 : (sel ? 1 : 0), pressed);
    cv_circle(c, (float)(x + 12), (float)(y + 12 + (pressed ? 2 : 0)), 4.2f,
              playing ? RGBA8(200, 255, 200, 255) : (sel ? K_YELLOW : RGBA8(150, 134, 108, 255)));
}

static const int g_side_ids[4] = { B_SL_MINUS, B_SL_PLUS, B_LOOP, B_FX };
static const int g_side_ics[4] = { IC_MINUS, IC_PLUS, IC_LOOP, IC_FX };

static int side_style(const UiState *u, int k) {
    if (g_side_ids[k] == B_LOOP && u->loop) return 1;
    if (g_side_ids[k] == B_FX && u->fx_page) return 1;
    return 0;
}

static void draw_side(Canvas *c, const UiState *u, int k, int cy0) {
    int x, y, w, h;
    btn_geom(g_side_ids[k], 1, 0, u->sl_n, &x, &y, &w, &h);
    y -= cy0;
    int style = side_style(u, k);
    int pressed = (u->btn_down == g_side_ids[k]);
    cv_button(c, x, y, w, h, style, pressed);
    cv_icon(c, g_side_ics[k], (float)(x + w / 2), (float)(y + h / 2 + (pressed ? 2 : 0)), style ? K_CREAM_HI : K_BROWN);
}

/* Обновляет холст органов управления. Возвращает 1, если что-то изменилось. */
static int ctl_update(Canvas *c, const UiState *u) {
    int page = u->fx_page ? 2 : (u->mode == 0 ? 0 : 1);
    int key = page * 100 + (page == 1 ? u->sl_n : 0);
    int changed = 0;
    if (key != g_page_key) {
        cv_clear(c, 0);
        for (int i = 0; i < W_COUNT; i++) g_wcode[i] = -1;
        g_page_key = key;
        changed = 1;
    }
    if (page == 2) {
        for (int i = 0; i < N_FX; i++) {
            int code = (g_fx[i] << 1 | (i == u->menu_sel)) + 1;
            if (code != g_wcode[W_FXROW0 + i]) {
                cv_clear_rect(c, 24, MENU_Y + 3 + i * MENU_ROW - CTL_TY - 3, 912, MENU_ROW + 4);
                draw_fx_row(c, u, i, CTL_TY);
                g_wcode[W_FXROW0 + i] = code;
                changed = 1;
            }
        }
        int code = 1 + (u->btn_down == B_FX_CLOSE);
        if (code != g_wcode[W_FXCLOSE]) {
            int x, y, w, h;
            btn_geom(B_FX_CLOSE, 0, 1, 0, &x, &y, &w, &h);
            cv_clear_part(c, x, y - CTL_TY, w, h);
            cv_button(c, x, y - CTL_TY, w, h, 1, u->btn_down == B_FX_CLOSE);
            g_wcode[W_FXCLOSE] = code;
            changed = 1;
        }
    } else if (page == 0) {
        for (int id = B_REC; id <= B_ZIN; id++) {
            int code = 1 + edit_style(u, id) + 4 * (u->btn_down == id) + 8 * (id == B_UNDO && !u->can_undo)
                         + 16 * (id == B_PLAY && u->state == ST_PLAYING);
            if (code != g_wcode[W_EDIT0 + id - B_REC]) {
                int x, y, w, h;
                btn_geom(id, 0, 0, 0, &x, &y, &w, &h);
                cv_clear_part(c, x, y - CTL_TY, w, h);
                draw_edit_button(c, u, id, CTL_TY);
                g_wcode[W_EDIT0 + id - B_REC] = code;
                changed = 1;
            }
        }
    } else {
        for (int i = 0; i < u->sl_n; i++) {
            int sel = (i == u->sl_sel), playing = (i == u->sl_playing);
            int code = 1 + (playing ? 3 : (sel ? 1 : 0)) + 4 * ((u->btn_down == B_PAD0 + i) || (sel && u->sl_flash > 0));
            if (code != g_wcode[W_PAD0 + i]) {
                int x, y, w, h;
                pad_geom(i, u->sl_n, &x, &y, &w, &h);
                cv_clear_part(c, x, y - CTL_TY, w, h);
                draw_pad(c, u, i, CTL_TY);
                g_wcode[W_PAD0 + i] = code;
                changed = 1;
            }
        }
        for (int k = 0; k < 4; k++) {
            int code = 1 + side_style(u, k) + 4 * (u->btn_down == g_side_ids[k]);
            if (code != g_wcode[W_SIDE0 + k]) {
                int x, y, w, h;
                btn_geom(g_side_ids[k], 1, 0, u->sl_n, &x, &y, &w, &h);
                cv_clear_part(c, x, y - CTL_TY, w, h);
                draw_side(c, u, k, CTL_TY);
                g_wcode[W_SIDE0 + k] = code;
                changed = 1;
            }
        }
    }
    return changed;
}

/* ====================================================================
 *  Текстуры интерфейса и их обновление
 * ==================================================================== */
static vita2d_texture *g_tex_bg = NULL, *g_tex_top[2], *g_tex_ctl[3];
static Canvas g_cv_bg, g_cv_top, g_cv_ctl;
static uint32_t g_sig_top = 0, g_sig_ctl = 0;
static int g_top_cur = -1, g_ctl_cur = -1, g_top_next = 0, g_ctl_next = 0;

static void upload_canvas(vita2d_texture *tex, const Canvas *c) {
    if (!tex) return;
    uint32_t *dst = (uint32_t *)vita2d_texture_get_datap(tex);
    int stride = (int)vita2d_texture_get_stride(tex) / 4;
    for (int y = 0; y < c->h; y++) memcpy(dst + (size_t)y * stride, c->px + (size_t)y * c->w, (size_t)c->w * 4);
}

static int ui_init(void) {
    g_cv_bg.w = SCR_W; g_cv_bg.h = SCR_H; g_cv_bg.px = (uint32_t *)malloc((size_t)SCR_W * SCR_H * 4);
    g_cv_top.w = SCR_W; g_cv_top.h = TOP_H; g_cv_top.px = (uint32_t *)malloc((size_t)SCR_W * TOP_H * 4);
    g_cv_ctl.w = SCR_W; g_cv_ctl.h = CTL_TH; g_cv_ctl.px = (uint32_t *)malloc((size_t)SCR_W * CTL_TH * 4);
    if (!g_cv_bg.px || !g_cv_top.px || !g_cv_ctl.px) return 0;
    g_tex_bg = vita2d_create_empty_texture(SCR_W, SCR_H);
    for (int i = 0; i < 2; i++) g_tex_top[i] = vita2d_create_empty_texture(SCR_W, TOP_H);
    for (int i = 0; i < 3; i++) g_tex_ctl[i] = vita2d_create_empty_texture(SCR_W, CTL_TH);
    skin_render_static(&g_cv_bg);
    upload_canvas(g_tex_bg, &g_cv_bg);
    g_sig_top = g_sig_ctl = 0xFFFFFFFFu;
    g_top_cur = g_ctl_cur = -1;
    g_page_key = -1;
    return 1;
}

static void ui_free_textures(void) {
    if (g_tex_bg) { vita2d_free_texture(g_tex_bg); g_tex_bg = NULL; }
    for (int i = 0; i < 2; i++) if (g_tex_top[i]) { vita2d_free_texture(g_tex_top[i]); g_tex_top[i] = NULL; }
    for (int i = 0; i < 3; i++) if (g_tex_ctl[i]) { vita2d_free_texture(g_tex_ctl[i]); g_tex_ctl[i] = NULL; }
}

/* после "сна" видеопамять могла обнулиться - создаём текстуры заново */
static void ui_recreate(void) {
    ui_free_textures();
    g_tex_bg = vita2d_create_empty_texture(SCR_W, SCR_H);
    for (int i = 0; i < 2; i++) g_tex_top[i] = vita2d_create_empty_texture(SCR_W, TOP_H);
    for (int i = 0; i < 3; i++) g_tex_ctl[i] = vita2d_create_empty_texture(SCR_W, CTL_TH);
    upload_canvas(g_tex_bg, &g_cv_bg);
    g_sig_top = g_sig_ctl = 0xFFFFFFFFu;
    g_top_cur = g_ctl_cur = -1;
    g_page_key = -1;
}

static inline uint32_t sig_add(uint32_t h, uint32_t v) { h ^= v; return h * 16777619u; }

/* вызывать ДО vita2d_start_drawing: процессор пишет в текстуры только когда GPU их не читает */
static void ui_update_textures(const UiState *u) {
    uint32_t st = 2166136261u;
    st = sig_add(st, (uint32_t)u->mode); st = sig_add(st, (uint32_t)u->state);
    st = sig_add(st, (uint32_t)u->loop); st = sig_add(st, (uint32_t)u->btn_down + 7u);
    if (u->state == ST_RECORDING) st = sig_add(st, (u->frame / 24) & 1u);
    if (st != g_sig_top) {
        g_sig_top = st;
        top_render(&g_cv_top, u);
        g_top_cur = g_top_next; g_top_next = (g_top_next + 1) % 2;
        upload_canvas(g_tex_top[g_top_cur], &g_cv_top);
    }
    if (ctl_update(&g_cv_ctl, u) || g_ctl_cur < 0) {
        g_ctl_cur = g_ctl_next; g_ctl_next = (g_ctl_next + 1) % 3;
        upload_canvas(g_tex_ctl[g_ctl_cur], &g_cv_ctl);
    }
}

/* ====================================================================
 *  Текст и мелочи поверх текстур
 * ==================================================================== */
static int tw(const char *s, float sc) { return (int)((float)strlen(s) * 9.6f * sc); }
static void text_c(int cx, int y, unsigned int col, float sc, const char *s) { text(cx - tw(s, sc) / 2, y, col, sc, s); }

/* курсоры, область между ними, головка воспроизведения */
static void draw_overlays(const UiState *u) {
    const Layout *L = &u->lay;
    double lo = u->c1 < u->c2 ? u->c1 : u->c2, hi = u->c1 < u->c2 ? u->c2 : u->c1;
    for (int sg = 0; sg < L->nseg; sg++) {
        int xa = sample_to_x(L, sg, lo), xb = sample_to_x(L, sg, hi);
        int segx = WAVE_X + L->x0[sg], segw = L->w[sg];
        double vs = (double)L->vs[sg], ve = vs + (double)L->vl[sg];
        if (hi > vs && lo < ve) {
            int sx = xa == -1000000 ? segx : xa;
            int ex = xb == -1000000 ? segx + segw : xb;
            rect(sx, WAVE_Y, ex - sx, WAVE_H, u->loop ? RGBA8(120, 230, 140, 40) : RGBA8(255, 200, 110, 34));
        }
        for (int k = 0; k < 2; k++) {
            double cs = (k == 0) ? u->c1 : u->c2;
            int x = sample_to_x(L, sg, cs);
            if (x == -1000000) continue;
            unsigned int col = (k == 0) ? RGBA8(255, 224, 90, 255) : RGBA8(255, 116, 140, 255);
            int th = (u->active == k) ? 3 : 2;
            rect(x - th / 2, WAVE_Y - 5, th, WAVE_H + 10, col);
            rect(x - 1, WAVE_Y - 5, 13, 14, col);
            text(x + 2, WAVE_Y + 6, K_LCD, 0.7f, k == 0 ? "A" : "B");
        }
        if (u->scrubbing) {
            int x = sample_to_x(L, sg, u->scrub_pos);
            if (x != -1000000) rect(x, WAVE_Y, 1, WAVE_H, RGBA8(255, 255, 255, 90));
        }
        int x = sample_to_x(L, sg, (double)u->playhead);
        if (x != -1000000) rect(x - 1, WAVE_Y - 3, 3, WAVE_H + 6, K_CREAM_HI);
    }
}

/* регионы нарезки: границы, номера, выбранный регион */
static void draw_slice_overlays(const UiState *u) {
    const Layout *L = &u->lay;
    for (int sg = 0; sg < L->nseg; sg++) {
        double vs = (double)L->vs[sg], ve = vs + (double)L->vl[sg];
        int sel = u->sl_sel;
        double a = g_sl_pos[sel], b = g_sl_pos[sel + 1];
        if (b > vs && a < ve) {
            int xa = sample_to_x(L, sg, a), xb = sample_to_x(L, sg, b);
            int sx = xa == -1000000 ? WAVE_X + L->x0[sg] : xa;
            int ex = xb == -1000000 ? WAVE_X + L->x0[sg] + L->w[sg] : xb;
            rect(sx, WAVE_Y, ex - sx, WAVE_H, u->loop ? RGBA8(120, 230, 140, 46) : RGBA8(255, 150, 60, 52));
        }
        for (int i = 0; i < u->sl_n; i++) {
            int x = sample_to_x(L, sg, (double)g_sl_pos[i]);
            if (x == -1000000) continue;
            int on = (i == sel);
            rect(x, WAVE_Y - 3, on ? 2 : 1, WAVE_H + 6, on ? K_YELLOW : RGBA8(255, 236, 200, 150));
            char nb[8];
            snprintf(nb, sizeof(nb), "%d", i + 1);
            rect(x, WAVE_Y - 3, (int)strlen(nb) * 7 + 6, 13, on ? K_YELLOW : RGBA8(255, 236, 200, 200));
            text(x + 3, WAVE_Y + 7, K_LCD, 0.68f, nb);
        }
        int x = sample_to_x(L, sg, (double)u->playhead);
        if (x != -1000000) rect(x - 1, WAVE_Y - 3, 3, WAVE_H + 6, K_CREAM_HI);
    }
}

static void draw_ticks(const UiState *u) {
    const Layout *L = &u->lay;
    int length = u->length;
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
        int y = WAVE_Y + WAVE_H + 4;
        int64_t first = ((int64_t)vs + step - 1) / step * step;
        int count = 0;
        for (int64_t s = first; s <= (int64_t)vs + vl && s <= length && count < 30; s += step, count++) {
            int x = WAVE_X + L->x0[sg] + (int)((s - vs) * w / vl);
            rect(x, y, 1, ((s / step) % 5 == 0) ? 8 : 4, K_AMBER_DIM);
        }
    }
}

static int g_legend = 0;

static void ui_draw(const UiState *u) {
    char buf[200], t1[32];

    vita2d_draw_texture(g_tex_bg, 0, 0);
    if (g_top_cur >= 0) vita2d_draw_texture(g_tex_top[g_top_cur], 0, 0);

    /* верхняя панель: название и вкладки */
    text(62, 30, K_BROWN, 1.25f, "VITA SAMPLER");
    text(62, 44, K_BROWN_LO, 0.62f, "SAMPLER / LOOPER   v0.9");
    text_c(384 + 46, 31, u->mode == 0 ? K_CREAM_HI : K_BROWN, 0.95f, "EDIT");
    text_c(484 + 46, 31, u->mode == 1 ? K_CREAM_HI : K_BROWN, 0.95f, "SLICE");
    text(622, 31, u->loop ? RGBA8(160, 240, 170, 255) : K_BROWN_LO, 0.9f, "LOOP");
    {
        const char *label = "READY";
        if (u->state == ST_RECORDING) label = "REC";
        if (u->state == ST_PLAYING) label = u->scrubbing ? "SCRUB" : "PLAY";
        text(746, 32, K_AMBER, 1.1f, label);
    }

    /* экран: время, номер сэмпла, уровень */
    int shown = u->live ? u->length : u->playhead;
    fmt_time(t1, sizeof(t1), shown);
    text(40, 104, K_AMBER, 2.0f, t1);
    snprintf(buf, sizeof(buf), "sample %d / %d", shown, u->length);
    text(262, 82, K_AMBER_LT, 0.95f, buf);
    {
        char t2[32];
        fmt_time(t2, sizeof(t2), u->length);
        int tenths = (int)((int64_t)u->view_len * 10 / WAVE_W);
        int zoomx = u->view_len > 0 ? u->length / u->view_len : 1;
        if (zoomx < 1) zoomx = 1;
        if (u->live) snprintf(buf, sizeof(buf), "max 05:00");
        else snprintf(buf, sizeof(buf), "total %s   zoom x%d   1 px = %d.%d smp%s", t2, zoomx, tenths / 10, tenths % 10, u->split ? "   SPLIT" : "");
        text(262, 104, K_AMBER_DIM, 0.8f, buf);
    }
    /* светодиодный индикатор уровня */
    text(588, 88, K_AMBER_DIM, 0.75f, "LEVEL");
    for (int i = 0; i < 20; i++) {
        int on = (u->level_pct * 20 / 100) > i;
        unsigned int col = i < 14 ? RGBA8(78, 205, 110, 255) : (i < 18 ? RGBA8(255, 190, 70, 255) : RGBA8(240, 78, 66, 255));
        if (!on) col = RGBA8(56, 46, 36, 255);
        rect(650 + i * 14, 74, 11, 14, col);
    }
    if (u->clip > 0) text(650, 108, K_WAVE_REC, 0.9f, "CLIP!");

    /* окно волны */
    if (u->wave_tex) vita2d_draw_texture(u->wave_tex, WAVE_X, WAVE_Y);
    if (u->length == 0 && !u->live) text_c(WAVE_X + WAVE_W / 2, WAVE_Y + 82, K_AMBER_DIM, 1.0f, "no recording yet - press R or tap REC");
    if (!u->live && u->length > 1) {
        if (u->mode == 0) draw_overlays(u); else draw_slice_overlays(u);
        draw_ticks(u);
        if (u->mode == 0)
            snprintf(buf, sizeof(buf), "A %d    B %d    region %d smp    active: %c", (int)u->c1, (int)u->c2, (int)(u->c2 - u->c1), u->active == 0 ? 'A' : 'B');
        else
            snprintf(buf, sizeof(buf), "slice %d / %d    start %d    length %d smp", u->sl_sel + 1, u->sl_n,
                     g_sl_pos[u->sl_sel], g_sl_pos[u->sl_sel + 1] - g_sl_pos[u->sl_sel]);
        text(WAVE_X, 296, K_AMBER_DIM, 0.8f, buf);
    }
    if (u->live) text(WAVE_X, 296, K_AMBER_DIM, 0.8f, "live view: last 8 seconds");
    if (u->scrubbing) {
        snprintf(buf, sizeof(buf), "scrub speed %d%%", (int)(fabs(u->scrub_speed) / (double)SAMPLE_RATE * 100.0 + 0.5));
        text(WAVE_X + WAVE_W - 170, 296, K_AMBER, 0.8f, buf);
    }
    text(WAVE_X, 314, u->confirm ? K_YELLOW : K_AMBER_LT, 1.0f, u->msg);

    /* органы управления */
    if (g_ctl_cur >= 0) vita2d_draw_texture(g_tex_ctl[g_ctl_cur], 0, CTL_TY);
    if (u->fx_page) {
        char v[48];
        for (int i = 0; i < N_FX; i++) {
            int y0 = MENU_Y + 3 + i * MENU_ROW;
            text(44, y0 + 17, i == u->menu_sel ? K_ORANGE_LO : K_BROWN, 0.85f, FX[i].name);
            fx_text(v, sizeof(v), i, g_fx[i]);
            text(SL_X + SL_W + 30, y0 + 18, g_fx[i] == FX[i].def ? K_BROWN : K_ORANGE_LO, 1.0f, v);
        }
        int x, y, w, h;
        btn_geom(B_FX_CLOSE, 0, 1, 0, &x, &y, &w, &h);
        text_c(x + w / 2, y + 16, K_CREAM_HI, 0.85f, "CLOSE");
    } else if (u->mode == 0) {
        static const char *names[10] = { "REC", "PLAY", "STOP", "LOOP", "FX", "CROP", "UNDO", "SAVE", "ZOOM -", "ZOOM +" };
        for (int id = B_REC; id <= B_ZIN; id++) {
            int x, y, w, h;
            btn_geom(id, 0, 0, 0, &x, &y, &w, &h);
            const char *nm = names[id - B_REC];
            if (id == B_PLAY && u->state == ST_PLAYING) nm = "PAUSE";
            int lit = (id == B_REC && u->state == ST_RECORDING) || (id == B_PLAY && u->state == ST_PLAYING) ||
                      (id == B_LOOP && u->loop) || (id == B_FX && u->fx_page) || (id == B_CROP && u->crop_armed);
            text(x + 62, y + h / 2 + 7 + (u->btn_down == id ? 2 : 0),
                 lit ? K_CREAM_HI : (id == B_UNDO && !u->can_undo ? K_BROWN_LO : K_BROWN), 1.0f, nm);
        }
    } else {
        for (int i = 0; i < u->sl_n; i++) {
            int x, y, w, h;
            pad_geom(i, u->sl_n, &x, &y, &w, &h);
            char nb[8];
            snprintf(nb, sizeof(nb), "%d", i + 1);
            int on = (i == u->sl_sel) || (i == u->sl_playing);
            float sc = h < 40 ? 0.8f : 1.3f;
            text_c(x + w / 2 + 6, y + h / 2 + (int)(7 * sc), on ? K_CREAM_HI : K_BROWN, sc, nb);
        }
        snprintf(buf, sizeof(buf), "SLICES  %d", u->sl_n);
        text(776, 354, K_BROWN, 1.0f, buf);
    }

    /* подсказка и окно справки */
    text(32, 520, K_BROWN_LO, 0.78f, "SELECT help     START save WAV     SELECT+START hold = exit");
    if (u->legend) {
        rect(24, 58, 912, 254, RGBA8(22, 18, 14, 238));
        rect_outline(24, 58, 912, 254, 2, K_ORANGE_LO);
        static const char *L[] = {
            "CONTROLS                                                              (SELECT closes)",
            "R  record / stop       L  play / pause  (hold L = glitch, right stick up/down = rate)",
            "TRIANGLE  stop (hold = keep position)      SQUARE  loop      CIRCLE  effects",
            "START  save WAV      SELECT  this help      SELECT + START (hold)  exit",
            "EDIT   sticks move cursors A / B      both sticks up / down = zoom in / out",
            "       D-pad left/right = active cursor      D-pad up/down = zoom      X hold = crop to A-B",
            "       touch the top of the wave next to A or B and drag = move the cursor",
            "       touch the wave elsewhere = scrub (max x2, quieter below 20%)",
            "SLICE  regions are built inside A-B chosen in EDIT      X  play slice",
            "       sticks nudge slice start / end      D-pad left/right = pick slice, up/down = count",
            "MENU   D-pad up/down row, left/right value, left stick, touch sliders, X reset (hold = all)"
        };
        for (int i = 0; i < 11; i++) text(40, 82 + i * 21, i == 0 ? K_YELLOW : K_AMBER_LT, 0.82f, L[i]);
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
    int old_len = view_len;
    view_len = (int)nl;
    if (factor > 1.0 && view_len <= old_len) view_len = old_len + 1;
    if (factor < 1.0 && view_len >= old_len && old_len > 1) view_len = old_len - 1;
    if (view_len < 1) view_len = 1;
    clamp_view(length);
    view_start = (int)(anchor - a * (double)view_len);
    clamp_view(length);
}

/* ---------- Состояние приложения ---------- */
static double c1 = 0.0, c2 = 0.0;      /* курсоры A и B (в сэмплах), всегда c1 < c2 */
static int active = 0;                 /* последний выбранный курсор: 0 = A, 1 = B */
static int split = 0;
static int dirty = 0;                  /* есть запись, не выгруженная в WAV */
static char msg[160] = "Press R to start recording";

typedef struct { uint64_t t; double v; } TouchSample;

/* Скорость пальца (сэмплов/с): разность по окну ~90 мс. Оставлено для проверок; в приложении
 * скорость считает ScrubTrack (фильтр), он плавнее. */
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

typedef struct {
    int fx_page, menu_sel, menu_touch_row, menu_touch_id, fx_hold, x_hold;
    float fx_acc;
    int confirm; unsigned confirm_frame;
    int crop_armed; unsigned crop_frame;
    int tri_active, tri_frames, tri_pos;
    int scrubbing, scrub_id, resume_state, scrub_seg;
    double scrub_pos, scrub_speed, mk_frac[2];
    ScrubTrack st;
    uint64_t prev_touch_t;
    int sl_flash;
    int btn_down, btn_id;
    int prev_ids[6], prev_n;
    unsigned frame;
    int nudge_frames, zoom_frames, nav_frames, cnt_frames;
    double zoom_rem, zoom_anchor;
    int l_frames, l_pre, l_glitch, l_acc;
    int drag_on, drag_id, drag_which;
} App;
static App A;

/* согласованная передача области и перемотки в аудио-поток */
static void region_publish(int lo, int hi, int seek) {
    g_rg_seq++;
    __sync_synchronize();
    g_lo = lo; g_hi = hi;
    if (seek >= 0) g_seek_to = seek;
    __sync_synchronize();
    g_rg_seq++;
}

/* Что играть: в EDIT - между курсорами, в SLICE - выбранный регион */
static void apply_region(void) {
    int length = g_length;
    if (length < 2) { g_lo = 0; g_hi = 0; return; }
    if (g_mode == 0) { g_lo = (int)c1; g_hi = (int)c2; return; }
    int sel = g_sl_sel;
    if (sel < 0) sel = 0;
    if (sel >= g_sl_n) sel = g_sl_n - 1;
    g_lo = g_sl_pos[sel];
    g_hi = g_sl_pos[sel + 1];
}

static void request_autosave(int length, int with_audio) {
    g_sess_c1 = (int)c1;
    g_sess_c2 = (int)c2;
    g_sess_dirty = dirty;
    g_save_len = length;
    __sync_synchronize();
    g_save_req |= (with_audio ? 3 : 2);
}

static void view_reset_full(int length) {
    view_start = 0;
    view_len = length > 0 ? length : MIN_VIEW;
    clamp_view(length);
}

static void finish_recording(void) {
    int length = g_length;
    dirty = (length > 0);
    g_audio_ver++;
    g_undo_valid = 0;
    view_reset_full(length);
    c1 = 0.0; c2 = (double)length; active = 0; split = 0;
    g_play_pos = 0; g_seek_to = -1;
    g_sl_lo = 0; g_sl_hi = length;
    if (g_mode == 1 && length >= 4 * SL_MIN_LEN) { slices_prepare(g_sl_req, 1); g_sl_sel = 0; }
    else if (g_mode == 1) g_mode = 0;
    apply_region();
    snprintf(msg, sizeof(msg), "Recording stopped. L: play   SELECT: save WAV");
    dbg_log("rec stop len=%d late=%d odd=%d", length, g_in_late, g_in_odd);
    if (length > 1) request_autosave(length, 1);
}

/* ====================================================================
 *  Действия (одинаково вызываются и с кнопок, и с сенсорного экрана)
 * ==================================================================== */
static void stop_scrub_state(void) {
    g_scrub = 0;
    A.scrubbing = 0;
    A.scrub_id = -1;
}

static void act_record_toggle(void) {
    if (g_state == ST_RECORDING) {
        g_state = ST_IDLE;
        sceKernelDelayThread(40000);
        finish_recording();
        return;
    }
    if (dirty && !A.confirm) {
        A.confirm = 1;
        A.confirm_frame = A.frame;
        snprintf(msg, sizeof(msg), "Unsaved recording! Press REC again to erase it, or SAVE first");
        return;
    }
    g_state = ST_IDLE;
    stop_scrub_state();
    sceKernelDelayThread(60000);
    for (int w = 0; w < 300 && (g_save_req || g_save_busy); w++) sceKernelDelayThread(10000);
    g_length = 0;
    g_play_pos = 0;
    g_seek_to = -1;
    g_peak_all = 0;
    g_error = 0;
    g_discard = 3;
    dirty = 0;
    A.confirm = 0;
    g_undo_valid = 0;
    view_start = 0;
    view_len = LIVE_SAMPLES;
    c1 = c2 = 0.0; split = 0; active = 0;
    g_lo = 0; g_hi = 0;
    __sync_synchronize();
    g_state = ST_RECORDING;
    snprintf(msg, sizeof(msg), "Recording... press REC / R to stop");
    dbg_log("rec start");
}

static void act_play_toggle(void) {
    if (g_state == ST_RECORDING || g_length < 2) return;
    if (g_state == ST_PLAYING) {
        g_state = ST_IDLE;
        snprintf(msg, sizeof(msg), "Paused");
        dbg_log("pause");
        return;
    }
    apply_region();
    g_error = 0;
    g_state = ST_PLAYING;
    snprintf(msg, sizeof(msg), g_loop ? "Playing (loop)" : "Playing");
    dbg_log("play");
}

/* стоп: следующее воспроизведение начнётся с начала области (A или начало региона) */
static void act_stop_press(void) {
    if (g_state == ST_RECORDING || g_length < 2) return;
    g_state = ST_IDLE;
    g_scrub = 0;
    sceKernelDelayThread(40000);
    A.tri_pos = g_play_pos;
    apply_region();
    int lo = g_lo, hi = g_hi;
    region_sanitize(&lo, &hi, g_length);
    g_play_pos = lo;
    g_seek_to = -1;
    snprintf(msg, sizeof(msg), g_mode == 0 ? "Stopped - next play starts from A   (hold TRIANGLE to keep the position)"
                                           : "Stopped - next play starts from the slice start");
    dbg_log("stop");
}

static void act_loop_toggle(void) {
    g_loop = !g_loop;
    snprintf(msg, sizeof(msg), g_loop ? "Loop ON" : "Loop off");
    dbg_log("loop %d", g_loop);
}

static void act_fx_toggle(void) {
    A.fx_page = !A.fx_page;
    A.menu_touch_row = -1;
    A.fx_acc = 0.0f;
}

static void crop_execute(void) {
    A.crop_armed = 0;
    if (g_mode != 0 || g_state == ST_RECORDING || g_length < 512) return;
    int lo = (int)c1, hi = (int)c2;
    if (hi - lo < 256) { snprintf(msg, sizeof(msg), "Region A-B is too short to crop"); return; }
    if (lo == 0 && hi == g_length) { snprintf(msg, sizeof(msg), "Move cursors A and B first - the region is the whole sample"); return; }
    g_state = ST_IDLE;
    stop_scrub_state();
    sceKernelDelayThread(60000);
    int old_len = g_length;
    int nl = audio_crop(lo, hi, (int)c1, (int)c2);
    if (nl < 0) { snprintf(msg, sizeof(msg), "Crop failed"); return; }
    c1 = 0.0; c2 = (double)nl; active = 0; split = 0;
    view_reset_full(nl);
    g_play_pos = 0; g_seek_to = -1;
    apply_region();
    dirty = 1;
    request_autosave(nl, 1);
    int t10 = (int)((int64_t)nl * 10 / SAMPLE_RATE);
    snprintf(msg, sizeof(msg), "Cropped to %d.%d s (was %d.%d s). UNDO restores it", t10 / 10, t10 % 10,
             (int)((int64_t)old_len * 10 / SAMPLE_RATE) / 10, (int)((int64_t)old_len * 10 / SAMPLE_RATE) % 10);
    dbg_log("crop %d..%d -> %d", lo, hi, nl);
}

/* касание кнопки CROP: первое нажатие "взводит", второе выполняет */
static void act_crop_tap(void) {
    if (g_mode != 0 || g_length < 512) return;
    if (A.crop_armed) { crop_execute(); return; }
    A.crop_armed = 1;
    A.crop_frame = A.frame;
    snprintf(msg, sizeof(msg), "Tap CROP again to keep only the region A-B");
}

static void act_undo(void) {
    if (g_state == ST_RECORDING) return;
    if (!g_undo_valid) { snprintf(msg, sizeof(msg), "Nothing to undo"); return; }
    g_state = ST_IDLE;
    stop_scrub_state();
    sceKernelDelayThread(60000);
    int a = 0, b = 0;
    int nl = audio_undo(&a, &b);
    if (nl < 0) { snprintf(msg, sizeof(msg), "Nothing to undo"); return; }
    c1 = (double)a; c2 = (double)b; active = 0; split = 0;
    enforce_cursors(nl);
    view_reset_full(nl);
    g_play_pos = 0; g_seek_to = -1;
    if (g_mode == 1) { g_sl_lo = (int)c1; g_sl_hi = (int)c2; slices_prepare(g_sl_req, 1); }
    apply_region();
    dirty = 1;
    request_autosave(nl, 1);
    snprintf(msg, sizeof(msg), "Undo: sample restored");
    dbg_log("undo -> %d", nl);
}

static void act_save(void) {
    if (g_state == ST_RECORDING) return;
    char path[128];
    int r = save_wav(path, sizeof(path));
    if (r == 0) {
        snprintf(msg, sizeof(msg), g_wav_overwrote ? "Saved (same file updated): %s" : "Saved: %s", path);
        dirty = 0;
        request_autosave(g_length, 0);
        dbg_log("saved %s", path);
    } else if (r == -1) snprintf(msg, sizeof(msg), "Nothing to save - record something first");
    else snprintf(msg, sizeof(msg), "Could not write the file");
}

static void act_zoom(int dir) {
    int length = g_length;
    if (length < 2 || g_state == ST_RECORDING) return;
    if (g_mode != 0) return;                     /* в SLICE масштаб не меняется */
    A.zoom_anchor = (active == 0 ? c1 : c2);
    A.zoom_rem += (dir > 0 ? -0.6931 : 0.6931);   /* плавно доезжаем за несколько кадров */
    if (A.zoom_rem > 4.2) A.zoom_rem = 4.2;
    if (A.zoom_rem < -4.2) A.zoom_rem = -4.2;
}

static void act_slice_trigger(int i) {
    if (g_length < 2 || g_state == ST_RECORDING || g_mode != 1) return;
    if (i < 0 || i >= g_sl_n) return;
    g_sl_sel = i;
    apply_region();
    g_error = 0;
    A.sl_flash = 8;
    g_play_pos = g_sl_pos[i];
    region_publish(g_lo, g_hi, g_sl_pos[i]);          /* область и перемотка - одной операцией */
    g_state = ST_PLAYING;
    snprintf(msg, sizeof(msg), "Slice %d of %d", i + 1, g_sl_n);
}

static void act_slice_count(int d) {
    if (g_mode != 1 || g_length < 2) return;
    int req = g_sl_n + d;
    if (req < SL_MIN) req = SL_MIN;
    if (req > SL_MAX) req = SL_MAX;
    slices_prepare(req, 1);
    if (g_sl_sel >= g_sl_n) g_sl_sel = g_sl_n - 1;
    apply_region();
    snprintf(msg, sizeof(msg), "%d slices", g_sl_n);
}

static void act_slice_select(int d) {
    if (g_mode != 1) return;
    int s = g_sl_sel + d;
    if (s < 0) s = 0;
    if (s > g_sl_n - 1) s = g_sl_n - 1;
    g_sl_sel = s;
    apply_region();
}

static int length_now(void) { return g_length; }
/* в SLICE всегда общий вид: окно = область, из которой нарезаны регионы */
static void slice_view_fit(int length) {
    int a = g_sl_pos[0], b = g_sl_pos[g_sl_n];
    if (b - a < 16) { view_reset_full(length); return; }
    view_start = a; view_len = b - a;
    clamp_view(length);
}

static void act_set_mode(int m) {
    if (m == g_mode) return;
    if (g_state == ST_RECORDING) return;
    if (m == 1) {
        if (g_length < 4 * SL_MIN_LEN) { snprintf(msg, sizeof(msg), "Record a longer sample first (at least 0.4 s)"); return; }
        int rlo = (int)c1, rhi = (int)c2;
        if (rhi - rlo < 4 * SL_MIN_LEN) { snprintf(msg, sizeof(msg), "Region A-B is too short for slices (at least 0.4 s) - widen it in EDIT"); return; }
        int changed = (rlo != g_sl_lo || rhi != g_sl_hi || g_sl_pos[0] != rlo || g_sl_pos[g_sl_n] != rhi);
        g_sl_lo = rlo; g_sl_hi = rhi;
        slices_prepare(g_sl_req, changed);
        if (changed) g_sl_sel = 0;
        if (g_sl_sel >= g_sl_n) g_sl_sel = g_sl_n - 1;
        g_mode = 1;
        split = 0;
        slice_view_fit(length_now());
        apply_region();
        snprintf(msg, sizeof(msg), "SLICE mode: %d regions - tap a pad to play from there", g_sl_n);
    } else {
        g_mode = 0;
        view_reset_full(g_length);
        apply_region();
        snprintf(msg, sizeof(msg), "EDIT mode");
    }
    A.crop_armed = 0;
    dbg_log("mode %d", g_mode);
}

/* сдвинуть границу региона idx (1..n-1) на delta сэмплов, не заходя за соседей */
static void slice_nudge(int idx, int delta) {
    if (idx < 1 || idx >= g_sl_n || delta == 0) return;
    int lo = g_sl_pos[idx - 1] + SL_MIN_LEN;
    int hi = g_sl_pos[idx + 1] - SL_MIN_LEN;
    int v = g_sl_pos[idx] + delta;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    g_sl_pos[idx] = v;
    apply_region();
}

/* ====================================================================
 *  Главная программа
 * ==================================================================== */
static int level_from_peak(int peak) { return level_percent(peak); }

#ifndef SAMPLER_NO_MAIN
int main(void) {
    sceIoMkdir("ux0:data/Sampler", 0777);
    sceIoRemove(LOG_PREV);
    sceIoRename(LOG_PATH, LOG_PREV);          /* лог прошлого запуска сохраняем: пригодится после сбоя */
    sceIoRemove(SESS_RAW_TMP);                /* остатки прерванного сохранения - не копим */
    sceIoRemove(SESS_META_TMP);
    {
        SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) sceIoClose(fd);
    }
    dbg_log("start v0.9");

    g_audio = (int16_t *)malloc(MAX_SAMPLES * sizeof(int16_t));
    if (!g_audio) { dbg_log("malloc failed"); sceKernelExitProcess(0); }
    g_undo = (int16_t *)malloc(MAX_SAMPLES * sizeof(int16_t));      /* для отмены обрезки; если не хватит памяти - без отмены */
    dbg_log("undo buffer %s", g_undo ? "ok" : "unavailable");

    for (int i = 0; i < W_N; i++)
        g_hann[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * (float)i / (float)W_N));
    ker_init();

    vita2d_init_advanced(4 * 1024 * 1024);
    vita2d_set_clear_color(K_CREAM);
    g_pgf = vita2d_load_default_pgf();
    for (int i = 0; i < N_WAVE_TEX; i++) g_wave_tex[i] = vita2d_create_empty_texture(WAVE_W, WAVE_H);
    if (!ui_init()) { dbg_log("ui_init failed"); sceKernelExitProcess(0); }
    dbg_log("gfx ready");

    memset(&A, 0, sizeof(A));
    A.menu_touch_row = -1; A.menu_touch_id = -1; A.scrub_id = -1; A.btn_down = -1; A.btn_id = -1;
    for (int i = 0; i < 6; i++) A.prev_ids[i] = -1;

    /* восстановить прошлую сессию, если она есть */
    {
        SessionMeta sm;
        if (session_restore(&sm)) {
            int len = sm.length;
            g_length = len;
            g_audio_ver = 1;
            c1 = (double)sm.c1; c2 = (double)sm.c2; active = 0;
            enforce_cursors(len);
            for (int i = 0; i < N_FX; i++)
                if (sm.fx[i] >= 0 && sm.fx[i] < FX[i].n) g_fx[i] = sm.fx[i];
            g_loop = sm.loop ? 1 : 0;
            dirty = sm.dirty ? 1 : 0;
            g_sl_req = (sm.sl_req >= SL_MIN && sm.sl_req <= SL_MAX) ? sm.sl_req : 8;
            int ok = (sm.sl_n >= 2 && sm.sl_n <= SL_MAX && sm.sl_pos[0] == 0 && sm.sl_pos[sm.sl_n] == len);
            for (int i = 0; ok && i < sm.sl_n; i++) if (sm.sl_pos[i + 1] <= sm.sl_pos[i]) ok = 0;
            if (ok) {
                g_sl_n = sm.sl_n;
                g_sl_lo = sm.sl_pos[0]; g_sl_hi = sm.sl_pos[sm.sl_n];
                for (int i = 0; i <= g_sl_n; i++) g_sl_pos[i] = sm.sl_pos[i];
                g_sl_sel = (sm.sl_sel >= 0 && sm.sl_sel < g_sl_n) ? sm.sl_sel : 0;
                g_slices_valid_ver = g_audio_ver;       /* границы сохранены вместе с записью */
                g_mode = (sm.mode == 1) ? 1 : 0;
            } else {
                g_mode = 0;
            }
            view_reset_full(len);
            apply_region();
            int t10 = (int)((int64_t)len * 10 / SAMPLE_RATE);
            snprintf(msg, sizeof(msg), "Restored last session (%d.%d s). SELECT: export WAV", t10 / 10, t10 % 10);
            dbg_log("restored len=%d dirty=%d mode=%d slices=%d", len, dirty, g_mode, g_sl_n);
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
    int shown_level = 0, clip_hold = 0;
    int last_err = 0, prev_state = ST_IDLE;

    int wave_cur = -1, wave_next = 0;
    int key[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    Layout lay_last;
    memset(&lay_last, 0xff, sizeof(lay_last));

    int last_sig = 0, sig_pending = 0;
    unsigned int last_sig_frame = 0;

    uint64_t last_t = sceKernelGetProcessTimeWide();
    uint64_t last_sys = sceKernelGetSystemTimeWide();

    while (1) {
        A.frame++;
        const unsigned frame = A.frame;
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
            stop_scrub_state();
            A.menu_touch_row = -1; A.btn_down = -1; A.btn_id = -1;
            g_reset_in = 1; g_reset_out = 1;
            vita2d_wait_rendering_done();
            for (int i = 0; i < N_WAVE_TEX; i++) {
                if (g_wave_tex[i]) vita2d_free_texture(g_wave_tex[i]);
                g_wave_tex[i] = vita2d_create_empty_texture(WAVE_W, WAVE_H);
            }
            ui_recreate();
            vita2d_free_pgf(g_pgf);
            g_pgf = vita2d_load_default_pgf();
            key[0] = -1;
            wave_cur = -1;
            dbg_log("resume: gpu resources recreated");
        }
        last_sys = sys_now;

        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old_buttons;
        unsigned int released = old_buttons & ~pad.buttons;
        old_buttons = pad.buttons;
        {   /* START - сохранить, SELECT - справка (по отпусканию короткого нажатия); обе вместе 0.75 с - выход */
            static int sel_f = 0, st_f = 0, exit_f = 0, combo = 0;
            int both = (pad.buttons & SCE_CTRL_SELECT) && (pad.buttons & SCE_CTRL_START);
            if (both) { combo = 1; if (++exit_f >= 45) break; } else exit_f = 0;
            if (pad.buttons & SCE_CTRL_SELECT) sel_f++;
            if (pad.buttons & SCE_CTRL_START) st_f++;
            if ((released & SCE_CTRL_SELECT) && !combo && sel_f < 30) g_legend = !g_legend;
            if ((released & SCE_CTRL_START) && !combo && st_f < 30) { act_save(); last_sys = sceKernelGetSystemTimeWide(); }
            if (!(pad.buttons & SCE_CTRL_SELECT)) sel_f = 0;
            if (!(pad.buttons & SCE_CTRL_START)) st_f = 0;
            if (!(pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START))) combo = 0;
        }

        int state = g_state;
        int length = g_length;

        /* запись остановилась сама (лимит 5 минут или ошибка) */
        if (prev_state == ST_RECORDING && state != ST_RECORDING) {
            finish_recording();
            state = g_state;
            length = g_length;
        }

        /* "взведённые" подтверждения гаснут сами */
        if (A.confirm && frame - A.confirm_frame > 240) A.confirm = 0;
        if (A.crop_armed && frame - A.crop_frame > 150) A.crop_armed = 0;
        if (A.sl_flash > 0) A.sl_flash--;

        /* ---- общие кнопки ---- */
        if (pressed & SCE_CTRL_RTRIGGER) act_record_toggle();
        if (pressed & SCE_CTRL_LTRIGGER) {
            A.l_pre = g_state;
            A.l_frames = 0; A.l_glitch = 0; A.l_acc = 0;
            act_play_toggle();
        }
        if ((pad.buttons & SCE_CTRL_LTRIGGER) && state != ST_RECORDING && length > 1) {
            A.l_frames++;
            if (A.l_frames >= 15) {                          /* удержание: глитч-режим */
                if (!A.l_glitch) {
                    A.l_glitch = 1; A.l_acc = 0;
                    apply_region();
                    g_error = 0;
                    g_state = ST_PLAYING;
                    snprintf(msg, sizeof(msg), "GLITCH: hold L, right stick up/down = rate. Release: back to previous state");
                }
                float ry = ((float)(128 - (int)pad.ry)) / 128.0f;     /* вверх - чаще */
                int period = 7 - (int)(ry * 5.0f + (ry > 0 ? 0.5f : -0.5f));
                if (period < 2) period = 2;
                if (period > 12) period = 12;
                if (++A.l_acc >= period) {
                    A.l_acc = 0;
                    if (g_state == ST_PLAYING) g_state = ST_IDLE;
                    else if (g_state == ST_IDLE) { g_error = 0; g_state = ST_PLAYING; }
                }
            }
        } else if (A.l_glitch) {
            A.l_glitch = 0;
            if (g_state != ST_RECORDING) {
                if (A.l_pre == ST_PLAYING) { g_error = 0; g_state = ST_PLAYING; }
                else g_state = ST_IDLE;
            }
            A.l_frames = 0;
        } else A.l_frames = 0;
        if (pressed & SCE_CTRL_SQUARE)   act_loop_toggle();
        if (pressed & SCE_CTRL_CIRCLE)   act_fx_toggle();

        /* Треугольник: стоп; удержание - позиция сохраняется */
        if (pressed & SCE_CTRL_TRIANGLE) {
            act_stop_press();
            A.tri_active = 1;
            A.tri_frames = 0;
        }
        if (A.tri_active) {
            if (pad.buttons & SCE_CTRL_TRIANGLE) {
                if (++A.tri_frames == 24) {
                    g_play_pos = A.tri_pos;
                    snprintf(msg, sizeof(msg), "Stopped - position kept");
                }
            } else A.tri_active = 0;
        }

        state = g_state;
        length = g_length;

        /* ---- страница эффектов ---- */
        if (A.fx_page) {
            if (pressed & SCE_CTRL_UP)   A.menu_sel = (A.menu_sel + N_FX - 1) % N_FX;
            if (pressed & SCE_CTRL_DOWN) A.menu_sel = (A.menu_sel + 1) % N_FX;

            if (pad.buttons & SCE_CTRL_CROSS) {
                A.x_hold++;
                if (A.x_hold == 1) fx_set(A.menu_sel, FX[A.menu_sel].def);
                if (A.x_hold == 30) {
                    for (int i = 0; i < N_FX; i++) fx_set(i, FX[i].def);
                    snprintf(msg, sizeof(msg), "All effects reset");
                }
            } else A.x_hold = 0;

            int dir = ((pad.buttons & SCE_CTRL_RIGHT) ? 1 : 0) - ((pad.buttons & SCE_CTRL_LEFT) ? 1 : 0);
            if (dir) {
                A.fx_hold++;
                int d = 0;
                if (A.fx_hold == 1) d = 1;
                else if (A.fx_hold > 14) d = 1 + (A.fx_hold - 14) / 6;
                if (d) fx_set(A.menu_sel, g_fx[A.menu_sel] + dir * d);
            } else A.fx_hold = 0;

            float a = stick_axis(pad.lx);
            if (a != 0.0f) {
                A.fx_acc += a * (float)(FX[A.menu_sel].n - 1) * 0.45f * dt;
                int whole = (int)A.fx_acc;
                if (whole) { fx_set(A.menu_sel, g_fx[A.menu_sel] + whole); A.fx_acc -= (float)whole; }
            } else A.fx_acc = 0.0f;
        }

        /* ---- режимы: курсоры (EDIT) или регионы (SLICE) ---- */
        double spp = (double)view_len / (double)WAVE_W;
        if (spp < 0.05) spp = 0.05;

        if (state != ST_RECORDING && length > 1) {
            if (g_mode == 0) {
                float a1 = A.fx_page ? 0.0f : stick_axis(pad.lx);
                float a2 = stick_axis(pad.rx);

                /* зум двумя стиками: оба вверх - ближе, оба вниз - дальше; разные стороны - без зума */
                float lyn = ((float)(128 - (int)pad.ly)) / 128.0f, ryn = ((float)(128 - (int)pad.ry)) / 128.0f;
                float lxn = ((float)pad.lx - 128.0f) / 128.0f, rxn = ((float)pad.rx - 128.0f) / 128.0f;
                int two_zoom = 0;
                if (!A.fx_page && fabsf(lyn) > 0.35f && fabsf(ryn) > 0.35f && (lyn > 0) == (ryn > 0)
                    && fabsf(lyn - ryn) < 0.35f
                    && fabsf(lyn) > 1.3f * fabsf(lxn) && fabsf(ryn) > 1.3f * fabsf(rxn)) two_zoom = 1;
                if (two_zoom) {
                    float mag = 0.5f * (fabsf(lyn) + fabsf(ryn));
                    mag = (mag - 0.35f) / 0.65f; if (mag < 0.0f) mag = 0.0f; if (mag > 1.0f) mag = 1.0f;
                    double mid = 0.5 * (c1 + c2);
                    double anc = (mid >= view_start && mid <= view_start + view_len) ? mid : (double)view_start + 0.5 * view_len;
                    zoom_view(exp((lyn > 0 ? -1.0 : 1.0) * (0.6 + 1.6 * mag) * (double)dt), anc, length);
                    a1 = 0.0f; a2 = 0.0f;
                }
                if (a1 != 0.0f) { c1 += (double)a1 * 420.0 * spp * (double)dt; active = 0; enforce_cursors(length); }
                if (a2 != 0.0f) { c2 += (double)a2 * 420.0 * spp * (double)dt; active = 1; enforce_cursors(length); }

                if (!A.fx_page) {
                    int dir = ((pad.buttons & SCE_CTRL_RIGHT) ? 1 : 0) - ((pad.buttons & SCE_CTRL_LEFT) ? 1 : 0);
                    if (dir) {
                        A.nudge_frames++;
                        double move = 0.0;
                        if (A.nudge_frames == 1) move = 1.0;
                        else if (A.nudge_frames > 15) {
                            double px = spp < 1.0 ? 1.0 : spp;
                            move = px * (double)(1 + (A.nudge_frames - 15) / 15);
                        }
                        if (move != 0.0) {
                            if (active == 0) c1 += dir * move; else c2 += dir * move;
                            enforce_cursors(length);
                        }
                    } else A.nudge_frames = 0;

                    int zd = ((pad.buttons & SCE_CTRL_UP) ? 1 : 0) - ((pad.buttons & SCE_CTRL_DOWN) ? 1 : 0);
                    if (zd) {
                        A.zoom_frames++;
                        double anchor = active == 0 ? c1 : c2;
                        double ramp = A.zoom_frames / 20.0; if (ramp > 1.0) ramp = 1.0;
                        zoom_view(exp((zd > 0 ? -1.0 : 1.0) * (0.35 + 1.45 * ramp) * (double)dt), anchor, length);
                    } else A.zoom_frames = 0;

                    if (A.zoom_rem > 0.003 || A.zoom_rem < -0.003) {          /* доводка от кнопок зума */
                        double st = A.zoom_rem * 0.22;
                        if (fabs(st) < 0.01) st = A.zoom_rem;
                        zoom_view(exp(st), A.zoom_anchor, length);
                        A.zoom_rem -= st;
                    } else A.zoom_rem = 0.0;

                    /* Крест: подсказка, удержание - обрезать по A-B */
                    if (pad.buttons & SCE_CTRL_CROSS) {
                        A.x_hold++;
                        if (A.x_hold == 1) snprintf(msg, sizeof(msg), "Hold X to crop to the region A-B (UNDO restores)");
                        if (A.x_hold == 30) crop_execute();
                    } else A.x_hold = 0;
                }
                enforce_cursors(length);
                g_lo = (int)c1;
                g_hi = (int)c2;
            } else {
                if (!A.fx_page) {
                    /* регионы: D-pad выбирает и меняет число, стики двигают границы, Крест играет */
                    int dir = ((pad.buttons & SCE_CTRL_RIGHT) ? 1 : 0) - ((pad.buttons & SCE_CTRL_LEFT) ? 1 : 0);
                    if (dir) {
                        A.nav_frames++;
                        if (A.nav_frames == 1 || (A.nav_frames > 15 && A.nav_frames % 5 == 0)) act_slice_select(dir);
                    } else A.nav_frames = 0;

                    int ud = ((pad.buttons & SCE_CTRL_UP) ? 1 : 0) - ((pad.buttons & SCE_CTRL_DOWN) ? 1 : 0);
                    if (ud) {
                        A.cnt_frames++;
                        if (A.cnt_frames == 1 || (A.cnt_frames > 15 && A.cnt_frames % 4 == 0)) act_slice_count(ud);
                    } else A.cnt_frames = 0;

                    if (pressed & SCE_CTRL_CROSS) act_slice_trigger(g_sl_sel);

                    float a1 = stick_axis(pad.lx), a2 = stick_axis(pad.rx);
                    if (a1 != 0.0f) {
                        A.mk_frac[0] += (double)a1 * 420.0 * spp * (double)dt;
                        int whole = (int)A.mk_frac[0];
                        if (whole) { slice_nudge(g_sl_sel, whole); A.mk_frac[0] -= (double)whole; }
                    } else A.mk_frac[0] = 0.0;
                    if (a2 != 0.0f) {
                        A.mk_frac[1] += (double)a2 * 420.0 * spp * (double)dt;
                        int whole = (int)A.mk_frac[1];
                        if (whole) { slice_nudge(g_sl_sel + 1, whole); A.mk_frac[1] -= (double)whole; }
                    } else A.mk_frac[1] = 0.0;
                }
            }
        }

        /* ---- положение окна: сплит (только EDIT и только если вся запись не видна) ---- */
        if (state != ST_RECORDING && length > 1) {
            clamp_view(length);
            if (g_mode == 1 || view_len >= length) {
                split = 0;
                if (g_mode == 1) slice_view_fit(length);
                else if (view_len >= length) view_start = 0;
            } else {
                double lo = c1, hi = c2, span = hi - lo;
                if (!split) { if (span > (double)view_len * 0.88) split = 1; }
                else if (span <= (double)view_len * 0.75) {
                    split = 0;
                    view_start = (int)((lo + hi) * 0.5 - (double)view_len * 0.5);
                    clamp_view(length);
                }
                if (!split) {
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
            }
        }

        /* ---- касания ---- */
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        int nrep = (int)touch.reportNum;
        if (nrep > 6) nrep = 6;

        Layout lay_now;
        compute_layout(&lay_now, length, state == ST_RECORDING, split, c1, c2);

        /* новые касания: кнопки, вкладки, пэды, ползунки, начало скраба */
        for (int k = 0; k < nrep; k++) {
            int id = touch.report[k].id;
            int is_new = 1;
            for (int j = 0; j < A.prev_n; j++) if (A.prev_ids[j] == id) is_new = 0;
            if (!is_new) continue;
            double fx = touch.report[k].x / 2.0, fy = touch.report[k].y / 2.0;
            int in_wave = (fx >= WAVE_X - 4 && fx <= WAVE_X + WAVE_W + 4 && fy >= WAVE_Y - 18 && fy <= WAVE_Y + WAVE_H + 18);

            if (in_wave && g_mode == 0 && !A.fx_page && !A.drag_on && !A.scrubbing && g_state != ST_RECORDING
                && length > 1 && fy < WAVE_Y + 46) {
                int best = -1; double bd = 28.0;
                for (int w = 0; w < 2; w++)
                    for (int sg = 0; sg < lay_now.nseg; sg++) {
                        int mxp = sample_to_x(&lay_now, sg, w ? c2 : c1);
                        if (mxp == -1000000) continue;
                        double d = fabs(fx - (double)mxp);
                        if (d < bd) { bd = d; best = w; }
                    }
                if (best >= 0) {
                    A.drag_on = 1; A.drag_id = id; A.drag_which = best; active = best;
                    snprintf(msg, sizeof(msg), "Dragging cursor %c", best ? 'B' : 'A');
                    continue;
                }
            }
            if (in_wave && !A.scrubbing && !A.drag_on && g_state != ST_RECORDING && length > 1) {
                int seg = 0;
                A.scrubbing = 1;
                A.scrub_id = id;
                A.resume_state = g_state;
                double target = x_to_sample(&lay_now, fx, &seg);
                if (target < 0.0) target = 0.0;
                if (target > (double)(length - 1)) target = (double)(length - 1);
                A.scrub_seg = seg;
                A.scrub_pos = target;
                scrub_track_reset(&A.st, fx);
                A.prev_touch_t = now_t;
                A.scrub_speed = 0.0;
                g_scrub_target = (float)target;
                g_scrub_vel = 0.0f;
                g_seek_to = (int)target;
                g_play_pos = (int)target;
                g_error = 0;
                g_scrub = 1;
                g_state = ST_PLAYING;
                dbg_log("scrub start %d", (int)target);
                continue;
            }

            int hit = hit_test((int)fx, (int)fy, g_mode, A.fx_page, g_sl_n);
            if (hit != B_NONE) {
                A.btn_down = hit;
                A.btn_id = id;
                if (hit == B_TAB_EDIT) act_set_mode(0);
                else if (hit == B_TAB_SLICE) act_set_mode(1);
                else if (hit == B_REC) act_record_toggle();
                else if (hit == B_PLAY) act_play_toggle();
                else if (hit == B_STOP) act_stop_press();
                else if (hit == B_LOOP) act_loop_toggle();
                else if (hit == B_FX || hit == B_FX_CLOSE) act_fx_toggle();
                else if (hit == B_CROP) act_crop_tap();
                else if (hit == B_UNDO) act_undo();
                else if (hit == B_SAVE) { act_save(); last_sys = sceKernelGetSystemTimeWide(); }
                else if (hit == B_ZIN) act_zoom(1);
                else if (hit == B_ZOUT) act_zoom(-1);
                else if (hit == B_SL_PLUS) act_slice_count(1);
                else if (hit == B_SL_MINUS) act_slice_count(-1);
                else if (hit >= B_PAD0) act_slice_trigger(hit - B_PAD0);
                continue;
            }

            if (A.fx_page && A.menu_touch_row < 0 && fy >= MENU_Y && fy < MENU_Y + MENU_H && fx >= MENU_X) {
                int row = (int)((fy - MENU_Y - 3.0) / MENU_ROW);
                if (row < 0) row = 0;
                if (row > N_FX - 1) row = N_FX - 1;
                A.menu_touch_row = row;
                A.menu_touch_id = id;
                A.menu_sel = row;
            }
        }

        /* уже идущие касания */
        int scrub_alive = 0, menu_alive = 0, btn_alive = 0, drag_alive = 0;
        for (int k = 0; k < nrep; k++) {
            int id = touch.report[k].id;
            double fx = touch.report[k].x / 2.0;
            if (A.scrubbing && id == A.scrub_id) {
                scrub_alive = 1;
                int seg = 0;
                double tdt = (double)(now_t - A.prev_touch_t) * 1e-6;
                A.prev_touch_t = now_t;
                double xf, vp;
                scrub_track_step(&A.st, fx, tdt, &xf, &vp);
                double target = x_to_sample(&lay_now, xf, &seg);
                if (target < 0.0) target = 0.0;
                if (target > (double)(length - 1)) target = (double)(length - 1);
                if (seg != A.scrub_seg) {                   /* перешли на другой мини-дисплей: прыжок */
                    A.scrub_seg = seg;
                    scrub_track_reset(&A.st, fx);
                    g_seek_to = (int)target;
                    vp = 0.0;
                }
                double spp_seg = (double)lay_now.vl[seg] / (double)lay_now.w[seg];
                A.scrub_pos = target;
                A.scrub_speed = vp * spp_seg;
                g_scrub_target = (float)target;
                g_scrub_vel = (float)(vp * spp_seg);
            }
            if (A.drag_on && id == A.drag_id) {
                drag_alive = 1;
                int sg2 = 0;
                double tg = x_to_sample(&lay_now, fx, &sg2);
                if (A.drag_which == 0) c1 = tg; else c2 = tg;
                active = A.drag_which;
                enforce_cursors(length);
                g_lo = (int)c1; g_hi = (int)c2;
            }
            if (A.menu_touch_row >= 0 && id == A.menu_touch_id) {
                menu_alive = 1;
                float f = (float)((fx - SL_X) / SL_W);
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
                if (fx >= SL_X - 20) fx_set(A.menu_touch_row, (int)(f * (float)(FX[A.menu_touch_row].n - 1) + 0.5f));
            }
            if (A.btn_down != B_NONE && id == A.btn_id) btn_alive = 1;
        }
        if (A.scrubbing && !scrub_alive) {
            A.scrubbing = 0;
            A.scrub_id = -1;
            g_scrub = 0;
            g_scrub_vel = 0.0f;
            int endpos = (int)A.scrub_pos;
            if (A.resume_state == ST_PLAYING) {
                g_seek_to = endpos;
            } else {
                g_state = ST_IDLE;
                sceKernelDelayThread(40000);
                g_play_pos = endpos;
                g_seek_to = -1;
            }
            dbg_log("scrub end %d", endpos);
        }
        if (A.drag_on && !drag_alive) { A.drag_on = 0; request_autosave(length, 0); }
        if (A.menu_touch_row >= 0 && !menu_alive) A.menu_touch_row = -1;
        if (A.btn_down != B_NONE && !btn_alive) { A.btn_down = B_NONE; A.btn_id = -1; }
        A.prev_n = nrep;
        for (int k = 0; k < nrep; k++) A.prev_ids[k] = touch.report[k].id;

        if ((pressed & ~(SCE_CTRL_RTRIGGER | SCE_CTRL_CROSS)) && A.confirm) A.confirm = 0;

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

        /* ---- автосохранение курсоров/эффектов/регионов (через 1.5 с после последнего изменения) ---- */
        if (length > 1 && state != ST_RECORDING) {
            uint32_t sig = (uint32_t)(int)c1 * 31u + (uint32_t)(int)c2 * 17u + (uint32_t)g_loop * 7u + (uint32_t)dirty * 3u
                         + (uint32_t)g_mode * 13u + (uint32_t)g_sl_sel * 5u + (uint32_t)g_sl_n * 3u;
            for (int i = 0; i < N_FX; i++) sig = sig * 131u + (uint32_t)g_fx[i];
            for (int i = 0; i <= g_sl_n; i++) sig = sig * 31u + (uint32_t)g_sl_pos[i];
            if ((int)sig != last_sig) { last_sig = (int)sig; last_sig_frame = frame; sig_pending = 1; }
            if (sig_pending && frame - last_sig_frame > 90) {
                request_autosave(length, 0);
                sig_pending = 0;
            }
        }

        /* ---- лог: пульс раз в ~1.5 с и ошибки ---- */
        if (frame % 90 == 0)
            dbg_log("hb f=%u st=%d mode=%d len=%d pos=%d lvl=%d split=%d fx=%d scrub=%d mic_late=%d mic_odd=%d r0=%d out_late=%d",
                    frame, state, g_mode, length, playhead, g_level, split, A.fx_page, A.scrubbing,
                    g_in_late, g_in_odd, g_in_r0, g_out_late);
        if (g_error != last_err) { last_err = g_error; dbg_log("audio error 0x%08X", (unsigned)g_error); }
        prev_state = state;

        /* ---- уровень ---- */
        int lvl = (state == ST_IDLE) ? 0 : level_from_peak(g_level);
        if (g_level >= 32000 && state != ST_IDLE) clip_hold = 60;
        if (clip_hold > 0) clip_hold--;
        if (lvl > shown_level) shown_level = lvl;
        else shown_level = shown_level * 88 / 100;

        /* ---- данные для рисования ---- */
        UiState u;
        memset(&u, 0, sizeof(u));
        u.legend = g_legend; u.mode = g_mode; u.fx_page = A.fx_page; u.state = state; u.scrubbing = A.scrubbing; u.loop = g_loop;
        u.length = length; u.playhead = playhead; u.live = live;
        u.split = split; u.active = active; u.c1 = c1; u.c2 = c2;
        u.menu_sel = A.menu_sel;
        u.level_pct = shown_level; u.clip = clip_hold;
        u.confirm = A.confirm || A.crop_armed;
        snprintf(u.msg, sizeof(u.msg), "%s", msg);
        u.sl_n = g_sl_n; u.sl_sel = g_sl_sel; u.sl_flash = A.sl_flash;
        u.sl_playing = (state == ST_PLAYING && g_mode == 1) ? g_sl_sel : -1;
        u.btn_down = A.btn_down; u.can_undo = g_undo_valid; u.crop_armed = A.crop_armed;
        u.scrub_pos = A.scrub_pos; u.scrub_speed = A.scrub_speed;
        u.frame = frame; u.lay = lay_now; u.view_len = view_len;
        u.wave_tex = (wave_cur >= 0) ? g_wave_tex[wave_cur] : NULL;

        ui_update_textures(&u);

        vita2d_start_drawing();
        vita2d_clear_screen();
        ui_draw(&u);
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
    ui_free_textures();
    vita2d_free_pgf(g_pgf);
    vita2d_fini();
    free(g_audio);
    if (g_undo) free(g_undo);
    sceKernelExitProcess(0);
    return 0;
}
#endif
