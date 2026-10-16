/* Хост-превью: настоящий ui_draw() рисуется программно в PNG-подобный буфер (RGBA), текст — через FreeType.
 * Сборка: make -C host preview   (нужны libfreetype и python3 + PIL для сохранения PNG) */
#define SAMPLER_NO_MAIN
#include "../main.c"
#include <ft2build.h>
#include FT_FREETYPE_H

struct vita2d_texture { int w, h; uint32_t *px; };
static uint32_t *g_fb;
static FT_Library g_ft;
struct vita2d_font { FT_Face face; };

vita2d_texture *vita2d_create_empty_texture(unsigned int w, unsigned int h) {
    vita2d_texture *t = malloc(sizeof(*t)); t->w = (int)w; t->h = (int)h; t->px = calloc((size_t)w * h, 4); return t;
}
void vita2d_free_texture(vita2d_texture *t) { if (t) { free(t->px); free(t); } }
void *vita2d_texture_get_datap(const vita2d_texture *t) { return t->px; }
unsigned int vita2d_texture_get_stride(const vita2d_texture *t) { return (unsigned)t->w * 4; }
static void blend(int x, int y, uint32_t c, int cover) {   /* cover 0..255 */
    if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H) return;
    int a = ((int)(c >> 24) * cover) / 255;
    uint32_t *d = &g_fb[y * SCR_W + x];
    int dr = *d & 255, dg = (*d >> 8) & 255, db = (*d >> 16) & 255;
    int r = (int)(c & 255), g = (int)((c >> 8) & 255), b = (int)((c >> 16) & 255);
    dr += (r - dr) * a / 255; dg += (g - dg) * a / 255; db += (b - db) * a / 255;
    *d = RGBA8(dr, dg, db, 255);
}
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int c) {
    for (int j = (int)y; j < (int)(y + h); j++) for (int i = (int)x; i < (int)(x + w); i++) blend(i, j, c, 255);
}
void vita2d_draw_texture_part(const vita2d_texture *t, float x, float y, float tx, float ty, float tw, float th) {
    for (int j = 0; j < (int)th; j++) for (int i = 0; i < (int)tw; i++) {
        int sx = (int)tx + i, sy = (int)ty + j;
        if (sx < 0 || sy < 0 || sx >= t->w || sy >= t->h) continue;
        blend((int)x + i, (int)y + j, t->px[sy * t->w + sx], 255);
    }
}
void vita2d_draw_texture(const vita2d_texture *t, float x, float y) { vita2d_draw_texture_part(t, x, y, 0, 0, (float)t->w, (float)t->h); }
int vita2d_init_advanced(unsigned int n) { (void)n; return 1; }
int vita2d_fini(void) { return 1; }
void vita2d_clear_screen(void) { memset(g_fb, 0, (size_t)SCR_W * SCR_H * 4); }
void vita2d_set_clear_color(unsigned int c) { (void)c; }
void vita2d_start_drawing(void) {}
void vita2d_end_drawing(void) {}
void vita2d_swap_buffers(void) {}
void vita2d_wait_rendering_done(void) {}
vita2d_pgf *vita2d_load_default_pgf(void) { return NULL; }
void vita2d_free_pgf(vita2d_pgf *f) { (void)f; }
int vita2d_pgf_draw_text(vita2d_pgf *f, int x, int y, unsigned int c, float s, const char *t) { (void)f; (void)x; (void)y; (void)c; (void)s; (void)t; return 0; }
vita2d_font *vita2d_load_font_file(const char *fn) {
    const char *map = strstr(fn, "mono") ? "fonts/DejaVuSansMono-Bold.ttf" : "fonts/DejaVuSansCondensed-Bold.ttf";
    char path[256]; snprintf(path, sizeof(path), "%s/%s", getenv("FONT_DIR") ? getenv("FONT_DIR") : "..", map);
    vita2d_font *f = malloc(sizeof(*f));
    if (FT_New_Face(g_ft, path, 0, &f->face)) { free(f); return NULL; }
    return f;
}
void vita2d_free_font(vita2d_font *f) { if (f) { FT_Done_Face(f->face); free(f); } }
int vita2d_font_draw_text(vita2d_font *f, int x, int y, unsigned int c, unsigned int size, const char *t) {
    FT_Set_Pixel_Sizes(f->face, 0, size);
    for (; *t; t++) {
        if (FT_Load_Char(f->face, (unsigned char)*t, FT_LOAD_RENDER)) continue;
        FT_GlyphSlot g = f->face->glyph;
        for (unsigned j = 0; j < g->bitmap.rows; j++) for (unsigned i = 0; i < g->bitmap.width; i++)
            blend(x + g->bitmap_left + (int)i, y - g->bitmap_top + (int)j, c, g->bitmap.buffer[j * g->bitmap.pitch + i]);
        x += (int)(g->advance.x >> 6);
    }
    return 0;
}
int vita2d_font_text_width(vita2d_font *f, unsigned int size, const char *t) {
    FT_Set_Pixel_Sizes(f->face, 0, size);
    int w = 0;
    for (; *t; t++) if (!FT_Load_Char(f->face, (unsigned char)*t, FT_LOAD_DEFAULT)) w += (int)(f->face->glyph->advance.x >> 6);
    return w;
}

/* сцена: имитация записи, чтобы волна, курсоры, слайсы выглядели как в жизни */
static void scene(UiState *u, int page, int mode, int state) {
    memset(u, 0, sizeof(*u));
    int len = g_length;
    u->page = page; u->mode = mode; u->state = state; u->loop = 1; u->fx_sec = 0;
    u->length = len; u->playhead = len / 3; u->split = 0;
    u->c1 = len * 0.2; u->c2 = len * 0.75; u->active = 0;
    u->level_pct = 72; u->peak_pct = 84; u->clip = 0;
    snprintf(u->msg, sizeof(u->msg), "Loop ON");
    u->sl_n = 8; u->sl_sel = 2; u->sl_playing = (state == ST_PLAYING && mode == 1) ? 2 : -1;
    u->can_undo = 1; u->bpm = 124; u->bpm_t = 124; u->bpm_b = 120;
    u->gr_on = g_gr_on; u->flt_type = g_flt_type; u->knob_down = -1; u->frame = 10;
    u->view_len = len;
    compute_layout(&u->lay, len, 0, 0, u->c1, u->c2);
    view_start = 0; view_len = len;
}

int main(int argc, char **argv) {
    const char *outp = argc > 1 ? argv[1] : "/tmp/frame.raw";
    int page = argc > 2 ? atoi(argv[2]) : 0, mode = argc > 3 ? atoi(argv[3]) : 0;
    int fxsec = argc > 4 ? atoi(argv[4]) : 0, legend = argc > 5 ? atoi(argv[5]) : 0, sc = argc > 6 ? atoi(argv[6]) : 0;   /* sc: 0 обычная, 1 пусто, 2 запись */
    g_fb = calloc((size_t)SCR_W * SCR_H, 4);
    FT_Init_FreeType(&g_ft);
    for (int i = 0; i < W_N; i++) g_hann[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * (float)i / (float)W_N));
    g_fx[0] = 15; g_fx[1] = 100; g_fx[2] = FX_SPEED_DEF; g_fx[3] = 24; g_fx[4] = 50; g_fx[5] = 75;
    g_gr_on = 1; g_flt_type = 3;
    int len = SAMPLE_RATE * 6;
    g_audio = malloc((size_t)len * 2); g_length = len;
    for (int i = 0; i < len; i++) {                 /* "бочка-снейр-хэт": узнаваемые всплески */
        double t = (double)i / SAMPLE_RATE, ph = fmod(t * 2.0, 1.0);
        double env = exp(-ph * 7.0) * (((int)(t * 2.0) % 2) ? 0.55 : 1.0);
        double s = sin(2 * M_PI * (55 + 120 * exp(-ph * 30)) * t) * env + 0.18 * sin(2 * M_PI * 440 * t) * exp(-fmod(t, 0.5) * 3);
        g_audio[i] = (int16_t)(26000 * s * 0.8);
    }
    recompute_blocks(len);
    g_pgf = NULL;
    fonts_load();
    if (!ui_init()) { fprintf(stderr, "ui_init failed\n"); return 1; }
    g_wave_tex[0] = vita2d_create_empty_texture(WAVE_W, WAVE_H);
    UiState u;
    scene(&u, page, mode, ST_PLAYING);
    u.fx_sec = fxsec; u.legend = legend;
    if (mode == 1) { for (int i = 0; i <= 8; i++) g_sl_pos[i] = len * i / 8; g_sl_n = 8; u.sl_n = 8; }
    if (sc == 1) { u.length = 0; u.playhead = 0; u.state = ST_IDLE; u.loop = 0; compute_layout(&u.lay, 0, 0, 0, 0, 0); }
    if (sc == 2) { u.live = 1; u.state = ST_RECORDING; compute_layout(&u.lay, len, 1, 0, 0, 0); snprintf(u.msg, sizeof(u.msg), "Recording..."); u.clip = 30; }
    render_wave(g_wave_tex[0], &u.lay, sc == 1 ? 0 : len, u.live);
    u.wave_tex = g_wave_tex[0];
    ui_update_textures(&u);
    ui_draw(&u);
    FILE *f = fopen(outp, "wb"); fwrite(g_fb, 4, (size_t)SCR_W * SCR_H, f); fclose(f);
    return 0;
}
