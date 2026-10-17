/* Экспорт процедурных деталей интерфейса в отдельные PNG (прозрачный фон, родной размер).
 * Сборка и запуск: make -C host export   ->  ../design/parts/*.png */
#define SAMPLER_NO_MAIN
#include "../main.c"
#include <png.h>

static const char *g_out = "../design/parts";

static Canvas mk(int w, int h) { Canvas c; c.w = w; c.h = h; c.px = (uint32_t *)calloc((size_t)w * h, 4); return c; }
static void save(const char *name, Canvas *c) {
    char p[300];
    snprintf(p, sizeof p, "%s/%s.png", g_out, name);
    png_image im; memset(&im, 0, sizeof im);
    im.version = PNG_IMAGE_VERSION; im.width = (png_uint_32)c->w; im.height = (png_uint_32)c->h; im.format = PNG_FORMAT_RGBA;
    if (!png_image_write_to_file(&im, p, 0, c->px, 0, NULL)) fprintf(stderr, "fail %s\n", p);
    else printf("%s %dx%d\n", name, c->w, c->h);
    free(c->px);
}
/* стереть прямоугольник со скруглением (окно в рамке) */
static void punch(Canvas *c, float x, float y, float w, float h, float r) {
    for (int j = 0; j < c->h; j++) for (int i = 0; i < c->w; i++)
        if (sd_rrect(i + 0.5f, j + 0.5f, x, y, w, h, r) < -0.5f) c->px[j * c->w + i] = 0;
}

int main(int argc, char **argv) {
    if (argc > 1) g_out = argv[1];
    /* оранжевая рамка экрана (окно ЭЛТ прозрачное) */
    { Canvas c = mk(BEZ_W, BEZ_H);
      cv_rrect_grad(&c, 0, 0, BEZ_W, BEZ_H, 10, RGBA8(232, 124, 34, 255), RGBA8(150, 62, 10, 255));
      cv_rrect_stroke(&c, 0, 0, BEZ_W, BEZ_H, 10, 1.4f, RGBA8(96, 38, 6, 255));
      cv_rrect_grad(&c, 3, 3, BEZ_W - 6, BEZ_H - 6, 8, RGBA8(248, 150, 56, 255), RGBA8(190, 82, 16, 255));
      cv_rrect_grad(&c, 9, 8, BEZ_W - 18, BEZ_H - 17, 7, RGBA8(206, 96, 20, 255), RGBA8(120, 48, 8, 255));
      cv_rrect_stroke(&c, 9, 8, BEZ_W - 18, BEZ_H - 17, 7, 1.0f, RGBA8(80, 30, 4, 255));
      cv_rrect(&c, 14, 4.5f, BEZ_W - 28, 1.6f, 0.8f, RGBA8(255, 226, 180, 190));
      cv_rrect(&c, 4.5f, 16, 1.4f, BEZ_H - 34, 0.7f, RGBA8(255, 200, 140, 90));
      cv_rrect(&c, 14, BEZ_H - 5.5f, BEZ_W - 28, 1.2f, 0.6f, RGBA8(255, 170, 90, 120));
      punch(&c, CRT_X - BEZ_X, CRT_Y - BEZ_Y, CRT_W, CRT_H, 9);
      save("bezel_frame", &c); }
    /* тёмная трубка ЭЛТ с сеткой */
    { Canvas c = mk(CRT_W, CRT_H);
      cv_rrect_grad(&c, 0, 0, CRT_W, CRT_H, 9, RGBA8(30, 20, 14, 255), RGBA8(12, 8, 5, 255));
      for (int i = 1; i < 4; i++) { int yy = WAVE_Y - CRT_Y + i * WAVE_H / 4; for (int x = WAVE_X - CRT_X; x < WAVE_X - CRT_X + WAVE_W; x += 8) cv_rect(&c, x, yy, 4, 1, RGBA8(255, 150, 46, 34)); }
      for (int i = 1; i < 16; i++) cv_rect(&c, WAVE_X - CRT_X + i * WAVE_W / 16, WAVE_Y - CRT_Y, 1, WAVE_H, RGBA8(255, 150, 46, 20));
      save("crt_screen", &c); }
    /* стекло поверх экрана (выпуклость, блики, строки развёртки) */
    { Canvas c = mk(GL_W, GL_H); glass_render(&c); save("crt_glass", &c); }
    /* шкала уровня */
    { Canvas c = mk(METER_W + 12, METER_H + 12);
      cv_rrect(&c, 0, 1, METER_W + 12, METER_H + 11, 6, RGBA8(255, 255, 255, 120));
      cv_rrect(&c, 0, 0, METER_W + 12, METER_H + 11, 6, RGBA8(96, 88, 72, 255));
      cv_rrect(&c, 2, 2, METER_W + 8, METER_H + 8, 5, RGBA8(26, 22, 18, 255));
      for (int x = 0; x < METER_W; x++) { float u = ((float)x + 0.5f) / (float)METER_W * 2.0f - 1.0f; float sh = 1.0f - 0.30f * u * u;
        for (int y = 0; y < METER_H; y++) { int v = (int)(58.0f * sh); c.px[(6 + y) * c.w + 6 + x] = RGBA8(v, v - 5 > 0 ? v - 5 : 0, v - 10 > 0 ? v - 10 : 0, 255); } }
      save("meter_frame", &c); }
    { Canvas c = mk(METER_W, METER_H); meter_render(&c); save("meter_fill", &c); }
    { Canvas c = mk(METER_W + 8, METER_H + 8); meter_glass_render(&c); save("meter_glass", &c); }
    for (int on = 0; on < 2; on++) {
      Canvas c = mk(50, 20);
      cv_rrect(&c, 0, 0, 50, 20, 3, on ? RGBA8(214, 32, 20, 255) : RGBA8(24, 15, 10, 255));
      cv_rrect_stroke(&c, 0, 0, 50, 20, 3, 1.0f, RGBA8(10, 6, 4, 255));
      save(on ? "clip_window_on" : "clip_window_off", &c); }
    { Canvas c = mk(50, 26); cv_rrect(&c, 0, 0, 50, 26, 3, RGBA8(24, 15, 10, 255)); cv_rrect_stroke(&c, 0, 0, 50, 26, 3, 1.0f, RGBA8(10, 6, 4, 255)); save("peak_window", &c); }
    /* логотип */
    { Canvas c = mk(26, 26);
      cv_rrect_grad(&c, 0, 0, 26, 26, 3, K_ORANGE_HI, K_ORANGE); cv_rrect_stroke(&c, 0, 0, 26, 26, 3, 1.0f, K_ORANGE_LO);
      static const int bh[] = { 7, 14, 10, 17, 6 };
      for (int i = 0; i < 5; i++) cv_rrect(&c, 5.5f + i * 3.6f, 13.0f - bh[i] * 0.5f, 2.0f, (float)bh[i], 1.0f, K_CREAM_HI);
      save("logo", &c); }
    /* сегментные окошки (пустые) и цифры */
    { Canvas c = mk(70, 29); cv_seg_box(&c, 0, 0, 70, 29, "", RGBA8(255, 70, 48, 255), 19.0f); save("seg_box_top_70x29", &c); }
    { Canvas c = mk(52, 38); cv_seg_box(&c, 0, 0, 52, 38, "", K_AMBER, 22.0f); save("seg_box_amber_52x38", &c); }
    { static const char *ds[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "-" };
      for (int i = 0; i < 11; i++) {
        char nm[48];
        Canvas c = mk(28, 28); cv_seg_text(&c, 3, 3, 22.0f, ds[i], K_AMBER);
        snprintf(nm, sizeof nm, "digit_amber_%s", i == 10 ? "dash" : ds[i]); save(nm, &c);
        Canvas r = mk(24, 24); cv_seg_text(&r, 3, 3, 19.0f, ds[i], RGBA8(255, 70, 48, 255));
        snprintf(nm, sizeof nm, "digit_red_%s", i == 10 ? "dash" : ds[i]); save(nm, &r); } }
    /* значки на кнопках (цвет К_BROWN, центр спрайта) */
    { static const struct { const char *n; int k; } ic[] = { { "icon_crop", IC_CROP }, { "icon_undo", IC_UNDO }, { "icon_reverse", IC_REV }, { "icon_normalize", IC_NORM },
        { "icon_delete", IC_DEL }, { "icon_silence", IC_SIL }, { "icon_duplicate", IC_DUP }, { "icon_gap", IC_GAP }, { "icon_grain", IC_GRAIN }, { "icon_plus", IC_PLUS }, { "icon_minus", IC_MINUS } };
      for (unsigned i = 0; i < sizeof ic / sizeof ic[0]; i++) { Canvas c = mk(40, 40); cv_icon_ex(&c, ic[i].k, 20, 20, K_BROWN); save(ic[i].n, &c); } }
    { static const float sh[4][4][2] = {
        { { -12, 0 }, { 12, 0 }, { 12, 0 }, { 12, 0 } }, { { -12, 0 }, { 0, 0 }, { 12, 9 }, { 12, 9 } },
        { { -12, 9 }, { 0, 0 }, { 12, 0 }, { 12, 0 } }, { { -12, 9 }, { -4, 0 }, { 4, 0 }, { 12, 9 } } };
      static const char *fn[4] = { "icon_filter_off", "icon_filter_lpf", "icon_filter_hpf", "icon_filter_bpf" };
      for (int i = 0; i < 4; i++) { Canvas c = mk(40, 24);
        for (int q = 0; q < 3; q++) cv_line(&c, 20 + sh[i][q][0], 7 + sh[i][q][1], 20 + sh[i][q + 1][0], 7 + sh[i][q + 1][1], 1.3f, K_BROWN);
        save(fn[i], &c); } }
    /* окно АЧХ фильтра (пустое, без кривой) */
    { Canvas c = mk(CURVE_W + 8, CURVE_H + 8);
      cv_rrect_grad(&c, 0, 0, CURVE_W + 8, CURVE_H + 8, 5, RGBA8(214, 104, 24, 255), RGBA8(140, 56, 8, 255));
      cv_rrect_stroke(&c, 0, 0, CURVE_W + 8, CURVE_H + 8, 5, 1.0f, RGBA8(90, 34, 4, 255));
      cv_rrect_grad(&c, 4, 4, CURVE_W, CURVE_H, 3, RGBA8(30, 20, 14, 255), RGBA8(12, 8, 5, 255));
      save("filter_window", &c); }
    return 0;
}
