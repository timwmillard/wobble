/**
 * Raven example - pen_test
 * A test bench for a pen or a graphics tablet (Wacom...): draw on the canvas
 * and read what the pen tells.
 *
 *   - the canvas takes the strokes of the pen (Rwidget_set_pen_callback):
 *     the width follows the pressure, the eraser end draws in red;
 *   - "Show samples" puts a dot on each event, to see how dense they are
 *     when the pen moves fast;
 *   - the panel shows the last event (phase, position, pressure, tilt,
 *     rotation, eraser, buttons), the rate of the events, how many arrive
 *     in one frame and the largest gap between two samples of a stroke;
 *   - the graph shows the pressure of the last samples;
 *   - "Log events" prints every event to stdout, with its time.
 */

#include "example.h"

#include <math.h>
#include <stdarg.h>

#define MAX_SAMPLES 100000
#define HISTORY 256

typedef struct Sample {
  float x, y, pressure;
  unsigned char eraser, start;
} Sample;

static struct {
  Rid window, canvas, graph;
  Rid phase, position, pressure, raw, tilt, rotation, eraser, buttons, range;
  Rid rate, per_frame, max_per_frame, stroke, gap, total;
  Rid curve_value;

  Sample *samples;
  int count;
  int drawing;

  /* the last event */
  RPenEvent last;
  int have_last;
  int hovering;

  /* pressure of the last samples, for the graph */
  float history[HISTORY];
  int history_next, history_count;

  /* statistics */
  double rate_start;
  int rate_events;
  float rate_hz;
  int frame_events, last_frame_events, max_frame_events;
  int stroke_samples;
  float stroke_gap;
  unsigned long total_events;

  int show_samples;
  int smooth;
  int log_events;
  double log_start;
} app;

static const char *phase_name(RPenPhase phase) {
  switch (phase) {
  case R_PEN_DOWN:
    return "down";
  case R_PEN_MOVE:
    return "move";
  case R_PEN_UP:
    return "up";
  case R_PEN_HOVER:
    return "hover";
  case R_PEN_LEAVE:
    return "leave";
  }
  return "?";
}

/** Sets the text of a label only when it changes (a label that changes asks for
 * another frame). */
static void set_text(Rid label, const char *format, ...) {
  char text[128];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (strcmp(Rlabel_get_text(label), text) != 0)
    Rlabel_set_text(label, text);
}

static void add_sample(const RPenEvent *event, int start) {
  Sample *s;
  if (app.count >= MAX_SAMPLES)
    return;
  s = &app.samples[app.count++];
  s->x = event->local_x;
  s->y = event->local_y;
  s->pressure = event->pressure;
  s->eraser = (unsigned char)(event->eraser != 0);
  s->start = (unsigned char)start;
  if (!start && app.count > 1) {
    float dx = s->x - s[-1].x, dy = s->y - s[-1].y;
    float gap = sqrtf(dx * dx + dy * dy);
    if (gap > app.stroke_gap)
      app.stroke_gap = gap;
  }
  app.stroke_samples++;
  app.history[app.history_next] = event->pressure;
  app.history_next = (app.history_next + 1) % HISTORY;
  if (app.history_count < HISTORY)
    app.history_count++;
}

static int on_pen(Rid widget, const RPenEvent *event, void *user_data) {
  double now = Rget_time();
  (void)user_data;

  app.last = *event;
  app.have_last = R_TRUE;
  app.total_events++;
  app.frame_events++;
  app.rate_events++;
  if (now - app.rate_start >= 0.5) {
    app.rate_hz = (float)(app.rate_events / (now - app.rate_start));
    app.rate_start = now;
    app.rate_events = 0;
  }

  if (app.log_events) {
    printf("%10.4f %-5s x %8.2f y %8.2f  p %.3f raw %.3f  tilt %6.1f %6.1f  "
           "rot %6.1f  eraser %d  buttons %u\n",
           now - app.log_start, phase_name(event->phase), event->local_x,
           event->local_y, event->pressure, event->raw_pressure, event->tilt_x,
           event->tilt_y, event->rotation, event->eraser, event->buttons);
    fflush(stdout);
  }

  switch (event->phase) {
  case R_PEN_DOWN:
    app.drawing = R_TRUE;
    app.stroke_samples = 0;
    app.stroke_gap = 0.0f;
    add_sample(event, R_TRUE);
    break;
  case R_PEN_MOVE:
    if (app.drawing)
      add_sample(event, R_FALSE);
    break;
  case R_PEN_UP:
    app.drawing = R_FALSE;
    break;
  case R_PEN_HOVER:
    app.hovering = R_TRUE;
    break;
  case R_PEN_LEAVE:
    app.hovering = R_FALSE;
    app.drawing = R_FALSE;
    break;
  }

  Rwidget_invalidate(widget);
  Rwidget_invalidate(app.graph);
  /* takes the stroke: the pen does not act as a mouse on the canvas */
  return R_TRUE;
}

static float sample_width(const Sample *s) {
  return 1.0f + 15.0f * s->pressure;
}

/*
 * The segment from sample i - 1 to sample i as a Catmull-Rom curve through
 * the samples around it (the ends of a stroke repeat their sample), cut in
 * short lines whose width goes smoothly from the pressure of one sample to
 * the other: no corners between the samples, no steps in the width.
 */
static void paint_smooth_segment(RPainter *painter, int i, float r, float g,
                                 float b) {
  const Sample *p1 = &app.samples[i - 1];
  const Sample *p2 = &app.samples[i];
  const Sample *p0 = p1->start ? p1 : p1 - 1;
  const Sample *p3 = (i + 1 < app.count && !p2[1].start) ? p2 + 1 : p2;
  float dx = p2->x - p1->x, dy = p2->y - p1->y;
  int steps = (int)(sqrtf(dx * dx + dy * dy) / 2.0f), k;
  float x = p1->x, y = p1->y, w = sample_width(p1);

  if (steps < 1)
    steps = 1;
  if (steps > 32)
    steps = 32;
  for (k = 1; k <= steps; k++) {
    float t = (float)k / (float)steps, t2 = t * t, t3 = t2 * t;
    float nx =
        0.5f * (2.0f * p1->x + (p2->x - p0->x) * t +
                (2.0f * p0->x - 5.0f * p1->x + 4.0f * p2->x - p3->x) * t2 +
                (3.0f * p1->x - p0->x - 3.0f * p2->x + p3->x) * t3);
    float ny =
        0.5f * (2.0f * p1->y + (p2->y - p0->y) * t +
                (2.0f * p0->y - 5.0f * p1->y + 4.0f * p2->y - p3->y) * t2 +
                (3.0f * p1->y - p0->y - 3.0f * p2->y + p3->y) * t3);
    float nw = sample_width(p1) + (sample_width(p2) - sample_width(p1)) * t;
    Rdraw_line(painter, x, y, nx, ny, 0.5f * (w + nw), r, g, b, 1.0f);
    x = nx;
    y = ny;
    w = nw;
  }
}

static void paint_canvas(Rid widget, RPainter *painter, float width,
                         float height, void *user_data) {
  int i;
  (void)widget;
  (void)user_data;

  Rdraw_rect(painter, 0.0f, 0.0f, width, height, 6.0f, 0.97f, 0.97f, 0.95f,
             1.0f);
  if (app.count == 0)
    Rdraw_text(painter, R_INVALID_ID, 15.0f, 20.0f, 18.0f,
               "Draw here with the pen", 0.55f, 0.55f, 0.55f, 1.0f);

  for (i = 0; i < app.count; i++) {
    const Sample *s = &app.samples[i];
    float r = s->eraser ? 0.85f : 0.12f, g = s->eraser ? 0.2f : 0.14f,
          b = s->eraser ? 0.2f : 0.2f;
    if (s->start || i == 0) {
      Rdraw_circle(painter, s->x, s->y, sample_width(s) * 0.5f, r, g, b, 1.0f);
    } else if (app.smooth) {
      paint_smooth_segment(painter, i, r, g, b);
    } else {
      const Sample *p = s - 1;
      /* the width of a segment is the mean of its ends */
      Rdraw_line(painter, p->x, p->y, s->x, s->y,
                 0.5f * (sample_width(p) + sample_width(s)), r, g, b, 1.0f);
    }
  }

  if (app.show_samples) {
    for (i = 0; i < app.count; i++)
      Rdraw_circle(painter, app.samples[i].x, app.samples[i].y, 1.5f, 0.1f,
                   0.6f, 1.0f, 1.0f);
  }

  /* the pen near the canvas: where it points and where it leans */
  if (app.have_last && (app.hovering || app.drawing) &&
      app.last.target == app.canvas) {
    float x = app.last.local_x, y = app.last.local_y;
    const float pi = 3.14159265f;
    float tx = sinf(app.last.tilt_x * pi / 180.0f) * 60.0f;
    float ty = sinf(app.last.tilt_y * pi / 180.0f) * 60.0f;
    Rdraw_line(painter, x - 10.0f, y, x + 10.0f, y, 1.0f, 0.1f, 0.6f, 1.0f,
               0.8f);
    Rdraw_line(painter, x, y - 10.0f, x, y + 10.0f, 1.0f, 0.1f, 0.6f, 1.0f,
               0.8f);
    if (tx != 0.0f || ty != 0.0f)
      Rdraw_line(painter, x, y, x + tx, y + ty, 2.0f, 1.0f, 0.55f, 0.1f, 0.9f);
  }
}

static void paint_graph(Rid widget, RPainter *painter, float width,
                        float height, void *user_data) {
  int i;
  (void)widget;
  (void)user_data;

  Rdraw_rect(painter, 0.0f, 0.0f, width, height, 4.0f, 0.15f, 0.16f, 0.18f,
             1.0f);
  for (i = 1; i < 4; i++) {
    float y = height * (float)i / 4.0f;
    Rdraw_line(painter, 0.0f, y, width, y, 1.0f, 1.0f, 1.0f, 1.0f, 0.08f);
  }
  for (i = 1; i < app.history_count; i++) {
    int first = (app.history_next - app.history_count + HISTORY) % HISTORY;
    float p0 = app.history[(first + i - 1) % HISTORY],
          p1 = app.history[(first + i) % HISTORY];
    float x0 = width * (float)(i - 1) / (HISTORY - 1),
          x1 = width * (float)i / (HISTORY - 1);
    Rdraw_line(painter, x0, height - 2.0f - p0 * (height - 4.0f), x1,
               height - 2.0f - p1 * (height - 4.0f), 1.5f, 0.3f, 0.8f, 0.5f,
               1.0f);
  }
}

static void measure_graph(Rid widget, float *width, float *height,
                          void *user_data) {
  (void)widget;
  (void)user_data;
  *width = 260.0f;
  *height = 90.0f;
}

/* after each frame: the events of the frame and the panel */
static void on_frame(void) {
  const RPenEvent *e = &app.last;

  app.last_frame_events = app.frame_events;
  if (app.frame_events > app.max_frame_events)
    app.max_frame_events = app.frame_events;
  app.frame_events = 0;
  if (Rget_time() - app.rate_start > 1.0)
    app.rate_hz = 0.0f;

  if (app.have_last) {
    set_text(app.phase, "Phase: %s", phase_name(e->phase));
    set_text(app.position, "Position: %.2f, %.2f", e->local_x, e->local_y);
    set_text(app.pressure, "Pressure: %.3f", e->pressure);
    set_text(app.raw, "Raw pressure: %.3f", e->raw_pressure);
    set_text(app.tilt, "Tilt: %.1f°, %.1f°", e->tilt_x, e->tilt_y);
    set_text(app.rotation, "Rotation: %.1f°", e->rotation);
    set_text(app.eraser, "Eraser: %s", e->eraser ? "yes" : "no");
    set_text(app.buttons, "Buttons:%s%s%s",
             (e->buttons & R_PEN_BUTTON_BARREL) ? " barrel" : "",
             (e->buttons & R_PEN_BUTTON_BARREL2) ? " barrel2" : "",
             e->buttons ? "" : " none");
  }
  set_text(app.range, "In range: %s",
           Rwindow_is_pen_in_range(app.window) ? "yes" : "no");
  set_text(app.rate, "Event rate: %.0f Hz", app.rate_hz);
  set_text(app.per_frame, "Events in last frame: %d", app.last_frame_events);
  set_text(app.max_per_frame, "Most in one frame: %d", app.max_frame_events);
  set_text(app.stroke, "Samples in stroke: %d", app.stroke_samples);
  set_text(app.gap, "Largest gap in stroke: %.1f", app.stroke_gap);
  set_text(app.total, "Total events: %lu%s", app.total_events,
           app.count >= MAX_SAMPLES ? " (canvas full)" : "");
}

static void on_clear(Rid button, void *user_data) {
  (void)button;
  (void)user_data;
  app.count = 0;
  app.history_count = 0;
  app.history_next = 0;
  app.max_frame_events = 0;
  app.stroke_samples = 0;
  app.stroke_gap = 0.0f;
  Rwidget_invalidate(app.canvas);
  Rwidget_invalidate(app.graph);
}

static void on_show_samples(Rid checkbox, int checked, void *user_data) {
  (void)checkbox;
  (void)user_data;
  app.show_samples = checked;
  Rwidget_invalidate(app.canvas);
}

static void on_smooth(Rid checkbox, int checked, void *user_data) {
  (void)checkbox;
  (void)user_data;
  app.smooth = checked;
  Rwidget_invalidate(app.canvas);
}

static void on_log(Rid checkbox, int checked, void *user_data) {
  (void)checkbox;
  (void)user_data;
  app.log_events = checked;
  app.log_start = Rget_time();
}

static void on_curve(Rid slider, float value, void *user_data) {
  RTouchSettings settings;
  (void)slider;
  (void)user_data;
  Rtouch_get_settings(&settings);
  settings.pressure_curve = value;
  Rtouch_set_settings(&settings);
  set_text(app.curve_value, "Pressure curve: %.2f", value);
}

static Rid add_label(Rid parent, const char *text) {
  Rid label = Rwidget_add_child(parent, Rlabel_create(text));
  Rlabel_set_font(label, Rfont_get_monospace());
  return label;
}

static Rid add_heading(Rid parent, const char *text) {
  Rid label = Rwidget_add_child(parent, Rlabel_create(text));
  Rlabel_set_text_size(label, 16.0f);
  Rslot_set_padding(label, 0.0f, 8.0f, 0.0f, 2.0f);
  return label;
}

int main(void) {
  Rid root, panel, row, button, check, slider;

  if (!example_init("pen_test"))
    return 1;
  app.samples = (Sample *)malloc(sizeof(Sample) * MAX_SAMPLES);
  if (!app.samples) {
    Rshutdown();
    return 1;
  }
  app.rate_start = Rget_time();

  app.window = Rwindow_create("Pen test", 1200, 760, R_WINDOW_DEFAULT);

  root = Rwidget_create(R_WIDGET_HBOX);
  Rpanel_set_spacing(root, 12.0f);
  Rwindow_set_content(app.window, root);

  /* the canvas: only the pen draws on it (fingers and the pen as a mouse are
   * left out) */
  app.canvas = Rwidget_add_child(root, Rwidget_create(R_WIDGET_CUSTOM));
  Rslot_set_fill(app.canvas, 1.0f);
  Rslot_set_padding(app.canvas, 12.0f, 12.0f, 0.0f, 12.0f);
  Rcustom_set_paint_callback(app.canvas, paint_canvas, NULL);
  Rwidget_set_pen_callback(app.canvas, on_pen, NULL);
  Rwidget_set_pen_mouse(app.canvas, R_FALSE);
  Rwidget_set_touch_mode(app.canvas, R_TOUCH_MODE_NONE);

  panel = Rwidget_add_child(root, Rwidget_create(R_WIDGET_VBOX));
  Rpanel_set_spacing(panel, 4.0f);
  Rslot_set_padding(panel, 0.0f, 12.0f, 12.0f, 12.0f);
  Rwidget_set_min_size(panel, 280.0f, 0.0f);

  add_heading(panel, "Last event");
  app.phase = add_label(panel, "Phase: -");
  app.position = add_label(panel, "Position: -");
  app.pressure = add_label(panel, "Pressure: -");
  app.raw = add_label(panel, "Raw pressure: -");
  app.tilt = add_label(panel, "Tilt: -");
  app.rotation = add_label(panel, "Rotation: -");
  app.eraser = add_label(panel, "Eraser: -");
  app.buttons = add_label(panel, "Buttons: -");
  app.range = add_label(panel, "In range: -");

  add_heading(panel, "Sampling");
  app.rate = add_label(panel, "Event rate: -");
  app.per_frame = add_label(panel, "Events in last frame: -");
  app.max_per_frame = add_label(panel, "Most in one frame: -");
  app.stroke = add_label(panel, "Samples in stroke: -");
  app.gap = add_label(panel, "Largest gap in stroke: -");
  app.total = add_label(panel, "Total events: 0");

  add_heading(panel, "Pressure");
  app.graph = Rwidget_add_child(panel, Rwidget_create(R_WIDGET_CUSTOM));
  Rcustom_set_measure_callback(app.graph, measure_graph, NULL);
  Rcustom_set_paint_callback(app.graph, paint_graph, NULL);

  app.curve_value = add_label(panel, "Pressure curve: 1.00");
  slider = Rwidget_add_child(panel, Rwidget_create(R_WIDGET_SLIDER));
  Rslider_set_range(slider, 0.25f, 3.0f);
  Rslider_set_value(slider, 1.0f);
  Rslider_set_changed_callback(slider, on_curve, NULL);
  Rwidget_set_tooltip(
      slider,
      "Below 1 softer, above 1 harder (RTouchSettings::pressure_curve)");

  add_heading(panel, "Options");
  app.smooth = R_TRUE;
  check = Rwidget_add_child(panel, Rcheckbox_create("Smooth strokes"));
  Rcheckbox_set_checked(check, R_TRUE);
  Rcheckbox_set_changed_callback(check, on_smooth, NULL);
  check = Rwidget_add_child(panel, Rcheckbox_create("Show samples"));
  Rcheckbox_set_changed_callback(check, on_show_samples, NULL);
  check = Rwidget_add_child(panel, Rcheckbox_create("Log events to stdout"));
  Rcheckbox_set_changed_callback(check, on_log, NULL);

  row = Rwidget_add_child(panel, Rwidget_create(R_WIDGET_HBOX));
  Rslot_set_padding(row, 0.0f, 8.0f, 0.0f, 0.0f);
  button = Rwidget_add_child(row, Rbutton_create("Clear"));
  Rbutton_set_clicked_callback(button, on_clear, NULL);

  example_run(app.window, on_frame, NULL);
  free(app.samples);
  return 0;
}
