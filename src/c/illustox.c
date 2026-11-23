// Illustox -- Watchface fuer Emery und Gabbro.
//
// Zeichnet eine flaechige Berglandschaft im Stil von Boho-Wandbildern:
// Himmel, Sonne oder Mondsichel, mehrere Bergketten hintereinander, manchmal
// ein Tal mit gewundenem Fluss. Die Szene entsteht aus einem Zufallssamen,
// der sich je nach Einstellung jede Minute, Viertelstunde oder Stunde aendert
// (optional zusaetzlich per Handgelenk-Schuetteln). Oben steht nur die Uhrzeit.
//
// Die Szene wird bei jedem Neuzeichnen aus dem Samen neu berechnet, es gibt
// also keinen Szenen-Zustand ausser dem Samen selbst.
//
// Alle Flaechen schreibt der Code selbst in den Framebuffer. Zwei Stile:
// "Flat" nimmt je Flaeche eine der 64 Palettenfarben, "Print" rastert je
// Flaeche zwei benachbarte Palettenfarben per 4x4-Bayer-Matrix und gibt dem
// Himmel einen Verlauf des Mischanteils.

#include <pebble.h>

// ---- Farbwelten ------------------------------------------------------------
// Je Welt: Himmel, Sonne/Mond, Schrift, vier Bergfarben von hinten nach vorn.
// flat: je eine Palettenfarbe (Stil Flat).
// soft: je zwei benachbarte Palettenfarben a/b und ein Mischanteil von b in
// Sechzehnteln (Stil Print). Nur zwei Farben je Flaeche, damit der Farbton
// sauber bleibt; kanalweises Dithering ergab graue, matschige Flaechen.
// Gabbro zeigt alles entsaettigt.
typedef struct {
  uint32_t sky, sun, ink;
  uint32_t layer[4];
} Flat;

typedef struct {
  uint32_t a, b;
  uint8_t lv;         // 0 = rein a, 16 = rein b
} Mix;

typedef struct {
  Mix sky;            // lv gilt oben
  uint8_t sky_lv_bottom;
  Mix sun;
  Mix layer[4];
} Soft;

typedef struct {
  Flat flat;
  Soft soft;
} World;

static const World WORLDS[] = {
  // 0 Desert: Creme, Pfirsich, Rost, Petrol, Nachtblau (wie die Vorlage)
  { { 0xFFFFAA, 0xFF5500, 0x550000, { 0xFFAA55, 0xAA5500, 0x005555, 0x000055 } },
    { { 0xFFFFFF, 0xFFFFAA, 8 }, 15, { 0xFF5500, 0xFFAA55, 6 },
      { { 0xFFAA55, 0xFFAAAA, 5 }, { 0xAA5500, 0xAA5555, 5 },
        { 0x005555, 0x005555, 0 }, { 0x000055, 0x000055, 0 } } } },
  // 1 Dusk: Rosa Himmel, Flieder bis Aubergine
  { { 0xFFAAAA, 0xFFFFAA, 0x550055, { 0xAA55AA, 0x5555AA, 0x550055, 0x000055 } },
    { { 0xFFFFFF, 0xFFAAAA, 9 }, 14, { 0xFFFFAA, 0xFFFFFF, 8 },
      { { 0xAA55AA, 0xFFAAFF, 5 }, { 0x5555AA, 0xAA55AA, 5 },
        { 0x550055, 0x5555AA, 4 }, { 0x000055, 0x550055, 6 } } } },
  // 2 Sage: Creme, Oliv und Waldgruen
  { { 0xFFFFAA, 0xFFAA55, 0x005500, { 0xAAAA55, 0x55AA55, 0x555500, 0x005500 } },
    { { 0xFFFFFF, 0xFFFFAA, 8 }, 15, { 0xFFAA55, 0xFFFFAA, 4 },
      { { 0xAAAA55, 0xFFFFAA, 5 }, { 0x55AA55, 0xAAAA55, 6 },
        { 0x555500, 0x55AA55, 5 }, { 0x005500, 0x000000, 4 } } } },
  // 3 Ocean: Eisblau, Kadettblau bis Tiefsee
  { { 0xAAFFFF, 0xFFFFFF, 0x000055, { 0x55AAAA, 0x0055AA, 0x005555, 0x000055 } },
    { { 0xFFFFFF, 0xAAFFFF, 6 }, 13, { 0xFFAA55, 0xFFFFAA, 8 },
      { { 0x55AAAA, 0xAAFFFF, 6 }, { 0x0055AA, 0x55AAAA, 6 },
        { 0x005555, 0x0055AA, 6 }, { 0x000055, 0x005555, 5 } } } },
  // 4 Sunset: Apricot-Himmel, Koralle bis Nachtblau (Schwarz verschwimmt
  // auf Gabbro mit dem Rand)
  { { 0xFFAA55, 0xFFFFAA, 0x550000, { 0xFF5555, 0xAA5555, 0x550000, 0x000055 } },
    { { 0xFFAA55, 0xFFAAAA, 9 }, 3, { 0xFFFFAA, 0xFFFFFF, 8 },
      { { 0xFF5555, 0xFFAAAA, 6 }, { 0xAA5555, 0xFF5555, 4 },
        { 0x550000, 0xAA5555, 5 }, { 0x000055, 0x550055, 6 } } } },
  // 5 Candy: Weiss, Pastellrosa, Lavendel, Himmelblau
  { { 0xFFFFFF, 0xFFAA55, 0x5555AA, { 0xFFAAFF, 0xAAAAFF, 0x55AAFF, 0x5555AA } },
    { { 0xFFFFFF, 0xFFAAFF, 1 }, 6, { 0xFFAA55, 0xFFAAAA, 8 },
      { { 0xFFAAFF, 0xFFFFFF, 3 }, { 0xAAAAFF, 0xAA55FF, 4 },
        { 0x55AAFF, 0xAAAAFF, 4 }, { 0x5555AA, 0x0055AA, 6 } } } },
};
#define WORLD_COUNT        ((int)ARRAY_LENGTH(WORLDS))
#define WORLD_SURPRISE     99     // jede Szene eine zufaellige Welt

#define TEXTURE_FLAT       0
#define TEXTURE_PRINT      1

// ---- Layout ----------------------------------------------------------------
// Roboto Light 56, nur Ziffern und Doppelpunkt (Bitham 42 Light war zu klein)
#define TIME_FONT_ID       RESOURCE_ID_ROBOTO_LIGHT_56
#define TIME_HEIGHT        62
#ifdef PBL_PLATFORM_EMERY
#define TIME_Y             -6
#define SUN_R_MIN          9
#define SUN_R_MAX          15
#define SIDE_MARGIN        8      // Sonne bleibt so weit vom Rand weg
#else
#define TIME_Y             12     // rund: oben ist der Kreis schmal
#define SUN_R_MIN          11
#define SUN_R_MAX          18
#define SIDE_MARGIN        40
#endif
#define TIME_INK_GAP       6      // Abstand Uhrzeit -> oberste Sonnenposition

// Bergketten, Angaben in Promille der Displayhoehe
#define RIDGE_BASE_BACK    520    // Grundlinie der hintersten Kette
#define RIDGE_BASE_FRONT   880    // Grundlinie der vordersten Kette
#define RIDGE_AMP_BACK     170    // Hoehe der Gipfel ueber der Grundlinie
#define RIDGE_AMP_FRONT    110
#define VALLEY_CHANCE      30     // Prozent der Szenen mit Tal und Fluss
#define SKY_GRADIENT_END   600    // Print: Himmelsverlauf endet hier

// ---- Zeit ------------------------------------------------------------------
#define DAY_FROM_HOUR      7      // Sonne von 7 bis 18:59, sonst Mond
#define DAY_TO_HOUR        19

// ---- AppMessage ------------------------------------------------------------
#define INBOX_SIZE         256
#define OUTBOX_SIZE        64

// ---- Einstellungen ---------------------------------------------------------
// Byte-Dump im Persistenzspeicher: neue Felder NUR am Ende anhaengen.
#define SETTINGS_KEY       1
#define SETTINGS_VERSION   1

typedef struct {
  uint8_t version;
  uint8_t interval;   // Minuten je Szene: 1, 15 oder 60
  uint8_t world;      // Index in WORLDS oder WORLD_SURPRISE
  bool    shake;      // Schuetteln zeigt eine neue Szene
  uint8_t texture;    // TEXTURE_FLAT oder TEXTURE_PRINT (seit 1.1.0)
} Settings;

static Window *s_window;
static Layer *s_canvas;
static GFont s_time_font;
static Settings s_settings;
static uint32_t s_shakes;     // Anzahl Schuettler, verschiebt den Samen
static uint32_t s_rng;

// ---- Zufall ----------------------------------------------------------------
static void rng_seed(uint32_t seed) {
  // Samen streuen, damit benachbarte Zeitschlitze nicht aehnlich aussehen
  seed ^= seed >> 16; seed *= 0x7feb352d;
  seed ^= seed >> 15; seed *= 0x846ca68b;
  seed ^= seed >> 16;
  s_rng = seed ? seed : 0x9e3779b9;
}

static uint32_t rnd(void) {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return s_rng;
}

// gleichverteilt in [lo, hi]
static int rnd_range(int lo, int hi) {
  if (hi <= lo) return lo;
  return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

// ---- Farbe und Framebuffer -------------------------------------------------
// Eine Flaeche ist entweder eine feste Palettenfarbe (flat) oder ein
// Bayer-Raster aus zwei Palettenfarben; der Mischanteil kann sich von Zeile
// ya (lv_a) bis Zeile yb (lv_b) aendern (Himmelsverlauf).
typedef struct {
  bool dither;
  uint8_t flat;       // argb-Byte fuer den Stil Flat
  uint8_t ca, cb;     // argb-Bytes der beiden Rasterfarben
  uint8_t lv_a, lv_b; // Anteil von cb in Sechzehnteln
  int16_t ya, yb;
} Paint;

static const uint8_t BAYER4[4][4] = {
  {  0,  8,  2, 10 },
  { 12,  4, 14,  6 },
  {  3, 11,  1,  9 },
  { 15,  7, 13,  5 },
};

#define FB_ROWS_MAX        260
static GBitmapDataRowInfo s_rows[FB_ROWS_MAX];
static int s_fb_w, s_fb_h;

static Paint paint_solid(uint32_t flat_hex, Mix soft) {
  Paint p = {
    .dither = s_settings.texture == TEXTURE_PRINT,
    .flat = GColorFromHEX(flat_hex).argb,
    .ca = GColorFromHEX(soft.a).argb, .cb = GColorFromHEX(soft.b).argb,
    .lv_a = soft.lv, .lv_b = soft.lv,
    .ya = 0, .yb = 1,
  };
  return p;
}

// Mischanteil einer Zeile in 1/256 (Verlauf zwischen lv_a und lv_b)
static int paint_level(const Paint *p, int y) {
  if (y <= p->ya) return p->lv_a * 16;
  if (y >= p->yb) return p->lv_b * 16;
  return p->lv_a * 16 + (p->lv_b - p->lv_a) * 16 * (y - p->ya) / (p->yb - p->ya);
}

// Rasterpunkt: cb, wenn die Bayer-Schwelle unter dem Mischanteil liegt
static inline uint8_t paint_px(const Paint *p, int level, int x, int y) {
  return (BAYER4[y & 3][x & 3] * 16 + 8 < level) ? p->cb : p->ca;
}

// waagerechte Strecke [x0, x1) in Zeile y, auf das sichtbare Display begrenzt
static void span(int y, int x0, int x1, const Paint *p) {
  if (y < 0 || y >= s_fb_h) return;
  GBitmapDataRowInfo *ri = &s_rows[y];
  if (x0 < ri->min_x) x0 = ri->min_x;
  if (x1 > ri->max_x + 1) x1 = ri->max_x + 1;
  if (x0 >= x1) return;
  if (!p->dither) {
    memset(ri->data + x0, p->flat, x1 - x0);
    return;
  }
  int level = paint_level(p, y);
  uint8_t pat[4];
  for (int i = 0; i < 4; i++) pat[i] = paint_px(p, level, i, y);
  for (int x = x0; x < x1; x++) ri->data[x] = pat[x & 3];
}

// senkrechte Strecke von y0 bis zum unteren Rand in Spalte x
static void column(int x, int y0, const Paint *p) {
  if (x < 0 || x >= s_fb_w) return;
  if (y0 < 0) y0 = 0;
  for (int y = y0; y < s_fb_h; y++) {
    GBitmapDataRowInfo *ri = &s_rows[y];
    if (x < ri->min_x || x > ri->max_x) continue;
    ri->data[x] = p->dither ? paint_px(p, paint_level(p, y), x, y) : p->flat;
  }
}

static int isqrt(int n) {
  if (n <= 0) return 0;
  int x = n, y = (x + 1) / 2;
  while (y < x) { x = y; y = (x + n / x) / 2; }
  return x;
}

static void disc(int cx, int cy, int r, const Paint *p) {
  for (int dy = -r; dy <= r; dy++) {
    int hw = isqrt(r * r - dy * dy);
    span(cy + dy, cx - hw, cx + hw + 1, p);
  }
}

// ---- Kurven ----------------------------------------------------------------
// Weicher Uebergang 0..1 (als 0..65535): (1 - cos(pi * t)) / 2
static int32_t ease(int32_t num, int32_t den) {
  if (den <= 0 || num >= den) return TRIG_MAX_RATIO;
  if (num <= 0) return 0;
  int32_t angle = (TRIG_MAX_ANGLE / 2) * num / den;
  return (TRIG_MAX_RATIO - cos_lookup(angle)) / 2;
}

// Feine Unruhe der Kammlinie: Sinus mit zufaelliger Frequenz und Phase
typedef struct {
  int32_t freq, phase, amp;
} Wiggle;

static Wiggle wiggle_new(int amp_max) {
  Wiggle w = {
    .freq = rnd_range(TRIG_MAX_ANGLE / 70, TRIG_MAX_ANGLE / 28),
    .phase = rnd_range(0, TRIG_MAX_ANGLE),
    .amp = rnd_range(1, amp_max),
  };
  return w;
}

static int wiggle_at(const Wiggle *w, int x) {
  return (int)(sin_lookup(w->phase + w->freq * x) * w->amp / TRIG_MAX_RATIO);
}

// ---- Bergkette -------------------------------------------------------------
// Stuetzpunkte ueber die Breite, dazwischen weich (Huegel) oder gerade
// (Gipfel) interpoliert, darueber die Unruhe.
#define RIDGE_POINTS_MAX   8

static void draw_ridge(GRect b, int base, int amp, const Paint *paint) {
  int n = rnd_range(3, 6);
  bool sharp = rnd_range(0, 99) < 45;
  int px[RIDGE_POINTS_MAX];
  int py[RIDGE_POINTS_MAX];
  int w = b.size.w;
  int parity = (int)(rnd() & 1);
  for (int i = 0; i < n; i++) {
    // Punkte reichen links und rechts etwas ueber den Rand hinaus
    int slot = (w + 40) / (n - 1);
    px[i] = -20 + i * slot + (i > 0 && i < n - 1 ? rnd_range(-slot / 3, slot / 3) : 0);
    // Gipfel und Saettel wechseln sich ab
    bool peak = (i % 2) == parity;
    int lo = peak ? amp * 55 / 100 : 0;
    int hi = peak ? amp : amp * 45 / 100;
    py[i] = base - rnd_range(lo, hi);
  }
  Wiggle wg = wiggle_new(sharp ? 2 : 3);

  int seg = 0;
  for (int x = 0; x < w; x++) {
    while (seg < n - 2 && x > px[seg + 1]) seg++;
    int x0 = px[seg], x1 = px[seg + 1];
    int32_t f = sharp ? (int32_t)(x - x0) * TRIG_MAX_RATIO / (x1 - x0)
                      : ease(x - x0, x1 - x0);
    if (f < 0) f = 0;
    if (f > TRIG_MAX_RATIO) f = TRIG_MAX_RATIO;
    int y = py[seg] + (int)((py[seg + 1] - py[seg]) * f / TRIG_MAX_RATIO);
    column(x, y + wiggle_at(&wg, x), paint);
  }
}

// Einzelner Berg mit Gipfel (px, py); die Flanken fallen weich bis
// (left_x, left_y) bzw. (right_x, right_y). Fuer das Tal.
static void draw_mount(GRect b, int px, int py, int left_x, int left_y,
                       int right_x, int right_y, const Paint *paint) {
  Wiggle wg = wiggle_new(2);
  int from = left_x < 0 ? 0 : left_x;
  int to = right_x > b.size.w ? b.size.w : right_x;
  for (int x = from; x < to; x++) {
    int y;
    if (x <= px) {
      y = py + (int)((left_y - py) * ease(px - x, px - left_x) / TRIG_MAX_RATIO);
    } else {
      y = py + (int)((right_y - py) * ease(x - px, right_x - px) / TRIG_MAX_RATIO);
    }
    column(x, y + wiggle_at(&wg, x), paint);
  }
}

// ---- Himmelskoerper --------------------------------------------------------
static void draw_sky_body(GRect b, const Paint *sun, const Paint *sky, bool day,
                          int top, int bottom) {
  int r = rnd_range(SUN_R_MIN, SUN_R_MAX);
  int x = rnd_range(SIDE_MARGIN + r, b.size.w - SIDE_MARGIN - r);
  int y = rnd_range(top + r, bottom);
  disc(x, y, r, sun);
  if (!day) {
    // Mondsichel: versetzter Kreis in Himmelsfarbe frisst die Scheibe an
    int dir = (rnd() & 1) ? 1 : -1;
    disc(x + dir * r * 2 / 5, y - r / 4, r * 9 / 10, sky);
  }
}

// ---- Fluss -----------------------------------------------------------------
// Windet sich von (x_top, y_top) bis zum unteren Rand bei x_bottom und wird
// dabei breiter (Perspektive).
static void draw_river(GRect b, int x_top, int y_top, int x_bottom,
                       int width_bottom, const Paint *paint) {
  int32_t phase = rnd_range(0, TRIG_MAX_ANGLE);
  int bends = rnd_range(2, 3);
  int amp = rnd_range(b.size.w / 12, b.size.w / 7);
  int sp = b.size.h - y_top;
  for (int y = y_top; y < b.size.h; y++) {
    int d = y - y_top;
    int cx = x_top + (x_bottom - x_top) * d / sp;
    // Ausschlag waechst nach vorn, oben bleibt der Fluss schmal und ruhig
    int32_t a = phase + (int32_t)bends * (TRIG_MAX_ANGLE / 2) * d / sp;
    cx += (int)(sin_lookup(a) * amp / TRIG_MAX_RATIO) * d / sp;
    int hw = 1 + width_bottom * d / sp / 2;
    span(y, cx - hw, cx + hw, paint);
  }
}

// ---- Szene -----------------------------------------------------------------
static int permille(int h, int pm) {
  return h * pm / 1000;
}

static Paint layer_paint(const World *wd, int i) {
  return paint_solid(wd->flat.layer[i], wd->soft.layer[i]);
}

static void draw_ridges(GRect b, const World *wd) {
  // drei oder vier Ketten; bei drei faellt eine der hinteren Farben weg
  int n = rnd_range(3, 4);
  int skip = (n == 3) ? rnd_range(0, 2) : -1;
  int k = 0;
  for (int i = 0; i < 4; i++) {
    if (i == skip) continue;
    int base = permille(b.size.h, RIDGE_BASE_BACK
                + (RIDGE_BASE_FRONT - RIDGE_BASE_BACK) * k / (n - 1));
    int amp = permille(b.size.h, RIDGE_AMP_BACK
                + (RIDGE_AMP_FRONT - RIDGE_AMP_BACK) * k / (n - 1));
    amp = amp * rnd_range(65, 100) / 100;
    Paint p = layer_paint(wd, i);
    draw_ridge(b, base + rnd_range(-6, 6), amp, &p);
    k++;
  }
}

static void draw_valley(GRect b, const World *wd, const Paint *sky) {
  int w = b.size.w, h = b.size.h;
  // ferne Kette hinter der Ebene
  int ground = permille(h, rnd_range(600, 660));
  Paint p0 = layer_paint(wd, 0);
  draw_ridge(b, ground, permille(h, rnd_range(150, 220)), &p0);

  // Ebene in der zweiten Farbe
  Paint p1 = layer_paint(wd, 1);
  for (int y = ground; y < h; y++) span(y, 0, w, &p1);

  // Die beiden Berge im Vordergrund lassen unten eine Luecke fuer den Fluss
  bool flip = rnd() & 1;
  int gap_l = w * rnd_range(38, 48) / 100;
  int gap_r = w * rnd_range(52, 62) / 100;
  int river_x = (gap_l + gap_r) / 2;
  // Der Fluss spiegelt den Himmel in dessen hellster Farbe
  Paint water = *sky;
  water.lv_b = water.lv_a;
  draw_river(b, w * rnd_range(35, 65) / 100, ground, river_x,
             (gap_r - gap_l) * rnd_range(45, 70) / 100, &water);

  // hoher Berg auf der einen, niedrigerer auf der anderen Seite
  int tall = permille(h, rnd_range(380, 470));
  int low = permille(h, rnd_range(520, 600));
  int tall_y = flip ? low : tall;
  int low_y = flip ? tall : low;
  // links: Gipfel im linken Drittel, rechte Flanke endet unten an der Luecke
  int lp = w * rnd_range(8, 30) / 100;
  Paint p3 = layer_paint(wd, 3);
  draw_mount(b, lp, tall_y, -w / 3, tall_y + permille(h, 120), gap_l, h, &p3);
  // rechts: Gipfel im rechten Drittel, linke Flanke endet unten an der Luecke
  int rp = w * rnd_range(70, 92) / 100;
  Paint p2 = layer_paint(wd, 2);
  draw_mount(b, rp, low_y, gap_r, h, w + w / 3, low_y + permille(h, 120), &p2);
}

static uint32_t scene_seed(time_t now) {
  uint32_t secs = (uint32_t)s_settings.interval * 60;
  return (uint32_t)(now / secs) * 2654435761u + s_shakes * 40503u;
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  bool day = t->tm_hour >= DAY_FROM_HOUR && t->tm_hour < DAY_TO_HOUR;

  rng_seed(scene_seed(now));
  int wi = s_settings.world < WORLD_COUNT ? s_settings.world : rnd_range(0, WORLD_COUNT - 1);
  const World *wd = &WORLDS[wi];

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (fb) {
    GRect fbb = gbitmap_get_bounds(fb);
    s_fb_w = fbb.size.w;
    s_fb_h = fbb.size.h < FB_ROWS_MAX ? fbb.size.h : FB_ROWS_MAX;
    for (int y = 0; y < s_fb_h; y++) s_rows[y] = gbitmap_get_data_row_info(fb, y);

    // Himmel: bei Print ein Verlauf von oben bis knapp unter den Horizont
    Paint sky = paint_solid(wd->flat.sky, wd->soft.sky);
    sky.lv_b = wd->soft.sky_lv_bottom;
    sky.yb = permille(b.size.h, SKY_GRADIENT_END);
    for (int y = 0; y < s_fb_h; y++) span(y, 0, s_fb_w, &sky);

    // Sonne/Mond unterhalb der Uhrzeit; darf hinter den Bergen versinken
    Paint sun = paint_solid(wd->flat.sun, wd->soft.sun);
    int sky_top = TIME_Y + TIME_HEIGHT + TIME_INK_GAP;
    draw_sky_body(b, &sun, &sky, day, sky_top, permille(b.size.h, 470));

    if (rnd_range(0, 99) < VALLEY_CHANCE) {
      draw_valley(b, wd, &sky);
    } else {
      draw_ridges(b, wd);
    }
    graphics_release_frame_buffer(ctx, fb);
  }

  static char time_buf[8];
  clock_copy_time_string(time_buf, sizeof(time_buf));
  graphics_context_set_text_color(ctx, GColorFromHEX(wd->flat.ink));
  graphics_draw_text(ctx, time_buf, s_time_font,
                     GRect(0, TIME_Y, b.size.w, TIME_HEIGHT),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// ---- Einstellungen ---------------------------------------------------------
static void settings_defaults(void) {
  s_settings.version = SETTINGS_VERSION;
  s_settings.interval = 60;
  s_settings.world = 0;
  s_settings.shake = true;
  s_settings.texture = TEXTURE_FLAT;
}

static void settings_fix(void) {
  if (s_settings.interval != 1 && s_settings.interval != 15 && s_settings.interval != 60) {
    s_settings.interval = 60;
  }
  if (s_settings.world >= WORLD_COUNT && s_settings.world != WORLD_SURPRISE) {
    s_settings.world = 0;
  }
  if (s_settings.texture > TEXTURE_PRINT) s_settings.texture = TEXTURE_FLAT;
}

static void settings_load(void) {
  settings_defaults();
  if (persist_exists(SETTINGS_KEY)) {
    persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
  }
  settings_fix();
}

static void settings_save(void) {
  s_settings.version = SETTINGS_VERSION;
  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

// ---- Ereignisse ------------------------------------------------------------
static void tick_handler(struct tm *tick_time, TimeUnits changed) {
  layer_mark_dirty(s_canvas);
}

static void tap_handler(AccelAxisType axis, int32_t direction) {
  s_shakes++;
  layer_mark_dirty(s_canvas);
}

static void shake_apply(void) {
  accel_tap_service_unsubscribe();
  if (s_settings.shake) accel_tap_service_subscribe(tap_handler);
}

// Clay schickt Auswahlfelder als Text, Schalter als Zahl
static int tuple_int(Tuple *t) {
  return (t->type == TUPLE_CSTRING) ? atoi(t->value->cstring) : (int)t->value->int32;
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;
  if ((t = dict_find(iter, MESSAGE_KEY_INTERVAL))) s_settings.interval = tuple_int(t);
  if ((t = dict_find(iter, MESSAGE_KEY_WORLD)))    s_settings.world = tuple_int(t);
  if ((t = dict_find(iter, MESSAGE_KEY_SHAKE)))    s_settings.shake = tuple_int(t) != 0;
  if ((t = dict_find(iter, MESSAGE_KEY_TEXTURE)))  s_settings.texture = tuple_int(t);
  settings_fix();
  settings_save();
  shake_apply();
  layer_mark_dirty(s_canvas);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
}

static void init(void) {
  settings_load();
  s_time_font = fonts_load_custom_font(resource_get_handle(TIME_FONT_ID));

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  shake_apply();

  app_message_register_inbox_received(inbox_received);
  AppMessageResult r = app_message_open(INBOX_SIZE, OUTBOX_SIZE);
  if (r != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "app_message_open: %d", (int)r);
  }
}

static void deinit(void) {
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
  fonts_unload_custom_font(s_time_font);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
