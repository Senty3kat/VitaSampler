/*
 * Vita Sampler v1.7
 *
 * Кнопки: R запись/стоп, L пауза/play (удержание L = глитч, задняя панель: палец вверх/вниз = 1-30 Гц),
 *   Треугольник стоп, Квадрат петля, Круг страница FX, START сохранить WAV, SELECT справка,
 *   SELECT + D-pad влево/вправо - соседняя вкладка, SELECT + START (удержание) - выход.
 * Вкладки сверху: EDIT, SLICE, GRAIN, ADV, FX. Все ручки - только через сенсорный экран.
 * Запись автоматически сохраняется в ux0:data/Sampler/session.* и восстанавливается при запуске.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/audioin.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <vita2d.h>
#include <png.h>

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

#define SCR_W   960
#define SCR_H   544
#define TOP_H   40
#define CTL_TY  358
#define CTL_TH  186
/* стеклянная рамка, окно ЭЛТ, окно волны, шкала уровня, панель управления */
#define BEZ_X 10
#define BEZ_Y 46
#define BEZ_W 880
#define BEZ_H 306
#define CRT_X 26
#define CRT_Y 62
#define CRT_W 848
#define CRT_H 274
#define WAVE_X  36
#define WAVE_Y  98
#define WAVE_W  828
#define WAVE_H  180
#define SEG_W   378                            /* ширина мини-дисплея в режиме сплита */
#define N_WAVE_TEX 4
#define METER_X 915              /* стеклянная трубка уровня */
#define METER_Y 80
#define METER_W 18
#define METER_H 228
#define METER_TICKS 8           /* от 0 до -24 дБ, засечка каждые 3 дБ */
#define METER_DB 24
#define GL_M 7                    /* поле вокруг экрана в текстуре стекла (литая кромка) */
#define GL_W (CRT_W + 2 * GL_M)
#define GL_H (CRT_H + 2 * GL_M)
/* страница PAD: большие пады слева, маленький осциллограф справа сверху */
#define PBZ_X 10
#define PBZ_Y 46
#define PBZ_W 394
#define PBZ_H 158
#define PCRT_X (PBZ_X + 16)
#define PCRT_Y (PBZ_Y + 16)
#define PCRT_W (PBZ_W - 32)
#define PCRT_H (PBZ_H - 32)
#define GL2_W (PCRT_W + 2 * GL_M)
#define GL2_H (PCRT_H + 2 * GL_M)
#define PAD_CY 42                  /* холст органов управления страницы PAD начинается ниже верхней планки */
#define PAD_CH (SCR_H - PAD_CY)
#define PN_X 10
#define PN_Y 362
#define PN_W 938
#define PN_H 174

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
static volatile int g_rev = 0, g_rev_req = 0;   /* обратное воспроизведение области (g_rev применяется вместе с областью) */
static volatile float g_glitch_hz = 0.0f;  /* глитч: частота вкл/выкл звука, Гц (0 = выключен) */
static volatile int g_mute = 0;            /* глитч: звук выключается, воспроизведение идёт дальше */
static volatile int g_rate_only = 1;       /* скраб управляется только скоростью (без подтяжки к позиции) */
static volatile int g_loop = 0;
/* фильтр (2-й порядок, без резонанса): тип 0 выкл, 1 LPF, 2 HPF, 3 BPF; центр и ширина - ползунки 0..100 */
static volatile int g_flt_type = 0, g_flt_c = 60, g_flt_w = 50;
/* гранулярный эффект: вкл, смесь %, число зёрен (индекс 0..15 -> 1..16), длина, разброс (ползунки 0..100) */
static volatile int g_kb_on = 0, g_kb_semi = 0;      /* клавиатура GRAIN: зажата ли клавиша и сдвиг тона в полутонах (-12..+12) */
static volatile int g_gr_on = 0, g_gr_mix = 70, g_gr_n = 7, g_gr_size = 48, g_gr_spr = 40, g_gr_det = 0;
static volatile int g_lo = 0;
static volatile int g_hi = 0;
static volatile float g_scrub_target = 0.0f;
static volatile float g_scrub_vel = 0.0f;       /* сэмплов в секунду */
static volatile int g_reset_in = 0;             /* после "сна" переоткрыть аудио-порты */
static volatile int g_reset_out = 0;
static volatile int g_in_late = 0;              /* диагностика микрофона: опоздавшие блоки */
static volatile int g_in_odd = 0;               /* диагностика микрофона: необычные коды возврата */
static volatile int g_in_r0 = -999;
static volatile int g_out_load = 0;              /* нагрузка вывода: время расчёта блока / длительность блока, ‰ (пик за ~1.5 с) */
static volatile int g_out_late = 0;             /* диагностика вывода: опоздавшие блоки */

/* режимы: 0 = EDIT (курсоры A/B), 1 = SLICE (авто-нарезка на регионы) */
#define SL_MAX 32
#define SL_MIN 4
static int g_mode = 0;
static volatile float g_tempo_mul = 1.0f;   /* множитель скорости от BPM-регулятора */
static int g_bpm_base = 120, g_bpm_target = 120;
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
/* профиль скорости скраба по расстоянию пальца от линии (пиксели): подбирается на практике */
#define SC_NEAR  8.0       /* ближе - скорость плавно падает до нуля */
#define SC_HOLD  110.0      /* от NEAR до HOLD - ровно 1.0x */
#define SC_FAR   340.0     /* от HOLD до FAR скорость растёт до MAX_SCRUB_X */
static double scrub_profile(double dpx) {
    double a = fabs(dpx), r;
    if (a < SC_NEAR) { double t = a / SC_NEAR; r = t * t * (3.0 - 2.0 * t); }
    else if (a <= SC_HOLD) r = 1.0;
    else if (a >= SC_FAR) r = MAX_SCRUB_X;
    else { double t = (a - SC_HOLD) / (SC_FAR - SC_HOLD); t = t * t * (3.0 - 2.0 * t); r = 1.0 + (MAX_SCRUB_X - 1.0) * t; }
    return dpx < 0.0 ? -r : r;
}
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

#define GR_MAX 24
typedef struct { double sp, rt; float age, len, amp; int on; } Grain;

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
    double gphase;              /* фаза глитча 0..1 */
    Grain gr[GR_MAX];           /* гранулярный эффект: зёрна */
    float gtimer;               /* до следующего зерна, в выходных сэмплах */
    double gref;                /* позиция головки для привязки зёрен (режим WSOLA) */
    uint32_t rng;
    float bz[2][2];             /* состояния двух звеньев фильтра */
    float gmix, gnv;            /* сглаженные доля эффекта и нормировка (без щелчков при изменении) */
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
    int mute, rate_only, rev;
    float glitch_hz;
    int flt_type; float flt_f1, flt_f2;
    int gr_on, gr_n; float gr_mix, gr_len, gr_spread, gr_det;   /* длина - в выходных сэмплах, разброс - в сэмплах источника */
} Params;

static void engine_reset(Engine *e, double pos) {
    memset(e, 0, sizeof(*e));
    e->pos = pos;
    e->wpos = pos;
    e->ppos = pos;
    e->gref = pos;
    e->rng = 2463534242u;
}

static int g_mir = 0;                    /* поток звука: читаем зеркально внутри области */
static double g_mir_c = 0.0;

static inline float rd_clamped(const int16_t *a, int len, double pos) {
    if (g_mir) pos = g_mir_c - pos;
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
    if (g_mir) idx = (int)g_mir_c - idx;
    if (idx < 0 || idx >= len) return 0.0f;
    return (float)a[idx];
}


/* линейное чтение с учётом зеркала (для зёрен: дёшево и достаточно) */
static inline float rd_lin(const int16_t *a, int len, double pos) {
    if (g_mir) pos = g_mir_c - pos;
    if (pos < 0.0) pos = 0.0;
    if (pos > (double)(len - 2)) pos = (double)(len - 2);
    int i = (int)pos;
    float f = (float)(pos - (double)i);
    return (float)a[i] * (1.0f - f) + (float)a[i + 1] * f;
}

static inline float rng01(uint32_t *r) {
    *r = *r * 1664525u + 1013904223u;
    return (float)(*r >> 8) * (1.0f / 16777216.0f);
}

/* Баттерворт 2-го порядка (Q = 0.7071, без резонанса): c = { b0, b1, b2, a1, a2 } */
static void bq_calc(float *c, int hp, float f) {
    if (f < 20.0f) f = 20.0f;
    if (f > 0.45f * (float)OUT_RATE) f = 0.45f * (float)OUT_RATE;
    float w0 = 2.0f * 3.14159265f * f / (float)OUT_RATE;
    float cw = cosf(w0), sw = sinf(w0), al = sw / (2.0f * 0.70710678f);
    float a0 = 1.0f + al, b0, b1, b2;
    if (hp) { b0 = (1.0f + cw) * 0.5f; b1 = -(1.0f + cw); b2 = b0; }
    else    { b0 = (1.0f - cw) * 0.5f; b1 = 1.0f - cw;    b2 = b0; }
    c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = -2.0f * cw / a0; c[4] = (1.0f - al) / a0;
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
    g_mir = (p->rev && !p->scrub) ? 1 : 0;
    g_mir_c = (double)(p->lo + p->hi - 1);
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

    /* фильтр: коэффициенты на блок */
    float bc[2][5];
    const int nbq = (p->flt_type == 3) ? 2 : (p->flt_type ? 1 : 0);
    if (p->flt_type == 1) bq_calc(bc[0], 0, p->flt_f1);
    else if (p->flt_type == 2) bq_calc(bc[0], 1, p->flt_f1);
    else if (p->flt_type == 3) { bq_calc(bc[0], 1, p->flt_f1); bq_calc(bc[1], 0, p->flt_f2); }

    /* зёрна: привязаны к головке воспроизведения, читают звук с той же скоростью (высота тона - по PITCH) */
    const int use_gr = p->gr_on && p->gr_mix > 0.001f && p->gr_n > 0 && mode != 0 && span > 4;
    const double gadv = (mode == 1) ? step : step * (double)p->speed;
    const double grate = step * (double)p->pratio;
    const float ginterval = p->gr_len / (float)(p->gr_n > 0 ? p->gr_n : 1);
    const float gnorm = 1.9f / powf((float)(p->gr_n > 0 ? p->gr_n : 1), 0.7f);
    if (e->gnv <= 0.0f) e->gnv = gnorm;
    if (mode == 2) e->gref = e->ppos;

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
        float ft = (e->force_fade || p->mute) ? 0.0f : 1.0f;     /* цель огибающей */
        if (p->glitch_hz > 0.0f) {
            e->gphase += (double)p->glitch_hz / (double)OUT_RATE;
            if (e->gphase >= 1.0) e->gphase -= 1.0;
            if (e->gphase >= 0.5) ft = 0.0f;                    /* вторая половина периода - звук закрыт */
        }

        if (ended) { mono[i] = 0; continue; }

        if (mode == 0) {
            /* скорость = скорость пальца + мягкая подтяжка к позиции пальца, всё сглажено */
            double desired = p->scrub_vel / (double)OUT_RATE
                           + (p->rate_only ? 0.0 : (p->scrub_target - e->pos) * (SCRUB_KP / (double)OUT_RATE));
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

        /* гранулярный эффект: доля и нормировка меняются плавно; зёрна дочитываются без обрывов */
        e->gmix += ((use_gr ? p->gr_mix : 0.0f) - e->gmix) * 0.0015f;
        e->gnv += (gnorm - e->gnv) * 0.0015f;
        if (use_gr || e->gmix > 0.0008f) {
            if (use_gr) {
                double gp;
                if (mode == 1) gp = e->pos;
                else {
                    e->gref += gadv;
                    if (e->gref >= (double)hi) {
                        if (p->loop) e->gref = (double)lo + fmod(e->gref - (double)hi, (double)span);
                        else e->gref = (double)hi - 1.0;
                    }
                    gp = e->gref;
                }
                e->gtimer -= 1.0f;
                if (e->gtimer <= 0.0f) {
                    for (int q = 0; q < GR_MAX; q++) if (!e->gr[q].on) {
                        double off = (double)(rng01(&e->rng) * 2.0f - 1.0f) * (double)p->gr_spread;
                        double sp = gp + off;
                        if (p->loop) {
                            sp = (double)lo + fmod(sp - (double)lo, (double)span);
                            if (sp < (double)lo) sp += (double)span;
                        } else {
                            if (sp < (double)lo) sp = (double)lo;
                            if (sp > (double)hi - 2.0) sp = (double)hi - 2.0;
                        }
                        e->gr[q].sp = sp; e->gr[q].age = 0.0f; e->gr[q].len = p->gr_len;
                        e->gr[q].rt = grate * (p->gr_det > 0.0f ? (double)exp2f((rng01(&e->rng) * 2.0f - 1.0f) * p->gr_det / 1200.0f) : 1.0);
                        e->gr[q].amp = 0.8f + 0.2f * rng01(&e->rng); e->gr[q].on = 1;
                        break;
                    }
                    e->gtimer += ginterval * (0.92f + 0.16f * rng01(&e->rng));
                }
            }
            float gs = 0.0f;
            for (int q = 0; q < GR_MAX; q++) {
                Grain *g = &e->gr[q];
                if (!g->on) continue;
                float t = g->age / g->len;
                if (t >= 1.0f) { g->on = 0; continue; }
                float wf = t * (float)(W_N - 1);
                int wi = (int)wf;
                float w = g_hann[wi] + (g_hann[wi + 1 < W_N ? wi + 1 : wi] - g_hann[wi]) * (wf - (float)wi);
                gs += g->amp * w * rd_lin(a, len, g->sp);
                g->sp += g->rt;                       /* зерно дочитывается до конца окна, даже за границей области: без скачков */
                g->age += 1.0f;
            }
            s = s * (1.0f - e->gmix) + e->gmix * gs * e->gnv;
            {   /* мягкое ограничение вместо жёсткого среза */
                float as = fabsf(s);
                if (as > 20000.0f) { float tt = (as - 20000.0f) * (1.0f / 12767.0f); as = 20000.0f + 12767.0f * tt / (1.0f + tt); s = s < 0.0f ? -as : as; }
            }
        }

        /* эффекты: понижение частоты дискретизации и разрядности */
        if (do_hold) {
            e->hold_acc += hold_inc;
            if (e->hold_acc >= 1.0f) { e->hold_acc -= 1.0f; e->held = s; }
            s = e->held;
        }
        int v = quantize(clamp16((int)s), p->bits);
        float vf = (float)v;
        for (int k = 0; k < nbq; k++) {                         /* фильтр: транспонированная форма II */
            float y = bc[k][0] * vf + e->bz[k][0];
            e->bz[k][0] = bc[k][1] * vf - bc[k][3] * y + e->bz[k][1];
            e->bz[k][1] = bc[k][2] * vf - bc[k][4] * y;
            vf = y;
        }

        /* огибающая: плавный вход/выход, чтобы не было щелчков */
        if (e->gain < ft) { e->gain += 1.0f / FADE_SAMPLES; if (e->gain > ft) e->gain = ft; }
        else if (e->gain > ft) { e->gain -= 1.0f / FADE_SAMPLES; if (e->gain < ft) e->gain = ft; }
        float g = e->gain * p->volume;
        if (mode == 0) g *= e->scrub_gain;
        v = clamp16((int)(vf * g));

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
    float tm = (g_mode == 1) ? g_tempo_mul : 1.0f;                /* BPM-регулятор работает в SLICE */
    p->speed = speed_from_idx(fs) * tm;
    p->glitch_hz = g_glitch_hz;
    static float kb_cur = 0.0f;                                    /* плавный переход (глиссандо) между клавишами */
    {
        float tg = g_kb_on ? (float)g_kb_semi : 0.0f;
        kb_cur += (tg - kb_cur) * 0.45f;
        if (fabsf(tg - kb_cur) < 0.02f) kb_cur = tg;
    }
    p->pratio = powf(2.0f, ((float)(fp - 24) + (float)(fc - 50) / 100.0f + kb_cur) / 12.0f);
    p->neutral = (fs == FX_SPEED_DEF && fp == 24 && fc == 50 && fabsf(tm - 1.0f) < 1e-4f && kb_cur == 0.0f);
    p->volume = (float)g_fx[5] / 100.0f;
    p->lo = lo;
    p->hi = hi;
    p->loop = g_loop;
    p->scrub = g_scrub;
    p->scrub_target = (double)g_scrub_target;
    p->scrub_vel = (double)g_scrub_vel;
    p->mute = g_mute;
    p->rate_only = g_rate_only;
    {   /* фильтр: центр 40 Гц ... 8 кГц (логарифмически), ширина полосы 0.25 ... 4 октавы */
        float fc = 40.0f * powf(200.0f, (float)g_flt_c / 100.0f);
        float wo = 0.25f * powf(16.0f, (float)g_flt_w / 100.0f);
        p->flt_type = g_flt_type;
        p->flt_f1 = fc; p->flt_f2 = fc;
        if (g_flt_type == 3) { float k = powf(2.0f, wo * 0.5f); p->flt_f1 = fc / k; p->flt_f2 = fc * k; }
    }
    p->gr_on = g_gr_on;
    p->gr_mix = (float)g_gr_mix / 100.0f;
    p->gr_n = 1 + g_gr_n;
    p->gr_len = 48.0f * 20.0f * powf(25.0f, (float)g_gr_size / 100.0f);               /* 20 ... 500 мс в выходных сэмплах */
    { float sp = (float)g_gr_spr / 100.0f; p->gr_spread = 16.0f * 1000.0f * sp * sp; }
    { float d = (float)g_gr_det / 100.0f; p->gr_det = 1200.0f * d * d; }                    /* детюн зёрен: 0 ... +-1200 центов */   /* 0 ... 1000 мс, в сэмплах источника */
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
/* ====================================================================
 *  PAD (MPC): до 16 сэмплов из WAV на пэдах, ADSR, режимы ONE-SHOT / GATE / LOOP
 *  Голоса смешиваются в потоке вывода поверх основного звука (работают и без воспроизведения дорожки).
 * ==================================================================== */
#define PAD_N 16
#define PAD_VOICES 24
#define PAD_CFG "ux0:data/Sampler/pads.cfg"
typedef struct {
    int16_t *d; int n, rate;
    char path[160], name[32];
    volatile int loading;
    uint8_t pk[208];                               /* обзор волны для экрана */
    int cth, crat, cmk, cmx;                       /* компрессор: порог (100 = выкл.), степень, подъём, микс 0..100 */
    int vol, pitch, start, mode, a, dc, s, r;      /* vol 0..100, pitch 0..48 (=-24..+24 пт), start 0..99 %, mode 0/1/2, a/dc/r 0..100 (время), s 0..100 % */
} Pad;
#define PAD_PK 208
static Pad g_pad[PAD_N];
static volatile int g_pe_vol = 80, g_pe_pitch = 24, g_pe_start = 0;   /* значения ручек выбранного пада */
static volatile int g_pe_a = 0, g_pe_dc = 30, g_pe_s = 100, g_pe_r = 10;
static volatile int g_pe_cth = 100, g_pe_crat = 40, g_pe_cmk = 0, g_pe_cmx = 100;
static int g_pad_env_sub = 0;                      /* подстраница PAD: 0 основная, 1 огибающая ADSR */
typedef struct { int pad, stage, gate; unsigned age; double pos; float env, rel, cenv, cg; } PVoice;   /* stage: 0 пусто, 1 A, 2 D, 3 S, 4 R */
static PVoice g_pv[PAD_VOICES];
static volatile unsigned g_pad_trig[PAD_N], g_pad_rel[PAD_N];
static volatile int g_pad_mask;                    /* биты пэдов, у которых сейчас звучит голос (для индикации) */
static unsigned g_pad_seen_t[PAD_N], g_pad_seen_r[PAD_N];
static volatile int g_pad_drop = 0, g_pad_steal = 0, g_pad_lock = 0, g_pad_hits = 0;   /* диагностика: потерянные / украденные / подавленные удары, всего ударов */
static unsigned g_pv_age = 0;                       /* счётчик ударов: по нему находим самый старый голос */
#define PAD_LAYERS 4                                /* ONE-SHOT: столько ударов одного пада звучат одновременно */

/* Запас в сегменте кода: vita-elf-create дописывает в конец сегмента 0 таблицы импорта (~2.8 КБ); если код заканчивается
 * почти у границы страницы, сборка падает с "Cannot allocate ... segment 1 overlaps". Тогда меняем TEXT_PAD (на 1-2 КБ). */
#define TEXT_PAD 3000
static void __attribute__((used, noinline)) text_pad(void) { __asm__ volatile(".space " "3000"); }

static float pad_ms(int v) { float t = (float)v / 100.0f; return 3000.0f * t * t; }
static void pad_defaults(Pad *p) { p->vol = 80; p->pitch = 24; p->start = 0; p->mode = 0; p->a = 0; p->dc = 30; p->s = 100; p->r = 10; p->cth = 100; p->crat = 40; p->cmk = 0; p->cmx = 100; }

static void pv_fade(PVoice *v) {                    /* быстро (5 мс) погасить голос, без щелчка */
    if (v->stage == 0 || v->stage == 4) return;
    v->stage = 4; v->rel = v->env / 240.0f + 1e-6f;
}
static void pad_voice_trigger(int i) {
    Pad *p = &g_pad[i];
    if (!p->d || p->loading || p->n < 2) { g_pad_drop++; return; }
    int cnt = 0, oldest = -1; unsigned oa = 0xffffffffu;
    for (int k = 0; k < PAD_VOICES; k++) {
        PVoice *o = &g_pv[k];
        if (o->pad != i || o->stage == 0 || o->stage == 4) continue;
        if (p->mode != 0) { pv_fade(o); continue; }        /* GATE / LOOP: повторный удар заменяет прежний */
        cnt++;
        if ((int)(o->age - oa) < 0 || oldest < 0) { oa = o->age; oldest = k; }
    }
    if (p->mode == 0 && cnt >= PAD_LAYERS && oldest >= 0) pv_fade(&g_pv[oldest]);   /* ONE-SHOT: слои, лишний старый плавно уходит */
    int fv = -1;
    for (int k = 0; k < PAD_VOICES; k++) if (g_pv[k].stage == 0) { fv = k; break; }
    if (fv < 0) {                                          /* свободных нет: берём самый тихий (уходящие в приоритете) */
        float me = 1e9f;
        g_pad_steal++;
        for (int k = 0; k < PAD_VOICES; k++) { float e = g_pv[k].env + (g_pv[k].stage == 4 ? 0.0f : 1.0f); if (e < me) { me = e; fv = k; } }
    }
    PVoice *v = &g_pv[fv];
    v->pad = i; v->stage = 1; v->gate = 1; v->env = 0.0f; v->rel = 0.0f; v->cenv = 0.0f; v->cg = 1.0f; v->age = ++g_pv_age;
    v->pos = (double)p->n * (double)p->start / 100.0;
}
static void pad_voice_release(int i) {
    Pad *p = &g_pad[i];
    for (int k = 0; k < PAD_VOICES; k++) {
        PVoice *v = &g_pv[k];
        if (v->pad != i || v->stage == 0 || !v->gate) continue;
        v->gate = 0;
        if (p->mode != 0 && v->stage < 4) {
            float rs = pad_ms(p->r) * 48.0f;
            if (rs < 96.0f) rs = 96.0f;
            v->stage = 4; v->rel = (v->env > 1e-4f ? v->env : 1e-4f) / rs;
        }
    }
}
/* перед заменой сэмпла: голоса пада плавно гаснут (5 мс), ждём два блока - щелчка при смене нет */
static void pad_quiet(int i) {
    int any = 0;
    for (int k = 0; k < PAD_VOICES; k++) if (g_pv[k].pad == i && g_pv[k].stage != 0) { pv_fade(&g_pv[k]); any = 1; }
    if (any) sceKernelDelayThread(50000);
}
static int pad_busy(void) {
    for (int i = 0; i < PAD_N; i++) if (g_pad_trig[i] != g_pad_seen_t[i] || g_pad_rel[i] != g_pad_seen_r[i]) return 1;
    for (int k = 0; k < PAD_VOICES; k++) if (g_pv[k].stage != 0) return 1;
    return 0;
}
/* добавляет голоса пэдов в mono[OUT_GRAIN]; peak - пик результата */
static void pad_mix_add(int16_t *mono, int *peak) {
    for (int i = 0; i < PAD_N; i++) {
        unsigned t = g_pad_trig[i], r = g_pad_rel[i];
        if (t != g_pad_seen_t[i]) { g_pad_seen_t[i] = t; pad_voice_trigger(i); }
        if (r != g_pad_seen_r[i]) { g_pad_seen_r[i] = r; pad_voice_release(i); }
    }
    static float acc[OUT_GRAIN];
    int any = 0, mask = 0;
    memset(acc, 0, sizeof(acc));
    for (int k = 0; k < PAD_VOICES; k++) {
        PVoice *v = &g_pv[k];
        if (v->stage == 0) continue;
        Pad *p = &g_pad[v->pad];
        if (!p->d || p->loading || p->n < 2) { v->stage = 0; continue; }
        any = 1; mask |= 1 << v->pad;
        const float step = (float)p->rate / 48000.0f * powf(2.0f, (float)(p->pitch - 24) / 12.0f);
        float as = pad_ms(p->a) * 48.0f, ds = pad_ms(p->dc) * 48.0f, rs = pad_ms(p->r) * 48.0f;
        if (as < 96.0f) as = 96.0f;
        if (ds < 48.0f) ds = 48.0f;
        if (rs < 96.0f) rs = 96.0f;
        const float sus = (float)p->s / 100.0f;
        const float ia = 1.0f / as, id = (1.0f - sus) / ds;
        float gain = (float)p->vol / 100.0f; gain = gain * sqrtf(gain) * 0.9f;
        const int comp_on = (p->cth < 100 && p->crat > 0) || p->cmk > 0;
        const float cthr = powf(10.0f, -48.0f * (1.0f - (float)p->cth / 100.0f) / 20.0f);
        const float cratio = 1.0f + 19.0f * ((float)p->crat / 100.0f) * ((float)p->crat / 100.0f);
        const float cslope = 1.0f - 1.0f / cratio;
        const float cmake = powf(10.0f, 18.0f * (float)p->cmk / 100.0f / 20.0f), cmix = (float)p->cmx / 100.0f;
        const double endp = (double)(p->n - 1);
        const double lstart = (double)p->n * (double)p->start / 100.0;
        const double rthr = endp - (double)step * (double)rs;
        for (int i = 0; i < OUT_GRAIN; i++) {
            if (v->pos >= endp) {
                if (p->mode == 2 && v->stage != 4 && endp - lstart > 16.0) v->pos -= (endp - lstart);
                else { v->stage = 0; break; }
            }
            int ix = (int)v->pos;
            float fr = (float)(v->pos - (double)ix);
            float smp = (float)p->d[ix] + ((float)p->d[ix + 1] - (float)p->d[ix]) * fr;
            switch (v->stage) {
            case 1: v->env += ia; if (v->env >= 1.0f) { v->env = 1.0f; v->stage = 2; } break;
            case 2: v->env -= id; if (v->env <= sus) { v->env = sus; v->stage = 3; } break;
            case 3: break;
            default: v->env -= v->rel; if (v->env <= 0.0f) { v->env = 0.0f; v->stage = 0; } break;
            }
            if (v->stage == 0) break;
            if (p->mode == 0 && v->stage != 4 && v->pos >= rthr) {          /* ONE-SHOT: хвост затухает как раз до конца сэмпла */
                float rem = (float)((endp - v->pos) / (double)step);
                v->stage = 4; v->rel = v->env / (rem > 1.0f ? rem : 1.0f);
            }
            float xs = smp * v->env;
            if (comp_on) {                                                  /* компрессор пада: следящая огибающая, усиление каждые 16 отсчётов */
                float lv = fabsf(xs) * (1.0f / 32768.0f);
                v->cenv += (lv > v->cenv ? 0.0042f : 0.0002f) * (lv - v->cenv);
                if ((i & 15) == 0) {
                    float tg = 1.0f;
                    if (v->cenv > cthr) tg = expf(-logf(v->cenv / cthr) * cslope);       /* (log2f / exp2f в newlib vitasdk нет - это тот же расчёт через logf / expf) */
                    v->cg += (tg - v->cg) * 0.3f;
                }
                xs = xs * (1.0f - cmix) + xs * v->cg * cmake * cmix;
            }
            acc[i] += xs * gain;
            v->pos += (double)step;
        }
    }
    g_pad_mask = mask;
    if (!any) return;
    int pk = 0;
    for (int i = 0; i < OUT_GRAIN; i++) {
        float f = (float)mono[i] + acc[i];
        if (f > 24000.0f || f < -24000.0f) {                 /* мягкий лимитер вместо среза: колено с 24000 */
            float a = fabsf(f) - 24000.0f, sg = f < 0 ? -1.0f : 1.0f;
            f = sg * (24000.0f + 8767.0f * (a / (a + 8767.0f)));
        }
        int s = clamp16((int)f);
        mono[i] = (int16_t)s;
        int a = s < 0 ? -s : s;
        if (a > pk) pk = a;
    }
    if (pk > *peak) *peak = pk;
}

/* ---- загрузка WAV: PCM 8/16/24/32, float32, моно/стерео (стерео усредняется), любая частота до 96 кГц, не длиннее 60 с ---- */
static uint32_t rd32(const unsigned char *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static int pad_load_wav(int pi, const char *path, char *err, size_t ecap) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, ecap, "Cannot open the file"); return 0; }
    unsigned char hd[12];
    if (fread(hd, 1, 12, f) != 12 || memcmp(hd, "RIFF", 4) || memcmp(hd + 8, "WAVE", 4)) { fclose(f); snprintf(err, ecap, "Not a WAV file"); return 0; }
    int fmt = 0, ch = 0, bits = 0; unsigned rate = 0;
    long data_off = -1; uint32_t data_sz = 0;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t sz = rd32(ck + 4);
        long here = ftell(f);
        if (!memcmp(ck, "fmt ", 4) && sz >= 16) {
            unsigned char fb[40]; memset(fb, 0, sizeof fb);
            size_t rn = sz < 40 ? sz : 40;
            if (fread(fb, 1, rn, f) != rn) break;
            fmt = fb[0] | (fb[1] << 8); ch = fb[2] | (fb[3] << 8); rate = rd32(fb + 4); bits = fb[14] | (fb[15] << 8);
            if (fmt == 0xFFFE && sz >= 26) fmt = fb[24] | (fb[25] << 8);
        } else if (!memcmp(ck, "data", 4)) { data_off = here; data_sz = sz; break; }
        fseek(f, here + (long)sz + (long)(sz & 1u), SEEK_SET);
    }
    if (data_off < 0 || ch < 1 || ch > 8 || rate < 4000 || rate > 96000 || !((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) {
        fclose(f); snprintf(err, ecap, "Unsupported WAV format"); return 0;
    }
    int bps = bits / 8;
    long frames = (long)(data_sz / (uint32_t)(bps * ch));
    long maxf = (long)rate * 60;
    if (frames > maxf) frames = maxf;
    if (frames < 16) { fclose(f); snprintf(err, ecap, "The file is too short"); return 0; }
    int16_t *out = (int16_t *)malloc((size_t)frames * 2);
    unsigned char *blk = (unsigned char *)malloc((size_t)bps * ch * 4096);
    if (!out || !blk) { free(out); free(blk); fclose(f); snprintf(err, ecap, "Out of memory"); return 0; }
    fseek(f, data_off, SEEK_SET);
    long done = 0;
    while (done < frames) {
        long nf = frames - done; if (nf > 4096) nf = 4096;
        if (fread(blk, (size_t)bps * ch, (size_t)nf, f) != (size_t)nf) { frames = done; break; }
        for (long i = 0; i < nf; i++) {
            float acc = 0.0f; int cc = ch < 2 ? ch : 2;
            for (int c = 0; c < cc; c++) {
                const unsigned char *q = blk + (size_t)(i * ch + c) * (size_t)bps;
                float v;
                if (fmt == 3) { float fl; memcpy(&fl, q, 4); v = fl * 32767.0f; }
                else if (bits == 8) v = ((float)q[0] - 128.0f) * 256.0f;
                else if (bits == 16) v = (float)(int16_t)(q[0] | (q[1] << 8));
                else if (bits == 24) v = (float)((int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 16);
                else v = (float)((int32_t)rd32(q) >> 16);
                acc += v;
            }
            acc /= (float)cc;
            int iv = (int)(acc + (acc < 0 ? -0.5f : 0.5f));
            out[done + i] = (int16_t)clamp16(iv);
        }
        done += nf;
    }
    free(blk); fclose(f);
    if (frames < 16) { free(out); snprintf(err, ecap, "The file is too short"); return 0; }
    for (int i = 0; i < 24 && i < frames / 2; i++) {          /* края сэмпла в ноль: нет щелчка на старте, в конце и на стыке петли */
        out[i] = (int16_t)((int)out[i] * i / 24);
        out[frames - 1 - i] = (int16_t)((int)out[frames - 1 - i] * i / 24);
    }
    Pad *p = &g_pad[pi];
    uint8_t pk[PAD_PK];
    for (int c = 0; c < PAD_PK; c++) {
        long a = frames * c / PAD_PK, b = frames * (c + 1) / PAD_PK;
        if (b <= a) b = a + 1;
        int mx = 0;
        for (long i = a; i < b && i < frames; i++) { int v = out[i] < 0 ? -out[i] : out[i]; if (v > mx) mx = v; }
        pk[c] = (uint8_t)(mx >> 7 > 255 ? 255 : mx >> 7);
    }
    if (p->d) {
        pad_quiet(pi);
        p->loading = 1;                                /* аудиопоток снимает голоса этого пэда, потом можно менять буфер */
        sceKernelDelayThread(60000);
    }
    memcpy(p->pk, pk, sizeof(pk));
    int16_t *old = p->d;
    p->d = out; p->n = (int)frames; p->rate = (int)rate;
    snprintf(p->path, sizeof(p->path), "%s", path);
    const char *b = strrchr(path, '/');
    snprintf(p->name, sizeof(p->name), "%s", b ? b + 1 : path);
    size_t nl = strlen(p->name);
    if (nl > 4 && strcasecmp(p->name + nl - 4, ".wav") == 0) p->name[nl - 4] = 0;
    p->loading = 0;
    free(old);
    return 1;
}

/* ---- настройки пэдов: файл pads.cfg ---- */
typedef struct { char path[160]; int32_t v[12]; } PadCfgRec;
static void pad_cfg_save(void) {
    FILE *f = fopen(PAD_CFG, "wb");
    if (!f) return;
    uint32_t hdr[2] = { 0x50414443u, 2u };
    fwrite(hdr, 4, 2, f);
    for (int i = 0; i < PAD_N; i++) {
        PadCfgRec r; memset(&r, 0, sizeof r);
        Pad *p = &g_pad[i];
        snprintf(r.path, sizeof r.path, "%s", p->d ? p->path : "");
        r.v[0] = p->vol; r.v[1] = p->pitch; r.v[2] = p->start; r.v[3] = p->mode; r.v[4] = p->a; r.v[5] = p->dc; r.v[6] = p->s; r.v[7] = p->r;
        r.v[8] = p->cth; r.v[9] = p->crat; r.v[10] = p->cmk; r.v[11] = p->cmx;
        fwrite(&r, sizeof r, 1, f);
    }
    fclose(f);
}
/* ---- заводской набор: звуки синтезируются в коде и при первом запуске записываются в ux0:data/Sampler/kit_*.wav ---- */
#define KIT_N 16
#define KIT_RATE 22050
static const char *const g_kit_name[KIT_N] = {
    "kit_kick", "kit_snare", "kit_hat_closed", "kit_hat_open", "kit_clap", "kit_tom_low", "kit_tom_high", "kit_rim",
    "kit_bass", "kit_sub", "kit_pad_c", "kit_pad_am", "kit_pluck", "kit_bell", "kit_riser", "kit_shaker" };
static uint32_t g_kit_rng;
static float kit_nz(void) { g_kit_rng = g_kit_rng * 1664525u + 1013904223u; return (float)(int32_t)g_kit_rng / 2147483648.0f; }
static float kit_sat(float x) { return x / (1.0f + fabsf(x)); }
/* генерирует звук k в o (не больше cap отсчётов), возвращает длину; пик нормируется до 0.9 */
static int kit_gen(int k, float *o, int cap) {
    const float sr = (float)KIT_RATE, tp = 6.2831853f;
    float ph = 0.0f, lp = 0.0f, lp2 = 0.0f, pp[8] = { 0 };
    int n = 0;
    g_kit_rng = 12345u + (uint32_t)k * 7919u;
    static const float pad_c[2][4] = { { 130.81f, 164.81f, 196.0f, 261.63f }, { 110.0f, 130.81f, 164.81f, 220.0f } };
    switch (k) {
    case 0: n = (int)(0.55f * sr); break;
    case 1: n = (int)(0.30f * sr); break;
    case 2: n = (int)(0.09f * sr); break;
    case 3: n = (int)(0.50f * sr); break;
    case 4: n = (int)(0.35f * sr); break;
    case 5: case 6: n = (int)(0.50f * sr); break;
    case 7: n = (int)(0.08f * sr); break;
    case 8: n = (int)(0.70f * sr); break;
    case 9: n = (int)(1.00f * sr); break;
    case 10: case 11: n = (int)(3.00f * sr); break;
    case 12: n = (int)(0.60f * sr); break;
    case 13: n = (int)(1.50f * sr); break;
    case 14: n = (int)(1.60f * sr); break;
    default: n = (int)(0.25f * sr); break;
    }
    if (n > cap) n = cap;
    const float dur = (float)n / sr;
    for (int i = 0; i < n; i++) {
        const float t = (float)i / sr;
        float x = 0.0f, w = kit_nz();
        switch (k) {
        case 0: {                                                  /* бочка: синус с падающей частотой + щелчок */
            ph += tp * (45.0f + 120.0f * expf(-t * 28.0f)) / sr;
            x = sinf(ph) * expf(-t * 7.0f) + (i < 40 ? w * 0.4f * (1.0f - (float)i / 40.0f) : 0.0f);
            break; }
        case 1: {                                                  /* малый: тон + шум */
            lp += 0.25f * (w - lp);
            x = sinf(tp * 185.0f * t) * expf(-t * 22.0f) * 0.55f + (w - lp) * expf(-t * 14.0f) * 0.9f;
            break; }
        case 2: lp += 0.6f * (w - lp); x = (w - lp) * expf(-t * 55.0f); break;
        case 3: lp += 0.6f * (w - lp); x = (w - lp) * expf(-t * 9.0f); break;
        case 4: {                                                  /* хлопок: три коротких шумовых удара и хвост */
            lp += 0.35f * (w - lp); lp2 += 0.05f * (lp - lp2);
            float bp = lp - lp2, e = expf(-t * 16.0f) * 0.5f;
            for (int b = 0; b < 3; b++) { float tb = t - 0.012f * (float)b; if (tb > 0.0f) e += expf(-tb * 130.0f); }
            x = bp * e * 1.6f;
            break; }
        case 5: ph += tp * (65.0f + 45.0f * expf(-t * 12.0f)) / sr; x = sinf(ph) * expf(-t * 5.5f); break;
        case 6: ph += tp * (120.0f + 70.0f * expf(-t * 14.0f)) / sr; x = sinf(ph) * expf(-t * 7.0f); break;
        case 7: x = (sinf(tp * 1700.0f * t) * 0.6f + sinf(tp * 480.0f * t)) * expf(-t * 70.0f); break;
        case 8: {                                                  /* бас: пила из 4 гармоник */
            ph += tp * 55.0f / sr;
            for (int h = 1; h <= 4; h++) x += sinf(ph * (float)h) / (float)h;
            x *= expf(-t * 4.0f) * (t < 0.005f ? t / 0.005f : 1.0f);
            break; }
        case 9: ph += tp * 41.0f / sr; x = (sinf(ph) + 0.25f * sinf(ph * 2.0f)) * expf(-t * 2.5f) * (t < 0.01f ? t / 0.01f : 1.0f); break;
        case 10: case 11: {                                        /* аккорд из расстроенных пил, мягкий фильтр, медленная атака */
            float a = 0.0f;
            for (int v = 0; v < 4; v++)
                for (int d = 0; d < 2; d++) {
                    pp[v * 2 + d] += (pad_c[k - 10][v] * (d ? 1.004f : 0.996f)) / sr;
                    if (pp[v * 2 + d] >= 1.0f) pp[v * 2 + d] -= 1.0f;
                    a += pp[v * 2 + d] * 2.0f - 1.0f;
                }
            lp += 0.10f * (a * 0.12f - lp);
            float e = (t < 0.4f ? t / 0.4f : 1.0f) * (dur - t < 0.8f ? (dur - t) / 0.8f : 1.0f);
            x = lp * e * 2.0f;
            break; }
        case 12: {                                                 /* щипок: мягко ограниченная прямоугольная волна */
            ph += tp * 440.0f / sr;
            lp += 0.35f * (kit_sat(sinf(ph) * 4.0f) - lp);
            x = lp * expf(-t * 7.0f);
            break; }
        case 13: x = sinf(tp * 660.0f * t + 3.0f * expf(-t * 5.0f) * sinf(tp * 924.0f * t)) * expf(-t * 3.5f); break;
        case 14: {                                                 /* нарастание: шум с растущим срезом */
            float r = t / dur;
            lp += (0.02f + 0.7f * r * r) * (w - lp);
            x = lp * r * (dur - t < 0.03f ? (dur - t) / 0.03f : 1.0f) * 2.0f;
            break; }
        default: {                                                 /* шейкер */
            lp += 0.5f * (w - lp);
            x = (w - lp) * (t < 0.03f ? t / 0.03f : expf(-(t - 0.03f) * 18.0f));
            break; }
        }
        o[i] = x;
    }
    float pk = 1e-6f;
    for (int i = 0; i < n; i++) if (fabsf(o[i]) > pk) pk = fabsf(o[i]);
    for (int i = 0; i < n; i++) o[i] *= 0.9f / pk;
    if (n > 64) for (int i = 0; i < 64; i++) o[n - 1 - i] *= (float)i / 64.0f;     /* короткий спад в ноль: без щелчка в конце */
    return n;
}
static int kit_write_wav(const char *path, const int16_t *d, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    unsigned char h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
    uint32_t db = (uint32_t)n * 2u, rt = KIT_RATE, br = KIT_RATE * 2u, rs = 36u + db;
    memcpy(h + 4, &rs, 4); memcpy(h + 24, &rt, 4); memcpy(h + 28, &br, 4); memcpy(h + 40, &db, 4);
    fwrite(h, 1, 44, f);
    fwrite(d, 2, (size_t)n, f);
    fclose(f);
    return 1;
}
/* записывает недостающие kit_*.wav */
static void kit_install(void) {
    static float buf[KIT_RATE * 3 + 16];
    static int16_t pcm[KIT_RATE * 3 + 16];
    for (int k = 0; k < KIT_N; k++) {
        char path[96];
        snprintf(path, sizeof path, "%s/%s.wav", "ux0:data/Sampler", g_kit_name[k]);
        FILE *t = fopen(path, "rb");
        if (t) { fclose(t); continue; }
        int n = kit_gen(k, buf, KIT_RATE * 3);
        for (int i = 0; i < n; i++) { int v = (int)(buf[i] * 32767.0f); pcm[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }
        kit_write_wav(path, pcm, n);
    }
}
/* первый запуск (нет pads.cfg): набор раскладывается по пэдам */
static void kit_assign(void) {
    for (int k = 0; k < KIT_N; k++) {
        char path[96], e[48];
        snprintf(path, sizeof path, "ux0:data/Sampler/%s.wav", g_kit_name[k]);
        if (!pad_load_wav(k, path, e, sizeof e)) continue;
        Pad *p = &g_pad[k];
        if (k == 10 || k == 11) { p->mode = 1; p->a = 22; p->r = 35; }          /* аккорды: играют, пока держишь */
        if (k == 14) { p->mode = 1; p->r = 20; }
        if (k == 9) p->vol = 90;
    }
}

static void pad_cfg_load(void) {
    for (int i = 0; i < PAD_N; i++) pad_defaults(&g_pad[i]);
    FILE *f = fopen(PAD_CFG, "rb");
    if (!f) { kit_assign(); return; }
    uint32_t hdr[2];
    if (fread(hdr, 4, 2, f) == 2 && hdr[0] == 0x50414443u && hdr[1] == 2u) {
        for (int i = 0; i < PAD_N; i++) {
            PadCfgRec r;
            if (fread(&r, sizeof r, 1, f) != 1) break;
            Pad *p = &g_pad[i];
            int *dst[12] = { &p->vol, &p->pitch, &p->start, &p->mode, &p->a, &p->dc, &p->s, &p->r, &p->cth, &p->crat, &p->cmk, &p->cmx };
            static const int mx[12] = { 100, 48, 99, 2, 100, 100, 100, 100, 100, 100, 100, 100 };
            for (int q = 0; q < 12; q++) *dst[q] = r.v[q] < 0 ? 0 : (r.v[q] > mx[q] ? mx[q] : r.v[q]);
            r.path[sizeof r.path - 1] = 0;
            if (r.path[0]) { char e[48]; pad_load_wav(i, r.path, e, sizeof e); }
        }
    }
    fclose(f);
}

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
            int did = 0, peak = 0;
            /* плавное затухание при паузе/стопе, чтобы не было щелчка */
            if (was_playing && out_port >= 0 && g_length > 1) {
                int len = g_length, lo = g_lo, hi = g_hi;
                region_sanitize(&lo, &hi, len);
                Params prm;
                load_params(&prm, lo, hi);
                eng.force_fade = 1;
                engine_render(&eng, g_audio, len, &prm, mono, &peak);
                did = 1;
            }
            /* голоса пэдов звучат и без воспроизведения дорожки */
            if (pad_busy()) {
                if (out_port < 0) {
                    out_port = open_out(&out_ch);
                    if (out_port >= 0) { g_out_ch = out_ch; out_err = 0; }
                }
                if (out_port >= 0) {
                    if (!did) memset(mono, 0, sizeof(mono));
                    pad_mix_add(mono, &peak);
                    did = 1;
                }
            }
            if (did && out_port >= 0) {
                int16_t *ob = out_bufs[bi];
                if (out_ch == 1) memcpy(ob, mono, sizeof(mono));
                else for (int i = 0; i < OUT_GRAIN; i++) { ob[2 * i] = mono[i]; ob[2 * i + 1] = mono[i]; }
                sceAudioOutOutput(out_port, ob);
                bi = (bi + 1) % N_OUT_BUFS;
                g_level = peak;
            }
            was_playing = 0;
            have_prm = 0;
            old_active = 0;
            t_last = 0;
            if (!did) sceKernelDelayThread(3000);
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
        int lo, hi, seek, rev;
        for (;;) {
            unsigned q1 = g_rg_seq;
            __sync_synchronize();
            lo = g_lo; hi = g_hi; seek = g_seek_to; rev = g_rev;
            __sync_synchronize();
            if (q1 == g_rg_seq && !(q1 & 1u)) break;
        }
        region_sanitize(&lo, &hi, len);
        const int scrub_now = g_scrub;
        const int mirror_now = rev && !scrub_now;
        const double mc = (double)(lo + hi - 1);

        if (!was_playing) {
            double p0 = (double)g_play_pos;
            if (p0 < (double)lo || p0 >= (double)hi) p0 = rev ? (double)(hi - 1) : (double)lo;
            if (mirror_now) p0 = mc - p0;
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
            engine_reset(&eng, mirror_now ? mc - (double)seek : (double)seek);
            __sync_bool_compare_and_swap(&g_seek_to, seek, -1);   /* если пришла новая перемотка - не затираем */
        }

        Params prm;
        load_params(&prm, lo, hi);
        prm.scrub = scrub_now;
        prm.rev = rev;
        int peak = 0;
        uint64_t t_calc0 = sceKernelGetProcessTimeWide();
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
        pad_mix_add(mono, &peak);
        {
            int ld = (int)((sceKernelGetProcessTimeWide() - t_calc0) * 1000 / 21333);
            static int ld_pk = 0, ld_n = 0;
            if (ld > ld_pk) ld_pk = ld;
            if (++ld_n >= 70) { g_out_load = ld_pk; ld_pk = 0; ld_n = 0; }
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
            g_play_pos = rev ? hi - 1 : lo;
            was_playing = 0;
            __sync_bool_compare_and_swap(&g_state, ST_PLAYING, ST_IDLE);
        } else if (g_state == ST_PLAYING) {
            double ep = engine_pos(&eng);
            g_play_pos = (int)(mirror_now ? mc - ep : ep);
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

static int write_wav_file(const char *path, const int16_t *d, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t bytes = (uint32_t)n * 2;
    WavHeader h;
    memcpy(h.riff, "RIFF", 4);
    h.size = 36 + bytes;
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt, "fmt ", 4);
    h.fmt_size = 16; h.format = 1; h.channels = 1;
    h.rate = SAMPLE_RATE; h.byte_rate = SAMPLE_RATE * 2; h.block_align = 2; h.bits = 16;
    memcpy(h.data, "data", 4);
    h.data_size = bytes;
    fwrite(&h, sizeof(h), 1, f);
    fwrite(d, 2, (size_t)n, f);
    fclose(f);
    return 0;
}

static int save_wav(char *path, size_t path_cap, const char *name) {
    int len = g_length;
    if (len <= 0) return -1;

    sceIoMkdir("ux0:data/Sampler", 0777);

    /* звук не менялся с прошлой выгрузки - перезаписываем тот же файл, а не плодим копии */
    g_wav_overwrote = 0;
    if (name && name[0]) {
        /* имя выбрано в диалоге: тот же файл (тот же звук) перезаписываем, чужой с таким именем - не трогаем, добавляем _2, _3... */
        snprintf(path, path_cap, "ux0:data/Sampler/%s.wav", name);
        if (g_wav_path[0] && g_wav_ver == g_audio_ver && strcmp(path, g_wav_path) == 0) g_wav_overwrote = 1;
        else {
            for (int i = 2; i < 1000; i++) {
                FILE *t = fopen(path, "rb");
                if (!t) break;
                fclose(t);
                snprintf(path, path_cap, "ux0:data/Sampler/%s_%d.wav", name, i);
            }
        }
    } else if (g_wav_path[0] && g_wav_ver == g_audio_ver) {
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
#define UNDO_N 4
typedef struct { int16_t *d; int len, c1, c2; } UndoSlot;
static UndoSlot g_us[UNDO_N];
static int g_un = 0;                               /* сколько шагов отмены сохранено */
static void undo_clear(void) {
    for (int i = 0; i < g_un; i++) { free(g_us[i].d); g_us[i].d = NULL; }
    g_un = 0;
}
/* запомнить текущий звук перед изменением; 0 - не хватило памяти */
static int undo_push(int c1, int c2) {
    int len = g_length;
    int16_t *p = (int16_t *)malloc((size_t)(len > 0 ? len : 1) * sizeof(int16_t));
    if (!p) return 0;
    memcpy(p, g_audio, (size_t)len * sizeof(int16_t));
    if (g_un == UNDO_N) {
        free(g_us[0].d);
        for (int k = 1; k < UNDO_N; k++) g_us[k - 1] = g_us[k];
        g_un--;
    }
    g_us[g_un].d = p; g_us[g_un].len = len; g_us[g_un].c1 = c1; g_us[g_un].c2 = c2;
    g_un++;
    return 1;
}

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
    if (!undo_push(c1, c2)) return -2;
    memmove(g_audio, g_audio + lo, (size_t)(hi - lo) * sizeof(int16_t));
    g_length = hi - lo;
    recompute_blocks(g_length);
    g_audio_ver++;
    return g_length;
}

/* Вернуть звук, каким он был до обрезки. Возвращает длину или -1. */
static int audio_undo(int *c1, int *c2) {
    if (g_un <= 0) return -1;
    UndoSlot *u = &g_us[g_un - 1];
    memcpy(g_audio, u->d, (size_t)u->len * sizeof(int16_t));
    g_length = u->len;
    *c1 = u->c1; *c2 = u->c2;
    free(u->d); u->d = NULL;
    g_un--;
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

    /* сглаженная огибающая громкости */
    static float ls[ON_MAXF];
    for (int f = 0; f < nf; f++) {
        float a0 = lf[f > 0 ? f - 1 : 0], a2 = lf[f + 1 < nf ? f + 1 : nf - 1];
        ls[f] = 0.25f * a0 + 0.5f * lf[f] + 0.25f * a2;
    }
    /* одиночные щелчки: если после "атаки" громкость не держится, значимость снижаем */
    for (int f = 0; f < nf; f++) {
        float pre = 0.0f, post = 0.0f; int np_ = 0, nq = 0;
        for (int k = (f > 6 ? f - 6 : 0); k < f; k++) { pre += ls[k]; np_++; }
        for (int k = f + 2; k <= f + 14 && k < nf; k++) { post += ls[k]; nq++; }
        if (np_ > 0 && nq > 0 && post / (float)nq - pre / (float)np_ < 0.25f) g_nov[f] *= 0.3f;
    }
    /* провалы громкости (паузы между словами и нотами): граница ставится там, где звук снова нарастает */
    const int WN = 25;                                    /* ~200 мс */
    for (int f = 1; f < nf - 1; f++) {
        if (!(ls[f] <= ls[f - 1] && ls[f] < ls[f + 1])) continue;
        float lm = ls[f], rm = ls[f];
        for (int k = (f > WN ? f - WN : 0); k <= f; k++) if (ls[k] > lm) lm = ls[k];
        for (int k = f; k <= f + WN && k < nf; k++) if (ls[k] > rm) rm = ls[k];
        float depth = (lm < rm ? lm : rm) - ls[f];
        if (depth < 0.45f) continue;
        int rise = f;
        float lvl = ls[f] + 0.5f * (rm - ls[f]);
        for (int k = f; k <= f + WN && k < nf; k++) { rise = k; if (ls[k] >= lvl) break; }
        float add = 0.9f * depth; if (add > 3.0f) add = 3.0f;
        g_nov[rise] += add;
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

/* ---------- Темп сэмпла: автокорреляция "новизны" внутри области ---------- */
static int g_sl_bpm = 0;                      /* 0 - темп не определён */

static int snap_zero(const int16_t *a, int len, int x) {
    int best = x, bestd = 1 << 30;
    for (int d = -40; d <= 40; d++) {
        int i = x + d;
        if (i < 1 || i >= len - 1) continue;
        if (a[i - 1] <= 0 && a[i] > 0 && abs(d) < bestd) { bestd = abs(d); best = i; }
    }
    return best;
}

/* возвращает 1, если темп уверенный; T - период в сэмплах, g0 - позиция одной из долей */
static int tempo_estimate(int rlo, int rhi, double *T_smp, double *g0_smp) {
    static float x[ON_MAXF];
    int f0 = rlo / ON_HOP, f1 = rhi / ON_HOP;
    if (f1 > g_nov_frames) f1 = g_nov_frames;
    int n = f1 - f0;
    g_sl_bpm = 0;
    if (n < 300) return 0;                                  /* меньше ~2.4 с - темп не определить */
    double mean = 0.0;
    for (int i = 0; i < n; i++) mean += g_nov[f0 + i];
    mean /= (double)n;
    double e0 = 0.0;
    for (int i = 0; i < n; i++) { x[i] = g_nov[f0 + i] - (float)mean; e0 += (double)x[i] * x[i]; }
    if (e0 < 1e-6) return 0;
    e0 /= (double)n;
    int lmin = 40, lmax = 110;                              /* 187..68 уд/мин при 125 кадрах/с */
    if (lmax > n / 3) lmax = n / 3;
    double best = -1e9, ac_best = 0.0; int bl = 0;
    static double acs[130];
    for (int lag = lmin - 1; lag <= lmax + 1 && lag < 128; lag++) {
        double a = 0.0;
        for (int i = 0; i + lag < n; i++) a += (double)x[i] * x[i + lag];
        acs[lag] = a / (double)(n - lag);
    }
    for (int lag = lmin; lag <= lmax; lag++) {
        double a2 = 0.0;
        if (2 * lag < n) {
            for (int i = 0; i + 2 * lag < n; i++) a2 += (double)x[i] * x[i + 2 * lag];
            a2 /= (double)(n - 2 * lag);
        }
        double sc = acs[lag] + 0.5 * a2;
        if (sc > best) { best = sc; bl = lag; ac_best = acs[lag]; }
    }
    if (bl == 0 || ac_best / e0 < 0.18) return 0;          /* биение выражено слабо */
    double Tf = (double)bl;
    {   /* уточнение периода параболой */
        double ym = acs[bl - 1], y0 = acs[bl], yp = acs[bl + 1];
        double den = ym - 2.0 * y0 + yp;
        if (den < -1e-12) Tf += 0.5 * (ym - yp) / den;
    }
    /* фаза: смещение сетки, при котором на доли попадает больше новизны */
    double bestsum = -1e30; int bph = 0;
    for (int ph = 0; ph < bl; ph++) {
        double sum = 0.0;
        for (double p = (double)ph; p < (double)n - 1.0; p += Tf) sum += x[(int)(p + 0.5)];
        if (sum > bestsum) { bestsum = sum; bph = ph; }
    }
    double bpm = 60.0 * (16000.0 / (double)ON_HOP) / Tf;
    while (bpm < 80.0) bpm *= 2.0;
    while (bpm > 165.0) bpm *= 0.5;
    g_sl_bpm = (int)(bpm + 0.5);
    *T_smp = Tf * (double)ON_HOP;
    *g0_smp = (double)(f0 + bph) * (double)ON_HOP;
    return 1;
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

    /* подгон под темп: границы, близкие к долям, садятся точно на сетку */
    double Ts = 0.0, G0 = 0.0;
    int have_grid = tempo_estimate(rlo, rhi, &Ts, &G0);
    if (have_grid) {
        for (int j = 0; j < np; j++) {
            double k = floor(((double)pos[j] - G0) / Ts + 0.5);
            double g = G0 + k * Ts;
            if (fabs((double)pos[j] - g) <= 0.12 * Ts) pos[j] = snap_zero(a, g_length, (int)(g + 0.5));
        }
        qsort(pos, (size_t)np, sizeof(int), int_cmp);
        int m = 0;                                          /* после сдвига убираем слишком близкие и вышедшие за область */
        for (int j = 0; j < np; j++) {
            if (pos[j] < rlo + SL_MIN_LEN || pos[j] > rhi - SL_MIN_LEN) continue;
            if (m > 0 && pos[j] - pos[m - 1] < SL_MIN_LEN) continue;
            pos[m++] = pos[j];
        }
        np = m;
    }

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
        if (have_grid) {                                    /* делим по ближайшей доле, а не ровно пополам */
            double k = floor(((double)mid - G0) / Ts + 0.5);
            int gm = (int)(G0 + k * Ts + 0.5);
            if (gm > start + SL_MIN_LEN && gm < start + bestgap - SL_MIN_LEN) mid = snap_zero(a, g_length, gm);
        }
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

static void slices_ensure_nov(void) {
    if (!g_nov_ready || g_nov_ver != g_audio_ver) {
        onset_analyze(g_audio, g_length);
        g_nov_ver = g_audio_ver;
        g_nov_ready = 1;
    }
}

/* исходный темп = найденный (или 120, если не нашёлся); reset - сбросить целевой темп на исходный */
static void bpm_sync(int reset) {
    int b = g_sl_bpm > 0 ? g_sl_bpm : 120;
    if (reset || b != g_bpm_base) { g_bpm_base = b; g_bpm_target = b; g_tempo_mul = 1.0f; }
}

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
    bpm_sync(1);
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

/* шрифт: DejaVu Sans Condensed Bold и DejaVu Sans Mono Bold из app0:fonts; если файла нет - запасной системный */
static vita2d_font *g_font = NULL, *g_font_m = NULL;
static void fonts_load(void) {
    g_font = vita2d_load_font_file("app0:fonts/cond.ttf");
    g_font_m = vita2d_load_font_file("app0:fonts/mono.ttf");
}
static void fonts_free(void) {
    if (g_font) { vita2d_free_font(g_font); g_font = NULL; }
    if (g_font_m) { vita2d_free_font(g_font_m); g_font_m = NULL; }
}
static void text(int x, int y, unsigned int c, float s, const char *t) {
    if (g_font) vita2d_font_draw_text(g_font, x, y, c, (unsigned int)(s * 17.0f + 0.5f), t);
    else vita2d_pgf_draw_text(g_pgf, x, y, c, s, t);
}
static void text_m(int x, int y, unsigned int c, float s, const char *t) {
    vita2d_font *f = g_font_m ? g_font_m : g_font;
    if (f) vita2d_font_draw_text(f, x, y, c, (unsigned int)(s * 17.0f + 0.5f), t);
    else vita2d_pgf_draw_text(g_pgf, x, y, c, s, t);
}
static int text_w(const char *t, float s) {
    if (g_font) return vita2d_font_text_width(g_font, (unsigned int)(s * 17.0f + 0.5f), t);
    return (int)((float)strlen(t) * 9.6f * s);
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
    float gain = 0.8f * 32768.0f / (float)peak;      /* лёгкий подъём тихих записей, но не до упора: громкость видна */
    if (gain > 3.0f) gain = 3.0f;
    if (gain < 1.0f) gain = 1.0f;

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
            {   /* яркая серединка: волна "горит" ярче у нулевой линии, как люминофор */
                int yt2 = cy - (int)(fmx * half * 0.5f), yb2 = cy - (int)(fmn * half * 0.5f);
                if (yb2 - yt2 < 1) { yt2 = cy - 1; yb2 = cy + 1; }
                uint32_t col2 = live ? RGBA8(255, 170, 150, 255) : RGBA8(255, 208, 128, 255);
                for (int y = yt2; y <= yb2; y++) if (y >= ytop && y <= ybot) px[y * stride + x0 + x] = col2;
            }
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
/* палитра */

typedef struct { uint32_t *px; int w, h; } Canvas;

/* ---------- PNG-спрайты (ручки, клавиши, лампочки); если файла нет — рисуем процедурно ---------- */
#ifndef ASSET_PREFIX
#define ASSET_PREFIX "app0:assets/"
#endif
typedef struct { uint32_t *px; int w, h; } Img;
static Img g_img_knobs, g_img_keys, g_img_lamps, g_img_body;

static int img_load(Img *im, const char *name) {
    char path[256];
    snprintf(path, sizeof path, "%s%s", ASSET_PREFIX, name);
    png_image pi;
    memset(&pi, 0, sizeof pi);
    pi.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&pi, path)) return 0;
    pi.format = PNG_FORMAT_RGBA;
    size_t sz = PNG_IMAGE_SIZE(pi);
    uint32_t *buf = (uint32_t *)malloc(sz);
    if (!buf) { png_image_free(&pi); return 0; }
    if (!png_image_finish_read(&pi, NULL, buf, 0, NULL)) { free(buf); png_image_free(&pi); return 0; }
    im->px = buf; im->w = (int)pi.width; im->h = (int)pi.height;
    return 1;
}
static void assets_init(void) {
    img_load(&g_img_knobs, "knobs.png");
    img_load(&g_img_keys, "keys.png");
    img_load(&g_img_lamps, "lamps.png");
    img_load(&g_img_body, "body.png");
}

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

/* копия прямоугольника спрайта (alpha-смешивание) */
static void cv_blit(Canvas *c, const Img *im, int sx, int sy, int sw, int sh, int dx, int dy, float alpha) {
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            cv_put(c, dx + x, dy + y, im->px[(sy + y) * im->w + sx + x], alpha);
}
/* спрайт с поворотом вокруг центра (билинейно), угол a в радианах по часовой от «вверх» */
static void cv_blit_rot(Canvas *c, const Img *im, int sx, int sy, int sz, int cx, int cy, float a, float alpha) {
    float sn = sinf(a), cs = cosf(a), h = sz * 0.5f;
    for (int y = 0; y < sz; y++)
        for (int x = 0; x < sz; x++) {
            float px = x + 0.5f - h, py = y + 0.5f - h;           /* точка назначения от центра */
            float u = px * cs + py * sn + h - 0.5f, v = -px * sn + py * cs + h - 0.5f;
            int u0 = (int)floorf(u), v0 = (int)floorf(v);
            float fu = u - u0, fv = v - v0, acc[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 4; k++) {
                int xx = u0 + (k & 1), yy = v0 + (k >> 1);
                float wgt = ((k & 1) ? fu : 1 - fu) * ((k >> 1) ? fv : 1 - fv);
                if (xx < 0 || yy < 0 || xx >= sz || yy >= sz) continue;
                uint32_t p = im->px[(sy + yy) * im->w + sx + xx];
                float pa = (float)(p >> 24) * wgt;
                acc[0] += (float)(p & 255) * pa; acc[1] += (float)((p >> 8) & 255) * pa;
                acc[2] += (float)((p >> 16) & 255) * pa; acc[3] += pa;
            }
            if (acc[3] <= 0.5f) continue;
            uint32_t col = RGBA8((int)(acc[0] / acc[3] + 0.5f), (int)(acc[1] / acc[3] + 0.5f),
                                 (int)(acc[2] / acc[3] + 0.5f), (int)(acc[3] + 0.5f));
            cv_put(c, cx - (int)h + x, cy - (int)h + y, col, alpha);
        }
}
/* 9-slice: спрайт sz×sz, угол m; середина растягивается (ближайший пиксель); приёмник dw×dh */
static void cv_blit9(Canvas *c, const Img *im, int sx, int sy, int sz, int m, int dx, int dy, int dw, int dh) {
    int mid = sz - 2 * m, dmx = dw - 2 * m, dmy = dh - 2 * m;
    for (int y = 0; y < dh; y++) {
        float ty = y < m ? (float)y : (y >= dh - m ? (float)(sz - (dh - y)) : (float)m + ((float)(y - m) + 0.5f) * (float)mid / (float)dmy - 0.5f);
        for (int x = 0; x < dw; x++) {
            float tx = x < m ? (float)x : (x >= dw - m ? (float)(sz - (dw - x)) : (float)m + ((float)(x - m) + 0.5f) * (float)mid / (float)dmx - 0.5f);
            if (tx < 0.0f) tx = 0.0f;
            if (ty < 0.0f) ty = 0.0f;
            int x0 = (int)tx, y0 = (int)ty, x1 = x0 + 1 < sz ? x0 + 1 : x0, y1 = y0 + 1 < sz ? y0 + 1 : y0;
            float fx = tx - (float)x0, fy = ty - (float)y0;
            uint32_t p00 = im->px[(sy + y0) * im->w + sx + x0], p10 = im->px[(sy + y0) * im->w + sx + x1];
            uint32_t p01 = im->px[(sy + y1) * im->w + sx + x0], p11 = im->px[(sy + y1) * im->w + sx + x1];
            uint32_t out = 0;
            for (int sh = 0; sh < 32; sh += 8) {                    /* билинейно по каналам: растянутая середина не даёт блоков */
                float a0 = (float)((p00 >> sh) & 255) * (1 - fx) + (float)((p10 >> sh) & 255) * fx;
                float a1 = (float)((p01 >> sh) & 255) * (1 - fx) + (float)((p11 >> sh) & 255) * fx;
                out |= (uint32_t)(a0 * (1 - fy) + a1 * fy + 0.5f) << sh;
            }
            cv_put(c, dx + x, dy + y, out, 1.0f);
        }
    }
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
enum { IC_REC, IC_PLAY, IC_PAUSE, IC_STOP, IC_LOOP, IC_FX, IC_CROP, IC_UNDO, IC_SAVE, IC_REV, IC_NORM, IC_PLUS, IC_MINUS };

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
    case IC_REV:
        cv_tri(c, cx - 1, cy - 9, cx - 1, cy + 9, cx - 12, cy, col);
        cv_tri(c, cx + 11, cy - 9, cx + 11, cy + 9, cx, cy, col);
        break;
    case IC_NORM: {
        cv_rect(c, (int)cx - 12, (int)cy - 12, 24, 2, col);
        cv_rect(c, (int)cx - 12, (int)cy + 10, 24, 2, col);
        static const int bh[5] = { 6, 12, 20, 12, 6 };
        for (int k = 0; k < 5; k++) cv_rect(c, (int)cx - 11 + k * 5, (int)cy - bh[k] / 2, 3, bh[k], col);
        break; }
    case IC_PLUS:  cv_rect(c, (int)cx - 10, (int)cy - 2, 20, 4, col); cv_rect(c, (int)cx - 2, (int)cy - 10, 4, 20, col); break;
    case IC_MINUS: cv_rect(c, (int)cx - 10, (int)cy - 2, 20, 4, col); break;
    }
}

/* ---------- квадратные клавиши: 0 кремовая, 1 оранжевая, 2 красная, 3 зелёная, 4 серая ---------- */
static void cv_key(Canvas *c, int x, int y, int w, int h, int style, int pressed) {
    if (g_img_keys.px && style != 2) {
        int col = style == 1 ? 1 : (style == 3 ? 2 : (style == 4 ? 3 : 0));
        cv_blit9(c, &g_img_keys, col * 52, pressed ? 52 : 0, 52, 22, x - 8, y - 5, w + 16, h + 16);
        return;
    }
    uint32_t top, bot, edge, side;
    switch (style) {
    case 1:  top = K_ORANGE_HI; bot = K_ORANGE; edge = K_ORANGE_LO; side = RGBA8(150, 58, 12, 255); break;
    case 2:  top = RGBA8(240, 98, 82, 255); bot = K_RED; edge = RGBA8(150, 40, 30, 255); side = RGBA8(120, 34, 26, 255); break;
    case 3:  top = RGBA8(120, 206, 130, 255); bot = K_GREEN; edge = RGBA8(48, 120, 62, 255); side = RGBA8(40, 100, 52, 255); break;
    case 4:  top = RGBA8(226, 224, 216, 255); bot = RGBA8(188, 185, 176, 255); edge = RGBA8(122, 116, 102, 255); side = RGBA8(134, 128, 114, 255); break;
    default: top = RGBA8(249, 245, 232, 255); bot = RGBA8(219, 213, 195, 255); edge = RGBA8(150, 142, 120, 255); side = RGBA8(170, 162, 140, 255); break;
    }
    int off = pressed ? 2 : 0;
    if (!pressed) cv_rrect(c, (float)x, (float)(y + 3), (float)w, (float)h, 3.0f, side);       /* нижняя грань клавиши */
    if (pressed) { uint32_t t = top; top = bot; bot = t; }
    cv_rrect_grad(c, (float)x, (float)(y + off), (float)w, (float)h, 3.0f, top, bot);
    cv_rrect_stroke(c, (float)x, (float)(y + off), (float)w, (float)h, 3.0f, 1.0f, edge);
    if (!pressed) cv_rrect(c, (float)(x + 3), (float)(y + 1.5f), (float)(w - 6), 1.2f, 0.6f, RGBA8(255, 255, 255, style ? 90 : 120));
}

/* линия из кружков (для стрелки ручки) */
static void cv_line(Canvas *c, float x0, float y0, float x1, float y1, float r, uint32_t col) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    int n = (int)(len / (r * 0.8f)) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n;
        cv_circle(c, x0 + dx * t, y0 + dy * t, r, col);
    }
}

/* ручка: v 0..1, стрелка и точки шкалы; управляется только касанием экрана */
static void cv_knob(Canvas *c, int cx, int cy, int r, float v, int active, int dim) {
    int ri = r <= 20 ? 0 : (r <= 26 ? 1 : (r <= 28 ? 2 : 3));
    if (g_img_knobs.px && g_img_knobs.h >= 4 * 88) {
        cv_blit(c, &g_img_knobs, active ? 88 : 0, ri * 88, 88, 88, cx - 44, cy - 44, 1.0f);
        cv_blit_rot(c, &g_img_knobs, 176, ri * 88, 88, cx, cy, (-135.0f + 270.0f * v) * 3.14159265f / 180.0f, dim ? 0.5f : 1.0f);
        for (int k = 0; k < 11; k++) {
            float t = (float)k / 10.0f, b = (-135.0f + 270.0f * t) * 3.14159265f / 180.0f;
            int lit = t <= v + 1e-4f && !dim;
            cv_circle(c, cx + sinf(b) * (r + 10.0f), cy - cosf(b) * (r + 10.0f), lit ? 1.9f : 1.3f, lit ? K_ORANGE : RGBA8(150, 138, 112, 255));
        }
        return;
    }
    cv_shadow(c, (float)(cx - r - 3), (float)(cy - r - 3), (float)(2 * r + 6), (float)(2 * r + 6), (float)(r + 3), 3.0f);
    cv_circle(c, (float)cx, (float)cy, (float)r + 3.0f, active ? K_ORANGE : RGBA8(120, 112, 94, 255));
    cv_circle(c, (float)cx, (float)cy, (float)r + 1.5f, RGBA8(120, 112, 94, 255));
    for (int i = 0; i < 12; i++) {
        float t = (float)i / 11.0f;
        cv_circle(c, (float)cx - t * 3.0f, (float)cy - t * 3.4f, (float)r * (1.0f - t * 0.88f),
                  lerp_col(RGBA8(184, 178, 162, 255), RGBA8(252, 248, 236, 255), t));
    }
    cv_ring(c, (float)cx, (float)cy, (float)r, 1.2f, RGBA8(110, 102, 84, 255));
    cv_ring(c, (float)cx, (float)cy, (float)r * 0.58f, 1.0f, RGBA8(176, 168, 146, 255));
    float a = (-135.0f + 270.0f * v) * 3.14159265f / 180.0f;
    float sn = sinf(a), cs = cosf(a);
    cv_line(c, cx + sn * r * 0.30f, cy - cs * r * 0.30f, cx + sn * r * 0.86f, cy - cs * r * 0.86f, 1.7f, dim ? K_BROWN_LO : K_BROWN);
    for (int k = 0; k < 11; k++) {
        float t = (float)k / 10.0f, b = (-135.0f + 270.0f * t) * 3.14159265f / 180.0f;
        int lit = t <= v + 1e-4f && !dim;
        cv_circle(c, cx + sinf(b) * (r + 10.0f), cy - cosf(b) * (r + 10.0f), lit ? 1.9f : 1.3f, lit ? K_ORANGE : RGBA8(150, 138, 112, 255));
    }
}

/* лампочка: цветной ободок, стеклянный купол, блик и ореол (по образцам: маленькая круглая линза) */
static void cv_lamp(Canvas *c, int cx, int cy, int r, uint32_t col, int on) {
    if (g_img_lamps.px) {
        int ci = (col & 255) > 200 && ((col >> 8) & 255) < 100 ? 0 : (((col >> 8) & 255) > 200 ? 1 : 2);
        cv_blit(c, &g_img_lamps, (ci * 2 + (on ? 1 : 0)) * 40, 0, 40, 40, cx - 20, cy - 20, 1.0f);
        return;
    }
    cv_circle(c, (float)cx, (float)cy + 1.2f, (float)r + 3.0f, RGBA8(0, 0, 0, 46));               /* тень */
    cv_circle(c, (float)cx, (float)cy, (float)r + 2.6f, RGBA8(52, 40, 32, 255));                  /* тёмная оправа */
    cv_circle(c, (float)cx, (float)cy, (float)r + 1.7f, lerp_col(col, RGBA8(60, 48, 38, 255), on ? 0.0f : 0.45f));   /* цветной ободок */
    cv_circle(c, (float)cx, (float)cy, (float)r + 0.4f, RGBA8(30, 22, 18, 255));
    if (on) {
        uint32_t cc = col & 0x00FFFFFFu;
        cv_circle(c, (float)cx, (float)cy, r * 2.6f, cc | (14u << 24));
        cv_circle(c, (float)cx, (float)cy, r * 2.0f, cc | (22u << 24));
        cv_circle(c, (float)cx, (float)cy, r * 1.5f, cc | (34u << 24));
    }
    uint32_t base = on ? col : lerp_col(col, RGBA8(40, 32, 28, 255), 0.82f);
    for (int i = 0; i < 9; i++) {
        float t = (float)i / 8.0f;
        cv_circle(c, (float)cx - t * 1.3f, (float)cy - t * 1.5f, (float)r * (1.0f - t * 0.8f),
                  lerp_col(lerp_col(base, RGBA8(0, 0, 0, 255), 0.5f), lerp_col(base, RGBA8(255, 255, 255, 255), on ? 0.6f : 0.14f), t));
    }
    cv_rrect(c, cx - r * 0.6f, cy - r * 0.72f, r * 0.62f, r * 0.42f, r * 0.21f, RGBA8(255, 255, 255, on ? 200 : 90));
}

/* сегментные цифры (7 сегментов) */
static const char *SEGMAP[] = { "abcdef", "bc", "abdeg", "abcdg", "bcfg", "acdfg", "acdefg", "abc", "abcdefg", "abcdfg" };
static float seg_adv(float h) { return h * 0.56f + h * 0.2f; }
static float seg_width(const char *s, float h) {
    float w = 0.0f;
    for (; *s; s++) w += (*s == '.') ? h * 0.14f : seg_adv(h);
    return w > 0.0f ? w - h * 0.2f : 0.0f;
}
static void cv_seg_text(Canvas *c, float x, float y, float h, const char *s, uint32_t col) {
    const float w = h * 0.56f, t = h * 0.13f, g = t * 0.2f;
    uint32_t dim = lerp_col(col, RGBA8(20, 12, 8, 255), 0.82f);
    for (; *s; s++) {
        if (*s == '.') { cv_circle(c, x + h * 0.04f, y + h - t * 0.6f, t * 0.55f, col); x += h * 0.14f; continue; }
        const char *on = (*s >= '0' && *s <= '9') ? SEGMAP[*s - '0'] : (*s == '-' ? "g" : "");
        float xl = x + t * 0.5f, xr = x + w - t * 0.5f, ym = y + h * 0.5f, yb = y + h - t * 0.5f, yt = y + t * 0.5f;
        for (char k = 'a'; k <= 'g'; k++) {
            uint32_t cc = strchr(on, k) ? col : dim;
            switch (k) {
            case 'a': cv_rrect(c, xl + g + t * 0.5f, y, w - t - 2 * g, t, t * 0.45f, cc); break;
            case 'g': cv_rrect(c, xl + g + t * 0.5f, ym - t * 0.5f, w - t - 2 * g, t, t * 0.45f, cc); break;
            case 'd': cv_rrect(c, xl + g + t * 0.5f, y + h - t, w - t - 2 * g, t, t * 0.45f, cc); break;
            case 'f': cv_rrect(c, x, yt + g + t * 0.5f, t, h * 0.5f - t - 2 * g, t * 0.45f, cc); break;
            case 'e': cv_rrect(c, x, ym + g, t, h * 0.5f - t * 0.5f - 2 * g - t * 0.2f, t * 0.45f, cc); break;
            case 'b': cv_rrect(c, xr - t * 0.5f, yt + g + t * 0.5f, t, h * 0.5f - t - 2 * g, t * 0.45f, cc); break;
            case 'c': cv_rrect(c, xr - t * 0.5f, ym + g, t, h * 0.5f - t * 0.5f - 2 * g - t * 0.2f, t * 0.45f, cc); break;
            }
        }
        (void)yb;
        x += seg_adv(h);
    }
}
static void cv_seg_box(Canvas *c, int x, int y, int w, int h, const char *s, uint32_t col, float dh) {
    cv_rrect(c, (float)x, (float)y, (float)w, (float)h, 3.0f, RGBA8(26, 16, 10, 255));
    cv_rrect_stroke(c, (float)x, (float)y, (float)w, (float)h, 3.0f, 1.0f, RGBA8(10, 6, 4, 255));
    float tw_ = seg_width(s, dh);
    cv_seg_text(c, (float)x + ((float)w - tw_) * 0.5f, (float)y + ((float)h - dh) * 0.5f, dh, s, col);
}

/* ====================================================================
 *  Раскладка. Экран 960x544: сверху лампы и BPM, слева названия режимов, в центре стеклянное окно
 *  с ЭЛТ, справа шкала уровня, снизу панель управления
 * ==================================================================== */
enum {
    B_NONE = -1,
    B_CROP, B_UNDO, B_REV, B_NORM,
    B_SL_MINUS, B_SL_PLUS, B_BPM_MINUS, B_BPM_PLUS,
    B_TAB0, B_TAB1, B_TAB2, B_TAB3, B_TAB4, B_TAB5,
    B_ADV0, B_ADV1, B_ADV2, B_ADV3,
    B_GR_ON, B_FSEC0, B_FSEC1, B_FT0, B_FT1, B_FT2, B_FT3, B_CURVE, B_PLOAD, B_PMODE, B_PENV, B_PCMP, B_PCLR,
    B_KNOB0 = 60,
    B_PAD0 = 100
};

#define PADS_X 316
#define PADS_Y 372
#define PADS_W 470
#define PADS_H 150
#define CURVE_X 316
#define CURVE_Y 410
#define CURVE_W 340
#define CURVE_H 104

static void pad_layout(int n, int *cols, int *rows) {
    int r = n <= 4 ? 1 : (n <= 16 ? 2 : 4);
    *rows = r;
    *cols = (n + r - 1) / r;
}

/* пады всегда квадратные, группа центрируется в своём поле */
static void pad_geom(int i, int n, int *x, int *y, int *w, int *h) {
    int cols, rows;
    pad_layout(n, &cols, &rows);
    int gap = 6;
    int sw = (PADS_W - (cols - 1) * gap) / cols, sh = (PADS_H - (rows - 1) * gap) / rows;
    int s = sw < sh ? sw : sh;
    int gw = cols * s + (cols - 1) * gap, gh = rows * s + (rows - 1) * gap;
    int ox = PADS_X + (PADS_W - gw) / 2, oy = PADS_Y + (PADS_H - gh) / 2;
    *x = ox + (i % cols) * (s + gap);
    *y = oy + (i / cols) * (s + gap);
    *w = *h = s;
}

/* сетка пэдов PAD: 4x4 */
static void pad5_geom(int i, int *x, int *y, int *w, int *h) {
    *w = 116; *h = 104;
    *x = 432 + (i % 4) * 130;
    *y = 52 + (i / 4) * 118;
}

/* ---------- ручки ---------- */
typedef struct {
    volatile int *v; int n, def;
    const char *label;
    int cx, cy, r;
    int pages;                /* битовая маска страниц, на которых ручка видна */
    int sec;                  /* страница FX: раздел, в котором ручка видна (-1 = везде) */
} KnobDef;
enum { PG_EDIT = 1, PG_SLICE = 2, PG_GRAIN = 4, PG_FX = 8, PG_ADV = 16, PG_ALL = 31, PG_PAD = 32 };
static const int g_tab_page[6] = { 0, 1, 2, 4, 3, 5 };      /* вкладки сверху: EDIT SLICE GRAIN ADV FX PAD -> номер страницы */
#define N_KNOBS 23
static KnobDef g_kd[N_KNOBS];

static void knobs_init(void) {
    static const KnobDef k[N_KNOBS] = {
        { &g_fx[2],     311, 200, "SPEED",    60, 438, 26, PG_ALL,   -1 },
        { &g_fx[3],      49,  24, "PITCH",   152, 438, 26, PG_ALL,   -1 },
        { &g_fx[5],     151,  75, "VOLUME",  244, 438, 26, PG_ALL,   -1 },
        { &g_gr_mix,    101,  70, "MIX",     344, 436, 28, PG_GRAIN, -1 },
        { &g_gr_n,       16,   7, "GRAINS",  440, 436, 28, PG_GRAIN, -1 },
        { &g_gr_size,   101,  48, "SIZE",    536, 436, 28, PG_GRAIN, -1 },
        { &g_gr_spr,    101,  40, "SPREAD",  632, 436, 28, PG_GRAIN, -1 },
        { &g_flt_c,     101,  60, "CENTER",  860, 440, 20, PG_FX,     0 },
        { &g_flt_w,     101,  50, "WIDTH",   918, 440, 20, PG_FX,     0 },
        { &g_fx[0],      16,  15, "BIT DEPTH", 470, 448, 30, PG_FX,   1 },
        { &g_fx[1],     101, 100, "SAMPLE RATE", 620, 448, 30, PG_FX, 1 },
        { &g_gr_det,    101,   0, "DETUNE",  728, 436, 28, PG_GRAIN, -1 },
        { &g_pe_vol,    101,  80, "VOLUME",   62, 290, 28, PG_PAD, 0 },
        { &g_pe_pitch,   49,  24, "PITCH",   206, 290, 28, PG_PAD, 0 },
        { &g_pe_start,  100,   0, "START",   350, 290, 28, PG_PAD, 0 },
        { &g_pe_a,      101,   0, "ATTACK",   58, 290, 28, PG_PAD, 1 },
        { &g_pe_dc,     101,  30, "DECAY",   154, 290, 28, PG_PAD, 1 },
        { &g_pe_s,      101, 100, "SUSTAIN", 250, 290, 28, PG_PAD, 1 },
        { &g_pe_r,      101,  10, "RELEASE", 346, 290, 28, PG_PAD, 1 },
        { &g_pe_cth,    101, 100, "THRESHOLD", 58, 290, 28, PG_PAD, 2 },
        { &g_pe_crat,   101,  40, "RATIO",   154, 290, 28, PG_PAD, 2 },
        { &g_pe_cmk,    101,   0, "MAKEUP",  250, 290, 28, PG_PAD, 2 },
        { &g_pe_cmx,    101, 100, "MIX",     346, 290, 28, PG_PAD, 2 },
    };
    memcpy(g_kd, k, sizeof(k));
}

static int knob_visible(int k, int page, int sec) {
    if (!(g_kd[k].pages & (1 << page))) return 0;
    if (page == 3 && g_kd[k].sec >= 0 && g_kd[k].sec != sec) return 0;
    if (page == 5 && g_kd[k].sec >= 0 && g_kd[k].sec != g_pad_env_sub) return 0;
    return 1;
}

static void knob_set(int k, int val) {
    if (val < 0) val = 0;
    if (val > g_kd[k].n - 1) val = g_kd[k].n - 1;
    *g_kd[k].v = val;
}

/* текст значения ручки */
static void knob_text(int k, char *out, size_t cap) {
    int v = *g_kd[k].v;
    switch (k) {
    case 0: fx_text(out, cap, 2, v); break;
    case 1: fx_text(out, cap, 3, v); break;
    case 2: fx_text(out, cap, 5, v); break;
    case 3: snprintf(out, cap, "%d %%", v); break;
    case 4: snprintf(out, cap, "%d", 1 + v); break;
    case 5: snprintf(out, cap, "%d ms", (int)(20.0f * powf(25.0f, (float)v / 100.0f) + 0.5f)); break;
    case 6: { float s = (float)v / 100.0f; snprintf(out, cap, "%d ms", (int)(1000.0f * s * s + 0.5f)); break; }
    case 7: {
        float f = 40.0f * powf(200.0f, (float)v / 100.0f);
        if (f >= 1000.0f) { snprintf(out, cap, "%d.%d kHz", (int)(f / 1000.0f), (int)(f / 100.0f) % 10); }
        else { snprintf(out, cap, "%d Hz", (int)(f + 0.5f)); }
        break; }
    case 8: { int o = (int)(25.0f * powf(16.0f, (float)v / 100.0f) + 0.5f); snprintf(out, cap, "%d.%02d oct", o / 100, o % 100); break; }
    case 9: fx_text(out, cap, 0, v); break;
    case 10: fx_text(out, cap, 1, v); break;
    case 12: snprintf(out, cap, "%d %%", v); break;
    case 13: snprintf(out, cap, "%+d st", v - 24); break;
    case 14: snprintf(out, cap, "%d %%", v); break;
    case 15: case 16: case 18: snprintf(out, cap, "%d ms", (int)(pad_ms(v) + 0.5f)); break;
    case 17: snprintf(out, cap, "%d %%", v); break;
    case 19: if (v >= 100) snprintf(out, cap, "off"); else snprintf(out, cap, "%d dB", (int)(-48.0f * (1.0f - (float)v / 100.0f) - 0.5f)); break;
    case 20: { float r = 1.0f + 19.0f * (float)v / 100.0f * (float)v / 100.0f; int r10 = (int)(r * 10.0f + 0.5f); snprintf(out, cap, "%d.%d : 1", r10 / 10, r10 % 10); break; }
    case 21: snprintf(out, cap, "+%d dB", (int)(18.0f * (float)v / 100.0f + 0.5f)); break;
    case 22: snprintf(out, cap, "%d %%", v); break;
    default: { float d = (float)v / 100.0f; snprintf(out, cap, "%d ct", (int)(1200.0f * d * d + 0.5f)); break; }
    }
}

/* ---------- геометрия кнопок ---------- */
static int btn_geom(int id, int *x, int *y, int *w, int *h) {
    if (id >= B_TAB0 && id <= B_TAB5) { *x = 204 + (id - B_TAB0) * 53; *y = 7; *w = 49; *h = 27; return 1; }
    if (id >= B_CROP && id <= B_NORM) { *x = 332 + (id - B_CROP) * 126; *y = 384; *w = 90; *h = 90; return 1; }
    if (id >= B_ADV0 && id <= B_ADV3) { *x = 332 + (id - B_ADV0) * 126; *y = 384; *w = 90; *h = 90; return 1; }
    switch (id) {
    case B_SL_MINUS:  *x = 796; *y = 396; *w = 38; *h = 38; return 1;
    case B_SL_PLUS:   *x = 894; *y = 396; *w = 38; *h = 38; return 1;
    case B_BPM_MINUS: *x = 796; *y = 472; *w = 38; *h = 38; return 1;
    case B_BPM_PLUS:  *x = 894; *y = 472; *w = 38; *h = 38; return 1;
    case B_GR_ON:     *x = 840; *y = 392; *w = 90; *h = 90; return 1;
    case B_CURVE:     *x = CURVE_X; *y = CURVE_Y; *w = CURVE_W; *h = CURVE_H; return 1;
    case B_PLOAD:     *x = 10;  *y = 384; *w = 192; *h = 64; return 1;
    case B_PMODE:     *x = 210; *y = 384; *w = 192; *h = 64; return 1;
    case B_PENV:      *x = 10;  *y = 462; *w = 122; *h = 64; return 1;
    case B_PCMP:      *x = 142; *y = 462; *w = 122; *h = 64; return 1;
    case B_PCLR:      *x = 274; *y = 462; *w = 128; *h = 64; return 1;
    }
    if (id >= B_FSEC0 && id <= B_FSEC1) { *x = 316 + (id - B_FSEC0) * 110; *y = 372; *w = 100; *h = 28; return 1; }
    if (id >= B_FT0 && id <= B_FT3) { int i = id - B_FT0; *x = 690 + (i % 2) * 62; *y = 410 + (i / 2) * 60; *w = 54; *h = 54; return 1; }
    return 0;
}

/* что под пальцем: кнопка, пад, ручка (B_KNOB0 + номер) */
/* ---- клавиатура GRAIN: 2 октавы C..C (25 клавиш), клавиша = сдвиг тона дорожки и зёрен, скорость не меняется ---- */
#define KB_X 24
#define KB_Y 486
#define KB_WW 52              /* ширина белой клавиши */
#define KB_H 46
#define KB_BH 28
#define KB_BW 30
static const int kb_wn[7] = { 0, 2, 4, 5, 7, 9, 11 };
static int kb_has_black(int w) { int m = w % 7; return w < 14 && (m == 0 || m == 1 || m == 3 || m == 4 || m == 5); }
static int kb_hit(double fx, double fy) {                   /* номер 0..24 или -1; чёрные клавиши в приоритете */
    if (fy < KB_Y - 6 || fy > KB_Y + KB_H + 6 || fx < KB_X || fx >= KB_X + 15 * KB_WW) return -1;
    if (fy < KB_Y + KB_BH)
        for (int w = 0; w < 14; w++) {
            if (!kb_has_black(w)) continue;
            double cx = KB_X + (w + 1) * KB_WW;
            if (fabs(fx - cx) < KB_BW * 0.5) return 12 * (w / 7) + kb_wn[w % 7] + 1;
        }
    int w = (int)((fx - KB_X) / KB_WW);
    if (w < 0) w = 0;
    if (w > 14) w = 14;
    return 12 * (w / 7) + kb_wn[w % 7];
}
/* на странице GRAIN ручки поднимаются, освобождая место клавиатуре; на остальных страницах - на своих местах */
static void knobs_relayout(int page) {
    static const int base[12] = { 438, 438, 438, 436, 436, 436, 436, 440, 440, 448, 448, 436 };
    for (int k = 0; k < 12; k++) {
        if (!(g_kd[k].pages & (1 << 2))) continue;
        g_kd[k].cy = base[k] - (page == 2 ? 26 : 0);
    }
}
static int hit_test(int tx, int ty, int page, int sec, int sl_n) {
    knobs_relayout(page);
    int ids[26], n = 0;
    ids[n++] = B_TAB0; ids[n++] = B_TAB1; ids[n++] = B_TAB2; ids[n++] = B_TAB3; ids[n++] = B_TAB4; ids[n++] = B_TAB5;
    if (page == 5) { ids[n++] = B_PLOAD; ids[n++] = B_PMODE; ids[n++] = B_PENV; ids[n++] = B_PCMP; ids[n++] = B_PCLR; }
    if (page == 0) { ids[n++] = B_CROP; ids[n++] = B_UNDO; ids[n++] = B_REV; ids[n++] = B_NORM; }
    if (page == 1) { ids[n++] = B_SL_MINUS; ids[n++] = B_SL_PLUS; ids[n++] = B_BPM_MINUS; ids[n++] = B_BPM_PLUS; }
    if (page == 2) ids[n++] = B_GR_ON;
    if (page == 4) { ids[n++] = B_ADV0; ids[n++] = B_ADV1; ids[n++] = B_ADV2; ids[n++] = B_ADV3; }
    if (page == 3) {
        ids[n++] = B_FSEC0; ids[n++] = B_FSEC1;
        if (sec == 0) { ids[n++] = B_FT0; ids[n++] = B_FT1; ids[n++] = B_FT2; ids[n++] = B_FT3; ids[n++] = B_CURVE; }
    }
    for (int i = 0; i < n; i++) {
        int x, y, w, h, sx = 0, sy = 0;
        if (ids[i] >= B_TAB0 && ids[i] <= B_TAB5) { sx = 2; sy = 7; }          /* названия режимов у края экрана - зона нажатия больше */
        if (btn_geom(ids[i], &x, &y, &w, &h) && tx >= x - sx && tx < x + w + sx && ty >= y - sy && ty < y + h + sy) return ids[i];
    }
    for (int k = 0; k < N_KNOBS; k++) {
        if (!knob_visible(k, page, sec)) continue;
        int dx = tx - g_kd[k].cx, dy = ty - g_kd[k].cy, rr = g_kd[k].r + 16;
        if (dx * dx + dy * dy <= rr * rr) return B_KNOB0 + k;
    }
    if (page == 1) {
        for (int i = 0; i < sl_n; i++) {
            int x, y, w, h;
            pad_geom(i, sl_n, &x, &y, &w, &h);
            if (tx >= x && tx < x + w && ty >= y && ty < y + h) return B_PAD0 + i;
        }
    }
    if (page == 5) {
        for (int i = 0; i < 16; i++) {
            int x, y, w, h;
            pad5_geom(i, &x, &y, &w, &h);
            if (tx >= x - 2 && tx < x + w + 2 && ty >= y - 2 && ty < y + h + 2) return B_PAD0 + i;
        }
    }
    return B_NONE;
}

/* ====================================================================
 *  Данные кадра для рисования
 * ==================================================================== */
typedef struct {
    int mode, page, fx_sec, state, scrubbing, loop;
    int length, playhead, live;
    int split, active;
    double c1, c2;
    int level_pct, clip, confirm;
    char msg[160];
    int sl_n, sl_sel, sl_flash, sl_playing;
    int btn_down, can_undo, knob_down;
    int crop_armed, legend, rev, bpm, bpm_t, bpm_b;
    int gr_on, flt_type, peak_pct, kb_note;
    double scrub_pos, scrub_speed;
    unsigned frame;
    Layout lay;
    int view_len;
    vita2d_texture *wave_tex;
} UiState;

/* градиент трубки уровня: внизу тёплый жёлтый, выше оранжевый, наверху красно-оранжевый; объём - тенью по ширине */
static void meter_render(Canvas *c) {
    static const float pos[5] = { 0.0f, 0.35f, 0.62f, 0.86f, 1.0f };
    static const int col[5][3] = { { 255, 180, 34 }, { 255, 150, 26 }, { 255, 118, 20 }, { 246, 80, 18 }, { 226, 40, 18 } };
    for (int y = 0; y < c->h; y++) {
        float t = 1.0f - (float)y / (float)(c->h - 1);
        int k = 0;
        while (k < 3 && t > pos[k + 1]) k++;
        float f = (t - pos[k]) / (pos[k + 1] - pos[k]);
        float r = col[k][0] + (col[k + 1][0] - col[k][0]) * f, g = col[k][1] + (col[k + 1][1] - col[k][1]) * f, b = col[k][2] + (col[k + 1][2] - col[k][2]) * f;
        for (int x = 0; x < c->w; x++) {
            float u = ((float)x + 0.5f) / (float)c->w * 2.0f - 1.0f;
            float sh = 1.0f - 0.30f * u * u;
            int rr = (int)(r * sh), gg = (int)(g * sh), bb = (int)(b * sh);
            c->px[y * c->w + x] = RGBA8(rr > 255 ? 255 : rr, gg > 255 ? 255 : gg, bb > 255 ? 255 : bb, 255);
        }
    }
}

/* стекло поверх трубки (размер METER_W+8 x METER_H+8): тень от оправы, блик сверху, вертикальный отсвет, нижний отсвет */
static void meter_glass_render(Canvas *c) {
    int w = c->w, h = c->h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float fx = (float)(x - 4) / (float)METER_W, fy = (float)(y - 4) / (float)METER_H;   /* 0..1 внутри трубки */
            float a = 0.0f, wr = 0.0f;
            /* внутренняя тень под оправой */
            float dtop = (float)(y - 4), dl = (float)(x - 4), dr = (float)(METER_W - 1 - (x - 4));
            if (dtop < 9.0f) a += 0.55f * (1.0f - dtop / 9.0f) * (1.0f - dtop / 9.0f);
            if (dl < 3.0f) a += 0.35f * (1.0f - dl / 3.0f);
            if (dr < 3.0f) a += 0.35f * (1.0f - dr / 3.0f);
            if (a > 0.8f) a = 0.8f;
            if (x >= 4 && x < 4 + METER_W && y >= 4 && y < 4 + METER_H && a > 0.0f) cv_put(c, x, y, RGBA8(0, 0, 0, 255), a);
            /* блик: светлая полоса вдоль левой трети, затухает к краям */
            if (x >= 4 && x < 4 + METER_W && y >= 4 && y < 4 + METER_H) {
                float sx = (fx - 0.28f) / 0.09f, hx = expf(-sx * sx);
                float vy = fy < 0.06f ? fy / 0.06f : (fy > 0.7f ? (1.0f - fy) / 0.3f : 1.0f);
                if (vy < 0.0f) vy = 0.0f;
                wr = 0.26f * hx * vy;
                float gl = fy < 0.14f ? (1.0f - fy / 0.14f) * 0.16f : 0.0f;         /* общий блик сверху */
                wr += gl;
                float sy = (fy - 0.97f) / 0.015f;                                    /* тонкий отсвет у дна */
                wr += 0.22f * expf(-sy * sy) * (fx > 0.1f && fx < 0.9f ? 1.0f : 0.0f);
                if (wr > 0.0f) cv_put(c, x, y, RGBA8(255, 255, 255, 255), wr);
            }
        }
    /* стеклянная кромка */
    cv_rrect_stroke(c, 3.5f, 3.5f, (float)METER_W + 1.0f, (float)METER_H + 1.0f, 3.0f, 1.0f, RGBA8(255, 255, 255, 60));
}

/* ====================================================================
 *  Статичный фон (рисуется один раз): корпус, стеклянное окно с ЭЛТ, панель
 * ==================================================================== */
static void skin_render_static_ex(Canvas *c, int pad) {
    int have_body = g_img_body.px && g_img_body.w == c->w && g_img_body.h == c->h;
    if (have_body) memcpy(c->px, g_img_body.px, (size_t)c->w * c->h * 4);
    else {
    for (int y = 0; y < c->h; y++) {
        uint32_t col = lerp_col(RGBA8(237, 233, 221, 255), RGBA8(222, 217, 203, 255), (float)y / (float)c->h);
        for (int x = 0; x < c->w; x++) c->px[y * c->w + x] = col;
    }
    cv_noise(c, 0, 0, c->w, c->h, 3);
    }
    if (pad && have_body) {                          /* страница PAD: нижняя панель корпуса не нужна - закрываем куском чистого фона */
        for (int y = 354; y < c->h; y++)
            for (int x = 0; x < c->w; x++) {
                int sy = 353 - (y - 354) % 38, sxx = (x >= 894 && x <= 954) ? x - 70 : x;     /* зеркально от края: без шва */
                c->px[y * c->w + x] = c->px[sy * c->w + sxx];
            }
        for (int y = 42; y < 354; y++)                                          /* панель индикатора уровня на PAD не нужна */
            for (int x = 884; x < c->w; x++) c->px[y * c->w + x] = c->px[y * c->w + x - 80];
    }

    /* верхняя планка */
    cv_rect(c, 0, 40, SCR_W, 1, K_EDGE);
    cv_rect(c, 0, 41, SCR_W, 1, RGBA8(255, 255, 255, 140));
    cv_rrect_grad(c, 14, 7, 26, 26, 3, K_ORANGE_HI, K_ORANGE);
    cv_rrect_stroke(c, 14, 7, 26, 26, 3, 1.0f, K_ORANGE_LO);
    static const int bh[] = { 7, 14, 10, 17, 6 };
    for (int i = 0; i < 5; i++) cv_rrect(c, 19.5f + i * 3.6f, 20.0f - bh[i] * 0.5f, 2.0f, (float)bh[i], 1.0f, K_CREAM_HI);

    /* стеклянная рамка: толстое литое оранжево-коричневое стекло */
    {
        const int bx = pad ? PBZ_X : BEZ_X, by = pad ? PBZ_Y : BEZ_Y, bw = pad ? PBZ_W : BEZ_W, bh = pad ? PBZ_H : BEZ_H;
        const int cx = bx + 16, cy = by + 16, cw = bw - 32, ch = bh - 32;
        cv_shadow(c, bx, by, bw, bh, 10, 5);
        cv_rrect_grad(c, bx, by, bw, bh, 10, RGBA8(232, 124, 34, 255), RGBA8(150, 62, 10, 255));
        cv_rrect_stroke(c, bx, by, bw, bh, 10, 1.4f, RGBA8(96, 38, 6, 255));
        cv_rrect_grad(c, bx + 3, by + 3, bw - 6, bh - 6, 8, RGBA8(248, 150, 56, 255), RGBA8(190, 82, 16, 255));
        cv_rrect_grad(c, bx + 9, by + 8, bw - 18, bh - 17, 7, RGBA8(206, 96, 20, 255), RGBA8(120, 48, 8, 255));
        cv_rrect_stroke(c, bx + 9, by + 8, bw - 18, bh - 17, 7, 1.0f, RGBA8(80, 30, 4, 255));
        cv_rrect(c, bx + 14, by + 4.5f, bw - 28, 1.6f, 0.8f, RGBA8(255, 226, 180, 190));
        cv_rrect(c, bx + 4.5f, by + 16, 1.4f, bh - 34, 0.7f, RGBA8(255, 200, 140, 90));
        cv_rrect(c, bx + 14, by + bh - 5.5f, bw - 28, 1.2f, 0.6f, RGBA8(255, 170, 90, 120));

        /* ЭЛТ: тёмная трубка, сетка */
        cv_rrect_grad(c, cx, cy, cw, ch, 9, RGBA8(30, 20, 14, 255), RGBA8(12, 8, 5, 255));
        if (!pad) {
            for (int i = 1; i < 4; i++) {
                int yy = WAVE_Y + i * WAVE_H / 4;
                for (int x = WAVE_X; x < WAVE_X + WAVE_W; x += 8) cv_rect(c, x, yy, 4, 1, RGBA8(255, 150, 46, 34));
            }
            for (int i = 1; i < 16; i++) cv_rect(c, WAVE_X + i * WAVE_W / 16, WAVE_Y, 1, WAVE_H, RGBA8(255, 150, 46, 20));
        } else {
            for (int i = 1; i < 4; i++) {
                int yy = 102 + i * 82 / 4;
                for (int x = PBZ_X + 24; x < PBZ_X + 24 + 346; x += 8) cv_rect(c, x, yy, 4, 1, RGBA8(255, 150, 46, 34));
            }
            for (int i = 1; i < 12; i++) cv_rect(c, PBZ_X + 24 + i * 346 / 12, 102, 1, 82, RGBA8(255, 150, 46, 20));
        }
    }

    if (!pad) {
    /* индикатор уровня: врезанная панель, стеклянная трубка, засечки, окна пика и перегруза */
    if (!have_body) {
        cv_rrect_grad(c, 896, 46, 56, 306, 5, RGBA8(200, 194, 176, 255), RGBA8(186, 179, 160, 255));
        cv_rrect_stroke(c, 896, 46, 56, 306, 5, 1.0f, K_EDGE);
    }
    cv_rrect(c, 899, 49, 50, 20, 3, RGBA8(24, 15, 10, 255));              /* окно CLIP */
    cv_rrect_stroke(c, 899, 49, 50, 20, 3, 1.0f, RGBA8(10, 6, 4, 255));
    cv_rrect(c, 899, 318, 50, 26, 3, RGBA8(24, 15, 10, 255));             /* окно пика */
    cv_rrect_stroke(c, 899, 318, 50, 26, 3, 1.0f, RGBA8(10, 6, 4, 255));
    /* окошко трубки: врезанная оправа со светлой кромкой, внутри тёмная пустая трубка */
    cv_rrect(c, METER_X - 6, METER_Y - 5, METER_W + 12, METER_H + 12, 6, RGBA8(255, 255, 255, 120));     /* светлая кромка снизу-справа */
    cv_rrect(c, METER_X - 6, METER_Y - 6, METER_W + 12, METER_H + 11, 6, RGBA8(96, 88, 72, 255));
    cv_rrect(c, METER_X - 4, METER_Y - 4, METER_W + 8, METER_H + 8, 5, RGBA8(26, 22, 18, 255));
    for (int x = 0; x < METER_W; x++) {                                    /* пустая часть: симметричная тёмная трубка */
        float u = ((float)x + 0.5f) / (float)METER_W * 2.0f - 1.0f;
        float sh = 1.0f - 0.30f * u * u;
        for (int y = 0; y < METER_H; y++) {
            int v = (int)(58.0f * sh);
            c->px[(METER_Y + y) * c->w + METER_X + x] = RGBA8(v, v - 5 > 0 ? v - 5 : 0, v - 10 > 0 ? v - 10 : 0, 255);
        }
    }
    }

    /* панель управления (при наличии body.png она уже нарисована в фоне) */
    if (!have_body && !pad) {
        cv_shadow(c, PN_X, PN_Y, PN_W, PN_H, 4, 2);
        cv_rrect_grad(c, PN_X, PN_Y, PN_W, PN_H, 4, RGBA8(214, 208, 190, 255), RGBA8(196, 189, 170, 255));
        cv_rrect_stroke(c, PN_X, PN_Y, PN_W, PN_H, 4, 1.0f, K_EDGE);
        cv_rect(c, PN_X + 2, PN_Y + 1, PN_W - 4, 1, RGBA8(255, 255, 255, 130));
        cv_rect(c, 292, 376, 1, 146, RGBA8(150, 142, 120, 255));
        cv_rect(c, 293, 376, 1, 146, RGBA8(255, 255, 255, 140));
    }
}

static void skin_render_static(Canvas *c) { skin_render_static_ex(c, 0); }

/* знаковое расстояние до скруглённого прямоугольника (внутри < 0) */
static float sd_rrect(float px, float py, float x0, float y0, float w, float h, float r) {
    float qx = fabsf(px - (x0 + w * 0.5f)) - (w * 0.5f - r), qy = fabsf(py - (y0 + h * 0.5f)) - (h * 0.5f - r);
    float ax = qx > 0.0f ? qx : 0.0f, ay = qy > 0.0f ? qy : 0.0f;
    float m = qx > qy ? qx : qy;
    return sqrtf(ax * ax + ay * ay) + (m < 0.0f ? m : 0.0f) - r;
}

/* выпуклое стекло ЭЛТ (холст GL_W x GL_H: экран + поле GL_M для литой кромки): светится центр, края темнее,
 * кромка стекла со скосом, тень от оправы сверху-слева, отражение софтбокса и дуга бликов вдоль края, строки развёртки */
static void glass_render_ex(Canvas *c, const int cw, const int ch) {
    const float CR = 9.0f;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            float fx = (float)x + 0.5f - GL_M, fy = (float)y + 0.5f - GL_M;           /* координаты относительно экрана */
            float d = sd_rrect(fx, fy, 0.0f, 0.0f, (float)cw, (float)ch, CR);
            uint32_t *dp = &c->px[y * c->w + x];
            *dp = 0;
            /* нормаль к краю (наружу) */
            float qx = fabsf(fx - cw * 0.5f) - (cw * 0.5f - CR), qy = fabsf(fy - ch * 0.5f) - (ch * 0.5f - CR);
            float nx = qx > 0.0f ? qx : 0.0f, ny = qy > 0.0f ? qy : 0.0f;
            if (nx == 0.0f && ny == 0.0f) { if (qx > qy) nx = 1.0f; else ny = 1.0f; }
            float nl = sqrtf(nx * nx + ny * ny);
            nx = (fx < cw * 0.5f ? -nx : nx) / nl; ny = (fy < ch * 0.5f ? -ny : ny) / nl;
            float lit = nx * -0.62f + ny * -0.78f;                                     /* >0: край обращён к свету (сверху-слева) */
            if (d > 0.0f) {
                if (d > (float)GL_M) continue;
                float t = d / (float)GL_M;                                            /* литая кромка снаружи: скос */
                float pr = sinf(t * 3.14159265f);
                if (lit > 0.0f) cv_put(c, x, y, RGBA8(255, 214, 160, 255), 0.62f * lit * pr);
                else cv_put(c, x, y, RGBA8(70, 24, 4, 255), 0.55f * -lit * pr);
                if (d < 1.4f) cv_put(c, x, y, RGBA8(40, 12, 2, 255), 0.7f * (1.0f - d / 1.4f));
                continue;
            }
            float in = -d;
            cv_put(c, x, y, RGBA8(255, 120, 20, 255), 0.06f);                       /* оранжевое стекло */
            float ex = (fx / (cw * 0.5f) - 1.0f), ey = (fy / (ch * 0.5f) - 1.0f);
            float rr = powf(powf(fabsf(ex), 2.6f) + powf(fabsf(ey), 2.6f), 1.0f / 2.6f);   /* суперэллипс: 0 в центре, 1 у края */
            float glow = 1.0f - rr * rr;
            if (glow > 0.0f) cv_put(c, x, y, RGBA8(255, 150, 50, 255), 0.05f * glow);   /* свечение люминофора в центре */
            float vg = rr > 0.55f ? (rr - 0.55f) / 0.45f : 0.0f;                    /* выпуклость: края темнеют */
            if (vg > 0.0f) cv_put(c, x, y, RGBA8(6, 2, 0, 255), fclampf(vg, 0.0f, 1.0f) * 0.6f);
            if (lit > 0.0f && in < 14.0f) cv_put(c, x, y, RGBA8(0, 0, 0, 255), 0.5f * lit * expf(-in / 5.0f));    /* тень от оправы */
            if (lit < 0.0f && in < 10.0f) cv_put(c, x, y, RGBA8(255, 190, 120, 255), 0.22f * -lit * expf(-in / 3.0f));   /* отсвет на нижнем крае */
            /* дуга блика вдоль верхнего левого края (отражение оправы в стекле) */
            if (lit > 0.35f && in > 4.0f && in < 8.0f) {
                float k = (in - 4.0f) / 4.0f; k = 1.0f - fabsf(k * 2.0f - 1.0f);
                float fade = fclampf((lit - 0.35f) / 0.65f, 0.0f, 1.0f);
                float along = (fx + fy * 0.4f) / (float)cw;
                float f2 = along < 0.55f ? 1.0f - along / 0.55f : 0.0f;
                cv_put(c, x, y, RGBA8(255, 230, 190, 255), 0.30f * k * fade * f2);
            }
            {                                                                       /* слабые вторичные отражения у краёв: не мешают тексту */
                float rx = (fx - 0.90f * cw) / (0.10f * cw), ry = (fy - 0.80f * ch) / (0.18f * ch);
                float re = 1.0f - (rx * rx + ry * ry);
                if (re > 0.0f) cv_put(c, x, y, RGBA8(255, 200, 140, 255), 0.06f * re);
            }
            if (y % 3 == 1) cv_put(c, x, y, RGBA8(0, 0, 0, 255), 0.15f * (1.0f - 0.5f * glow));   /* строки развёртки: к краям заметнее */
        }
}
static void glass_render(Canvas *c) { glass_render_ex(c, CRT_W, CRT_H); }

/* ====================================================================
 *  Верхняя планка: лампы REC / PLAY / LOOP и BPM сегментными цифрами
 * ==================================================================== */
static void top_render(Canvas *c, const UiState *u) {
    cv_clear(c, 0);
    uint32_t rec = RGBA8(255, 52, 38, 255), ply = RGBA8(96, 220, 110, 255), lp = RGBA8(255, 170, 30, 255);
    int blink = (u->frame / 24) % 2 == 0;
    cv_lamp(c, 548, 21, 6, rec, u->state == ST_RECORDING && blink);
    cv_lamp(c, 636, 21, 6, ply, u->state == ST_PLAYING);
    cv_lamp(c, 724, 21, 6, lp, u->loop);
    char b[8];
    if (u->bpm > 0) snprintf(b, sizeof(b), "%d", u->bpm_t); else snprintf(b, sizeof(b), "---");
    cv_seg_box(c, 806, 6, 70, 29, b, RGBA8(255, 70, 48, 255), 19.0f);
}

/* ====================================================================
 *  Органы управления: каждая деталь перерисовывается ТОЛЬКО когда изменилось её состояние
 * ==================================================================== */
#define W_KNOB0   0            /* 0..19  ручки */
#define W_EDIT0   20           /* 20..23 кнопки EDIT */
#define W_PAD0    24           /* 24..55 пэды */
#define W_SIDE0   56           /* 56..59 кнопки справа в SLICE / кнопки PAD */
#define W_SEG0    60           /* 60..61 сегментные значения */
#define W_GRON    62
#define W_FT0     63           /* 63..66 типы фильтра */
#define W_CURVE   67
#define W_KB      68
#define W_COUNT   69      /* W_SIDE0 + 4 (пятая кнопка PAD) = 60, на странице PAD W_SEG0 не используется */
static int g_wcode[W_COUNT];
static int g_page_key = -1;

static void cv_clear_rect(Canvas *c, int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= c->h) continue;
        for (int i = x; i < x + w; i++) if (i >= 0 && i < c->w) c->px[j * c->w + i] = 0;
    }
}
static void cv_clear_part(Canvas *c, int x, int y, int w, int h) { cv_clear_rect(c, x - 6, y - 3, w + 12, h + 10); }

enum { IC_GRAIN = 20, IC_DEL, IC_SIL, IC_DUP, IC_GAP };
static void cv_icon_ex(Canvas *c, int kind, float cx, float cy, uint32_t col) {
    if (kind == IC_GRAIN) {
        static const float dx[9] = { -12, 0, 12, -10, 2, 13, -13, -1, 10 }, dy[9] = { -9, -11, -8, 0, 1, -1, 9, 10, 8 };
        for (int i = 0; i < 9; i++) cv_circle(c, cx + dx[i], cy + dy[i], 2.6f + (i % 2), col);
    } else if (kind == IC_DEL) {
        cv_line(c, cx - 9, cy - 9, cx + 9, cy + 9, 2.4f, col);
        cv_line(c, cx + 9, cy - 9, cx - 9, cy + 9, 2.4f, col);
    } else if (kind == IC_SIL) {
        static const float hh[5] = { 15, 10, 6, 3, 1 };
        for (int i = 0; i < 5; i++) cv_rrect(c, cx - 14 + i * 6.0f, cy - hh[i] * 0.5f, 3.0f, hh[i] < 2 ? 2.0f : hh[i], 1.0f, col);
    } else if (kind == IC_DUP) {
        cv_rrect_stroke(c, cx - 12, cy - 9, 15, 15, 2.5f, 2.0f, col);
        cv_rrect_stroke(c, cx - 3, cy - 1, 15, 15, 2.5f, 2.0f, col);
    } else if (kind == IC_GAP) {
        static const float hh[2] = { 14, 9 };
        for (int i = 0; i < 2; i++) { cv_rrect(c, cx - 15 + i * 5.0f, cy - hh[i] * 0.5f, 3.0f, hh[i], 1.0f, col); cv_rrect(c, cx + 9 + i * 5.0f, cy - hh[1 - i] * 0.5f, 3.0f, hh[1 - i], 1.0f, col); }
        cv_rect(c, (int)cx - 5, (int)cy, 3, 2, col); cv_rect(c, (int)cx + 1, (int)cy, 3, 2, col);
    } else {
        cv_icon(c, kind, cx, cy, col);
    }
}

static float flt_db(int type, float fc, float wo, float f) {
    float m = 1.0f;
    if (type == 1) m = 1.0f / sqrtf(1.0f + powf(f / fc, 4.0f));
    else if (type == 2) m = 1.0f / sqrtf(1.0f + powf(fc / f, 4.0f));
    else if (type == 3) {
        float k = powf(2.0f, wo * 0.5f), f1 = fc / k, f2 = fc * k;
        m = 1.0f / sqrtf(1.0f + powf(f1 / f, 4.0f)) / sqrtf(1.0f + powf(f / f2, 4.0f));
    }
    return 20.0f * log10f(m + 1e-6f);
}
#define CV_F0 20.0f
#define CV_F1 8000.0f
static int curve_x_of(float f) { return (int)((float)CURVE_W * logf(f / CV_F0) / logf(CV_F1 / CV_F0)); }
static float curve_y_of(float db) { return (float)CURVE_H * (6.0f - db) / 42.0f; }

/* окно с АЧХ: мини-ЭЛТ в оранжевой оправе */
static void draw_curve(Canvas *c, int cy0) {
    const int ox = CURVE_X, oy = CURVE_Y - cy0, w = CURVE_W, h = CURVE_H;
    cv_rrect_grad(c, (float)(ox - 4), (float)(oy - 4), (float)(w + 8), (float)(h + 8), 5, RGBA8(214, 104, 24, 255), RGBA8(140, 56, 8, 255));
    cv_rrect_stroke(c, (float)(ox - 4), (float)(oy - 4), (float)(w + 8), (float)(h + 8), 5, 1.0f, RGBA8(90, 34, 4, 255));
    cv_rrect_grad(c, (float)ox, (float)oy, (float)w, (float)h, 3, RGBA8(30, 20, 14, 255), RGBA8(12, 8, 5, 255));
    static const float dbl[3] = { 0.0f, -12.0f, -24.0f };
    for (int i = 0; i < 3; i++) for (int x = 0; x < w; x += 6) cv_rect(c, ox + x, oy + (int)curve_y_of(dbl[i]), 3, 1, RGBA8(255, 150, 46, 40));
    static const float fl[3] = { 100.0f, 1000.0f, 4000.0f };
    for (int i = 0; i < 3; i++) cv_rect(c, ox + curve_x_of(fl[i]), oy, 1, h, RGBA8(255, 150, 46, 36));
    float fc = 40.0f * powf(200.0f, (float)g_flt_c / 100.0f);
    float wo = 0.25f * powf(16.0f, (float)g_flt_w / 100.0f);
    int type = g_flt_type;
    int py = -1000;
    for (int x = 0; x < w; x++) {
        float f = CV_F0 * powf(CV_F1 / CV_F0, (float)x / (float)(w - 1));
        float yy = curve_y_of(flt_db(type, fc, wo, f));
        if (yy > (float)(h - 1)) yy = (float)(h - 1);
        if (yy < 0.0f) yy = 0.0f;
        int iy = (int)yy;
        cv_rect(c, ox + x, oy + iy, 1, h - iy, RGBA8(233, 106, 38, 70));
        int y0 = iy, y1 = iy + 2;
        if (py > -1000) { if (py < y0) y0 = py; if (py > y1) y1 = py + 1; }
        cv_rect(c, ox + x, oy + y0, 1, y1 - y0 > 1 ? y1 - y0 : 2, RGBA8(255, 205, 120, 255));
        py = iy;
    }
    if (type) {                                   /* точка "центр" */
        int xc = curve_x_of(fc);
        float yc = curve_y_of(flt_db(type, fc, wo, fc));
        cv_ring(c, (float)(ox + xc), (float)oy + yc, 7.0f, 1.6f, RGBA8(255, 224, 90, 255));
    }
    for (int y = 1; y < h; y += 3) cv_rect(c, ox, oy + y, w, 1, RGBA8(0, 0, 0, 42));     /* строки развёртки */
}


/* ====================================================================
 *  PAD: интерфейс (большие пады 4x4 слева, маленький осциллограф и ручки справа, список WAV, подстраница ADSR)
 * ==================================================================== */
#define ENV_X (PBZ_X + 24)                    /* окно осциллографа */
#define ENV_Y 102
#define ENV_W 346
#define ENV_H 82
#define ENV_PX 0.8f                  /* пикселей на единицу времени A / D / R */
#define ENV_HOLD 40.0f
#define ENV_L 4.0f
#define ENV_TOP 6.0f
#define ENV_BASE (ENV_H - 6.0f)
#define LST_X 10                    /* список WAV занимает нижнюю правую часть, пады остаются доступны */
#define LST_Y 212
#define LST_W 394
#define LST_H 322
#define LST_ROW0 (LST_Y + 38)
#define LST_RH 27
#define PL_ROWS 10
#define PL_MAX 200
#define PAD_DIR "ux0:data/Sampler"
typedef struct {
    int sel, down_mask;
    int list_open, list_n, list_sel, list_top;
    char list[PL_MAX][40];
    int cfg_dirty, cfg_cnt;
} PadUi;
static PadUi g_pu;
static const char *const g_pmode_name[3] = { "ONE-SHOT", "GATE", "LOOP" };

static void pad_mark_dirty(void) { g_pu.cfg_dirty = 1; g_pu.cfg_cnt = 0; }

/* значения ручек <-> выбранный пад (по теневой копии: двойной тап по ручке тоже записывается) */
static void pad_sync(void) {
    enum { NV = 11 };
    static int sh[NV], sh_sel = -1;
    Pad *p = &g_pad[g_pu.sel];
    int *pv[NV] = { &p->vol, &p->pitch, &p->start, &p->a, &p->dc, &p->s, &p->r, &p->cth, &p->crat, &p->cmk, &p->cmx };
    volatile int *ev[NV] = { &g_pe_vol, &g_pe_pitch, &g_pe_start, &g_pe_a, &g_pe_dc, &g_pe_s, &g_pe_r, &g_pe_cth, &g_pe_crat, &g_pe_cmk, &g_pe_cmx };
    if (g_pu.sel != sh_sel) {
        for (int i = 0; i < NV; i++) { *ev[i] = *pv[i]; sh[i] = *pv[i]; }
        sh_sel = g_pu.sel;
        return;
    }
    int changed = 0;
    for (int i = 0; i < NV; i++) if (*ev[i] != sh[i]) changed = 1;
    if (changed) {
        for (int i = 0; i < NV; i++) { *pv[i] = *ev[i]; sh[i] = *ev[i]; }
        pad_mark_dirty();
    }
}

static void pad_clear(int i) {
    Pad *p = &g_pad[i];
    if (!p->d) return;
    pad_quiet(i);
    p->loading = 1;
    sceKernelDelayThread(60000);
    int16_t *old = p->d;
    p->d = NULL; p->n = 0; p->path[0] = 0; p->name[0] = 0;
    memset(p->pk, 0, sizeof(p->pk));
    p->loading = 0;
    free(old);
}

static int pl_cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }
static void pad_list_scan(void) {
    g_pu.list_n = 0;
    SceUID d = sceIoDopen(PAD_DIR);
    if (d >= 0) {
        SceIoDirent e;
        memset(&e, 0, sizeof(e));
        while (sceIoDread(d, &e) > 0 && g_pu.list_n < PL_MAX) {
            size_t n = strlen(e.d_name);
            if (n > 4 && n < 39 && strcasecmp(e.d_name + n - 4, ".wav") == 0)
                snprintf(g_pu.list[g_pu.list_n++], 40, "%s", e.d_name);
            memset(&e, 0, sizeof(e));
        }
        sceIoDclose(d);
    }
    qsort(g_pu.list, (size_t)g_pu.list_n, 40, pl_cmp);
}

/* ---- огибающая: точки в координатах окна осциллографа ---- */
static void env_points(const Pad *p, float *px, float *py) {
    float ys = ENV_BASE - (ENV_BASE - ENV_TOP) * (float)p->s / 100.0f;
    px[0] = ENV_L;                         py[0] = ENV_BASE;
    px[1] = px[0] + (float)p->a * ENV_PX;  py[1] = ENV_TOP;
    px[2] = px[1] + (float)p->dc * ENV_PX; py[2] = ys;
    px[3] = px[2] + ENV_HOLD;              py[3] = ys;
    px[4] = px[3] + (float)p->r * ENV_PX;  py[4] = ENV_BASE;
}

/* осциллограф: форма волны выбранного сэмпла; на подстранице ADSR поверх неё - огибающая */
static void env_render(Canvas *c, const Pad *p, int with_env) {
    cv_clear(c, 0);
    float mid = (ENV_BASE + ENV_TOP) * 0.5f, amp = (ENV_BASE - ENV_TOP) * 0.5f;
    cv_rect(c, 0, (int)mid, c->w, 1, RGBA8(255, 150, 46, 60));
    if (p->d) {
        for (int i = 0; i < PAD_PK; i++) {
            int hh = (int)((float)p->pk[i] / 255.0f * amp);
            int x0 = i * c->w / PAD_PK, x1 = (i + 1) * c->w / PAD_PK;
            cv_rect(c, x0, (int)mid - hh, x1 - x0 > 1 ? x1 - x0 : 1, 2 * hh + 1, RGBA8(255, 190, 90, with_env ? 150 : 235));
        }
        int sx = (int)((float)c->w * (float)p->start / 100.0f);
        cv_rect(c, sx, 0, 1, c->h, RGBA8(120, 230, 140, 190));
        cv_rect(c, sx, 0, 6, 2, RGBA8(120, 230, 140, 190));
    }
    if (with_env == 2) {                                   /* передаточная кривая компрессора: вход -48..0 дБ, выход -48..+18 дБ */
        float thr = -48.0f * (1.0f - (float)p->cth / 100.0f), mk = 18.0f * (float)p->cmk / 100.0f;
        float ratio = 1.0f + 19.0f * (float)p->crat / 100.0f * (float)p->crat / 100.0f, mix = (float)p->cmx / 100.0f;
        int pyy = -1;
        for (int x = 0; x < c->w; x++) {
            float in = -48.0f + 48.0f * (float)x / (float)(c->w - 1);
            float wet = (in < thr ? in : thr + (in - thr) / ratio) + mk;
            float out = in + (wet - in) * mix;
            float yy = ENV_BASE - (out + 48.0f) / 66.0f * (ENV_BASE - ENV_TOP);
            if (yy < ENV_TOP) yy = ENV_TOP;
            if (yy > ENV_BASE) yy = ENV_BASE;
            int iy = (int)yy;
            cv_rect(c, x, iy, 1, pyy >= 0 && pyy != iy ? (pyy > iy ? pyy - iy + 1 : 1) : 2, RGBA8(255, 232, 160, 255));
            if (pyy >= 0 && pyy < iy) cv_rect(c, x, pyy, 1, iy - pyy + 1, RGBA8(255, 232, 160, 255));
            pyy = iy;
        }
        float y0 = ENV_BASE - (0.0f + 48.0f) / 66.0f * (ENV_BASE - ENV_TOP);
        for (int x = 0; x < c->w; x += 6) cv_rect(c, x, (int)y0, 3, 1, RGBA8(255, 150, 46, 110));      /* уровень 0 дБ на выходе */
        float xt = (thr + 48.0f) / 48.0f * (float)(c->w - 1);
        cv_rect(c, (int)xt, 0, 1, c->h, RGBA8(120, 230, 140, 160));                                        /* порог */
    }
    if (with_env == 1) {
        float px[5], py[5];
        env_points(p, px, py);
        for (int x = (int)px[0]; x <= (int)px[4] && x < c->w; x++) {
            float y = py[4];
            for (int k = 0; k < 4; k++)
                if ((float)x >= px[k] && (float)x <= px[k + 1]) {
                    float t = px[k + 1] > px[k] ? ((float)x - px[k]) / (px[k + 1] - px[k]) : 1.0f;
                    y = py[k] + (py[k + 1] - py[k]) * t;
                    break;
                }
            cv_rect(c, x, (int)y, 1, (int)(ENV_BASE - y) + 1, RGBA8(233, 106, 38, 60));
        }
        for (int k = 0; k < 4; k++) cv_line(c, px[k], py[k], px[k + 1], py[k + 1], 1.3f, RGBA8(255, 232, 160, 255));
        static const int idx[3] = { 1, 2, 4 };
        for (int k = 0; k < 3; k++) cv_circle(c, px[idx[k]], py[idx[k]], 3.2f, RGBA8(255, 232, 160, 255));
    }
}

/* пад 104x104: объёмная клавиша; выбранный - оранжевый и залипает; внутри номер, мини-волна, имя (текст рисуется отдельно) */
static void cv_padcell(Canvas *c, int x, int y, int w, int h, int sel, int pr, int has, int play, const Pad *p) {
    cv_key(c, x, y, w, h, sel ? 1 : 0, pr);
    int off = pr ? 2 : 0;
    if (has) {
        cv_circle(c, (float)(x + w - 16), (float)(y + off + 17), 5.6f, RGBA8(60, 40, 20, 255));
        cv_circle(c, (float)(x + w - 16), (float)(y + off + 17), 4.2f, play ? RGBA8(255, 226, 90, 255) : RGBA8(70, 170, 90, 255));
        const int nb = 38;
        for (int i = 0; i < nb; i++) {
            int a0 = i * PAD_PK / nb, mx = 0;
            for (int q = a0; q < a0 + PAD_PK / nb + 1 && q < PAD_PK; q++) if (p->pk[q] > mx) mx = p->pk[q];
            int hh = (int)((float)mx / 255.0f * 17.0f);
            cv_rect(c, x + (w - 76) / 2 + i * 2, y + off + 58 - hh, 2, 2 * hh + 1, sel ? RGBA8(120, 46, 8, 210) : RGBA8(104, 92, 72, 200));
        }
    }
}

static int edit_style(const UiState *u, int id) {
    if (id == B_CROP && u->crop_armed) return 1;
    if (id == B_REV && u->rev) return 1;
    return 0;
}

/* Обновляет холст органов управления страницы. Возвращает 1, если что-то изменилось. */
static int ctl_update(Canvas *c, const UiState *u) {
    const int page = u->page;
    const int oy = (page == 5) ? PAD_CY : CTL_TY;
    knobs_relayout(page);
    int key = page * 1000 + (page == 1 ? u->sl_n : 0) + (page == 3 ? u->fx_sec * 100 : 0) + (page == 5 ? g_pad_env_sub * 100 : 0);
    int changed = 0;
    if (key != g_page_key) {
        cv_clear(c, 0);
        for (int i = 0; i < W_COUNT; i++) g_wcode[i] = -1;
        g_page_key = key;
        changed = 1;
    }
    for (int k = 0; k < N_KNOBS; k++) {
        if (!knob_visible(k, page, u->fx_sec)) continue;
        int dim = (k == 8 && u->flt_type != 3) || (k == 7 && u->flt_type == 0);
        int code = (((int)*g_kd[k].v + 1) << 3) | ((u->knob_down == k) << 1) | dim;
        if (code != g_wcode[W_KNOB0 + k]) {
            cv_clear_rect(c, g_kd[k].cx - g_kd[k].r - 13, g_kd[k].cy - g_kd[k].r - 13 - oy, 2 * g_kd[k].r + 26, 2 * g_kd[k].r + 26);
            cv_knob(c, g_kd[k].cx, g_kd[k].cy - oy, g_kd[k].r, (float)*g_kd[k].v / (float)(g_kd[k].n - 1), u->knob_down == k, dim);
            g_wcode[W_KNOB0 + k] = code;
            changed = 1;
        }
    }
    if (page == 0) {
        static const int icons[4] = { IC_CROP, IC_UNDO, IC_REV, IC_NORM };
        for (int id = B_CROP; id <= B_NORM; id++) {
            int st = edit_style(u, id), dim = (id == B_UNDO && !u->can_undo);
            int code = 1 + st + 4 * (u->btn_down == id) + 8 * dim;
            if (code != g_wcode[W_EDIT0 + id - B_CROP]) {
                int x, y, w, h;
                btn_geom(id, &x, &y, &w, &h);
                cv_clear_part(c, x, y - CTL_TY, w, h);
                int pr = (u->btn_down == id) || st;           /* включённая функция залипает в нажатом положении */
                cv_key(c, x, y - CTL_TY, w, h, 0, pr);
                cv_icon_ex(c, icons[id - B_CROP], (float)(x + w / 2), (float)(y - CTL_TY + 33 + (pr ? 2 : 0)), dim ? K_BROWN_LO : K_BROWN);
                g_wcode[W_EDIT0 + id - B_CROP] = code;
                changed = 1;
            }
        }
    } else if (page == 1) {
        for (int i = 0; i < u->sl_n && i < 32; i++) {
            int sel = (i == u->sl_sel), playing = (i == u->sl_playing);
            int code = 1 + (playing ? 3 : (sel ? 1 : 0)) + 4 * ((u->btn_down == B_PAD0 + i) || (sel && u->sl_flash > 0));
            if (code != g_wcode[W_PAD0 + i]) {
                int x, y, w, h;
                pad_geom(i, u->sl_n, &x, &y, &w, &h);
                cv_clear_part(c, x, y - CTL_TY, w, h);
                cv_key(c, x, y - CTL_TY, w, h, 0, (u->btn_down == B_PAD0 + i) || sel || playing);   /* выбранный пад залипает */
                g_wcode[W_PAD0 + i] = code;
                changed = 1;
            }
        }
        static const int sids[4] = { B_SL_MINUS, B_SL_PLUS, B_BPM_MINUS, B_BPM_PLUS };
        for (int k = 0; k < 4; k++) {
            int code = 1 + 4 * (u->btn_down == sids[k]);
            if (code != g_wcode[W_SIDE0 + k]) {
                int x, y, w, h;
                btn_geom(sids[k], &x, &y, &w, &h);
                cv_clear_part(c, x, y - CTL_TY, w, h);
                int pr = (u->btn_down == sids[k]);
                cv_key(c, x, y - CTL_TY, w, h, 0, pr);
                cv_icon(c, (k & 1) ? IC_PLUS : IC_MINUS, (float)(x + w / 2), (float)(y - CTL_TY + h / 2 + (pr ? 2 : 0)), K_BROWN);
                g_wcode[W_SIDE0 + k] = code;
                changed = 1;
            }
        }
        for (int j = 0; j < 2; j++) {
            int val = j == 0 ? u->sl_n : u->bpm_t;
            int code = 1 + val * 2 + (j == 1 && u->bpm <= 0);
            if (code != g_wcode[W_SEG0 + j]) {
                cv_clear_rect(c, 836, 396 + j * 76 - CTL_TY - 2, 58, 44);
                char b[12];
                if (j == 1 && u->bpm <= 0) snprintf(b, sizeof(b), "---"); else snprintf(b, sizeof(b), "%d", val);
                cv_seg_box(c, 838, 396 + j * 76 - CTL_TY, 52, 38, b, K_AMBER, 22.0f);
                g_wcode[W_SEG0 + j] = code;
                changed = 1;
            }
        }
    } else if (page == 2) {
        if (u->kb_note + 2 != g_wcode[W_KB]) {                  /* клавиатура: перерисовываем только при смене нажатой клавиши */
            cv_clear_rect(c, KB_X - 8, KB_Y - CTL_TY - 6, 15 * KB_WW + 16, KB_H + 16);
            for (int w = 0; w < 15; w++) {
                int note = 12 * (w / 7) + kb_wn[w % 7], on = (u->kb_note == note);
                float x = (float)(KB_X + w * KB_WW);
                cv_shadow(c, x + 1, (float)(KB_Y - CTL_TY), (float)KB_WW - 2, (float)KB_H, 4.0f, 2.0f);
                cv_rrect_grad(c, x + 1, (float)(KB_Y - CTL_TY + (on ? 2 : 0)), (float)KB_WW - 2, (float)KB_H - (on ? 2 : 0), 4.0f,
                              on ? RGBA8(255, 190, 110, 255) : RGBA8(252, 249, 240, 255), on ? RGBA8(240, 150, 70, 255) : RGBA8(222, 214, 196, 255));
                if (note == 12) cv_circle(c, x + KB_WW * 0.5f, (float)(KB_Y - CTL_TY + KB_H - 9), 3.0f, K_ORANGE_LO);   /* ноль: тон не меняется */
            }
            for (int w = 0; w < 14; w++) {
                if (!kb_has_black(w)) continue;
                int note = 12 * (w / 7) + kb_wn[w % 7] + 1, on = (u->kb_note == note);
                float x = (float)(KB_X + (w + 1) * KB_WW) - KB_BW * 0.5f;
                cv_rrect_grad(c, x, (float)(KB_Y - CTL_TY), (float)KB_BW, (float)KB_BH, 3.0f,
                              on ? RGBA8(250, 140, 60, 255) : RGBA8(92, 82, 74, 255), on ? RGBA8(200, 96, 36, 255) : RGBA8(40, 34, 30, 255));
            }
            g_wcode[W_KB] = u->kb_note + 2;
            changed = 1;
        }
        int code = 1 + (u->gr_on ? 1 : 0) + 4 * (u->btn_down == B_GR_ON);
        if (code != g_wcode[W_GRON]) {
            int x, y, w, h;
            btn_geom(B_GR_ON, &x, &y, &w, &h);
            cv_clear_part(c, x, y - CTL_TY, w, h);
            int pr = (u->btn_down == B_GR_ON) || u->gr_on;
            cv_key(c, x, y - CTL_TY, w, h, 0, pr);
            cv_icon_ex(c, IC_GRAIN, (float)(x + w / 2), (float)(y - CTL_TY + 32 + (pr ? 2 : 0)), K_BROWN);
            g_wcode[W_GRON] = code;
            changed = 1;
        }
    } else if (page == 4) {
        static const int icons[4] = { IC_DEL, IC_SIL, IC_DUP, IC_GAP };
        for (int id = B_ADV0; id <= B_ADV3; id++) {
            int code = 1 + 4 * (u->btn_down == id);
            if (code != g_wcode[W_EDIT0 + id - B_ADV0]) {
                int x, y, w, h;
                btn_geom(id, &x, &y, &w, &h);
                cv_clear_part(c, x, y - CTL_TY, w, h);
                int pr = (u->btn_down == id);
                cv_key(c, x, y - CTL_TY, w, h, 0, pr);
                cv_icon_ex(c, icons[id - B_ADV0], (float)(x + w / 2), (float)(y - CTL_TY + 33 + (pr ? 2 : 0)), K_BROWN);
                g_wcode[W_EDIT0 + id - B_ADV0] = code;
                changed = 1;
            }
        }
    } else if (page == 5) {
        for (int i = 0; i < PAD_N; i++) {
            int x, y, w, h;
            pad5_geom(i, &x, &y, &w, &h);
            int has = g_pad[i].d != NULL, pl = (g_pad_mask >> i) & 1, dn = (g_pu.down_mask >> i) & 1, sl = (g_pu.sel == i);
            int code = 1 + sl * 2 + has * 4 + pl * 8 + dn * 16 + (has ? g_pad[i].pk[7] * 32 + g_pad[i].pk[100] * 8192 : 0);
            if (code != g_wcode[W_PAD0 + i]) {
                cv_clear_part(c, x, y - oy, w, h);
                cv_padcell(c, x, y - oy, w, h, sl, sl || dn, has, pl, &g_pad[i]);
                g_wcode[W_PAD0 + i] = code;
                changed = 1;
            }
        }
        static const int pids[5] = { B_PLOAD, B_PMODE, B_PENV, B_PCMP, B_PCLR };
        for (int k = 0; k < 5; k++) {
            int id = pids[k];
            int on = (k == 0 && g_pu.list_open) || (k == 2 && g_pad_env_sub == 1) || (k == 3 && g_pad_env_sub == 2);
            int code = 1 + 4 * (u->btn_down == id) + 2 * on + (k == 1 ? 8 * g_pad[g_pu.sel].mode : 0);
            if (code != g_wcode[W_SIDE0 + k]) {
                int x, y, w, h;
                btn_geom(id, &x, &y, &w, &h);
                cv_clear_part(c, x, y - oy, w, h);
                cv_key(c, x, y - oy, w, h, 0, (u->btn_down == id) || on);
                g_wcode[W_SIDE0 + k] = code;
                changed = 1;
            }
        }
    } else if (page == 3 && u->fx_sec == 0) {
        for (int i = 0; i < 4; i++) {
            int sel = (u->flt_type == i);
            int code = 1 + sel + 4 * (u->btn_down == B_FT0 + i);
            if (code != g_wcode[W_FT0 + i]) {
                int x, y, w, h;
                btn_geom(B_FT0 + i, &x, &y, &w, &h);
                cv_clear_part(c, x, y - CTL_TY, w, h);
                int pr = sel || (u->btn_down == B_FT0 + i);
                cv_key(c, x, y - CTL_TY, w, h, 0, pr);
                static const float sh[4][4][2] = {
                    { { -12, 0 }, { 12, 0 }, { 12, 0 }, { 12, 0 } },
                    { { -12, 0 }, { 0, 0 }, { 12, 9 }, { 12, 9 } },
                    { { -12, 9 }, { 0, 0 }, { 12, 0 }, { 12, 0 } },
                    { { -12, 9 }, { -4, 0 }, { 4, 0 }, { 12, 9 } } };
                float cx0 = (float)(x + w / 2), cy0 = (float)(y - CTL_TY + 16 + (pr ? 2 : 0));
                for (int q = 0; q < 3; q++)
                    cv_line(c, cx0 + sh[i][q][0], cy0 + sh[i][q][1], cx0 + sh[i][q + 1][0], cy0 + sh[i][q + 1][1], 1.3f, K_BROWN);
                g_wcode[W_FT0 + i] = code;
                changed = 1;
            }
        }
        int code = 1 + u->flt_type * 1000003 + g_flt_c * 1009 + g_flt_w * 7;
        if (code != g_wcode[W_CURVE]) {
            cv_clear_rect(c, CURVE_X - 8, CURVE_Y - CTL_TY - 8, CURVE_W + 16, CURVE_H + 16);
            draw_curve(c, CTL_TY);
            g_wcode[W_CURVE] = code;
            changed = 1;
        }
    }
    return changed;
}

/* ====================================================================
 *  Текстуры интерфейса и их обновление
 * ==================================================================== */
static vita2d_texture *g_tex_bg = NULL, *g_tex_top[2], *g_tex_ctl[3], *g_tex_glass = NULL, *g_tex_meter = NULL;
static vita2d_texture *g_tex_bgpad = NULL, *g_tex_glass2 = NULL, *g_tex_pctl[3];
static Canvas g_cv_bg, g_cv_top, g_cv_glass, g_cv_page[6], g_cv_bgpad, g_cv_glass2;     /* панель управления: по холсту на страницу */
static int g_wcodes[6][W_COUNT], g_page_keys[6] = { -1, -1, -1, -1, -1, -1 }, g_last_page = -1, g_last_key = -1;
static uint32_t g_sig_top = 0;
static int g_top_cur = -1, g_ctl_cur = -1, g_top_next = 0, g_ctl_next = 0;

static Canvas g_cv_meter, g_cv_mglass, g_cv_env;
static vita2d_texture *g_tex_env[2];
static int g_env_cur = -1, g_env_next = 0;
static uint32_t g_sig_env = 0;
static vita2d_texture *g_tex_mglass = NULL;

static void upload_canvas(vita2d_texture *tex, const Canvas *c) {
    if (!tex) return;
    uint32_t *dst = (uint32_t *)vita2d_texture_get_datap(tex);
    int stride = (int)vita2d_texture_get_stride(tex) / 4;
    for (int y = 0; y < c->h; y++) memcpy(dst + (size_t)y * stride, c->px + (size_t)y * c->w, (size_t)c->w * 4);
}

static void ui_create_textures(void) {
    g_tex_bg = vita2d_create_empty_texture(SCR_W, SCR_H);
    g_tex_glass = vita2d_create_empty_texture(GL_W, GL_H);
    g_tex_bgpad = vita2d_create_empty_texture(SCR_W, SCR_H);
    g_tex_glass2 = vita2d_create_empty_texture(GL2_W, GL2_H);
    for (int i = 0; i < 3; i++) g_tex_pctl[i] = vita2d_create_empty_texture(SCR_W, PAD_CH);
    g_tex_meter = vita2d_create_empty_texture(METER_W, METER_H);
    upload_canvas(g_tex_meter, &g_cv_meter);
    g_tex_mglass = vita2d_create_empty_texture(METER_W + 8, METER_H + 8);
    upload_canvas(g_tex_mglass, &g_cv_mglass);
    for (int i = 0; i < 2; i++) g_tex_top[i] = vita2d_create_empty_texture(SCR_W, TOP_H);
    for (int i = 0; i < 3; i++) g_tex_ctl[i] = vita2d_create_empty_texture(SCR_W, CTL_TH);
    upload_canvas(g_tex_bg, &g_cv_bg);
    upload_canvas(g_tex_glass, &g_cv_glass);
    upload_canvas(g_tex_bgpad, &g_cv_bgpad);
    upload_canvas(g_tex_glass2, &g_cv_glass2);
    for (int i = 0; i < 2; i++) g_tex_env[i] = vita2d_create_empty_texture(ENV_W, ENV_H);
    g_env_cur = -1; g_sig_env = 0;
    g_sig_top = 0xFFFFFFFFu;
    g_top_cur = g_ctl_cur = -1;
    g_last_page = -1; g_last_key = -1;
}

static int ui_init(void) {
    knobs_init();
    assets_init();
    g_cv_bg.w = SCR_W; g_cv_bg.h = SCR_H; g_cv_bg.px = (uint32_t *)malloc((size_t)SCR_W * SCR_H * 4);
    g_cv_top.w = SCR_W; g_cv_top.h = TOP_H; g_cv_top.px = (uint32_t *)malloc((size_t)SCR_W * TOP_H * 4);
    g_cv_glass.w = GL_W; g_cv_glass.h = GL_H; g_cv_glass.px = (uint32_t *)calloc((size_t)GL_W * GL_H, 4);
    for (int i = 0; i < 6; i++) {
        g_cv_page[i].w = SCR_W; g_cv_page[i].h = (i == 5) ? PAD_CH : CTL_TH;
        g_cv_page[i].px = (uint32_t *)calloc((size_t)SCR_W * g_cv_page[i].h, 4);
        g_page_keys[i] = -1;
    }
    if (!g_cv_bg.px || !g_cv_top.px || !g_cv_glass.px) return 0;
    for (int i = 0; i < 6; i++) if (!g_cv_page[i].px) return 0;
    g_cv_meter.w = METER_W; g_cv_meter.h = METER_H; g_cv_meter.px = (uint32_t *)calloc((size_t)METER_W * METER_H, 4);
    if (!g_cv_meter.px) return 0;
    meter_render(&g_cv_meter);
    g_cv_mglass.w = METER_W + 8; g_cv_mglass.h = METER_H + 8; g_cv_mglass.px = (uint32_t *)calloc((size_t)(METER_W + 8) * (METER_H + 8), 4);
    if (!g_cv_mglass.px) return 0;
    meter_glass_render(&g_cv_mglass);
    g_cv_env.w = ENV_W; g_cv_env.h = ENV_H; g_cv_env.px = (uint32_t *)calloc((size_t)ENV_W * ENV_H, 4);
    if (!g_cv_env.px) return 0;
    g_cv_bgpad.w = SCR_W; g_cv_bgpad.h = SCR_H; g_cv_bgpad.px = (uint32_t *)malloc((size_t)SCR_W * SCR_H * 4);
    g_cv_glass2.w = GL2_W; g_cv_glass2.h = GL2_H; g_cv_glass2.px = (uint32_t *)calloc((size_t)GL2_W * GL2_H, 4);
    if (!g_cv_bgpad.px || !g_cv_glass2.px) return 0;
    skin_render_static(&g_cv_bg);
    skin_render_static_ex(&g_cv_bgpad, 1);
    glass_render(&g_cv_glass);
    glass_render_ex(&g_cv_glass2, PCRT_W, PCRT_H);
    ui_create_textures();
    return 1;
}

static void ui_free_textures(void) {
    if (g_tex_bg) { vita2d_free_texture(g_tex_bg); g_tex_bg = NULL; }
    if (g_tex_glass) { vita2d_free_texture(g_tex_glass); g_tex_glass = NULL; }
    if (g_tex_bgpad) { vita2d_free_texture(g_tex_bgpad); g_tex_bgpad = NULL; }
    if (g_tex_glass2) { vita2d_free_texture(g_tex_glass2); g_tex_glass2 = NULL; }
    for (int i = 0; i < 3; i++) if (g_tex_pctl[i]) { vita2d_free_texture(g_tex_pctl[i]); g_tex_pctl[i] = NULL; }
    if (g_tex_meter) { vita2d_free_texture(g_tex_meter); g_tex_meter = NULL; }
    if (g_tex_mglass) { vita2d_free_texture(g_tex_mglass); g_tex_mglass = NULL; }
    for (int i = 0; i < 2; i++) if (g_tex_top[i]) { vita2d_free_texture(g_tex_top[i]); g_tex_top[i] = NULL; }
    for (int i = 0; i < 3; i++) if (g_tex_ctl[i]) { vita2d_free_texture(g_tex_ctl[i]); g_tex_ctl[i] = NULL; }
    for (int i = 0; i < 2; i++) if (g_tex_env[i]) { vita2d_free_texture(g_tex_env[i]); g_tex_env[i] = NULL; }
}

/* после "сна" видеопамять могла обнулиться - создаём текстуры заново */
static void ui_recreate(void) {
    ui_free_textures();
    ui_create_textures();
}

static inline uint32_t sig_add(uint32_t h, uint32_t v) { h ^= v; return h * 16777619u; }

/* вызывать ДО vita2d_start_drawing: процессор пишет в текстуры только когда GPU их не читает */
static void ui_update_textures(const UiState *u) {
    uint32_t st = 2166136261u;
    st = sig_add(st, (uint32_t)u->state); st = sig_add(st, (uint32_t)u->loop);
    st = sig_add(st, (uint32_t)(u->bpm > 0 ? u->bpm_t : -1) + 7u);
    if (u->state == ST_RECORDING) st = sig_add(st, (u->frame / 24) & 1u);
    if (st != g_sig_top) {
        g_sig_top = st;
        top_render(&g_cv_top, u);
        g_top_cur = g_top_next; g_top_next = (g_top_next + 1) % 2;
        upload_canvas(g_tex_top[g_top_cur], &g_cv_top);
    }
    if (u->page == 5) {                                    /* огибающая пада: перерисовывается только при изменении */
        const Pad *p = &g_pad[g_pu.sel];
        uint32_t h = 2166136261u;
        int vals[11] = { g_pu.sel, p->a, p->dc, p->s, p->r, p->start, p->n, (int)(uintptr_t)p->d, p->mode, p->loading, g_pad_env_sub };
        for (int i = 0; i < 11; i++) h = sig_add(h, (uint32_t)vals[i]);
        if (h != g_sig_env || g_env_cur < 0) {
            g_sig_env = h;
            env_render(&g_cv_env, p, g_pad_env_sub);
            g_env_cur = g_env_next; g_env_next = (g_env_next + 1) % 2;
            upload_canvas(g_tex_env[g_env_cur], &g_cv_env);
        }
    }
    {   /* у каждой страницы свой холст: при переключении ничего не перерисовывается заново */
        int page = u->page;
        memcpy(g_wcode, g_wcodes[page], sizeof(g_wcode));
        g_page_key = g_page_keys[page];
        int ch = ctl_update(&g_cv_page[page], u);
        memcpy(g_wcodes[page], g_wcode, sizeof(g_wcode));
        g_page_keys[page] = g_page_key;
        if (ch || page != g_last_page || g_page_key != g_last_key || g_ctl_cur < 0) {
            g_last_page = page; g_last_key = g_page_key;
            g_ctl_cur = g_ctl_next; g_ctl_next = (g_ctl_next + 1) % 3;
            upload_canvas(page == 5 ? g_tex_pctl[g_ctl_cur] : g_tex_ctl[g_ctl_cur], &g_cv_page[page]);
        }
    }
}

/* ====================================================================
 *  Текст и мелочи поверх текстур
 * ==================================================================== */
static int tw(const char *s, float sc) { return text_w(s, sc); }
static void text_c(int cx, int y, unsigned int col, float sc, const char *s) { text(cx - tw(s, sc) / 2, y, col, sc, s); }
static void text_r(int rx, int y, unsigned int col, float sc, const char *s) { text(rx - tw(s, sc), y, col, sc, s); }

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
            if (u->rev && ex - sx > 90) text(sx + 22, WAVE_Y + 14, K_YELLOW, 0.8f, "<< REVERSE <<");
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
            if ((int64_t)vl * WAVE_W / w / steps[i] <= 24) { step = steps[i]; break; }
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

/* шкала времени вдоль нижнего края волны */
static void draw_time_axis(const UiState *u) {
    const Layout *L = &u->lay;
    if (u->length <= 0 || u->live) return;
    static const double steps[] = { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300 };
    const int n = (int)(sizeof(steps) / sizeof(steps[0]));
    for (int sg = 0; sg < L->nseg; sg++) {
        int vs = L->vs[sg], vl = L->vl[sg], w = L->w[sg];
        if (vl <= 0) continue;
        double px_per_s = (double)SAMPLE_RATE * (double)w / (double)vl;
        double step = steps[n - 1];
        for (int i = 0; i < n; i++) if (steps[i] * px_per_s >= 84.0) { step = steps[i]; break; }
        double t0 = (double)vs / (double)SAMPLE_RATE, t1 = (double)(vs + vl) / (double)SAMPLE_RATE;
        long long k0 = (long long)ceil(t0 / step - 1e-9);
        for (long long k = k0; (double)k * step <= t1 && k - k0 < 14; k++) {
            double t = (double)k * step;
            int x = WAVE_X + L->x0[sg] + (int)((t * (double)SAMPLE_RATE - (double)vs) * (double)w / (double)vl);
            if (x < WAVE_X + L->x0[sg] || x > WAVE_X + L->x0[sg] + w - 30) continue;
            char lb[24];
            if (t >= 60.0) snprintf(lb, sizeof(lb), "%d:%02d", (int)(t / 60.0), (int)fmod(t, 60.0));
            else if (step >= 1.0) snprintf(lb, sizeof(lb), "%ds", (int)(t + 0.5));
            else if (step >= 0.1) snprintf(lb, sizeof(lb), "%.1fs", t);
            else if (step >= 0.01) snprintf(lb, sizeof(lb), "%.2fs", t);
            else snprintf(lb, sizeof(lb), "%.3fs", t);
            rect(x, WAVE_Y + WAVE_H - 7, 1, 7, RGBA8(255, 220, 160, 170));
            text(x + 3, WAVE_Y + WAVE_H - 3, RGBA8(255, 220, 160, 200), 0.62f, lb);
        }
    }
}


static int g_legend = 0;

/* зёрна: тонкие линии с плавным проявлением и затуханием вокруг линии воспроизведения */
static void draw_grain_viz(const UiState *u) {
    const Layout *L = &u->lay;
    if (u->length <= 1 || u->live || !u->gr_on || L->vl[0] <= 0) return;
    double pxps = (double)L->w[0] / (double)L->vl[0];
    int xh = sample_to_x(L, 0, (double)u->playhead);
    if (xh == -1000000) return;
    float sp = (float)g_gr_spr / 100.0f;
    int half = (int)(16.0 * 1000.0 * sp * sp * pxps);
    if (half > WAVE_W / 2) half = WAVE_W / 2;
    if (half < 3) half = 3;
    int n = 1 + g_gr_n;
    int lenpx = (int)(20.0f * powf(25.0f, (float)g_gr_size / 100.0f) * 16.0f * pxps);
    if (lenpx < 28) lenpx = 28;
    if (lenpx > 260) lenpx = 260;
    rect(xh - half, WAVE_Y, 2 * half, WAVE_H, RGBA8(255, 200, 90, 14));          /* зона разброса: лёгкое облако */
    rect(xh - half, WAVE_Y + 10, 2 * half, 1, RGBA8(255, 224, 90, 200));
    rect(xh - half, WAVE_Y + 5, 1, 11, RGBA8(255, 224, 90, 200));
    rect(xh + half - 1, WAVE_Y + 5, 1, 11, RGBA8(255, 224, 90, 200));
    int rowh = (WAVE_H - 44) / n;
    if (rowh > 14) rowh = 14;
    if (rowh < 3) rowh = 3;
    uint32_t seed = (u->frame / 45) * 2654435761u + 12345u;
    for (int i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u + (uint32_t)i * 7919u;
        float r = (float)((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        int x0 = xh + (int)(r * (float)half) - lenpx / 2, y = WAVE_Y + 28 + i * rowh;
        for (int s = 0; s < 12; s++) {
            int x1 = x0 + s * lenpx / 12;
            if (x1 < WAVE_X || x1 > WAVE_X + WAVE_W - 4) continue;
            float t = ((float)s + 0.5f) / 12.0f;
            int a = (int)(255.0f * 0.5f * (1.0f - cosf(6.2831853f * t)));
            rect(x1, y, lenpx / 12 + 1, 2, RGBA8(255, 232, 170, a));
        }
    }
}

static void trunc_name(char *dst, size_t cap, const char *src, int maxc) {
    snprintf(dst, cap, "%s", src);
    if ((int)strlen(dst) > maxc) { dst[maxc - 1] = '.'; dst[maxc] = 0; }
}

/* маленький осциллограф на странице PAD: заголовок, форма волны, линии воспроизведения звучащих голосов */
static void draw_pad_crt(const UiState *u) {
    const Pad *p = &g_pad[g_pu.sel];
    char buf[200], nm[48];
    snprintf(buf, sizeof(buf), "PAD %02d", g_pu.sel + 1);
    text_m(ENV_X, 90, K_AMBER, 1.0f, buf);
    if (p->d) { trunc_name(nm, sizeof(nm), p->name, 26); text(ENV_X + 96, 81, K_AMBER_LT, 0.66f, nm); }
    else text(ENV_X + 96, 81, K_AMBER_DIM, 0.66f, "empty - press LOAD WAV");
    if (p->d) {
        int tenth = (int)((int64_t)p->n * 10 / (p->rate > 0 ? p->rate : 1));
        snprintf(buf, sizeof(buf), "%d.%d s   %d Hz   %s", tenth / 10, tenth % 10, p->rate, g_pmode_name[p->mode]);
    } else snprintf(buf, sizeof(buf), "%s", g_pmode_name[p->mode]);
    if (g_pad_env_sub == 2) { char b2[40]; snprintf(b2, sizeof(b2), "   COMP  in -48..0 dB"); strncat(buf, b2, sizeof(buf) - strlen(buf) - 1); }
    text(ENV_X + 96, 94, K_AMBER_DIM, 0.55f, buf);
    if (g_env_cur >= 0 && g_tex_env[g_env_cur]) vita2d_draw_texture(g_tex_env[g_env_cur], ENV_X, ENV_Y);
    if (p->d && p->n > 1)
        for (int k = 0; k < PAD_VOICES; k++) {
            const PVoice *v = &g_pv[k];
            if (v->stage == 0 || v->pad != g_pu.sel) continue;
            int x = ENV_X + (int)(v->pos * (double)ENV_W / (double)p->n);
            if (x >= ENV_X && x < ENV_X + ENV_W) rect(x, ENV_Y, 2, ENV_H, RGBA8(255, 236, 176, 230));
        }
}

/* список WAV поверх нижней правой части: стрелки + X, или касание строки */
static void draw_pad_list(void) {
    rect(LST_X, LST_Y, LST_W, LST_H, RGBA8(22, 14, 8, 250));
    rect_outline(LST_X, LST_Y, LST_W, LST_H, 2, K_ORANGE_LO);
    char buf[120];
    snprintf(buf, sizeof(buf), "WAV FOR PAD %02d", g_pu.sel + 1);
    text(LST_X + 12, LST_Y + 22, K_YELLOW, 0.75f, buf);
    text(LST_X + 168, LST_Y + 22, K_AMBER_DIM, 0.5f, "UP/DOWN + X = assign   O = close   drag = scroll");
    if (g_pu.list_n == 0) {
        text(LST_X + 12, LST_Y + 70, K_AMBER_LT, 0.7f, "no .wav files in ux0:data/Sampler");
        text(LST_X + 12, LST_Y + 92, K_AMBER_DIM, 0.6f, "record something and press START to save it");
        return;
    }
    for (int i = 0; i < PL_ROWS; i++) {
        int k = g_pu.list_top + i;
        if (k >= g_pu.list_n) break;
        int y = LST_ROW0 + i * LST_RH;
        if (k == g_pu.list_sel) rect(LST_X + 6, y, LST_W - 22, LST_RH - 2, RGBA8(255, 150, 46, 60));
        text(LST_X + 14, y + 18, k == g_pu.list_sel ? K_AMBER : K_AMBER_LT, 0.74f, g_pu.list[k]);
    }
    int track = PL_ROWS * LST_RH;
    int sbh = track * PL_ROWS / (g_pu.list_n > PL_ROWS ? g_pu.list_n : PL_ROWS);
    int sby = LST_ROW0 + (g_pu.list_n > PL_ROWS ? (track - sbh) * g_pu.list_top / (g_pu.list_n - PL_ROWS) : 0);
    rect(LST_X + LST_W - 12, LST_ROW0, 3, track, RGBA8(255, 150, 46, 30));
    rect(LST_X + LST_W - 13, sby, 5, sbh, RGBA8(255, 170, 70, 200));
}

static void ui_draw(const UiState *u) {
    knobs_relayout(u->page);
    char buf[200], t1[32];
    static const char *page_name[6] = { "EDIT", "SLICE", "GRAIN", "ADV", "FX", "PAD" };

    vita2d_draw_texture(u->page == 5 ? g_tex_bgpad : g_tex_bg, 0, 0);
    if (g_top_cur >= 0) vita2d_draw_texture(g_tex_top[g_top_cur], 0, 0);

    /* верхняя планка */
    text(50, 22, K_BROWN, 1.1f, "VITA SAMPLER");
    text(50, 34, K_BROWN_LO, 0.5f, "SAMPLER / LOOPER   v1.7");
    text(560, 26, K_BROWN, 0.62f, "REC");
    text(648, 26, K_BROWN, 0.62f, "PLAY");
    text(736, 26, K_BROWN, 0.62f, "LOOP");
    text(774, 26, K_BROWN, 0.62f, "BPM");

    /* названия режимов: просто текст, выбранный обведён */
    for (int i = 0; i < 6; i++) {
        int x, y, w, h;
        btn_geom(B_TAB0 + i, &x, &y, &w, &h);
        int sel = (u->page == g_tab_page[i]);
        if (sel) {
            rect(x, y, w, h, RGBA8(250, 246, 234, 255));
            rect_outline(x, y, w, h, 2, K_BROWN);
        }
        text_c(x + w / 2, y + h / 2 + 5, sel ? K_BROWN : K_BROWN_LO, 0.74f, page_name[i]);
    }

    if (u->page == 5) draw_pad_crt(u);
    else {
    /* окно ЭЛТ: время, номер сэмпла, уровень */
    int shown = u->live ? u->length : u->playhead;
    fmt_time(t1, sizeof(t1), shown);
    text_m(WAVE_X, 90, K_AMBER, 1.5f, t1);
    snprintf(buf, sizeof(buf), "sample %d / %d", shown, u->length);
    text(WAVE_X + 190, 76, K_AMBER_LT, 0.72f, buf);
    {
        char t2[32];
        fmt_time(t2, sizeof(t2), u->length);
        int tenths = (int)((int64_t)u->view_len * 10 / WAVE_W);
        int zoomx = u->view_len > 0 ? u->length / u->view_len : 1;
        if (zoomx < 1) zoomx = 1;
        if (u->live) snprintf(buf, sizeof(buf), "max 05:00");
        else snprintf(buf, sizeof(buf), "total %s   zoom x%d   1 px = %d.%d smp%s", t2, zoomx, tenths / 10, tenths % 10, u->split ? "   SPLIT" : "");
        text(WAVE_X + 190, 91, K_AMBER_DIM, 0.62f, buf);
    }
    if (u->clip > 0) text(WAVE_X + WAVE_W - 62, 84, K_WAVE_REC, 0.85f, "CLIP!");

    /* волна */
    if (u->wave_tex) vita2d_draw_texture(u->wave_tex, WAVE_X, WAVE_Y);
    if (u->length == 0 && !u->live) text_c(WAVE_X + WAVE_W / 2, WAVE_Y + 92, K_AMBER_DIM, 0.9f, "no recording yet - press R");
    if (!u->live && u->length > 1) {
        if (u->mode == 0) draw_overlays(u); else if (u->page != 2) draw_slice_overlays(u);      /* в GRAIN регионы слайсера не показываем */
        if (u->page == 2) draw_grain_viz(u);
        draw_ticks(u);
        draw_time_axis(u);
        if (u->page == 2) {
            char a1[24], a2[24], a3[24], a4[24], a5[24];
            knob_text(4, a1, sizeof(a1)); knob_text(5, a2, sizeof(a2)); knob_text(6, a3, sizeof(a3)); knob_text(3, a4, sizeof(a4)); knob_text(11, a5, sizeof(a5));
            snprintf(buf, sizeof(buf), "GRAINS %s   SIZE %s   SPREAD %s   DETUNE %s   MIX %s   %s", a1, a2, a3, a5, a4, u->gr_on ? "ON" : "OFF");
            if (u->kb_note >= 0) { char kb[24]; snprintf(kb, sizeof(kb), "   KEY %+d st", u->kb_note - 12); strncat(buf, kb, sizeof(buf) - strlen(buf) - 1); }
        } else if (u->page == 3) {
            static const char *ft[4] = { "OFF", "LOWPASS", "HIGHPASS", "BANDPASS" };
            char a1[24], a2[24];
            knob_text(7, a1, sizeof(a1)); knob_text(8, a2, sizeof(a2));
            if (u->flt_type == 0) snprintf(buf, sizeof(buf), "filter %s    (12 dB/oct, no resonance)", ft[0]);
            else if (u->flt_type == 3) snprintf(buf, sizeof(buf), "filter %s    center %s    width %s    (12 dB/oct per edge)", ft[3], a1, a2);
            else snprintf(buf, sizeof(buf), "filter %s    cutoff %s    (12 dB/oct, no resonance)", ft[u->flt_type], a1);
        } else if (u->page == 4) {
            snprintf(buf, sizeof(buf), "ADVANCED edit of region A-B (%d smp):  delete / silence / duplicate / add silence after it    UNDO is in EDIT", (int)(u->c2 - u->c1));
        } else if (u->mode == 0)
            snprintf(buf, sizeof(buf), "A %d    B %d    region %d smp    active: %c", (int)u->c1, (int)u->c2, (int)(u->c2 - u->c1), u->active == 0 ? 'A' : 'B');
        else {
            snprintf(buf, sizeof(buf), "slice %d / %d    start %d    length %d smp    %s", u->sl_sel + 1, u->sl_n,
                     g_sl_pos[u->sl_sel], g_sl_pos[u->sl_sel + 1] - g_sl_pos[u->sl_sel],
                     u->bpm > 0 ? "tempo ~" : "");
            if (u->bpm > 0) { char bb[24]; snprintf(bb, sizeof(bb), "%d BPM", u->bpm); strncat(buf, bb, sizeof(buf) - strlen(buf) - 1); }
        }
        text(WAVE_X, 312, K_AMBER_DIM, 0.72f, buf);
    }
    if (u->live) text(WAVE_X, 312, K_AMBER_DIM, 0.72f, "live view: last 8 seconds");
    if (u->scrubbing) {
        snprintf(buf, sizeof(buf), "scrub speed %d%%", (int)(fabs(u->scrub_speed) / (double)SAMPLE_RATE * 100.0 + 0.5));
        text(WAVE_X + WAVE_W - 150, 312, K_AMBER, 0.72f, buf);
    }
    }
    if (u->page == 5) text(PBZ_X + 4, 226, u->confirm ? K_YELLOW : K_BROWN, 0.66f, u->msg);
    else text(WAVE_X, 329, u->confirm ? K_YELLOW : K_AMBER_LT, 0.82f, u->msg);

    /* справка поверх экрана */
    if (u->legend) {
        rect(CRT_X + 4, CRT_Y + 4, CRT_W - 8, CRT_H - 8, RGBA8(22, 14, 8, 252));
        rect_outline(CRT_X + 4, CRT_Y + 4, CRT_W - 8, CRT_H - 8, 2, K_ORANGE_LO);
        static const char *L[] = {
            "CONTROLS                                                                      (SELECT closes)",
            "R  record / stop     L  play / pause  (hold L = glitch; rear panel: finger up / down = rate 1-30 Hz)",
            "TRIANGLE  stop (hold = keep position)    TRIANGLE x2  reset A-B    SQUARE  loop    CIRCLE  FX page",
            "START  save WAV (+ slices in SLICE)    SELECT  help    SELECT+LEFT/RIGHT  next / previous tab    SELECT+START hold  exit",
            "EDIT   sticks = cursors A / B     both sticks up / down = zoom     D-pad left/right = active cursor",
            "       touch the top of the wave next to A or B and drag = move it; inside A-B = move both",
            "       CROP (tap twice)  UNDO  REVERSE (hold = bake)  NORMALIZE      touch lower part of the wave = scrub",
            "SLICE  regions are built inside A-B chosen in EDIT     X  play slice     BPM +/- = tempo (speed follows)",
            "GRAIN  cloud of grains around the play line: MIX, GRAINS, SIZE, SPREAD, DETUNE     GRAIN ON  switch     KEYS  pitch",
            "ADV    DELETE region   SILENCE region   DUPLICATE region   + SILENCE (same width, right of the region)",
            "FX     FILTER: LPF / HPF / BPF (BPF = center + width), drag in the curve window     CRUSH: bits, rate",
            "KNOBS  touch a knob and drag up / down (or left / right); double tap = default",
        };
        for (int i = 0; i < 12; i++) text(CRT_X + 16, CRT_Y + 30 + i * 19, i == 0 ? K_YELLOW : K_AMBER_LT, 0.72f, L[i]);
    }

    /* стекло перед ЭЛТ */
    if (u->page == 5) { if (g_tex_glass2) vita2d_draw_texture(g_tex_glass2, PCRT_X - GL_M, PCRT_Y - GL_M); }
    else if (g_tex_glass) vita2d_draw_texture(g_tex_glass, CRT_X - GL_M, CRT_Y - GL_M);

    /* индикатор уровня: сплошной градиент в стеклянной трубке, пик, засечки, окна (на странице PAD его нет) */
    if (u->page != 5) {
        int lit = u->level_pct * METER_H / 1000;
        if (lit > METER_H) lit = METER_H;
        if (lit > 0 && g_tex_meter)
            vita2d_draw_texture_part(g_tex_meter, (float)METER_X, (float)(METER_Y + METER_H - lit), 0.0f, (float)(METER_H - lit), (float)METER_W, (float)lit);
        int ph = u->peak_pct * METER_H / 1000;
        if (ph > 1 && ph <= METER_H) rect(METER_X, METER_Y + METER_H - ph, METER_W, 2, RGBA8(255, 236, 176, 235));
        for (int k = 0; k <= METER_TICKS; k += 2) {
            char nb[8];
            snprintf(nb, sizeof(nb), "%d", k * (METER_DB / METER_TICKS));
            text_r(METER_X - 9, METER_Y + k * (METER_H - 1) / METER_TICKS + 3, K_BROWN, 0.42f, nb);
        }
        for (int k = 0; k <= METER_TICKS; k++) {                       /* засечки каждые 3 дБ на краях трубки (поверх заливки) */
            int yy = METER_Y + k * (METER_H - 1) / METER_TICKS, tl = (k % 2 == 0) ? 5 : 3;
            rect(METER_X, yy, tl, 1, RGBA8(20, 12, 6, 150));
            rect(METER_X + METER_W - tl, yy, tl, 1, RGBA8(20, 12, 6, 150));
        }
        if (g_tex_mglass) vita2d_draw_texture(g_tex_mglass, METER_X - 4, METER_Y - 4);   /* стекло поверх трубки */
        if (u->clip > 0) { rect(899, 49, 50, 20, RGBA8(214, 32, 20, 255)); text_c(924, 63, K_CREAM_HI, 0.62f, "CLIP"); }
        else text_c(924, 63, RGBA8(120, 78, 40, 255), 0.62f, "CLIP");
        if (u->peak_pct > 0) {
            int d10 = (u->peak_pct * METER_DB * 10 / 1000) - METER_DB * 10;
            char pb[12];
            snprintf(pb, sizeof(pb), "%d.%d", d10 / 10, abs(d10) % 10);
            text_c(924, 336, RGBA8(255, 138, 40, 255), 0.7f, pb);
        } else text_c(924, 336, RGBA8(120, 78, 40, 255), 0.7f, "-inf");
    }

    /* органы управления */
    if (g_ctl_cur >= 0) {
        if (u->page == 5) vita2d_draw_texture(g_tex_pctl[g_ctl_cur], 0, PAD_CY);
        else vita2d_draw_texture(g_tex_ctl[g_ctl_cur], 0, CTL_TY);
    }
    char v[48];
    for (int k = 0; k < N_KNOBS; k++) {
        if (!knob_visible(k, u->page, u->fx_sec)) continue;
        int dim = (k == 8 && u->flt_type != 3) || (k == 7 && u->flt_type == 0);
        const char *lb = g_kd[k].label;
        if (k == 7 && u->flt_type != 3) lb = "CUTOFF";
        text_c(g_kd[k].cx, g_kd[k].cy + g_kd[k].r + 25, dim ? K_BROWN_LO : K_BROWN, 0.66f, lb);
        knob_text(k, v, sizeof(v));
        int isdef = (*g_kd[k].v == g_kd[k].def);
        text_c(g_kd[k].cx, g_kd[k].cy + g_kd[k].r + 38, dim ? K_BROWN_LO : (isdef ? K_BROWN_LO : K_ORANGE_LO), 0.62f, v);
    }
    if (u->page == 0) {
        static const char *names[4] = { "CROP", "UNDO", "REVERSE", "NORMALIZE" };
        for (int id = B_CROP; id <= B_NORM; id++) {
            int x, y, w, h;
            btn_geom(id, &x, &y, &w, &h);
            int st = edit_style(u, id), pr = (u->btn_down == id) || st;
            text_c(x + w / 2, y + h - 18 + (pr ? 2 : 0), (id == B_UNDO && !u->can_undo) ? K_BROWN_LO : K_BROWN, 0.68f, names[id - B_CROP]);
        }
        text(332, 500, K_BROWN, 0.58f, "SELECT help     START save WAV     TRIANGLE x2 reset A-B");
        text(332, 514, K_BROWN, 0.58f, "hold L = glitch (rear panel: up / down = rate)     drag a knob: up / down");
    } else if (u->page == 4) {
        static const char *an[4] = { "DELETE", "SILENCE", "DUPLICATE", "+ SILENCE" };
        for (int id = B_ADV0; id <= B_ADV3; id++) {
            int x, y, w, h;
            btn_geom(id, &x, &y, &w, &h);
            text_c(x + w / 2, y + h - 18 + ((u->btn_down == id) ? 2 : 0), K_BROWN, 0.66f, an[id - B_ADV0]);
        }
        text(332, 500, K_BROWN, 0.58f, "all four work on the region A-B chosen in EDIT");
        text(332, 514, K_BROWN, 0.58f, "+ SILENCE inserts a gap of the same width right after B     UNDO is in EDIT");
    } else if (u->page == 1) {
        for (int i = 0; i < u->sl_n; i++) {
            int x, y, w, h;
            pad_geom(i, u->sl_n, &x, &y, &w, &h);
            char nb[8];
            snprintf(nb, sizeof(nb), "%d", i + 1);
            int on = (i == u->sl_sel) || (i == u->sl_playing);
            float sc = h < 40 ? 0.62f : (h < 60 ? 0.85f : 1.2f);
            text_c(x + w / 2, y + h / 2 + (int)(6 * sc) + (((u->btn_down == B_PAD0 + i) || on) ? 2 : 0), K_BROWN, sc, nb);
        }
        text(796, 390, K_BROWN, 0.66f, "SLICES");
        text(796, 466, u->bpm_t != u->bpm_b ? K_ORANGE_LO : K_BROWN, 0.66f, "BPM");
        if (u->bpm > 0) { snprintf(buf, sizeof(buf), "orig %d", u->bpm_b); text(850, 466, K_BROWN_LO, 0.55f, buf); }
    } else if (u->page == 5) {
        for (int i = 0; i < PAD_N; i++) {
            int x, y, w, h;
            pad5_geom(i, &x, &y, &w, &h);
            int sl = (g_pu.sel == i), pr = sl || ((g_pu.down_mask >> i) & 1), off = pr ? 2 : 0;
            char nb[8], nm[24];
            snprintf(nb, sizeof(nb), "%d", i + 1);
            text(x + 13, y + 29 + off, sl ? K_CREAM_HI : K_BROWN, 0.95f, nb);
            if (g_pad[i].d) {
                trunc_name(nm, sizeof(nm), g_pad[i].name, 15);
                text_c(x + w / 2, y + h - 16 + off, sl ? K_CREAM_HI : K_BROWN, 0.55f, nm);
            } else text_c(x + w / 2, y + h - 16 + off, sl ? K_CREAM_HI : K_BROWN_LO, 0.55f, "empty");
        }
        static const char *bl[5] = { "LOAD WAV", NULL, "ADSR", "COMP", "CLEAR" };
        static const char *bs[5] = { "pick a file for this pad", NULL, "volume envelope", "compressor", "remove sample" };
        static const int bid[5] = { B_PLOAD, B_PMODE, B_PENV, B_PCMP, B_PCLR };
        for (int k = 0; k < 5; k++) {
            int x, y, w, h;
            btn_geom(bid[k], &x, &y, &w, &h);
            int on = (k == 0 && g_pu.list_open) || (k == 2 && g_pad_env_sub == 1) || (k == 3 && g_pad_env_sub == 2);
            int off = ((u->btn_down == bid[k]) || on) ? 2 : 0;
            if (k == 1) {
                text_c(x + w / 2, y + 28 + off, K_BROWN_LO, 0.55f, "MODE");
                text_c(x + w / 2, y + 47 + off, K_BROWN, 0.82f, g_pmode_name[g_pad[g_pu.sel].mode]);
            } else {
                int back = (k == 2 && g_pad_env_sub == 1) || (k == 3 && g_pad_env_sub == 2);
                text_c(x + w / 2, y + 30 + off, K_BROWN, k >= 2 ? 0.76f : 0.82f, back ? "BACK" : bl[k]);
                text_c(x + w / 2, y + 48 + off, K_BROWN_LO, 0.46f, back ? "volume / pitch / start" : bs[k]);
            }
        }
        text(10, 536, K_BROWN_LO, 0.5f, "pads: touch or X     D-pad: select     SELECT+arrows: tabs");
        if (g_pu.list_open) draw_pad_list();
    } else if (u->page == 2) {
        text_c(885, 461 + (((u->btn_down == B_GR_ON) || u->gr_on) ? 2 : 0), K_BROWN, 0.68f, u->gr_on ? "GRAIN ON" : "GRAIN OFF");
        text_c(885, 504, K_BROWN, 0.55f, "same pitch and speed,");
        text_c(885, 517, K_BROWN, 0.55f, "fade in / fade out");
    } else {
        static const char *sec[2] = { "FILTER", "CRUSH" };
        for (int i = 0; i < 2; i++) {
            int x, y, w, h;
            btn_geom(B_FSEC0 + i, &x, &y, &w, &h);
            if (u->fx_sec == i) { rect(x, y, w, h, RGBA8(250, 246, 234, 255)); rect_outline(x, y, w, h, 2, K_BROWN); }
            text_c(x + w / 2, y + h / 2 + 5, u->fx_sec == i ? K_BROWN : K_BROWN_LO, 0.75f, sec[i]);
        }
        if (u->fx_sec == 0) {
            static const char *ft[4] = { "OFF", "LPF", "HPF", "BPF" };
            for (int i = 0; i < 4; i++) {
                int x, y, w, h;
                btn_geom(B_FT0 + i, &x, &y, &w, &h);
                int sel = (u->flt_type == i);
                text_c(x + w / 2, y + h - 15 + (((u->btn_down == B_FT0 + i) || sel) ? 2 : 0), K_BROWN, 0.62f, ft[i]);
            }
            static const char *fl[3] = { "100 Hz", "1 kHz", "4 kHz" };
            static const float ff[3] = { 100.0f, 1000.0f, 4000.0f };
            for (int i = 0; i < 3; i++) text(CURVE_X + curve_x_of(ff[i]) + 3, CURVE_Y + CURVE_H - 5, K_AMBER_DIM, 0.5f, fl[i]);
            text(CURVE_X + CURVE_W - 38, CURVE_Y + (int)curve_y_of(0.0f) - 3, K_AMBER_DIM, 0.5f, "0 dB");
            text(CURVE_X + CURVE_W - 44, CURVE_Y + (int)curve_y_of(-12.0f) - 3, K_AMBER_DIM, 0.5f, "-12 dB");
            text(CURVE_X + CURVE_W - 44, CURVE_Y + (int)curve_y_of(-24.0f) - 3, K_AMBER_DIM, 0.5f, "-24 dB");
            text(CURVE_X + 6, CURVE_Y + CURVE_H + 17, K_BROWN, 0.5f, "drag in the window: left/right = center, up/down = width");
        } else {
            text(560, 391, K_BROWN, 0.58f, "lo-fi: fewer bits and a lower sample rate");
        }
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
static char msg[160] = "Press R to record, SELECT = help";

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
    int page, fx_sec, x_hold;
    int knob_on, knob_k, knob_id, knob_last_k; unsigned knob_last_frame;
    int kb_on, kb_id, kb_started, kb_loop0;      /* клавиатура GRAIN: касание, запустила ли она воспроизведение, прежний LOOP */
    double knob_x0, knob_y0, knob_v0;
    int curve_on, curve_id;
    int confirm; unsigned confirm_frame;
    int crop_armed; unsigned crop_frame;
    int tri_active, tri_frames, tri_pos;
    unsigned tri_last;
    int btn_hold, gl_period;
    float gl_hz;
    int sdrag_on, sdrag_id, sdrag_idx;
    double sdrag_x0, sdrag_c0, sdrag_spp;
    double scrub_head;
    int scrubbing, scrub_id, resume_state, scrub_seg;
    double scrub_pos, scrub_speed, mk_frac[2];
    ScrubTrack st;
    uint64_t prev_touch_t;
    int sl_flash;
    int btn_down, btn_id;
    int prev_ids[6], prev_n;
    unsigned frame;
    int nudge_frames, zoom_frames, nav_frames, cnt_frames;
    int l_frames, l_pre, l_glitch, l_acc;
    int drag_on[3], drag_id[3];                  /* 0 = маркер A, 1 = маркер B, 2 = вся область */
    double drag_x0[3], drag_spp[3], drag_c0[3], drag_c1_0, drag_c2_0;
    int pz_on, pz_id[2];                         /* зум двумя пальцами по волне */
    double pz_d0, pz_len0, pz_cs;
    int pt_id[4], pt_pad[4];                     /* касания / кнопка X, держащие пады PAD (-1 свободно, -2 кнопка X) */
    int ph_on, ph_id, ph_k;                      /* ручка огибающей */
    int ls_on, ls_id, ls_moved; double ls_y0; int ls_top0;   /* прокрутка списка WAV */
    int ls_rep; double ls_fx, ls_fy;
} App;
static App A;

/* согласованная передача области и перемотки в аудио-поток */
static void region_publish(int lo, int hi, int seek) {
    g_rg_seq++;
    __sync_synchronize();
    g_lo = lo; g_hi = hi; g_rev = g_rev_req;
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
    undo_clear();
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

static void act_set_mode(int m);
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
        snprintf(msg, sizeof(msg), "Unsaved recording! Press R again to erase it, or START to save first");
        return;
    }
    g_state = ST_IDLE;
    stop_scrub_state();
    sceKernelDelayThread(60000);
    for (int w = 0; w < 300 && (g_save_req || g_save_busy); w++) sceKernelDelayThread(10000);
    g_rev_req = 0; g_rev = 0;
    g_length = 0;
    g_play_pos = 0;
    g_seek_to = -1;
    g_peak_all = 0;
    g_error = 0;
    g_discard = 3;
    dirty = 0;
    A.confirm = 0;
    undo_clear();
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

/* клавиатура GRAIN: нажатие запускает воспроизведение (если стоит) в петле, отпускание - плавно останавливает */
static void kb_press(int note) {
    g_kb_semi = note - 12;
    if (!g_kb_on && !A.kb_started && g_state == ST_IDLE && g_length > 1) {
        A.kb_loop0 = g_loop; g_loop = 1; A.kb_started = 1;
        act_play_toggle();
    }
    g_kb_on = 1;
}
static void kb_release(void) {
    g_kb_on = 0;
    if (A.kb_started) {
        A.kb_started = 0;
        if (g_state == ST_PLAYING) g_state = ST_IDLE;
        g_loop = A.kb_loop0;
    }
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
    g_play_pos = g_rev ? hi - 1 : lo;
    g_seek_to = -1;
    snprintf(msg, sizeof(msg), g_mode == 0 ? "Stopped - next play starts from A   (hold TRIANGLE to keep the position)"
                                           : "Stopped - next play starts from the slice start");
    dbg_log("stop");
}

/* двойной Треугольник: сбросить выбор региона */
static void act_reset_selection(void) {
    if (g_state == ST_RECORDING || g_length < 2) return;
    if (g_mode == 0) {
        c1 = 0.0; c2 = (double)g_length; active = 0; split = 0;
        view_reset_full(g_length);
        apply_region();
        g_play_pos = 0;
        snprintf(msg, sizeof(msg), "Selection reset: A-B = whole sample");
    } else {
        g_sl_sel = 0;
        apply_region();
        g_play_pos = g_sl_pos[0];
        snprintf(msg, sizeof(msg), "Slice 1 selected");
    }
}

static void act_loop_toggle(void) {
    g_loop = !g_loop;
    snprintf(msg, sizeof(msg), g_loop ? "Loop ON" : "Loop off");
    dbg_log("loop %d", g_loop);
}

/* страницы: 0 EDIT, 1 SLICE, 2 GRAIN, 3 FX, 4 ADV (EDIT / SLICE - режимы, ADV работает в EDIT, GRAIN и FX - над текущим режимом) */
static void act_tab(int t) {
    if (t == 0 || t == 2 || t == 4) { act_set_mode(0); if (g_mode == 0) A.page = t; }   /* GRAIN работает по области A-B (вся дорожка или выбранная), слайсы не мешают */
    else if (t == 1) { act_set_mode(1); if (g_mode == 1) A.page = 1; }
    else if (t == 5) A.page = 5;                       /* PAD не зависит от режима EDIT / SLICE */
    else A.page = t;
    A.knob_on = 0; A.curve_on = 0;
}

/* SELECT + стрелки: соседняя вкладка в том порядке, как они стоят сверху */
static void act_tab_step(int d) {
    int idx = 0;
    for (int i = 0; i < 6; i++) if (g_tab_page[i] == A.page) idx = i;
    idx += d;
    if (idx < 0 || idx > 5) return;
    act_tab(g_tab_page[idx]);
}

static void act_fx_toggle(void) {
    if (A.page == 3) A.page = g_mode; else A.page = 3;
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
    if (nl < 0) { snprintf(msg, sizeof(msg), nl == -2 ? "Not enough memory for undo - crop cancelled" : "Crop failed"); return; }
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

static int edit_prepare(void) {
    if (g_mode != 0 || g_state == ST_RECORDING || g_length < 256) return 0;
    g_state = ST_IDLE;
    stop_scrub_state();
    sceKernelDelayThread(60000);
    return 1;
}

static void edit_finish(const char *text) {
    recompute_blocks(g_length);
    g_audio_ver++;
    dirty = 1;
    request_autosave(g_length, 1);
    snprintf(msg, sizeof(msg), "%s   (UNDO: %d step%s)", text, g_un, g_un == 1 ? "" : "s");
}

static void act_reverse_toggle(void) {
    if (g_mode != 0 || g_state == ST_RECORDING || g_length < 16) return;
    apply_region();
    g_rev_req = !g_rev_req;
    int seek = (g_state == ST_PLAYING && !g_scrub) ? g_play_pos : -1;     /* позиция на экране остаётся прежней */
    region_publish(g_lo, g_hi, seek);
    snprintf(msg, sizeof(msg), g_rev_req ? "REVERSE ON: region plays backwards (hold the button = bake into the sample)"
                                          : "Reverse off");
}

static void act_reverse(void) {
    if (!edit_prepare()) return;
    int lo = (int)c1, hi = (int)c2;
    if (lo < 0) lo = 0;
    if (hi > g_length) hi = g_length;
    if (hi - lo < 16) { snprintf(msg, sizeof(msg), "Region A-B is too short"); return; }
    if (!undo_push((int)c1, (int)c2)) { snprintf(msg, sizeof(msg), "Not enough memory for undo"); return; }
    for (int a = lo, b = hi - 1; a < b; a++, b--) { int16_t t = g_audio[a]; g_audio[a] = g_audio[b]; g_audio[b] = t; }
    edit_finish("Reversed region A-B");
}

static void act_normalize(void) {
    if (!edit_prepare()) return;
    int lo = (int)c1, hi = (int)c2;
    if (lo < 0) lo = 0;
    if (hi > g_length) hi = g_length;
    if (hi - lo < 16) { snprintf(msg, sizeof(msg), "Region A-B is too short"); return; }
    int pk = 0;
    for (int i = lo; i < hi; i++) { int v = abs((int)g_audio[i]); if (v > pk) pk = v; }
    if (pk < 100) { snprintf(msg, sizeof(msg), "Region is silent - nothing to normalize"); return; }
    double gain = 0.95 * 32767.0 / (double)pk;
    if (gain > 0.985 && gain < 1.015) { snprintf(msg, sizeof(msg), "Already at full level"); return; }
    if (!undo_push((int)c1, (int)c2)) { snprintf(msg, sizeof(msg), "Not enough memory for undo"); return; }
    for (int i = lo; i < hi; i++) {
        double v = (double)g_audio[i] * gain;
        g_audio[i] = (int16_t)(v > 32767.0 ? 32767 : (v < -32768.0 ? -32768 : (int)(v < 0 ? v - 0.5 : v + 0.5)));
    }
    char t[64];
    int db10 = (int)(200.0 * log10(gain) + (gain >= 1.0 ? 0.5 : -0.5));
    snprintf(t, sizeof(t), "Normalized region A-B (%+d.%d dB)", db10 / 10, abs(db10) % 10);
    edit_finish(t);
}

/* ---- ADV: правки области A-B, как в Beaterator ---- */
static void adv_after_resize(int nl, double nc1, double nc2) {
    g_length = nl;
    c1 = nc1; c2 = nc2; active = 0; split = 0;
    enforce_cursors(nl);
    view_reset_full(nl);
    g_play_pos = 0; g_seek_to = -1;
    apply_region();
}

static int adv_region(int *lo, int *hi) {
    if (!edit_prepare()) return 0;
    *lo = (int)c1; *hi = (int)c2;
    if (*lo < 0) *lo = 0;
    if (*hi > g_length) *hi = g_length;
    if (*hi - *lo < 16) { snprintf(msg, sizeof(msg), "Region A-B is too short"); return 0; }
    return 1;
}

static void act_adv_delete(void) {
    int lo, hi;
    if (!adv_region(&lo, &hi)) return;
    int n = hi - lo, nl = g_length - n;
    if (nl < 256) { snprintf(msg, sizeof(msg), "Nothing would be left - move A and B first"); return; }
    if (!undo_push((int)c1, (int)c2)) { snprintf(msg, sizeof(msg), "Not enough memory for undo"); return; }
    memmove(g_audio + lo, g_audio + hi, (size_t)(g_length - hi) * sizeof(int16_t));
    adv_after_resize(nl, 0.0, (double)nl);
    edit_finish("Deleted region A-B");
}

static void act_adv_silence(void) {
    int lo, hi;
    if (!adv_region(&lo, &hi)) return;
    if (!undo_push((int)c1, (int)c2)) { snprintf(msg, sizeof(msg), "Not enough memory for undo"); return; }
    int fade = 48;
    if (fade * 2 > hi - lo) fade = (hi - lo) / 2;
    for (int i = lo; i < hi; i++) {
        float k = 0.0f;                                  /* короткие скаты на краях: без щелчков */
        if (i - lo < fade) k = 1.0f - (float)(i - lo) / (float)fade;
        else if (hi - 1 - i < fade) k = 1.0f - (float)(hi - 1 - i) / (float)fade;
        g_audio[i] = (int16_t)((float)g_audio[i] * k);
    }
    edit_finish("Region A-B replaced with silence");
}

static void act_adv_duplicate(void) {
    int lo, hi;
    if (!adv_region(&lo, &hi)) return;
    int n = hi - lo;
    if (g_length + n > MAX_SAMPLES) { snprintf(msg, sizeof(msg), "Not enough room: the sample is limited to 5 minutes"); return; }
    if (!undo_push((int)c1, (int)c2)) { snprintf(msg, sizeof(msg), "Not enough memory for undo"); return; }
    memmove(g_audio + hi + n, g_audio + hi, (size_t)(g_length - hi) * sizeof(int16_t));
    memcpy(g_audio + hi, g_audio + lo, (size_t)n * sizeof(int16_t));
    adv_after_resize(g_length + n, (double)lo, (double)(hi + n));
    edit_finish("Region A-B duplicated (selection now covers both copies)");
}

static void act_adv_gap(void) {
    int lo, hi;
    if (!adv_region(&lo, &hi)) return;
    int n = hi - lo;
    if (g_length + n > MAX_SAMPLES) { snprintf(msg, sizeof(msg), "Not enough room: the sample is limited to 5 minutes"); return; }
    if (!undo_push((int)c1, (int)c2)) { snprintf(msg, sizeof(msg), "Not enough memory for undo"); return; }
    memmove(g_audio + hi + n, g_audio + hi, (size_t)(g_length - hi) * sizeof(int16_t));
    memset(g_audio + hi, 0, (size_t)n * sizeof(int16_t));
    adv_after_resize(g_length + n, (double)lo, (double)hi);
    edit_finish("Silence of the same width added after B");
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
    if (g_un <= 0) { snprintf(msg, sizeof(msg), "Nothing to undo"); return; }
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
    snprintf(msg, sizeof(msg), "Undo: step restored (%d more)", g_un);
    dbg_log("undo -> %d", nl);
}

/* каждый регион нарезки - отдельным файлом рядом с основным: rec_005_s01.wav, rec_005_s02.wav ... */
static int export_slices(void) {
    if (!g_wav_path[0] || g_sl_n < 1) return 0;
    char base[128];
    snprintf(base, sizeof(base), "%s", g_wav_path);
    size_t bl = strlen(base);
    if (bl > 4 && strcmp(base + bl - 4, ".wav") == 0) base[bl - 4] = 0;
    int ok = 0;
    for (int i = 0; i < g_sl_n; i++) {
        int a = g_sl_pos[i], b = g_sl_pos[i + 1];
        if (a < 0) a = 0;
        if (b > g_length) b = g_length;
        if (b - a < 16) continue;
        char p[160];
        snprintf(p, sizeof(p), "%s_s%02d.wav", base, i + 1);
        if (write_wav_file(p, g_audio + a, b - a) == 0) ok++;
    }
    return ok;
}

/* ---------- ввод имени файла: системная экранная клавиатура ---------- */
static void u8to16(const char *s, uint16_t *d, int max) {
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < max - 2) {
        unsigned c;
        if (*p < 0x80) c = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { c = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { c = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); p += 3; }
        else { p++; continue; }
        d[n++] = (uint16_t)c;
    }
    d[n] = 0;
}
static void u16to8(const uint16_t *s, char *d, int max) {
    int n = 0;
    while (*s && n < max - 4) {
        unsigned c = *s++;
        if (c < 0x80) d[n++] = (char)c;
        else if (c < 0x800) { d[n++] = (char)(0xC0 | (c >> 6)); d[n++] = (char)(0x80 | (c & 0x3F)); }
        else { d[n++] = (char)(0xE0 | (c >> 12)); d[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); d[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    d[n] = 0;
}
/* убираем символы, запрещённые в именах файлов, и пробелы по краям */
static void name_clean(char *s) {
    for (char *p = s; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|' || (unsigned char)*p < 32) *p = '_';
    char *a = s;
    while (*a == ' ' || *a == '.') a++;
    memmove(s, a, strlen(a) + 1);
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '.')) s[--n] = 0;
}
/* 1 - имя введено (в out), 0 - отмена */
static int ime_name(const char *init, char *out, int cap) {
    static uint16_t title[] = { 'F', 'i', 'l', 'e', ' ', 'n', 'a', 'm', 'e', 0 };
    static uint16_t ini[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1], buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
    u8to16(init, ini, 48);
    memset(buf, 0, sizeof buf);
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = 0;
    p.languagesForced = 0;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.title = title;
    p.maxTextLength = 40;
    p.initialText = ini;
    p.inputTextBuffer = buf;
    if (sceImeDialogInit(&p) < 0) return 0;
    while (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        vita2d_start_drawing();
        vita2d_clear_screen();
        vita2d_end_drawing();
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
    }
    SceImeDialogResult r;
    memset(&r, 0, sizeof r);
    sceImeDialogGetResult(&r);
    int ok = (r.button == SCE_IME_DIALOG_BUTTON_ENTER);
    sceImeDialogTerm();
    if (!ok) return 0;
    u16to8(buf, out, cap);
    name_clean(out);
    return out[0] != 0;
}

/* имя по умолчанию: то, под которым этот звук уже сохранён, иначе первое свободное rec_NNN */
static void default_save_name(char *out, size_t cap) {
    if (g_wav_path[0] && g_wav_ver == g_audio_ver) {
        const char *b = strrchr(g_wav_path, '/');
        snprintf(out, cap, "%s", b ? b + 1 : g_wav_path);
        size_t n = strlen(out);
        if (n > 4 && strcmp(out + n - 4, ".wav") == 0) out[n - 4] = 0;
        return;
    }
    for (int i = 1; i < 1000; i++) {
        char t[128];
        snprintf(t, sizeof t, "ux0:data/Sampler/rec_%03d.wav", i);
        FILE *f = fopen(t, "rb");
        if (!f) { snprintf(out, cap, "rec_%03d", i); return; }
        fclose(f);
    }
    snprintf(out, cap, "rec");
}

static void act_save(void) {
    if (g_state == ST_RECORDING) return;
    if (g_length <= 0) { snprintf(msg, sizeof(msg), "Nothing to save - record something first"); return; }
    char nm[96], init[96];
    default_save_name(init, sizeof init);
    if (!ime_name(init, nm, sizeof nm)) { snprintf(msg, sizeof(msg), "Save cancelled"); return; }
    char path[160];
    int r = save_wav(path, sizeof(path), nm);
    if (r == 0) {
        snprintf(msg, sizeof(msg), g_wav_overwrote ? "Saved (same file updated): %s" : "Saved: %s", path);
        if (g_mode == 1) {
            int n = export_slices();
            snprintf(msg, sizeof(msg), "Saved %s + %d slice files (_s01 ...)", path, n);
        }
        dirty = 0;
        request_autosave(g_length, 0);
        dbg_log("saved %s", path);
    } else if (r == -1) snprintf(msg, sizeof(msg), "Nothing to save - record something first");
    else snprintf(msg, sizeof(msg), "Could not write the file");
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

static void act_bpm_change(int d) {
    if (g_mode != 1 || g_length < 2) return;
    int t = g_bpm_target + d;
    int lo = (int)(g_bpm_base * 0.4f), hi = (int)(g_bpm_base * 2.5f);
    if (lo < 30) lo = 30;
    if (hi > 400) hi = 400;
    if (t < lo) t = lo;
    if (t > hi) t = hi;
    g_bpm_target = t;
    g_tempo_mul = (float)t / (float)g_bpm_base;
    snprintf(msg, sizeof(msg), "Tempo %d BPM  (original %d%s, speed x%d.%02d)", t, g_bpm_base, g_sl_bpm > 0 ? "" : "?",
             (int)g_tempo_mul, (int)(g_tempo_mul * 100.0f + 0.5f) % 100);
}

static void act_set_mode(int m) {
    if (m == g_mode) return;
    if (g_state == ST_RECORDING) return;
    g_rev_req = 0; g_rev = 0;
    if (m == 1) {
        if (g_length < 4 * SL_MIN_LEN) { snprintf(msg, sizeof(msg), "Record a longer sample first (at least 0.4 s)"); return; }
        int rlo = (int)c1, rhi = (int)c2;
        if (rhi - rlo < 4 * SL_MIN_LEN) { snprintf(msg, sizeof(msg), "Region A-B is too short for slices (at least 0.4 s) - widen it in EDIT"); return; }
        int changed = (rlo != g_sl_lo || rhi != g_sl_hi || g_sl_pos[0] != rlo || g_sl_pos[g_sl_n] != rhi);
        g_sl_lo = rlo; g_sl_hi = rhi;
        slices_prepare(g_sl_req, changed);
        if (!changed && g_sl_bpm == 0) {
            slices_ensure_nov();
            double T_, G_;
            tempo_estimate(g_sl_lo, g_sl_hi, &T_, &G_);
            bpm_sync(0);
        }
        if (changed) g_sl_sel = 0;
        if (g_sl_sel >= g_sl_n) g_sl_sel = g_sl_n - 1;
        g_mode = 1;
        split = 0;
        slice_view_fit(length_now());
        apply_region();
        if (g_sl_bpm > 0) snprintf(msg, sizeof(msg), "SLICE mode: %d regions, tempo ~%d BPM (cuts near beats snap to the grid)", g_sl_n, g_sl_bpm);
        else snprintf(msg, sizeof(msg), "SLICE mode: %d regions - tap a pad to play from there", g_sl_n);
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

/* ---- PAD: действия ---- */
static uint64_t g_pad_last_us[PAD_N];                /* защита от дребезга: один пад не чаще раза в 30 мс */
static int pad_lockout(int i) {
    uint64_t t = sceKernelGetProcessTimeWide();
    if (g_pad_last_us[i] && t - g_pad_last_us[i] < 30000) { g_pad_lock++; return 1; }
    g_pad_last_us[i] = t;
    g_pad_hits++;
    if (g_pad_hits <= 400) {                              /* первые удары: подробно, чтобы по логу понять, что происходит */
        int act = 0;
        for (int k = 0; k < PAD_VOICES; k++) if (g_pv[k].stage) act++;
        dbg_log("pad hit pad=%d n=%d voices=%d touches=%d load=%d late=%d", i, g_pad_hits, act, A.prev_n, g_out_load, g_out_late);
    }
    return 0;
}
static void pad_press(int id, int i) {
    if (pad_lockout(i)) { g_pu.sel = i; return; }
    int slot = -1;
    for (int k = 0; k < 4; k++) if (A.pt_id[k] == -1) { slot = k; break; }
    g_pu.sel = i;
    g_pad_trig[i]++;
    if (slot >= 0) { A.pt_id[slot] = id; A.pt_pad[slot] = i; g_pu.down_mask |= 1 << i; }
    else g_pad_rel[i]++;
}
static void pad_release_slot(int slot) {
    int i = A.pt_pad[slot];
    A.pt_id[slot] = -1;
    g_pad_rel[i]++;
    int still = 0;
    for (int k = 0; k < 4; k++) if (A.pt_id[k] != -1 && A.pt_pad[k] == i) still = 1;
    if (!still) g_pu.down_mask &= ~(1 << i);
}
static void pad_tap(int i) { g_pu.sel = i; if (pad_lockout(i)) return; g_pad_trig[i]++; g_pad_rel[i]++; }

static void pad_list_open(void) {
    pad_list_scan();
    g_pu.list_sel = 0;
    for (int i = 0; i < g_pu.list_n; i++) {                     /* текущий файл пада сразу выделен */
        char full[200];
        snprintf(full, sizeof(full), "%s/%s", PAD_DIR, g_pu.list[i]);
        if (strcmp(full, g_pad[g_pu.sel].path) == 0) g_pu.list_sel = i;
    }
    g_pu.list_top = g_pu.list_sel > PL_ROWS - 1 ? g_pu.list_sel - PL_ROWS + 1 : 0;
    g_pu.list_open = 1;
    snprintf(msg, sizeof(msg), "Pick a WAV for pad %d", g_pu.sel + 1);
}
static void pad_assign(int k) {
    if (k < 0 || k >= g_pu.list_n) return;
    char path[200], err[64];
    snprintf(path, sizeof(path), "%s/%s", PAD_DIR, g_pu.list[k]);
    if (pad_load_wav(g_pu.sel, path, err, sizeof(err))) {
        snprintf(msg, sizeof(msg), "Pad %d: %s", g_pu.sel + 1, g_pad[g_pu.sel].name);
        pad_mark_dirty();
        g_pu.list_open = 0;
        pad_tap(g_pu.sel);                                         /* сразу прослушать */
    } else snprintf(msg, sizeof(msg), "Pad %d: %s", g_pu.sel + 1, err);
}
static void pad_list_move(int d) {
    if (g_pu.list_n <= 0) return;
    g_pu.list_sel += d;
    if (g_pu.list_sel < 0) g_pu.list_sel = 0;
    if (g_pu.list_sel >= g_pu.list_n) g_pu.list_sel = g_pu.list_n - 1;
    if (g_pu.list_sel < g_pu.list_top) g_pu.list_top = g_pu.list_sel;
    if (g_pu.list_sel >= g_pu.list_top + PL_ROWS) g_pu.list_top = g_pu.list_sel - PL_ROWS + 1;
}

static void perform_hit(int hit) {
    if (hit >= B_TAB0 && hit <= B_TAB5) act_tab(g_tab_page[hit - B_TAB0]);
    else if (hit == B_ADV0) act_adv_delete();
    else if (hit == B_ADV1) act_adv_silence();
    else if (hit == B_ADV2) act_adv_duplicate();
    else if (hit == B_ADV3) act_adv_gap();
    else if (hit == B_CROP) act_crop_tap();
    else if (hit == B_UNDO) act_undo();
    else if (hit == B_NORM) act_normalize();
    else if (hit == B_REV) act_reverse_toggle();
    else if (hit == B_SL_PLUS) act_slice_count(1);
    else if (hit == B_SL_MINUS) act_slice_count(-1);
    else if (hit == B_BPM_PLUS) act_bpm_change(1);
    else if (hit == B_BPM_MINUS) act_bpm_change(-1);
    else if (hit == B_GR_ON) { g_gr_on = !g_gr_on; snprintf(msg, sizeof(msg), g_gr_on ? "GRAIN ON: a cloud of small grains around the play line" : "GRAIN off"); }
    else if (hit >= B_FSEC0 && hit <= B_FSEC1) A.fx_sec = hit - B_FSEC0;
    else if (hit >= B_FT0 && hit <= B_FT3) {
        static const char *nm[4] = { "Filter off", "Lowpass 12 dB/oct", "Highpass 12 dB/oct", "Bandpass: center + width" };
        g_flt_type = hit - B_FT0;
        snprintf(msg, sizeof(msg), "%s", nm[g_flt_type]);
    }
    else if (hit == B_PENV || hit == B_PCMP) {
        int want = hit == B_PENV ? 1 : 2;
        g_pad_env_sub = (g_pad_env_sub == want) ? 0 : want;
        A.knob_on = 0;
        static const char *mt[3] = { "Volume / pitch / start", "ADSR: shape the volume of this pad", "COMP: threshold, ratio, makeup, mix (the screen shows the curve)" };
        snprintf(msg, sizeof(msg), "%s", mt[g_pad_env_sub]);
    }
    else if (hit == B_PCLR) { pad_clear(g_pu.sel); pad_mark_dirty(); snprintf(msg, sizeof(msg), "Pad %d cleared", g_pu.sel + 1); }
    else if (hit == B_PLOAD) { if (g_pu.list_open) g_pu.list_open = 0; else pad_list_open(); }
    else if (hit == B_PMODE) {
        Pad *p = &g_pad[g_pu.sel];
        p->mode = (p->mode + 1) % 3;
        static const char *mt[3] = { "ONE-SHOT: plays to the end, release is ignored", "GATE: sound while the pad is held, then RELEASE", "LOOP: repeats while held, then RELEASE" };
        snprintf(msg, sizeof(msg), "%s", mt[p->mode]);
        pad_mark_dirty();
    }
    else if (hit >= B_PAD0) { if (A.page == 5) pad_tap(hit - B_PAD0); else act_slice_trigger(hit - B_PAD0); }
}

/* ====================================================================
 *  Главная программа
 * ==================================================================== */
/* уровень для трубки: 0..1000 на диапазон 0...-METER_DB дБ */
static int level_from_peak(int peak) {
    if (peak <= 0) return 0;
    float db = 20.0f * log10f((float)peak / 32768.0f);
    float p = (db + (float)METER_DB) / (float)METER_DB;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return (int)(p * 1000.0f);
}

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
    dbg_log("start v1.7");
    scePowerSetArmClockFrequency(444);              /* максимум: больше запаса для звука и отрисовки */
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    g_audio = (int16_t *)malloc(MAX_SAMPLES * sizeof(int16_t));
    if (!g_audio) { dbg_log("malloc failed"); sceKernelExitProcess(0); }

    for (int i = 0; i < W_N; i++)
        g_hann[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * (float)i / (float)W_N));
    ker_init();

    vita2d_init_advanced(4 * 1024 * 1024);
    vita2d_set_clear_color(K_CREAM);
    g_pgf = vita2d_load_default_pgf();
    fonts_load();
    dbg_log("fonts: %s", g_font ? "ttf" : "pgf fallback");
    for (int i = 0; i < N_WAVE_TEX; i++) g_wave_tex[i] = vita2d_create_empty_texture(WAVE_W, WAVE_H);
    if (!ui_init()) { dbg_log("ui_init failed"); sceKernelExitProcess(0); }
    dbg_log("gfx ready");

    memset(&A, 0, sizeof(A));
    A.scrub_id = -1; A.btn_down = -1; A.btn_id = -1; A.knob_last_k = -1; A.knob_id = -1; A.curve_id = -1;
    for (int i = 0; i < 6; i++) A.prev_ids[i] = -1;
    for (int i = 0; i < 4; i++) A.pt_id[i] = -1;
    A.kb_on = 0; A.kb_id = -1;
    kit_install();                              /* заводские звуки (если ещё не записаны) */
    pad_cfg_load();

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
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    A.gl_hz = 6.0f;

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
    int shown_level = 0, clip_hold = 0, peak_hold = 0, peak_f = 0;
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
            A.btn_down = -1; A.btn_id = -1; A.knob_on = 0; A.curve_on = 0;
            g_reset_in = 1; g_reset_out = 1;
            vita2d_wait_rendering_done();
            for (int i = 0; i < N_WAVE_TEX; i++) {
                if (g_wave_tex[i]) vita2d_free_texture(g_wave_tex[i]);
                g_wave_tex[i] = vita2d_create_empty_texture(WAVE_W, WAVE_H);
            }
            ui_recreate();
            vita2d_free_pgf(g_pgf);
            g_pgf = vita2d_load_default_pgf();
            fonts_free(); fonts_load();
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
            if (pad.buttons & SCE_CTRL_SELECT) {                 /* SELECT + D-pad: вправо - SLICE, влево - EDIT */
                const unsigned dp = SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_UP | SCE_CTRL_DOWN;
                if (pressed & SCE_CTRL_LEFT) act_tab_step(-1);
                if (pressed & SCE_CTRL_RIGHT) act_tab_step(1);
                if (pressed & dp) combo = 1;
                pad.buttons &= ~dp;
                pressed &= ~dp;
            }
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
            if (A.l_frames >= 15) {                          /* удержание: глитч - звук включается и выключается, ход не останавливается */
                if (!A.l_glitch) {
                    A.l_glitch = 1; A.l_acc = 0;
                    apply_region();
                    g_error = 0;
                    g_state = ST_PLAYING;
                    snprintf(msg, sizeof(msg), "GLITCH: hold L, move a finger up / down on the rear panel to set the rate (1-30 Hz). Release: back to previous state");
                }
                {   /* частота глитча - задняя панель: палец вверх / вниз (1..30 Гц), края не используются */
                    SceTouchData bt;
                    memset(&bt, 0, sizeof(bt));
                    sceTouchPeek(SCE_TOUCH_PORT_BACK, &bt, 1);
                    for (unsigned q = 0; q < bt.reportNum && q < 8; q++) {
                        int by = bt.report[q].y;
                        if (by < 110 || by > 780) continue;
                        double tt = (double)(780 - by) / 670.0;                       /* 0 - внизу, 1 - вверху */
                        float hz = (float)pow(30.0, tt);                              /* 1 .. 30 Гц, на слух равномерно */
                        if (fabsf(hz - A.gl_hz) > 0.05f * A.gl_hz) {
                            A.gl_hz = hz;
                            int tenth = (int)(hz * 10.0f + 0.5f);
                            snprintf(msg, sizeof(msg), "GLITCH %d.%d Hz  (rear panel: finger up = faster, down = slower)", tenth / 10, tenth % 10);
                        }
                        break;
                    }
                }
                g_glitch_hz = A.gl_hz;
            }
        } else if (A.l_glitch) {
            A.l_glitch = 0;
            g_mute = 0;
            g_glitch_hz = 0.0f;
            if (g_state != ST_RECORDING) {
                if (A.l_pre == ST_PLAYING) { g_error = 0; g_state = ST_PLAYING; }
                else g_state = ST_IDLE;
            }
            A.l_frames = 0;
        } else A.l_frames = 0;
        if (pressed & SCE_CTRL_SQUARE)   act_loop_toggle();
        for (int q = 0; q < 4; q++)                              /* X отпущена - отпускаем пад */
            if (A.pt_id[q] == -2 && !(pad.buttons & SCE_CTRL_CROSS)) pad_release_slot(q);
        if (A.page == 5 && !(pad.buttons & SCE_CTRL_SELECT)) {
            if (g_pu.list_open) {
                int ud = ((pad.buttons & SCE_CTRL_DOWN) ? 1 : 0) - ((pad.buttons & SCE_CTRL_UP) ? 1 : 0);
                if (ud) { A.ls_rep++; if (A.ls_rep == 1 || (A.ls_rep > 15 && A.ls_rep % 3 == 0)) pad_list_move(ud); }
                else A.ls_rep = 0;
                if (pressed & SCE_CTRL_CROSS) pad_assign(g_pu.list_sel);
            } else {
                int sx = g_pu.sel % 4, sy = g_pu.sel / 4;
                if (pressed & SCE_CTRL_LEFT)  sx = (sx + 3) % 4;
                if (pressed & SCE_CTRL_RIGHT) sx = (sx + 1) % 4;
                if (pressed & SCE_CTRL_UP)    sy = (sy + 3) % 4;
                if (pressed & SCE_CTRL_DOWN)  sy = (sy + 1) % 4;
                g_pu.sel = sy * 4 + sx;
                if (pressed & SCE_CTRL_CROSS) pad_press(-2, g_pu.sel);
            }
        }
        if (pressed & SCE_CTRL_CIRCLE) { if (A.page == 5 && g_pu.list_open) g_pu.list_open = 0; else act_fx_toggle(); }

        /* Треугольник: стоп; удержание - позиция сохраняется */
        if (pressed & SCE_CTRL_TRIANGLE) {
            int dbl = (frame - A.tri_last <= 22 && A.tri_last != 0);
            A.tri_last = frame;
            act_stop_press();
            if (dbl) act_reset_selection();
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

        if (A.page < 2) A.page = g_mode;                  /* EDIT / SLICE следуют за режимом */
        if (A.page == 4 && g_mode != 0) A.page = g_mode;   /* ADV работает только в EDIT */

        /* ---- режимы: курсоры (EDIT) или регионы (SLICE) ---- */
        double spp = (double)view_len / (double)WAVE_W;
        if (spp < 0.05) spp = 0.05;

        if (state != ST_RECORDING && length > 1 && A.page != 5) {
            if (g_mode == 0) {
                float a1 = stick_axis(pad.lx);
                float a2 = stick_axis(pad.rx);

                /* зум двумя стиками: оба вверх - ближе, оба вниз - дальше; разные стороны - без зума */
                float lyn = ((float)(128 - (int)pad.ly)) / 128.0f, ryn = ((float)(128 - (int)pad.ry)) / 128.0f;
                float lxn = ((float)pad.lx - 128.0f) / 128.0f, rxn = ((float)pad.rx - 128.0f) / 128.0f;
                int two_zoom = 0;
                if (fabsf(lyn) > 0.35f && fabsf(ryn) > 0.35f && (lyn > 0) == (ryn > 0)
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
                double zgain = 0.12 + 0.88 * fmin(1.0, sqrt(((double)view_len / (double)length) / 0.30));   /* ближе - медленнее */
                if (a1 != 0.0f) { c1 += (double)a1 * 420.0 * zgain * spp * (double)dt; active = 0; enforce_cursors(length); }
                if (a2 != 0.0f) { c2 += (double)a2 * 420.0 * zgain * spp * (double)dt; active = 1; enforce_cursors(length); }

                {
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
                {
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
        if (state != ST_RECORDING && length > 1 && !A.pz_on) {
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
        static SceTouchData tbuf[8];
        int nb = sceTouchRead(SCE_TOUCH_PORT_FRONT, tbuf, 8);          /* все отсчёты с прошлого кадра, а не только последний */
        if (nb > 0) touch = tbuf[nb - 1];
        else { sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1); nb = 0; }
        int nrep = (int)touch.reportNum;
        if (nrep > 6) nrep = 6;

        Layout lay_now;
        compute_layout(&lay_now, length, state == ST_RECORDING, split, c1, c2);

        /* очень короткие касания (нажали и отпустили между кадрами) - тоже считаются: иначе быстрый ритм по падам теряет удары */
        for (int si = 0; si < nb - 1; si++) {
            for (unsigned q = 0; q < tbuf[si].reportNum && q < 6; q++) {
                int id = tbuf[si].report[q].id, skip = 0;
                for (int j = 0; j < A.prev_n && !skip; j++) if (A.prev_ids[j] == id) skip = 1;
                for (int j = 0; j < nrep && !skip; j++) if (touch.report[j].id == id) skip = 1;
                for (int e2 = 0; e2 < si && !skip; e2++)
                    for (unsigned q2 = 0; q2 < tbuf[e2].reportNum && q2 < 6; q2++) if (tbuf[e2].report[q2].id == id) skip = 1;
                if (skip) continue;
                if (tbuf[si].report[q].x < 8 || tbuf[si].report[q].x > 1911 || tbuf[si].report[q].y < 8 || tbuf[si].report[q].y > 1079) continue;
                int hit = hit_test((int)(tbuf[si].report[q].x / 2), (int)(tbuf[si].report[q].y / 2), A.page, A.fx_sec, g_sl_n);
                if (hit != B_NONE && hit < B_KNOB0) perform_hit(hit);
                else if (hit >= B_PAD0) perform_hit(hit);
            }
        }

        /* новые касания: кнопки, вкладки, пэды, ползунки, начало скраба */
        for (int k = 0; k < nrep; k++) {
            int id = touch.report[k].id;
            int is_new = 1;
            for (int j = 0; j < A.prev_n; j++) if (A.prev_ids[j] == id) is_new = 0;
            if (!is_new) continue;
            double fx = touch.report[k].x / 2.0, fy = touch.report[k].y / 2.0;
            if (fx < 4.0 || fx > 956.0 || fy < 4.0 || fy > 540.0) continue;      /* самый край экрана - там держат консоль, ложные касания */
            int in_wave = (A.page != 5 && fx >= WAVE_X - 4 && fx <= WAVE_X + WAVE_W + 4 && fy >= WAVE_Y - 18 && fy <= WAVE_Y + WAVE_H + 18);

            if (A.page == 5) {                                    /* PAD: список WAV и пады */
                if (g_pu.list_open && fx >= LST_X && fx < LST_X + LST_W && fy >= LST_Y && fy < LST_Y + LST_H) {
                    if (!A.ls_on) { A.ls_on = 1; A.ls_id = id; A.ls_y0 = fy; A.ls_moved = 0; A.ls_top0 = g_pu.list_top; }
                    continue;
                }
                int ph = hit_test((int)fx, (int)fy, A.page, A.fx_sec, g_sl_n);
                if (ph >= B_PAD0) { pad_press(id, ph - B_PAD0); continue; }
            }
            if (A.page == 2 && !A.kb_on) {                        /* клавиатура GRAIN */
                int kn = kb_hit(fx, fy);
                if (kn >= 0) { A.kb_on = 1; A.kb_id = id; kb_press(kn); continue; }
            }
            if (in_wave && g_mode == 1 && A.page != 2 && !A.sdrag_on && !A.scrubbing && g_state != ST_RECORDING
                && length > 1 && fy < WAVE_Y + 46) {
                int bi2 = -1, bsg = 0; double bd2 = 28.0;
                for (int i2 = 1; i2 < g_sl_n; i2++)
                    for (int sg = 0; sg < lay_now.nseg; sg++) {
                        int mxp = sample_to_x(&lay_now, sg, (double)g_sl_pos[i2]);
                        if (mxp == -1000000) continue;
                        double d = fabs(fx - (double)mxp);
                        if (d < bd2) { bd2 = d; bi2 = i2; bsg = sg; }
                    }
                if (bi2 > 0) {
                    A.sdrag_on = 1; A.sdrag_id = id; A.sdrag_idx = bi2; A.sdrag_x0 = fx;
                    A.sdrag_c0 = (double)g_sl_pos[bi2];
                    A.sdrag_spp = (double)lay_now.vl[bsg] / (double)lay_now.w[bsg];
                    snprintf(msg, sizeof(msg), "Dragging slice boundary %d", bi2);
                    continue;
                }
            }
            if (in_wave && g_mode == 0 && !A.scrubbing && g_state != ST_RECORDING
                && length > 1 && fy < WAVE_Y + 46) {
                int best = -1, bseg = 0; double bd = 28.0;
                for (int w = 0; w < 2; w++) {
                    if (A.drag_on[w]) continue;
                    for (int sg = 0; sg < lay_now.nseg; sg++) {
                        int mxp = sample_to_x(&lay_now, sg, w ? c2 : c1);
                        if (mxp == -1000000) continue;
                        double d = fabs(fx - (double)mxp);
                        if (d < bd) { bd = d; best = w; bseg = sg; }
                    }
                }
                int whole = (c1 <= 0.5 && c2 >= (double)length - 0.5);
                if (best < 0 && !whole && !A.drag_on[0] && !A.drag_on[1] && !A.drag_on[2]) {
                    /* внутри области, не у маркера: тащим обе границы сразу */
                    int xa = sample_to_x(&lay_now, 0, c1), xb = sample_to_x(&lay_now, lay_now.nseg - 1, c2);
                    if (lay_now.nseg == 1 && xa != -1000000 && xb != -1000000 && fx > xa && fx < xb) best = 2;
                    else if (lay_now.nseg == 1 && (xa != -1000000 || xb != -1000000)) {
                        int xl = (xa == -1000000) ? WAVE_X : xa, xr = (xb == -1000000) ? WAVE_X + WAVE_W : xb;
                        if (fx > xl && fx < xr) best = 2;
                    }
                }
                if (best >= 0) {
                    double gz = 1.0;                                              /* маркер идёт точно за пальцем */
                    A.drag_on[best] = 1; A.drag_id[best] = id; A.drag_x0[best] = fx;
                    A.drag_spp[best] = (double)lay_now.vl[bseg] / (double)lay_now.w[bseg] * gz;
                    A.drag_c0[best] = best == 0 ? c1 : (best == 1 ? c2 : 0.0);
                    A.drag_c1_0 = c1; A.drag_c2_0 = c2;
                    if (best < 2) active = best;
                    snprintf(msg, sizeof(msg), best == 2 ? "Dragging the whole region A-B" : (best ? "Dragging cursor B" : "Dragging cursor A"));
                    continue;
                }
            }
            if (in_wave && !A.scrubbing && g_state != ST_RECORDING && length > 1) {
                int seg = 0;
                A.scrubbing = 1;
                A.scrub_id = id;
                A.resume_state = g_state;
                double target = x_to_sample(&lay_now, fx, &seg);
                if (target < 0.0) target = 0.0;
                if (target > (double)(length - 1)) target = (double)(length - 1);
                A.scrub_seg = seg;
                A.scrub_pos = target;
                A.scrub_head = target;
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

            int hit = hit_test((int)fx, (int)fy, A.page, A.fx_sec, g_sl_n);
            if (hit >= B_KNOB0 && hit < B_PAD0) {
                int kk = hit - B_KNOB0;
                if (A.knob_last_k == kk && frame - A.knob_last_frame < 26) {      /* двойное касание: значение по умолчанию */
                    knob_set(kk, g_kd[kk].def);
                    A.knob_last_k = -1;
                    snprintf(msg, sizeof(msg), "%s: default", g_kd[kk].label);
                } else if (!A.knob_on) {
                    A.knob_on = 1; A.knob_k = kk; A.knob_id = id;
                    A.knob_x0 = fx; A.knob_y0 = fy; A.knob_v0 = (double)*g_kd[kk].v;
                    A.knob_last_k = kk; A.knob_last_frame = frame;
                }
                continue;
            }
            if (hit == B_CURVE) {
                A.curve_on = 1; A.curve_id = id;
                continue;
            }
            if (hit != B_NONE) {
                A.btn_down = hit;
                A.btn_id = id;
                A.btn_hold = 0;
                perform_hit(hit);
                continue;
            }
        }

        /* ---- зум двумя пальцами по волне (EDIT): расстояние между пальцами = масштаб, середина удерживает точку волны ---- */
        {
            int wi[6], wn = 0;
            for (int k = 0; k < nrep && wn < 6; k++) {
                double tx = touch.report[k].x / 2.0, ty = touch.report[k].y / 2.0;
                if (tx >= WAVE_X - 10 && tx <= WAVE_X + WAVE_W + 10 && ty >= WAVE_Y - 18 && ty <= WAVE_Y + WAVE_H + 24) wi[wn++] = k;
            }
            if (A.pz_on) {
                int k0 = -1, k1 = -1;
                for (int k = 0; k < nrep; k++) {
                    if (touch.report[k].id == A.pz_id[0]) k0 = k;
                    if (touch.report[k].id == A.pz_id[1]) k1 = k;
                }
                if (k0 < 0 || k1 < 0 || g_mode != 0 || g_state == ST_RECORDING) A.pz_on = 0;
                else {
                    double x0 = touch.report[k0].x / 2.0, x1 = touch.report[k1].x / 2.0;
                    double d = fabs(x1 - x0);
                    if (d < 12.0) d = 12.0;
                    double nl = A.pz_len0 * A.pz_d0 / d;
                    if (nl < (double)MIN_VIEW) nl = (double)MIN_VIEW;
                    if (nl > (double)length) nl = (double)length;
                    view_len = (int)nl;
                    double cx = (x0 + x1) * 0.5;
                    view_start = (int)(A.pz_cs - (cx - WAVE_X) / (double)WAVE_W * (double)view_len);
                    clamp_view(length);
                    split = 0;
                }
            } else if (wn >= 2 && g_mode == 0 && A.page != 5 && g_state != ST_RECORDING && length > 1 && !A.knob_on && !A.curve_on) {
                int ka = wi[0], kb = wi[1];
                A.pz_on = 1; A.pz_id[0] = touch.report[ka].id; A.pz_id[1] = touch.report[kb].id;
                double xa = touch.report[ka].x / 2.0, xb = touch.report[kb].x / 2.0;
                A.pz_d0 = fabs(xb - xa) < 12.0 ? 12.0 : fabs(xb - xa);
                A.pz_len0 = (double)view_len;
                double cx = (xa + xb) * 0.5;
                A.pz_cs = (double)view_start + (cx - WAVE_X) / (double)WAVE_W * (double)view_len;
                split = 0;
                if (A.scrubbing) {                                  /* первый палец уже скрабил - отпускаем звук */
                    A.scrubbing = 0; A.scrub_id = -1; g_scrub = 0; g_scrub_vel = 0.0f;
                    if (A.resume_state == ST_PLAYING) g_seek_to = (int)A.scrub_pos;
                    else { g_state = ST_IDLE; sceKernelDelayThread(40000); g_play_pos = (int)A.scrub_pos; g_seek_to = -1; }
                }
                for (int w = 0; w < 3; w++) A.drag_on[w] = 0;
                A.sdrag_on = 0;
            }
        }

        /* уже идущие касания */
        int kb_alive = 0, ls_alive = 0, scrub_alive = 0, knob_alive = 0, curve_alive = 0, btn_alive = 0, sdrag_alive = 0, drag_alive[3] = { 0, 0, 0 };
        for (int k = 0; k < nrep; k++) {
            int id = touch.report[k].id;
            double fx = touch.report[k].x / 2.0;
            if (A.kb_on && id == A.kb_id) {                       /* палец ведёт по клавишам - глиссандо */
                kb_alive = 1;
                int kn = kb_hit(fx, touch.report[k].y / 2.0);
                if (kn >= 0 && kn != g_kb_semi + 12) g_kb_semi = kn - 12;
                continue;
            }
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
                /* скорость определяется расстоянием от пальца до линии воспроизведения:
                 * рядом - ровно 1.0x, дальше - быстрее (до x2), направление - в сторону пальца */
                double dtc = tdt < 0.0 ? 0.0 : (tdt > 0.05 ? 0.05 : tdt);
                double d_smp = target - A.scrub_head;                      /* своя модель линии: позиция из звука приходит ступеньками */
                double dpx = d_smp / spp_seg;
                double r = scrub_profile(dpx);
                double lim = fabs(d_smp) / (0.03 * (double)SAMPLE_RATE);   /* "пружина": у самого пальца замедляемся, без перелёта и качания */
                if (fabs(r) > lim) r = (r < 0.0 ? -lim : lim);
                double stp = r * (double)SAMPLE_RATE * dtc;
                if (fabs(stp) > fabs(d_smp)) stp = d_smp;
                A.scrub_head += stp;
                if (fabs((double)g_play_pos - A.scrub_head) > 0.3 * (double)SAMPLE_RATE)
                    A.scrub_head += ((double)g_play_pos - A.scrub_head) * 0.1;      /* подстраховка от долгого расхождения
                                                                                   */
                if (A.scrub_head < 0.0) A.scrub_head = 0.0;
                if (A.scrub_head > (double)(length - 1)) A.scrub_head = (double)(length - 1);
                g_scrub_target = (float)target;
                g_scrub_vel = (float)(r * (double)SAMPLE_RATE);
                A.scrub_speed = r * (double)SAMPLE_RATE;
                (void)vp;
            }
            if (A.sdrag_on && id == A.sdrag_id) {
                sdrag_alive = 1;
                int want = (int)(A.sdrag_c0 + (fx - A.sdrag_x0) * A.sdrag_spp);
                slice_nudge(A.sdrag_idx, want - g_sl_pos[A.sdrag_idx]);
            }
            for (int w = 0; w < 3; w++) {
                if (!A.drag_on[w] || id != A.drag_id[w]) continue;
                drag_alive[w] = 1;
                double dsamp = (fx - A.drag_x0[w]) * A.drag_spp[w];
                if (w == 0) { c1 = A.drag_c0[0] + dsamp; active = 0; }
                else if (w == 1) { c2 = A.drag_c0[1] + dsamp; active = 1; }
                else {
                    double span = A.drag_c2_0 - A.drag_c1_0;
                    double n1 = A.drag_c1_0 + dsamp;
                    if (n1 < 0.0) n1 = 0.0;
                    if (n1 > (double)length - span) n1 = (double)length - span;
                    c1 = n1; c2 = n1 + span;
                }
                enforce_cursors(length);
                g_lo = (int)c1; g_hi = (int)c2;
            }
            if (A.knob_on && id == A.knob_id) {
                knob_alive = 1;
                double fy2 = touch.report[k].y / 2.0;
                double d = (A.knob_y0 - fy2) + (fx - A.knob_x0) * 0.6;         /* вверх / вправо - больше */
                int n = g_kd[A.knob_k].n;
                int nv = (int)floor(A.knob_v0 + d / 170.0 * (double)(n - 1) + 0.5);
                if (n > 100 && abs(nv - g_kd[A.knob_k].def) <= 2) nv = g_kd[A.knob_k].def;   /* "магнит" к значению по умолчанию */
                knob_set(A.knob_k, nv);
                if (fabs(fx - A.knob_x0) + fabs(fy2 - A.knob_y0) > 8.0) A.knob_last_k = -1;   /* перетаскивание - не двойное касание */
            }
            if (A.ls_on && id == A.ls_id) {
                ls_alive = 1;
                double dy = touch.report[k].y / 2.0 - A.ls_y0;
                if (fabs(dy) > 8.0) A.ls_moved = 1;
                if (A.ls_moved) {
                    int top = A.ls_top0 - (int)floor(dy / (double)LST_RH + 0.5);
                    int mx = g_pu.list_n - PL_ROWS; if (mx < 0) mx = 0;
                    g_pu.list_top = top < 0 ? 0 : (top > mx ? mx : top);
                }
                A.ls_fx = fx; A.ls_fy = touch.report[k].y / 2.0;
            }
            if (A.curve_on && id == A.curve_id) {
                curve_alive = 1;
                double fy2 = touch.report[k].y / 2.0;
                double xf = (fx - CURVE_X) / (double)CURVE_W, yf = (fy2 - CURVE_Y) / (double)CURVE_H;
                if (xf < 0.0) xf = 0.0;
                if (xf > 1.0) xf = 1.0;
                if (yf < 0.0) yf = 0.0;
                if (yf > 1.0) yf = 1.0;
                double f = CV_F0 * pow(CV_F1 / CV_F0, xf);                      /* частота по горизонтали */
                int ci = (int)(100.0 * log(f / 40.0) / log(200.0) + 0.5);
                knob_set(7, ci);
                if (g_flt_type == 3) knob_set(8, (int)((1.0 - yf) * 100.0 + 0.5));      /* ширина по вертикали: выше - шире */
            }
            if (A.btn_down != B_NONE && id == A.btn_id) {
                btn_alive = 1;
                A.btn_hold++;
                if (A.btn_down == B_REV && A.btn_hold == 40) {          /* удержание: "запечь" реверс в сам звук */
                    if (g_rev_req) act_reverse_toggle();                /* снять временный режим */
                    act_reverse();
                }
                if ((A.btn_down == B_BPM_PLUS || A.btn_down == B_BPM_MINUS) && A.btn_hold > 24 && A.btn_hold % 4 == 0)
                    act_bpm_change((A.btn_down == B_BPM_PLUS ? 1 : -1) * (A.btn_hold > 90 ? 2 : 1));
                if ((A.btn_down == B_SL_PLUS || A.btn_down == B_SL_MINUS) && A.btn_hold > 24 && A.btn_hold % 8 == 0)
                    act_slice_count(A.btn_down == B_SL_PLUS ? 1 : -1);
            }
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
        if (A.sdrag_on && !sdrag_alive) A.sdrag_on = 0;
        if (A.ls_on && !ls_alive) {                              /* список: короткое касание без прокрутки = выбрать строку */
            A.ls_on = 0;
            if (!A.ls_moved && g_pu.list_open) {
                int row = (int)((A.ls_y0 - LST_ROW0) / (double)LST_RH);
                if (A.ls_y0 >= LST_ROW0 && row >= 0 && row < PL_ROWS && g_pu.list_top + row < g_pu.list_n) {
                    g_pu.list_sel = g_pu.list_top + row;
                    pad_assign(g_pu.list_sel);
                }
            }
        }
        for (int q = 0; q < 4; q++) {                            /* пады, которые держали пальцем: отпустили - отпускаем */
            if (A.pt_id[q] < 0) continue;
            int alive = 0;
            for (int k = 0; k < nrep; k++) if (touch.report[k].id == A.pt_id[q]) alive = 1;
            if (!alive) pad_release_slot(q);
        }
        for (int w = 0; w < 3; w++)
            if (A.drag_on[w] && !drag_alive[w]) { A.drag_on[w] = 0; request_autosave(length, 0); }
        if (A.knob_on && !knob_alive) A.knob_on = 0;
        if (A.kb_on && (!kb_alive || A.page != 2)) { A.kb_on = 0; kb_release(); }
        if (A.curve_on && !curve_alive) A.curve_on = 0;
        if (A.btn_down != B_NONE && !btn_alive) { A.btn_down = B_NONE; A.btn_id = -1; A.btn_hold = 0; }
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
            int newkey[8] = { length, live, peak, lay_now.nseg, g_audio_ver, 0, 0, 0 };
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
            dbg_log("hb f=%u st=%d mode=%d len=%d pos=%d lvl=%d split=%d fx=%d scrub=%d mic_late=%d mic_odd=%d r0=%d out_late=%d load=%d pad[hit=%d drop=%d steal=%d lock=%d]",
                    frame, state, g_mode, length, playhead, g_level, split, A.page, A.scrubbing,
                    g_in_late, g_in_odd, g_in_r0, g_out_late, g_out_load, g_pad_hits, g_pad_drop, g_pad_steal, g_pad_lock);
        if (g_error != last_err) { last_err = g_error; dbg_log("audio error 0x%08X", (unsigned)g_error); }
        prev_state = state;

        /* ---- уровень ---- */
        int lvl = (state == ST_IDLE) ? 0 : level_from_peak(g_level);
        if (g_level >= 32000 && state != ST_IDLE) clip_hold = 60;
        if (clip_hold > 0) clip_hold--;
        if (lvl > shown_level) shown_level += (lvl - shown_level) * 40 / 100 + 1;      /* быстро вверх, но не рывком */
        else shown_level -= (shown_level - lvl) * 7 / 100 + 3;                          /* медленно вниз */
        if (shown_level > 1000) shown_level = 1000;
        if (shown_level < 0) shown_level = 0;
        if (lvl >= peak_hold) { peak_hold = lvl; peak_f = 75; }      /* пик держится 1.2 с, потом медленно падает */
        else if (peak_f > 0) peak_f--;
        else { peak_hold -= 4; if (peak_hold < 0) peak_hold = 0; }

        /* ---- данные для рисования ---- */
        pad_sync();
        if (g_pu.cfg_dirty && ++g_pu.cfg_cnt > 90) { g_pu.cfg_dirty = 0; pad_cfg_save(); }   /* настройки пэдов пишем с задержкой после последней правки */
        UiState u;
        memset(&u, 0, sizeof(u));
        u.legend = g_legend; u.rev = g_rev_req; u.bpm = g_sl_bpm; u.bpm_t = g_bpm_target; u.bpm_b = g_bpm_base; u.mode = g_mode; u.page = A.page; u.fx_sec = A.fx_sec; u.gr_on = g_gr_on; u.kb_note = g_kb_on ? g_kb_semi + 12 : -1; u.flt_type = g_flt_type; u.knob_down = A.knob_on ? A.knob_k : -1; u.state = state; u.scrubbing = A.scrubbing; u.loop = g_loop;
        u.length = length; u.playhead = playhead; u.live = live;
        u.split = split; u.active = active; u.c1 = c1; u.c2 = c2;
        u.level_pct = shown_level; u.peak_pct = peak_hold; u.clip = clip_hold;
        u.confirm = A.confirm || A.crop_armed;
        snprintf(u.msg, sizeof(u.msg), "%s", msg);
        u.sl_n = g_sl_n; u.sl_sel = g_sl_sel; u.sl_flash = A.sl_flash;
        u.sl_playing = (state == ST_PLAYING && g_mode == 1) ? g_sl_sel : -1;
        u.btn_down = A.btn_down; u.can_undo = (g_un > 0); u.crop_armed = A.crop_armed;
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
    fonts_free();
    vita2d_free_pgf(g_pgf);
    vita2d_fini();
    free(g_audio);
    undo_clear();
    sceKernelExitProcess(0);
    return 0;
}
#endif
