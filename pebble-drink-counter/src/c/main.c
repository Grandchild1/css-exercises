/*
 * Tally - an alcoholic drink counter for the Pebble Time 2 (emery, 200x228).
 *
 * The main screen shows three stacked bars - today, the last 7 days and the
 * last 30 days - each filling towards its limit and recolouring as the limits
 * are passed. Both the 7 and 30 day figures are rolling windows ending today,
 * so they never reset at a week or month boundary.
 */

#include <pebble.h>

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

// Drinks are stored in tenths of a standard drink so no floating point maths is
// needed anywhere on the watch.
#define TENTHS_PER_DRINK 10

// Rolling history. 30 days of window plus slack for the day-rollover pass.
#define HISTORY_DAYS 40
#define HISTORY_SHOWN 14

#define WEEK_DAYS 7
#define MONTH_DAYS 30

#define DAY_SECONDS 86400

#define PERSIST_KEY_DATA 1
#define DATA_VERSION 1

#define DEFAULT_DAY_LIMIT (2 * TENTHS_PER_DRINK)
#define DEFAULT_HEAVY_DAY (5 * TENTHS_PER_DRINK)
#define DEFAULT_WEEK_LIMIT (14 * TENTHS_PER_DRINK)
#define DEFAULT_MONTH_LIMIT (60 * TENTHS_PER_DRINK)

typedef struct {
  uint32_t version;
  int32_t last_day;                 // day index the buffers were last rolled to
  int16_t tenths[HISTORY_DAYS];     // drinks per day, indexed by day % HISTORY_DAYS
  uint8_t dry[HISTORY_DAYS];        // 1 when the day was explicitly marked dry
  int16_t limit_day;
  int16_t heavy_day;                // "five or more" threshold
  int16_t limit_week;
  int16_t limit_month;
} DrinkData;

static DrinkData s_data;

// ---------------------------------------------------------------------------
// Layout and palette
// ---------------------------------------------------------------------------

#define ROW_COUNT 3
#define ROW_TOP 8
#define ROW_HEIGHT 62
#define ROW_GAP 6
#define LABEL_WIDTH 26
#define TEXT_LEFT_PAD 8
#define TEXT_RIGHT_PAD 6
#define TEXT_BASELINE_PAD 6
#define TEXT_GAP 5
#define FOOTER_HEIGHT 18

#define COLOR_BG GColorWhite
#define COLOR_TEXT GColorBlack
#define COLOR_FOOTER GColorDarkGray
#define COLOR_OK PBL_IF_COLOR_ELSE(GColorPictonBlue, GColorLightGray)
#define COLOR_OVER PBL_IF_COLOR_ELSE(GColorVividViolet, GColorDarkGray)
#define COLOR_ALERT PBL_IF_COLOR_ELSE(GColorRed, GColorBlack)
#define COLOR_DRY PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorDarkGray)

// Largest first: each row picks the biggest pair its numbers actually fit in.
static const char *const BIG_FONT_KEYS[] = {
  FONT_KEY_BITHAM_42_BOLD,
  FONT_KEY_BITHAM_30_BLACK,
  FONT_KEY_GOTHIC_28_BOLD,
  FONT_KEY_GOTHIC_24_BOLD,
  FONT_KEY_GOTHIC_18_BOLD,
};
#define BIG_FONT_COUNT (int)(sizeof(BIG_FONT_KEYS) / sizeof(BIG_FONT_KEYS[0]))

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

static Window *s_main_window;
static Layer *s_stats_layer;

static Window *s_menu_window;
static MenuLayer *s_menu_layer;

static Window *s_history_window;
static MenuLayer *s_history_layer;

static Window *s_limits_window;
static MenuLayer *s_limits_layer;

static NumberWindow *s_number_window;
static int s_editing_limit;

static GBitmap *s_label_bitmaps[ROW_COUNT];

// ---------------------------------------------------------------------------
// Storage helpers
// ---------------------------------------------------------------------------

static int slot_for_day(int32_t day) {
  int32_t index = day % HISTORY_DAYS;
  if (index < 0) {
    index += HISTORY_DAYS;
  }
  return (int)index;
}

static int32_t today_index(void) {
  // time_start_of_today() is local midnight expressed as a UTC timestamp, so
  // dividing gives a stable, monotonic index for the user's local day.
  return (int32_t)(time_start_of_today() / DAY_SECONDS);
}

static void data_reset(void) {
  memset(&s_data, 0, sizeof(s_data));
  s_data.version = DATA_VERSION;
  s_data.last_day = today_index();
  s_data.limit_day = DEFAULT_DAY_LIMIT;
  s_data.heavy_day = DEFAULT_HEAVY_DAY;
  s_data.limit_week = DEFAULT_WEEK_LIMIT;
  s_data.limit_month = DEFAULT_MONTH_LIMIT;
}

// Clears the slots for every day that has elapsed since the last run, so a day
// that is 40 days old cannot be mistaken for today.
static void roll_to_today(void) {
  int32_t today = today_index();
  if (today == s_data.last_day) {
    return;
  }
  if (today < s_data.last_day) {
    // Clock moved backwards; keep the stored days and re-anchor on today.
    s_data.last_day = today;
    return;
  }
  if (today - s_data.last_day >= HISTORY_DAYS) {
    memset(s_data.tenths, 0, sizeof(s_data.tenths));
    memset(s_data.dry, 0, sizeof(s_data.dry));
  } else {
    for (int32_t day = s_data.last_day + 1; day <= today; day++) {
      int index = slot_for_day(day);
      s_data.tenths[index] = 0;
      s_data.dry[index] = 0;
    }
  }
  s_data.last_day = today;
}

static void data_load(void) {
  if (persist_exists(PERSIST_KEY_DATA)) {
    DrinkData stored;
    int read = persist_read_data(PERSIST_KEY_DATA, &stored, sizeof(stored));
    if (read == (int)sizeof(stored) && stored.version == DATA_VERSION) {
      s_data = stored;
      roll_to_today();
      return;
    }
  }
  data_reset();
}

static void data_save(void) {
  persist_write_data(PERSIST_KEY_DATA, &s_data, sizeof(s_data));
}

// ---------------------------------------------------------------------------
// Totals
// ---------------------------------------------------------------------------

static int32_t sum_last_days(int days) {
  int32_t total = 0;
  for (int back = 0; back < days && back < HISTORY_DAYS; back++) {
    total += s_data.tenths[slot_for_day(s_data.last_day - back)];
  }
  return total;
}

static int32_t today_tenths(void) {
  return s_data.tenths[slot_for_day(s_data.last_day)];
}

static bool today_is_dry(void) {
  return s_data.dry[slot_for_day(s_data.last_day)] != 0;
}

static void add_tenths(int32_t delta) {
  int index = slot_for_day(s_data.last_day);
  int32_t before = s_data.tenths[index];
  int32_t after = before + delta;
  if (after < 0) {
    after = 0;
  }
  if (after > INT16_MAX) {
    after = INT16_MAX;
  }
  s_data.tenths[index] = (int16_t)after;
  if (after > 0) {
    // Recording a drink is what clears a dry day.
    s_data.dry[index] = 0;
  }
  data_save();

  if (delta > 0) {
    if (before < s_data.heavy_day && after >= s_data.heavy_day) {
      vibes_double_pulse();
    } else if (before <= s_data.limit_day && after > s_data.limit_day) {
      vibes_short_pulse();
    }
  }
}

static void set_dry_day(bool dry) {
  int index = slot_for_day(s_data.last_day);
  s_data.dry[index] = dry ? 1 : 0;
  if (dry) {
    s_data.tenths[index] = 0;
  }
  data_save();
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

static void format_tenths(char *buffer, size_t size, int32_t tenths, bool force_decimal) {
  int whole = (int)(tenths / TENTHS_PER_DRINK);
  int frac = (int)(tenths % TENTHS_PER_DRINK);
  if (force_decimal || frac != 0) {
    snprintf(buffer, size, "%d.%d", whole, frac);
  } else {
    snprintf(buffer, size, "%d", whole);
  }
}

static void format_day(char *buffer, size_t size, int32_t day) {
  time_t when = (time_t)day * DAY_SECONDS + (DAY_SECONDS / 2);
  struct tm *parts = localtime(&when);
  strftime(buffer, size, "%a %d %b", parts);
}

// ---------------------------------------------------------------------------
// Row colours - the rules the user asked for
// ---------------------------------------------------------------------------

static GColor day_color(void) {
  if (today_is_dry()) {
    return COLOR_DRY;
  }
  int32_t today = today_tenths();
  if (today >= s_data.heavy_day) {
    return COLOR_ALERT;
  }
  if (today > s_data.limit_day) {
    return COLOR_OVER;
  }
  return COLOR_OK;
}

static GColor week_color(void) {
  if (sum_last_days(WEEK_DAYS) > s_data.limit_week) {
    return COLOR_ALERT;
  }
  // A heavy day pushes the week into warning territory too.
  if (today_tenths() >= s_data.heavy_day) {
    return COLOR_OVER;
  }
  return COLOR_OK;
}

static GColor month_color(void) {
  if (sum_last_days(MONTH_DAYS) > s_data.limit_month) {
    return COLOR_ALERT;
  }
  return COLOR_OK;
}

// ---------------------------------------------------------------------------
// Stats screen drawing
// ---------------------------------------------------------------------------

typedef struct {
  int32_t value;
  int32_t limit;
  GColor color;
  bool dry;
  GBitmap *label;
} StatsRow;

static void build_row(int index, StatsRow *row) {
  switch (index) {
    case 0:
      row->value = today_tenths();
      row->limit = s_data.limit_day;
      row->color = day_color();
      row->dry = today_is_dry();
      break;
    case 1:
      row->value = sum_last_days(WEEK_DAYS);
      row->limit = s_data.limit_week;
      row->color = week_color();
      row->dry = false;
      break;
    default:
      row->value = sum_last_days(MONTH_DAYS);
      row->limit = s_data.limit_month;
      row->color = month_color();
      row->dry = false;
      break;
  }
  row->label = s_label_bitmaps[index];
}

// Bar length is the label block plus the share of the remaining width the value
// has used up of its limit, clamped to a full bar once the limit is passed.
static int bar_width(const StatsRow *row, int screen_width) {
  if (row->dry) {
    return screen_width;
  }
  int available = screen_width - LABEL_WIDTH;
  if (row->limit <= 0 || row->value <= 0) {
    return LABEL_WIDTH;
  }
  int32_t filled = (available * row->value) / row->limit;
  if (filled > available) {
    filled = available;
  }
  return LABEL_WIDTH + (int)filled;
}

static GSize measure(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, 1000, 100), GTextOverflowModeWordWrap, GTextAlignmentLeft);
}

// graphics_draw_text positions by the box's top edge, so bottom-aligning
// differently sized fonts means offsetting each by its own measured height.
static void draw_bottom_aligned(GContext *ctx, const char *text, GFont font, int x, int bottom) {
  GSize size = measure(text, font);
  graphics_draw_text(ctx, text, font, GRect(x, bottom - size.h, size.w + 4, size.h + 4),
                     GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
}

static void draw_row(GContext *ctx, GRect bounds, int index) {
  StatsRow row;
  build_row(index, &row);

  const int y = ROW_TOP + index * (ROW_HEIGHT + ROW_GAP);
  const int bottom = y + ROW_HEIGHT - TEXT_BASELINE_PAD;

  // Bar, with the label block forming its left end.
  graphics_context_set_fill_color(ctx, row.color);
  graphics_fill_rect(ctx, GRect(0, y, bar_width(&row, bounds.size.w), ROW_HEIGHT), 0, GCornerNone);

  // Sideways label, pre-rendered white on transparent.
  if (row.label != NULL) {
    GSize label_size = gbitmap_get_bounds(row.label).size;
    GRect target = GRect((LABEL_WIDTH - label_size.w) / 2,
                         y + (ROW_HEIGHT - label_size.h) / 2,
                         label_size.w, label_size.h);
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, row.label, target);
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
  }

  graphics_context_set_text_color(ctx, COLOR_TEXT);

  const int text_x = LABEL_WIDTH + TEXT_LEFT_PAD;
  const int text_room = bounds.size.w - text_x - TEXT_RIGHT_PAD;

  if (row.dry) {
    for (int i = 0; i < BIG_FONT_COUNT; i++) {
      GFont font = fonts_get_system_font(BIG_FONT_KEYS[i]);
      if (measure("DRY DAY", font).w <= text_room || i == BIG_FONT_COUNT - 1) {
        draw_bottom_aligned(ctx, "DRY DAY", font, text_x, bottom);
        break;
      }
    }
    return;
  }

  char value_text[12];
  char limit_text[12];
  format_tenths(value_text, sizeof(value_text), row.value, true);
  format_tenths(limit_text, sizeof(limit_text), row.limit, false);

  GFont small_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  const int of_width = measure("of", small_font).w;

  // Step down through the font sizes until the whole "N.N of M" line fits.
  GFont big_font = fonts_get_system_font(BIG_FONT_KEYS[BIG_FONT_COUNT - 1]);
  int value_width = 0;
  for (int i = 0; i < BIG_FONT_COUNT; i++) {
    GFont candidate = fonts_get_system_font(BIG_FONT_KEYS[i]);
    int candidate_value = measure(value_text, candidate).w;
    int total = candidate_value + TEXT_GAP + of_width + TEXT_GAP + measure(limit_text, candidate).w;
    big_font = candidate;
    value_width = candidate_value;
    if (total <= text_room) {
      break;
    }
  }

  int x = text_x;
  draw_bottom_aligned(ctx, value_text, big_font, x, bottom);
  x += value_width + TEXT_GAP;
  // "of" sits on the same baseline but a few pixels up from the descender line.
  draw_bottom_aligned(ctx, "of", small_font, x, bottom - 4);
  x += of_width + TEXT_GAP;
  draw_bottom_aligned(ctx, limit_text, big_font, x, bottom);
}

static void draw_footer(GContext *ctx, GRect bounds) {
  char date_text[16];
  format_day(date_text, sizeof(date_text), s_data.last_day);

  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  const int y = bounds.size.h - FOOTER_HEIGHT;
  graphics_context_set_text_color(ctx, COLOR_FOOTER);
  graphics_draw_text(ctx, date_text, font, GRect(4, y, bounds.size.w / 2, FOOTER_HEIGHT),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, "SELECT = menu", font,
                     GRect(bounds.size.w / 2 - 4, y, bounds.size.w / 2, FOOTER_HEIGHT),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

static void stats_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, COLOR_BG);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  for (int i = 0; i < ROW_COUNT; i++) {
    draw_row(ctx, bounds, i);
  }
  draw_footer(ctx, bounds);
}

static void refresh(void) {
  if (s_stats_layer != NULL) {
    layer_mark_dirty(s_stats_layer);
  }
}

// ---------------------------------------------------------------------------
// Limits editing
// ---------------------------------------------------------------------------

typedef struct {
  const char *title;
  int16_t *field;
  int min_drinks;
  int max_drinks;
} LimitSpec;

static LimitSpec s_limits[] = {
  {"Daily limit", &s_data.limit_day, 1, 20},
  {"Heavy day", &s_data.heavy_day, 1, 30},
  {"Weekly limit", &s_data.limit_week, 1, 99},
  {"Monthly limit", &s_data.limit_month, 1, 300},
};
#define LIMIT_COUNT (int)(sizeof(s_limits) / sizeof(s_limits[0]))

static void number_selected(struct NumberWindow *window, void *context) {
  int32_t drinks = number_window_get_value(window);
  *s_limits[s_editing_limit].field = (int16_t)(drinks * TENTHS_PER_DRINK);
  data_save();
  window_stack_pop(true);
  if (s_limits_layer != NULL) {
    menu_layer_reload_data(s_limits_layer);
  }
  refresh();
}

// ---------------------------------------------------------------------------
// Limits menu
// ---------------------------------------------------------------------------

static uint16_t limits_num_rows(MenuLayer *menu, uint16_t section, void *context) {
  return LIMIT_COUNT;
}

static void limits_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *context) {
  char subtitle[32];
  char amount[12];
  format_tenths(amount, sizeof(amount), *s_limits[index->row].field, false);
  if (index->row == 1) {
    snprintf(subtitle, sizeof(subtitle), "%s drinks or more", amount);
  } else {
    snprintf(subtitle, sizeof(subtitle), "%s drinks", amount);
  }
  menu_cell_basic_draw(ctx, cell, s_limits[index->row].title, subtitle, NULL);
}

static void limits_select(MenuLayer *menu, MenuIndex *index, void *context) {
  s_editing_limit = index->row;
  const LimitSpec *spec = &s_limits[s_editing_limit];
  number_window_set_label(s_number_window, spec->title);
  number_window_set_min(s_number_window, spec->min_drinks);
  number_window_set_max(s_number_window, spec->max_drinks);
  number_window_set_step_size(s_number_window, 1);
  number_window_set_value(s_number_window, *spec->field / TENTHS_PER_DRINK);
  window_stack_push(number_window_get_window(s_number_window), true);
}

static void limits_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_limits_layer = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_limits_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = limits_num_rows,
    .draw_row = limits_draw_row,
    .select_click = limits_select,
  });
  menu_layer_set_click_config_onto_window(s_limits_layer, window);
#ifdef PBL_COLOR
  menu_layer_set_highlight_colors(s_limits_layer, COLOR_OK, GColorBlack);
#endif
  layer_add_child(root, menu_layer_get_layer(s_limits_layer));
}

static void limits_window_unload(Window *window) {
  menu_layer_destroy(s_limits_layer);
  s_limits_layer = NULL;
}

// ---------------------------------------------------------------------------
// History menu
// ---------------------------------------------------------------------------

static uint16_t history_num_rows(MenuLayer *menu, uint16_t section, void *context) {
  return HISTORY_SHOWN;
}

static void history_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *context) {
  int32_t day = s_data.last_day - index->row;
  int slot = slot_for_day(day);

  char title[20];
  if (index->row == 0) {
    snprintf(title, sizeof(title), "Today");
  } else {
    format_day(title, sizeof(title), day);
  }

  char subtitle[24];
  if (s_data.dry[slot]) {
    snprintf(subtitle, sizeof(subtitle), "Dry day");
  } else if (s_data.tenths[slot] == 0) {
    snprintf(subtitle, sizeof(subtitle), "Nothing recorded");
  } else {
    char amount[12];
    format_tenths(amount, sizeof(amount), s_data.tenths[slot], true);
    snprintf(subtitle, sizeof(subtitle), "%s drinks", amount);
  }

  menu_cell_basic_draw(ctx, cell, title, subtitle, NULL);
}

static void history_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_history_layer = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_history_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = history_num_rows,
    .draw_row = history_draw_row,
  });
  menu_layer_set_click_config_onto_window(s_history_layer, window);
#ifdef PBL_COLOR
  menu_layer_set_highlight_colors(s_history_layer, COLOR_OK, GColorBlack);
#endif
  layer_add_child(root, menu_layer_get_layer(s_history_layer));
}

static void history_window_unload(Window *window) {
  menu_layer_destroy(s_history_layer);
  s_history_layer = NULL;
}

// ---------------------------------------------------------------------------
// Main menu
// ---------------------------------------------------------------------------

enum {
  MENU_ADD_ONE = 0,
  MENU_ADD_HALF,
  MENU_REMOVE_HALF,
  MENU_DRY_DAY,
  MENU_HISTORY,
  MENU_LIMITS,
  MENU_ROW_COUNT,
};

static uint16_t menu_num_rows(MenuLayer *menu, uint16_t section, void *context) {
  return MENU_ROW_COUNT;
}

static void menu_draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *context) {
  switch (index->row) {
    case MENU_ADD_ONE:
      menu_cell_basic_draw(ctx, cell, "Add a drink", "+1.0", NULL);
      break;
    case MENU_ADD_HALF:
      menu_cell_basic_draw(ctx, cell, "Add a half", "+0.5", NULL);
      break;
    case MENU_REMOVE_HALF:
      menu_cell_basic_draw(ctx, cell, "Remove a half", "-0.5", NULL);
      break;
    case MENU_DRY_DAY:
      if (today_is_dry()) {
        menu_cell_basic_draw(ctx, cell, "Clear dry day", "Today is marked dry", NULL);
      } else {
        menu_cell_basic_draw(ctx, cell, "Mark a dry day", "Today counts as zero", NULL);
      }
      break;
    case MENU_HISTORY:
      menu_cell_basic_draw(ctx, cell, "History", "Last 14 days", NULL);
      break;
    default:
      menu_cell_basic_draw(ctx, cell, "Limits", "Daily, weekly, monthly", NULL);
      break;
  }
}

static void menu_select(MenuLayer *menu, MenuIndex *index, void *context) {
  switch (index->row) {
    case MENU_ADD_ONE:
      add_tenths(TENTHS_PER_DRINK);
      window_stack_pop(true);
      break;
    case MENU_ADD_HALF:
      add_tenths(TENTHS_PER_DRINK / 2);
      window_stack_pop(true);
      break;
    case MENU_REMOVE_HALF:
      add_tenths(-TENTHS_PER_DRINK / 2);
      window_stack_pop(true);
      break;
    case MENU_DRY_DAY:
      set_dry_day(!today_is_dry());
      window_stack_pop(true);
      break;
    case MENU_HISTORY:
      window_stack_push(s_history_window, true);
      break;
    default:
      window_stack_push(s_limits_window, true);
      break;
  }
  refresh();
}

static void menu_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_menu_layer = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = menu_num_rows,
    .draw_row = menu_draw_row,
    .select_click = menu_select,
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
#ifdef PBL_COLOR
  menu_layer_set_highlight_colors(s_menu_layer, COLOR_OK, GColorBlack);
#endif
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void menu_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  s_menu_layer = NULL;
}

static void menu_window_appear(Window *window) {
  if (s_menu_layer != NULL) {
    menu_layer_reload_data(s_menu_layer);
  }
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------

static void up_click(ClickRecognizerRef recognizer, void *context) {
  add_tenths(TENTHS_PER_DRINK);
  refresh();
}

static void down_click(ClickRecognizerRef recognizer, void *context) {
  add_tenths(-TENTHS_PER_DRINK);
  refresh();
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  window_stack_push(s_menu_window, true);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
}

static void main_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_stats_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_stats_layer, stats_update_proc);
  layer_add_child(root, s_stats_layer);
}

static void main_window_unload(Window *window) {
  layer_destroy(s_stats_layer);
  s_stats_layer = NULL;
}

static void main_window_appear(Window *window) {
  refresh();
}

// Midnight rollover while the app happens to be open.
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  int32_t before = s_data.last_day;
  roll_to_today();
  if (s_data.last_day != before) {
    data_save();
  }
  refresh();
}

// ---------------------------------------------------------------------------
// App glance
// ---------------------------------------------------------------------------

static void glance_reload(AppGlanceReloadSession *session, size_t limit, void *context) {
  if (limit < 1) {
    return;
  }
  static char subtitle[40];
  if (today_is_dry()) {
    snprintf(subtitle, sizeof(subtitle), "Dry day");
  } else {
    char value_text[12];
    char limit_text[12];
    format_tenths(value_text, sizeof(value_text), today_tenths(), true);
    format_tenths(limit_text, sizeof(limit_text), s_data.limit_day, false);
    snprintf(subtitle, sizeof(subtitle), "%s of %s today", value_text, limit_text);
  }

  const AppGlanceSlice slice = {
    .layout = {
      .icon = APP_GLANCE_SLICE_DEFAULT_ICON,
      .subtitle_template_string = subtitle,
    },
    .expiration_time = time(NULL) + DAY_SECONDS,
  };
  app_glance_add_slice(session, slice);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void init(void) {
  data_load();

  s_label_bitmaps[0] = gbitmap_create_with_resource(RESOURCE_ID_LABEL_1D);
  s_label_bitmaps[1] = gbitmap_create_with_resource(RESOURCE_ID_LABEL_7D);
  s_label_bitmaps[2] = gbitmap_create_with_resource(RESOURCE_ID_LABEL_30D);

  s_number_window = number_window_create("Limit", (NumberWindowCallbacks){
    .selected = number_selected,
  }, NULL);

  s_menu_window = window_create();
  window_set_window_handlers(s_menu_window, (WindowHandlers){
    .load = menu_window_load,
    .appear = menu_window_appear,
    .unload = menu_window_unload,
  });

  s_history_window = window_create();
  window_set_window_handlers(s_history_window, (WindowHandlers){
    .load = history_window_load,
    .unload = history_window_unload,
  });

  s_limits_window = window_create();
  window_set_window_handlers(s_limits_window, (WindowHandlers){
    .load = limits_window_load,
    .unload = limits_window_unload,
  });

  s_main_window = window_create();
  window_set_background_color(s_main_window, COLOR_BG);
  window_set_click_config_provider(s_main_window, click_config_provider);
  window_set_window_handlers(s_main_window, (WindowHandlers){
    .load = main_window_load,
    .appear = main_window_appear,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);

  tick_timer_service_subscribe(HOUR_UNIT, tick_handler);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();

  window_destroy(s_main_window);
  window_destroy(s_limits_window);
  window_destroy(s_history_window);
  window_destroy(s_menu_window);
  number_window_destroy(s_number_window);

  for (int i = 0; i < ROW_COUNT; i++) {
    if (s_label_bitmaps[i] != NULL) {
      gbitmap_destroy(s_label_bitmaps[i]);
      s_label_bitmaps[i] = NULL;
    }
  }

  app_glance_reload(glance_reload, NULL);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
