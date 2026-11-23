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

#include <pebble.h>

// ---- Farbwelten ------------------------------------------------------------
// Je Welt: Himmel, Sonne/Mond, Schrift, vier Bergfarben von hinten nach vorn.
// Die Pebble-Palette kennt nur 64 Farben (Kanaele 00/55/AA/FF); Gabbro zeigt
// sie zusaetzlich entsaettigt, Gelb wird dort zu Creme.
typedef struct {
  uint32_t sky, sun, ink;
  uint32_t layer[4];
} World;

static const World WORLDS[] = {
  // 0 Desert: Creme, Pfirsich, Rost, Petrol, Nachtblau (wie die Vorlage)
  { 0xFFFFAA, 0xFF5500, 0x550000, { 0xFFAA55, 0xAA5500, 0x005555, 0x000055 } },
  // 1 Dusk: Rosa Himmel, Flieder bis Aubergine
  { 0xFFAAAA, 0xFFFFAA, 0x550055, { 0xAA55AA, 0x5555AA, 0x550055, 0x000055 } },
  // 2 Sage: Creme, Oliv und Waldgruen
  { 0xFFFFAA, 0xFFAA55, 0x005500, { 0xAAAA55, 0x55AA55, 0x555500, 0x005500 } },
  // 3 Ocean: Eisblau, Kadettblau bis Tiefsee
  { 0xAAFFFF, 0xFFFFFF, 0x000055, { 0x55AAAA, 0x0055AA, 0x005555, 0x000055 } },
  // 4 Sunset: Apricot-Himmel, Koralle bis Nachtblau (Schwarz verschwimmt
  // auf Gabbro mit dem Rand)
  { 0xFFAA55, 0xFFFFAA, 0x550000, { 0xFF5555, 0xAA5555, 0x550000, 0x000055 } },
  // 5 Candy: Weiss, Pastellrosa, Lavendel, Himmelblau
  { 0xFFFFFF, 0xFFAA55, 0x5555AA, { 0xFFAAFF, 0xAAAAFF, 0x55AAFF, 0x5555AA } },
};
#define WORLD_COUNT        ((int)ARRAY_LENGTH(WORLDS))
#define WORLD_SURPRISE     99     // jede Szene eine zufaellige Welt

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
#define VALLEY_CHANCE      30    // Prozent der Szenen mit Tal und Fluss

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

static void fill_column(GContext *ctx, int x, int y, int h) {
  if (y < h) graphics_fill_rect(ctx, GRect(x, y, 1, h - y), 0, GCornerNone);
}

// ---- Bergkette -------------------------------------------------------------
// Stuetzpunkte ueber die Breite, dazwischen weich (Huegel) oder gerade
// (Gipfel) interpoliert, darueber die Unruhe.
#define RIDGE_POINTS_MAX   8

static void draw_ridge(GContext *ctx, GRect b, int base, int amp, GColor color) {
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

  graphics_context_set_fill_color(ctx, color);
  int seg = 0;
  for (int x = 0; x < w; x++) {
    while (seg < n - 2 && x > px[seg + 1]) seg++;
    int x0 = px[seg], x1 = px[seg + 1];
    int32_t f = sharp ? (int32_t)(x - x0) * TRIG_MAX_RATIO / (x1 - x0)
                      : ease(x - x0, x1 - x0);
    if (f < 0) f = 0;
    if (f > TRIG_MAX_RATIO) f = TRIG_MAX_RATIO;
    int y = py[seg] + (int)((py[seg + 1] - py[seg]) * f / TRIG_MAX_RATIO);
    fill_column(ctx, x, y + wiggle_at(&wg, x), b.size.h);
  }
}

// Einzelner Berg mit Gipfel (px, py); die Flanken fallen weich bis
// (left_x, left_y) bzw. (right_x, right_y). Fuer das Tal.
static void draw_mount(GContext *ctx, GRect b, int px, int py,
                       int left_x, int left_y, int right_x, int right_y,
                       GColor color) {
  Wiggle wg = wiggle_new(2);
  graphics_context_set_fill_color(ctx, color);
  int from = left_x < 0 ? 0 : left_x;
  int to = right_x > b.size.w ? b.size.w : right_x;
  for (int x = from; x < to; x++) {
    int y;
    if (x <= px) {
      y = py + (int)((left_y - py) * ease(px - x, px - left_x) / TRIG_MAX_RATIO);
    } else {
      y = py + (int)((right_y - py) * ease(x - px, right_x - px) / TRIG_MAX_RATIO);
    }
    fill_column(ctx, x, y + wiggle_at(&wg, x), b.size.h);
  }
}

// ---- Himmelskoerper --------------------------------------------------------
static void draw_sky_body(GContext *ctx, GRect b, const World *wd, bool day,
                          int top, int bottom) {
  int r = rnd_range(SUN_R_MIN, SUN_R_MAX);
  int x = rnd_range(SIDE_MARGIN + r, b.size.w - SIDE_MARGIN - r);
  int y = rnd_range(top + r, bottom);
  GPoint c = GPoint(x, y);
  graphics_context_set_fill_color(ctx, GColorFromHEX(wd->sun));
  graphics_fill_circle(ctx, c, r);
  if (!day) {
    // Mondsichel: versetzter Kreis in Himmelsfarbe frisst die Scheibe an
    int dir = (rnd() & 1) ? 1 : -1;
    graphics_context_set_fill_color(ctx, GColorFromHEX(wd->sky));
    graphics_fill_circle(ctx, GPoint(x + dir * r * 2 / 5, y - r / 4), r * 9 / 10);
  }
}

// ---- Fluss -----------------------------------------------------------------
// Windet sich von (x_top, y_top) bis zum unteren Rand bei x_bottom und wird
// dabei breiter (Perspektive).
static void draw_river(GContext *ctx, GRect b, int x_top, int y_top,
                       int x_bottom, int width_bottom, GColor color) {
  int32_t phase = rnd_range(0, TRIG_MAX_ANGLE);
  int bends = rnd_range(2, 3);
  int amp = rnd_range(b.size.w / 12, b.size.w / 7);
  int span = b.size.h - y_top;
  graphics_context_set_fill_color(ctx, color);
  for (int y = y_top; y < b.size.h; y++) {
    int d = y - y_top;
    int cx = x_top + (x_bottom - x_top) * d / span;
    // Ausschlag waechst nach vorn, oben bleibt der Fluss schmal und ruhig
    int32_t a = phase + (int32_t)bends * (TRIG_MAX_ANGLE / 2) * d / span;
    cx += (int)(sin_lookup(a) * amp / TRIG_MAX_RATIO) * d / span;
    int hw = 1 + width_bottom * d / span / 2;
    graphics_fill_rect(ctx, GRect(cx - hw, y, hw * 2, 1), 0, GCornerNone);
  }
}

// ---- Szene -----------------------------------------------------------------
static int permille(int h, int pm) {
  return h * pm / 1000;
}

static void draw_ridges(GContext *ctx, GRect b, const World *wd) {
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
    draw_ridge(ctx, b, base + rnd_range(-6, 6), amp, GColorFromHEX(wd->layer[i]));
    k++;
  }
}

static void draw_valley(GContext *ctx, GRect b, const World *wd) {
  int w = b.size.w, h = b.size.h;
  // ferne Kette(n) hinter der Ebene
  int ground = permille(h, rnd_range(600, 660));
  draw_ridge(ctx, b, ground, permille(h, rnd_range(150, 220)), GColorFromHEX(wd->layer[0]));

  // Ebene in der zweiten Farbe
  graphics_context_set_fill_color(ctx, GColorFromHEX(wd->layer[1]));
  graphics_fill_rect(ctx, GRect(0, ground, w, h - ground), 0, GCornerNone);

  // Die beiden Berge im Vordergrund lassen unten eine Luecke fuer den Fluss
  bool flip = rnd() & 1;
  int gap_l = w * rnd_range(38, 48) / 100;
  int gap_r = w * rnd_range(52, 62) / 100;
  int river_x = (gap_l + gap_r) / 2;
  draw_river(ctx, b, w * rnd_range(35, 65) / 100, ground, river_x,
             (gap_r - gap_l) * rnd_range(45, 70) / 100, GColorFromHEX(wd->sky));

  // hoher Berg auf der einen, niedrigerer auf der anderen Seite
  int tall = permille(h, rnd_range(380, 470));
  int low = permille(h, rnd_range(520, 600));
  int tall_y = flip ? low : tall;
  int low_y = flip ? tall : low;
  // links: Gipfel im linken Drittel, rechte Flanke endet unten an der Luecke
  int lp = w * rnd_range(8, 30) / 100;
  draw_mount(ctx, b, lp, tall_y, -w / 3, tall_y + permille(h, 120), gap_l, h,
             GColorFromHEX(wd->layer[3]));
  // rechts: Gipfel im rechten Drittel, linke Flanke endet unten an der Luecke
  int rp = w * rnd_range(70, 92) / 100;
  draw_mount(ctx, b, rp, low_y, gap_r, h, w + w / 3, low_y + permille(h, 120),
             GColorFromHEX(wd->layer[2]));
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

  graphics_context_set_antialiased(ctx, false);
  graphics_context_set_fill_color(ctx, GColorFromHEX(wd->sky));
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Sonne/Mond unterhalb der Uhrzeit; darf hinter den Bergen versinken
  int sky_top = TIME_Y + TIME_HEIGHT + TIME_INK_GAP;
  draw_sky_body(ctx, b, wd, day, sky_top, permille(b.size.h, 470));

  if (rnd_range(0, 99) < VALLEY_CHANCE) {
    draw_valley(ctx, b, wd);
  } else {
    draw_ridges(ctx, b, wd);
  }

  static char time_buf[8];
  clock_copy_time_string(time_buf, sizeof(time_buf));
  graphics_context_set_text_color(ctx, GColorFromHEX(wd->ink));
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
}

static void settings_fix(void) {
  if (s_settings.interval != 1 && s_settings.interval != 15 && s_settings.interval != 60) {
    s_settings.interval = 60;
  }
  if (s_settings.world >= WORLD_COUNT && s_settings.world != WORLD_SURPRISE) {
    s_settings.world = 0;
  }
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
