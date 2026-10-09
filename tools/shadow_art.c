/* shadow_art.c — export MPC plugin-skin artwork drawn by force-shadow's own
 * offline renderer (tools/render_conf_preview.c), so a skin generated from a
 * shadow_page.conf is pixel-identical to the Force Shadow page it came from.
 *
 * Build (x86 host is fine; the renderer is vendored, see tools/vendor/force-shadow/README.md):
 *   gcc -O2 -Itools/vendor/force-shadow/tools -o shadow_art tools/shadow_art.c -lm
 *
 * Reads commands on stdin, one per line, fields separated by '|':
 *   clear|RRGGBB                      fill the whole 1280x800 canvas
 *   frame|x|y|w|h|TITLE               titled frame box
 *   frameblank|x|y|w|h                 frame box, no title text (a real TrueType font draws the
 *                                       title afterward via PIL -- see shadow_skin.py's TITLE_FONT)
 *   text|cx|y|scale|RRGGBB|TEXT       centred text (baked 9x9 font; uppercase only)
 *   knob|cx|cy|r|pct                  knob body: ring, face, pointer dot (no label/value)
 *   pill|cx|cy|on                     toggle pill (no label)
 *   button|cx|cy|RRGGBB|LABEL         push button (auto-sized to the label)
 *   button|cx|cy|RRGGBB|LABEL|w|h     push button, explicit size (0 = auto on that axis)
 *   seg|x|y|w|h|RRGGBB|RRGGBB|LABEL   one enum segment: fill colour, text colour
 *   crop|out.ppm|x|y|w|h              write a region of the canvas
 *   strip|out.ppm|r|frames|RRGGBB     vertical knob filmstrip (frames x (2r+10)^2) on a bg colour
 *   theme|conf                        apply a conf's style=/theme_* lines (render_conf_preview's load_conf)
 *   readout|cx|cy|w|h|LABEL           readout box + label, no text (MPC draws the live value)
 *   stepper|cx|cy|w|h|LABEL           < box > stepper + label, no text
 *   dotreadout|cx|cy|w|h|LABEL        dot-matrix LCD readout (JV-880-style), same "no text" convention
 *   dotstepper|cx|cy|w|h|LABEL        dot-matrix LCD stepper, same "no text" convention
 *   tile|x|y|w|h|FILL|BORDER|bw       list tile: fill, then a border of bw px (0 = the plate-line rules)
 *   sstrip|out.ppm|w|h|frames|v|RRGGBB  slider filmstrip (frames x w*h, stacked vertically); v=1 vertical
 */
#define main render_conf_preview_main
#include "render_conf_preview.c"
#undef main

static void write_region(FILE *f, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int px = x + i, py = y + j;
            unsigned char c[3] = {0, 0, 0};
            if (px >= 0 && px < LAND_W && py >= 0 && py < LAND_H) memcpy(c, canvas[py][px], 3);
            fwrite(c, 1, 3, f);
        }
}

static void crop(const char *path, int x, int y, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    write_region(f, x, y, w, h);
    fclose(f);
}

/* frame_box_blank() lives in render_conf_preview.c now (frame_box() minus its baked title text,
 * sharing frame_border() with frame_box() so the TD3 style force-acid uses is handled too --
 * this file's own stub only covered the non-TD3 branch and has been dropped in favour of it). */

static void knob_body(int cx, int cy, int r, int pct) {
    /* Mutable-Instruments knob: a matte black skirt with a white cap, and a single white position
     * line across the black skirt (like a real MI module — no line on the white cap itself).
     * Engraved end-stop ticks on the panel. Kept within the filmstrip's ~5px margin (strip = 2r+10).
     * shadow_art.c-only; force-shadow's on-device renderer keeps its plain ring. */
    double angle = (-135.0 + 270.0 * pct / 100.0) * M_PI / 180.0, s = sin(angle), co = cos(angle);
    int capr = (int)lround(r * 0.60), pw = r < 24 ? 1 : 2;
    /* engraved min/max end-stop ticks on the panel */
    for (int e = 0; e < 2; e++) {
        double a = (e ? 135.0 : -135.0) * M_PI / 180.0, es = sin(a), ec = cos(a);
        for (int t = 0; t <= 2; t++) {
            int rr = r + 1 + t;
            fill_circle(cx + (int)lround(rr * es), cy - (int)lround(rr * ec), 1, KNOB_RING);
        }
    }
    /* black skirt */
    fill_circle(cx, cy, r, KNOB_FACE);
    /* soft top sheen on the skirt for a moulded feel */
    for (int yy = -r; yy <= r; yy++)
        for (int xx = -r; xx <= r; xx++) {
            if (xx * xx + yy * yy > (r - 1) * (r - 1)) continue;
            double hx = xx + 0.42 * r, hy = yy + 0.42 * r;
            int alpha = (int)lround(38.0 - sqrt(hx * hx + hy * hy) * 38.0 / (r * 1.2));
            if (alpha > 0) put_px_blend(cx + xx, cy + yy, 0xffffff, alpha);
        }
    /* white position line across the black skirt (cap edge -> rim) */
    for (int t = 0; t <= 20; t++) {
        int rr = (capr - 1) + ((r - 2) - (capr - 1)) * t / 20;
        fill_circle(cx + (int)lround(rr * s), cy - (int)lround(rr * co), pw, 0xffffff);
    }
    /* white cap with a thin dark bezel — no line drawn on the cap (real MI look) */
    fill_circle(cx, cy, capr, 0xffffff);
    draw_ring(cx, cy, capr, 1, 0x2a2a2a);
}

static void pill(int cx, int cy, int on) {
    /* widget_toggle() minus its label */
    int pw = 51, ph = 27;
    fill_rect(cx - pw / 2, cy - ph / 2, pw, ph, 0x050403);
    draw_ring(cx - pw / 2 + ph / 2, cy, ph / 2 - 2, 1, PLATE_LINE);
    int lx = on ? (cx + pw / 2 - ph / 2) : (cx - pw / 2 + ph / 2);
    fill_circle(lx, cy, ph / 2 - 4, on ? ACCENT_HI : 0x4c473d);
}

static void clear(uint32_t c) {
    fill_rect(0, 0, LAND_W, LAND_H, c);
}

/* Self-contained dot-matrix readout/stepper for use INSIDE a plugin's own
 * canvas, unlike widget_readout()/widget_stepper()'s G_DSP branch (only
 * force_shadow.c's outer chrome, cy < TOPBAR_H, ever hits that). MPC skins
 * have no such chrome band -- our own "topbar" IS part of the tab -- so
 * this bakes the same bezel + dot_cell_fit() look at any position, gated
 * only by theme_*'s dot-matrix colours (set regardless of topbar_style so
 * this works even where render_conf_preview.c's own G_DSP stays off). */
static void dot_readout(int cx, int cy, int w, int h, const char *label) {
    int x0 = cx - w / 2, y0 = cy - h / 2;
    if (label[0]) draw_text(x0, y0 - 22, label, 1.5f, INK_DIM);
    fill_rr(x0 - 4, y0 - 4, w + 8, h + 8, 8, DSP_BEZEL);
    dot_cell_fit(x0, y0, w, h, "", DSP_CELL, DSP_OFF, DSP_INK);
}

static void dot_stepper(int cx, int cy, int w, int h, const char *label) {
    int x0 = cx - w / 2, y0 = cy - h / 2;
    if (label[0]) draw_text(x0, y0 - 22, label, 1.5f, INK_DIM);
    fill_rr(x0 - 4, y0 - 4, w + 8, h + 8, 8, DSP_BEZEL);
    fill_rr(x0, y0, h, h, 5, DSP_BEZEL);
    fill_rr(x0 + w - h, y0, h, h, 5, DSP_BEZEL);
    draw_arrow(x0 + h / 2, cy, h / 4, -1, DSP_BG);
    draw_arrow(x0 + w - h / 2, cy, h / 4, 1, DSP_BG);
    int bx = x0 + h + 3, bw = w - 2 * h - 6;
    dot_cell_fit(bx, y0, bw, h, "", DSP_CELL, DSP_OFF, DSP_INK);
}

static void strip(const char *path, int r, int frames, uint32_t bg) {
    int s = 2 * r + 10, c = s / 2;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", s, s * frames);
    for (int k = 0; k < frames; k++) {
        fill_rect(0, 0, s, s, bg);
        knob_body(c, c, r, (int)lround(100.0 * k / (frames - 1)));
        write_region(f, 0, 0, s, s);
    }
    fclose(f);
}

/* Mutable-style fader: a thin dark track, an accent (cyan) fill up to the value, and a rectangular
 * white cap (portrait for vertical, landscape for horizontal) with a dark bezel + centre groove. */
static void slider_body(int x, int y, int w, int h, int vert, double t) {
    if (vert) {
        int wellw = 8, wx = x + (w - wellw) / 2;
        fill_rr(wx, y, wellw, h, wellw / 2, 0x2a2a2a);                 /* thin track */
        int pad = 4, th = 28, tw = (w - 12 < 16 ? 16 : w - 12);       /* portrait cap */
        int travel = h - 2 * pad - th, ty = y + pad + (int)lround((1.0 - t) * travel);
        int fy = ty + th / 2;
        if (y + h - pad - fy > 0)
            fill_rr(wx, fy, wellw, y + h - pad - fy, wellw / 2, ACCENT);   /* fill below the cap */
        int tx = x + (w - tw) / 2;
        fill_rr(tx - 1, ty - 1, tw + 2, th + 2, 5, 0x2a2a2a);          /* bezel */
        fill_rr(tx, ty, tw, th, 4, 0xffffff);                         /* white cap */
        fill_rect(tx + 3, ty + th / 2 - 1, tw - 6, 2, 0x2a2a2a);      /* centre groove */
    } else {
        int wellh = 8, wy = y + (h - wellh) / 2;
        fill_rr(x, wy, w, wellh, wellh / 2, 0x2a2a2a);
        int pad = 4, tw = 28, th = (h - 12 < 16 ? 16 : h - 12);       /* landscape cap */
        int travel = w - 2 * pad - tw, tx = x + pad + (int)lround(t * travel);
        int fx = tx + tw / 2;
        if (fx - (x + pad) > 0)
            fill_rr(x + pad, wy, fx - (x + pad), wellh, wellh / 2, ACCENT);
        int ty = y + (h - th) / 2;
        fill_rr(tx - 1, ty - 1, tw + 2, th + 2, 5, 0x2a2a2a);
        fill_rr(tx, ty, tw, th, 4, 0xffffff);
        fill_rect(tx + tw / 2 - 1, ty + 3, 2, th - 6, 0x2a2a2a);
    }
}

static void sstrip(const char *path, int w, int h, int frames, int vert, uint32_t bg) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", w, h * frames);
    for (int k = 0; k < frames; k++) {
        fill_rect(0, 0, w + 4, h + 4, bg);
        slider_body(0, 0, w, h, vert, (double)k / (frames - 1));
        write_region(f, 0, 0, w, h);
    }
    fclose(f);
}

#define HEX(s) ((uint32_t)strtoul((s), NULL, 16))

int main(void) {
    char line[512], *a[10];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        int n = 0;
        for (char *t = strtok(line, "|"); t && n < 10; t = strtok(NULL, "|")) a[n++] = t;
        if (!n) continue;
        const char *op = a[0];
        if (!strcmp(op, "clear") && n == 2) clear(HEX(a[1]));
        else if (!strcmp(op, "frame") && n == 6) frame_box(atoi(a[1]), atoi(a[2]), atoi(a[3]), atoi(a[4]), a[5]);
        else if (!strcmp(op, "frameblank") && n == 5) frame_box_blank(atoi(a[1]), atoi(a[2]), atoi(a[3]), atoi(a[4]));
        else if (!strcmp(op, "text") && n == 6) label_text_c(atoi(a[1]), atoi(a[2]), a[5], (float)atof(a[3]), HEX(a[4]));
        else if (!strcmp(op, "knob") && n == 5) knob_body(atoi(a[1]), atoi(a[2]), atoi(a[3]), atoi(a[4]));
        else if (!strcmp(op, "pill") && n == 4) pill(atoi(a[1]), atoi(a[2]), atoi(a[3]));
        else if (!strcmp(op, "button") && n == 5) widget_button(atoi(a[1]), atoi(a[2]), a[4], HEX(a[3]));
        else if (!strcmp(op, "button") && n == 7) widget_button_sz(atoi(a[1]), atoi(a[2]), a[4], HEX(a[3]), atoi(a[5]), atoi(a[6]));
        else if (!strcmp(op, "seg") && (n == 8 || n == 9)) {
            int x = atoi(a[1]), y = atoi(a[2]), w = atoi(a[3]), h = atoi(a[4]);
            float sc = (n == 9) ? (float)atof(a[8]) : 1.6f;   /* optional per-seg scale: popup options pass a larger one */
            fill_rect(x, y, w, h, HEX(a[5]));
            label_text_c(x + w / 2, y + h / 2 - (int)lround(11.0 * sc / 1.6), a[7], sc, HEX(a[6]));   /* font_label TTF if set, else baked font; recenter scales with glyph height */
        }
        else if (!strcmp(op, "theme") && n == 2) load_conf(a[1]);
        else if (!strcmp(op, "readout") && n == 6) widget_readout(atoi(a[1]), atoi(a[2]), atoi(a[3]), atoi(a[4]), a[5][0] == '-' ? "" : a[5], "");
        else if (!strcmp(op, "stepper") && n == 6) widget_stepper(atoi(a[1]), atoi(a[2]), atoi(a[3]), atoi(a[4]), a[5][0] == '-' ? "" : a[5], "");
        else if (!strcmp(op, "dotreadout") && n == 6) dot_readout(atoi(a[1]), atoi(a[2]), atoi(a[3]), atoi(a[4]), a[5][0] == '-' ? "" : a[5]);
        else if (!strcmp(op, "dotstepper") && n == 6) dot_stepper(atoi(a[1]), atoi(a[2]), atoi(a[3]), atoi(a[4]), a[5][0] == '-' ? "" : a[5]);
        else if (!strcmp(op, "tile") && n == 8) {
            int x = atoi(a[1]), y = atoi(a[2]), w = atoi(a[3]), h = atoi(a[4]), bw = atoi(a[7]);
            fill_rect(x, y, w, h, HEX(a[5]));
            if (bw > 0) {
                fill_rect(x, y, w, bw, HEX(a[6])); fill_rect(x, y + h - bw, w, bw, HEX(a[6]));
                fill_rect(x, y, bw, h, HEX(a[6])); fill_rect(x + w - bw, y, bw, h, HEX(a[6]));
            } else {
                fill_rect(x, y, w, 1, PLATE_LINE); fill_rect(x, y + h - 1, w, 1, PLATE_LINE);
            }
        }
        else if (!strcmp(op, "crop") && n == 6) crop(a[1], atoi(a[2]), atoi(a[3]), atoi(a[4]), atoi(a[5]));
        else if (!strcmp(op, "sstrip") && n == 7) sstrip(a[1], atoi(a[2]), atoi(a[3]), atoi(a[4]), atoi(a[5]), HEX(a[6]));
        else if (!strcmp(op, "strip") && n == 5) strip(a[1], atoi(a[2]), atoi(a[3]), HEX(a[4]));
        else { fprintf(stderr, "shadow_art: bad command: %s (%d fields)\n", op, n); return 1; }
    }
    return 0;
}
