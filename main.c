/*
 * Vita Sampler - версия 0.1: диктофон
 *
 * Управление:
 *   X (крест)   - начать / остановить запись
 *   Квадрат     - воспроизвести / остановить
 *   Треугольник - сохранить запись в WAV (ux0:data/Sampler/)
 *   Касание полосы времени - мгновенный переход в нужное место
 *   START       - выход
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

/* Выделяем программе побольше памяти под записи */
unsigned int _newlib_heap_size_user = 64 * 1024 * 1024;

/* ---------- Настройки ---------- */
#define SAMPLE_RATE   16000              /* микрофон Vita пишет 16 кГц моно */
#define MAX_SECONDS   300                /* максимум 5 минут записи */
#define MAX_SAMPLES   (SAMPLE_RATE * MAX_SECONDS)
#define IN_GRAIN      256                /* размер блока записи (в сэмплах) */
#define OUT_GRAIN     1024               /* размер блока воспроизведения */

/* Полоса времени на экране (экран 960x544) */
#define BAR_X 40
#define BAR_Y 300
#define BAR_W 880
#define BAR_H 44

enum { ST_IDLE = 0, ST_RECORDING = 1, ST_PLAYING = 2 };

/* ---------- Общие данные (между главным потоком и аудио-потоком) ---------- */
static int16_t *g_audio = NULL;          /* сама запись */
static volatile int g_length = 0;        /* длина записи в сэмплах */
static volatile int g_play_pos = 0;      /* позиция воспроизведения */
static volatile int g_seek_to = -1;      /* запрос на перемотку (-1 = нет) */
static volatile int g_state = ST_IDLE;
static volatile int g_running = 1;
static volatile int g_level = 0;         /* громкость для индикатора */
static volatile int g_error = 0;         /* код ошибки аудио, если есть */

/* ---------- Аудио-поток: запись и воспроизведение ---------- */
static int audio_thread(SceSize args, void *argp) {
    static int16_t in_buf[IN_GRAIN] __attribute__((aligned(64)));
    static int16_t out_buf[OUT_GRAIN] __attribute__((aligned(64)));
    int in_port = -1;

    int out_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_GRAIN,
                                       SAMPLE_RATE, SCE_AUDIO_OUT_MODE_MONO);
    if (out_port < 0)
        g_error = out_port;

    while (g_running) {
        int st = g_state;

        if (st == ST_RECORDING) {
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

            sceAudioInInput(in_port, in_buf);   /* ждёт, пока наберётся блок */

            int peak = 0;
            int len = g_length;
            for (int i = 0; i < IN_GRAIN; i++) {
                int v = in_buf[i];
                if (v < 0) v = -v;
                if (v > peak) peak = v;
                if (len < MAX_SAMPLES)
                    g_audio[len++] = in_buf[i];
            }
            g_length = len;
            g_level = peak;

            if (len >= MAX_SAMPLES)
                __sync_bool_compare_and_swap(&g_state, ST_RECORDING, ST_IDLE);

        } else {
            /* не пишем - закрываем микрофон */
            if (in_port >= 0) {
                sceAudioInReleasePort(in_port);
                in_port = -1;
            }
            g_level = 0;

            if (st == ST_PLAYING && out_port >= 0) {
                int seek = g_seek_to;
                if (seek >= 0) {
                    if (seek > g_length) seek = g_length;
                    g_play_pos = seek;
                    g_seek_to = -1;
                }

                int pos = g_play_pos;
                int remaining = g_length - pos;
                if (remaining <= 0) {
                    g_play_pos = 0;
                    __sync_bool_compare_and_swap(&g_state, ST_PLAYING, ST_IDLE);
                    continue;
                }

                int n = remaining < OUT_GRAIN ? remaining : OUT_GRAIN;
                memcpy(out_buf, g_audio + pos, n * sizeof(int16_t));
                if (n < OUT_GRAIN)
                    memset(out_buf + n, 0, (OUT_GRAIN - n) * sizeof(int16_t));

                g_play_pos = pos + n;

                int peak = 0;
                for (int i = 0; i < n; i++) {
                    int v = out_buf[i];
                    if (v < 0) v = -v;
                    if (v > peak) peak = v;
                }
                g_level = peak;

                sceAudioOutOutput(out_port, out_buf);  /* ждёт, пока блок проиграется */
            } else {
                sceKernelDelayThread(5000);
            }
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
    h.format = 1;            /* PCM */
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

/* ---------- Вспомогательное ---------- */
static void fmt_time(char *out, size_t cap, int samples) {
    int s = samples / SAMPLE_RATE;
    int tenth = (samples % SAMPLE_RATE) * 10 / SAMPLE_RATE;
    snprintf(out, cap, "%02d:%02d.%d", s / 60, s % 60, tenth);
}

/* ---------- Главная программа ---------- */
int main(void) {
    g_audio = (int16_t *)malloc(MAX_SAMPLES * sizeof(int16_t));
    if (!g_audio) sceKernelExitProcess(0);

    vita2d_init();
    vita2d_set_clear_color(RGBA8(22, 24, 32, 255));
    vita2d_pgf *pgf = vita2d_load_default_pgf();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    SceUID thid = sceKernelCreateThread("audio", audio_thread, 0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(thid, 0, NULL);

    SceCtrlData pad;
    SceTouchData touch;
    unsigned int old_buttons = 0;
    char msg[160] = "Press X to start recording";
    char buf[128], t1[32], t2[32];
    int shown_level = 0;

    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old_buttons;
        old_buttons = pad.buttons;

        if (pressed & SCE_CTRL_START) break;

        int state = g_state;

        /* X: запись / стоп */
        if (pressed & SCE_CTRL_CROSS) {
            if (state == ST_RECORDING) {
                g_state = ST_IDLE;
                snprintf(msg, sizeof(msg), "Recording stopped");
            } else {
                g_state = ST_IDLE;              /* остановить воспроизведение */
                sceKernelDelayThread(60000);    /* дать аудио-потоку остановиться */
                g_length = 0;
                g_play_pos = 0;
                g_seek_to = -1;
                g_error = 0;
                __sync_synchronize();
                g_state = ST_RECORDING;
                snprintf(msg, sizeof(msg), "Recording... press X to stop");
            }
        }

        /* Квадрат: воспроизведение / стоп */
        if ((pressed & SCE_CTRL_SQUARE) && state != ST_RECORDING) {
            if (state == ST_PLAYING) {
                g_state = ST_IDLE;
                snprintf(msg, sizeof(msg), "Stopped");
            } else if (g_length > 0) {
                g_seek_to = 0;
                g_state = ST_PLAYING;
                snprintf(msg, sizeof(msg), "Playing");
            }
        }

        /* Треугольник: сохранить */
        if ((pressed & SCE_CTRL_TRIANGLE) && state != ST_RECORDING) {
            char path[128];
            int r = save_wav(path, sizeof(path));
            if (r == 0)      snprintf(msg, sizeof(msg), "Saved: %s", path);
            else if (r == -1) snprintf(msg, sizeof(msg), "Nothing to save - record something first");
            else              snprintf(msg, sizeof(msg), "Could not write the file");
        }

        /* Касание полосы времени: мгновенный переход */
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        if (touch.reportNum > 0 && g_state != ST_RECORDING && g_length > 0) {
            int tx = touch.report[0].x / 2;     /* сенсор 1920x1088 -> экран 960x544 */
            int ty = touch.report[0].y / 2;
            if (tx >= BAR_X && tx <= BAR_X + BAR_W &&
                ty >= BAR_Y - 30 && ty <= BAR_Y + BAR_H + 30) {
                int target = (int)((int64_t)(tx - BAR_X) * g_length / BAR_W);
                g_seek_to = target;
                g_state = ST_PLAYING;
            }
        }

        /* ---------- Рисуем ---------- */
        state = g_state;
        int length = g_length;
        int pos = g_play_pos;

        vita2d_start_drawing();
        vita2d_clear_screen();

        vita2d_pgf_draw_text(pgf, 40, 50, RGBA8(255, 255, 255, 255), 1.4f, "VITA SAMPLER 0.1  -  Recorder");

        const char *label = "READY";
        unsigned int color = RGBA8(160, 170, 190, 255);
        if (state == ST_RECORDING) { label = "REC";  color = RGBA8(255, 70, 70, 255); }
        if (state == ST_PLAYING)   { label = "PLAY"; color = RGBA8(80, 220, 120, 255); }
        vita2d_pgf_draw_text(pgf, 40, 110, color, 1.6f, label);

        if (state == ST_RECORDING) {
            vita2d_draw_fill_circle(220, 98, 12, RGBA8(255, 70, 70, 255));
        }

        fmt_time(t1, sizeof(t1), state == ST_PLAYING ? pos : length);
        fmt_time(t2, sizeof(t2), length);
        snprintf(buf, sizeof(buf), "%s / %s", t1, t2);
        vita2d_pgf_draw_text(pgf, 40, 175, RGBA8(255, 255, 255, 255), 1.8f, buf);

        /* индикатор уровня */
        int lvl = g_level * 100 / 32768;
        if (lvl > 100) lvl = 100;
        if (lvl > shown_level) shown_level = lvl;
        else shown_level = shown_level * 85 / 100;
        vita2d_draw_rectangle(BAR_X, 215, BAR_W, 14, RGBA8(45, 50, 66, 255));
        vita2d_draw_rectangle(BAR_X, 215, BAR_W * shown_level / 100, 14,
                              shown_level > 85 ? RGBA8(255, 70, 70, 255) : RGBA8(80, 220, 120, 255));
        vita2d_pgf_draw_text(pgf, BAR_X, 250, RGBA8(130, 140, 160, 255), 0.9f, "level");

        /* полоса времени */
        vita2d_draw_rectangle(BAR_X, BAR_Y, BAR_W, BAR_H, RGBA8(45, 50, 66, 255));
        if (length > 0 && state != ST_RECORDING) {
            int px = (int)((int64_t)pos * BAR_W / length);
            vita2d_draw_rectangle(BAR_X, BAR_Y, px, BAR_H, RGBA8(60, 120, 200, 255));
            vita2d_draw_rectangle(BAR_X + px - 2, BAR_Y - 6, 4, BAR_H + 12, RGBA8(255, 255, 255, 255));
        }
        vita2d_pgf_draw_text(pgf, BAR_X, BAR_Y + BAR_H + 28, RGBA8(130, 140, 160, 255), 0.9f,
                             "touch the bar to jump to any moment");

        /* сообщения */
        if (g_error != 0) {
            snprintf(buf, sizeof(buf), "Audio error: 0x%08X", (unsigned int)g_error);
            vita2d_pgf_draw_text(pgf, 40, 420, RGBA8(255, 120, 60, 255), 1.0f, buf);
        }
        vita2d_pgf_draw_text(pgf, 40, 455, RGBA8(255, 255, 255, 255), 1.0f, msg);
        vita2d_pgf_draw_text(pgf, 40, 505, RGBA8(130, 140, 160, 255), 0.95f,
                             "X: rec/stop    []: play/stop    /\\: save WAV    START: exit");

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    g_running = 0;
    sceKernelWaitThreadEnd(thid, NULL, NULL);

    vita2d_free_pgf(pgf);
    vita2d_fini();
    free(g_audio);
    sceKernelExitProcess(0);
    return 0;
}
