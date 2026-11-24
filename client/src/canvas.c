#include "canvas.h"

#include <math.h>

#define SWATCH 28.0f
#define SWATCH_GAP 6.0f
#define PALETTE_HEIGHT 44.0f

static const int brush_sizes[] = {3, 8, 16, 30};
#define BRUSH_COUNT ((int)(sizeof(brush_sizes) / sizeof(brush_sizes[0])))

static struct {
   Rid canvas, palette;
   game *g;
   u64 (*now_ms)(void);
   int color, size_index;
   bool mouse_down;
} c = {.color = 1, .size_index = 1};

static void rgb(u32 hex, float *r, float *g, float *b) {
   *r = (float)((hex >> 16) & 0xFF) / 255.0f;
   *g = (float)((hex >> 8) & 0xFF) / 255.0f;
   *b = (float)(hex & 0xFF) / 255.0f;
}

/* Where the game space sits in the widget. */
typedef struct {
   float scale, x, y;
} fit;

static fit fit_in(float width, float height) {
   float sx = width / GAME_CANVAS_W, sy = height / GAME_CANVAS_H;
   fit f = {sx < sy ? sx : sy, 0, 0};
   f.x = (width - GAME_CANVAS_W * f.scale) * 0.5f;
   f.y = (height - GAME_CANVAS_H * f.scale) * 0.5f;
   return f;
}

static float stroke_width(const game_stroke *s, game_point p, float scale) {
   float w = (float)s->size * scale;
   if (p.pressure != 255)
      w *= 0.3f + 0.7f * (float)p.pressure / 255.0f;
   return w < 1.0f ? 1.0f : w;
}

/* The segment from point k - 1 to point k as a Catmull-Rom curve through the
 * points around it (the ends of a stroke repeat their point), cut into short
 * lines whose width moves smoothly from one point's pressure to the next: no
 * corners at the samples, no steps in the width. */
static void paint_segment(RPainter *painter, const game_stroke *s, usize k,
                          fit f, float r, float g, float b) {
   game_point p0 = s->pts.items[k >= 2 ? k - 2 : k - 1];
   game_point p1 = s->pts.items[k - 1];
   game_point p2 = s->pts.items[k];
   game_point p3 = s->pts.items[k + 1 < s->pts.len ? k + 1 : k];
   float x0 = (float)p0.x, y0 = (float)p0.y, x1 = (float)p1.x,
         y1 = (float)p1.y, x2 = (float)p2.x, y2 = (float)p2.y,
         x3 = (float)p3.x, y3 = (float)p3.y;
   float w1 = stroke_width(s, p1, f.scale), w2 = stroke_width(s, p2, f.scale);
   float dx = (x2 - x1) * f.scale, dy = (y2 - y1) * f.scale;
   int steps = (int)(sqrtf(dx * dx + dy * dy) / 2.0f);
   if (steps < 1)
      steps = 1;
   if (steps > 32)
      steps = 32;

   float x = f.x + x1 * f.scale, y = f.y + y1 * f.scale, w = w1;
   for (int i = 1; i <= steps; i++) {
      float t = (float)i / (float)steps, t2 = t * t, t3 = t2 * t;
      float cx = 0.5f * (2.0f * x1 + (x2 - x0) * t +
                         (2.0f * x0 - 5.0f * x1 + 4.0f * x2 - x3) * t2 +
                         (3.0f * x1 - x0 - 3.0f * x2 + x3) * t3);
      float cy = 0.5f * (2.0f * y1 + (y2 - y0) * t +
                         (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3) * t2 +
                         (3.0f * y1 - y0 - 3.0f * y2 + y3) * t3);
      float nx = f.x + cx * f.scale, ny = f.y + cy * f.scale;
      float nw = w1 + (w2 - w1) * t;
      // Round caps join the pieces; the width is the mean of the ends.
      Rdraw_line(painter, x, y, nx, ny, 0.5f * (w + nw), r, g, b, 1.0f);
      x = nx;
      y = ny;
      w = nw;
   }
}

static void paint_canvas(Rid widget, RPainter *painter, float width,
                         float height, void *user_data) {
   (void)widget;
   (void)user_data;
   fit f = fit_in(width, height);

   Rdraw_rect(painter, f.x, f.y, GAME_CANVAS_W * f.scale,
              GAME_CANVAS_H * f.scale, 10.0f, 1.0f, 1.0f, 1.0f, 1.0f);
   Rdraw_push_clip_rect(painter, f.x, f.y, GAME_CANVAS_W * f.scale,
                        GAME_CANVAS_H * f.scale);

   usize count;
   const game_stroke *strokes = game_strokes(c.g, &count);
   for (usize i = 0; i < count; i++) {
      const game_stroke *s = &strokes[i];
      float r, g, b;
      rgb(game_palette[s->color], &r, &g, &b);
      for (usize k = 0; k < s->pts.len; k++) {
         if (k == 0) {
            game_point p = s->pts.items[0];
            Rdraw_circle(painter, f.x + (float)p.x * f.scale,
                         f.y + (float)p.y * f.scale,
                         stroke_width(s, p, f.scale) * 0.5f, r, g, b, 1.0f);
            continue;
         }
         paint_segment(painter, s, k, f, r, g, b);
      }
   }
   Rdraw_pop_clip_rect(painter);

   if (count == 0 && game_can_draw(c.g))
      Rdraw_text(painter, R_INVALID_ID, 18.0f * (f.scale > 1 ? 1 : f.scale),
                 f.x + 18.0f, f.y + 18.0f, "Draw here", 0.6f, 0.6f, 0.6f,
                 1.0f);
}

static void to_game(float lx, float ly, float *gx, float *gy) {
   float rx, ry, w = 0, h = 0;
   Rwidget_get_rect(c.canvas, &rx, &ry, &w, &h);
   fit f = fit_in(w, h);
   *gx = (lx - f.x) / f.scale;
   *gy = (ly - f.y) / f.scale;
}

/* The pen, with its pressure. */
static int on_pen(Rid widget, const RPenEvent *e, void *user_data) {
   (void)user_data;
   float x, y;
   to_game(e->local_x, e->local_y, &x, &y);
   u64 now = c.now_ms();
   switch (e->phase) {
   case R_PEN_DOWN:
      game_set_brush(c.g, e->eraser ? 0 : c.color, brush_sizes[c.size_index]);
      game_pen_down(c.g, x, y, e->pressure, now);
      break;
   case R_PEN_MOVE:
      game_pen_move(c.g, x, y, e->pressure, now);
      break;
   case R_PEN_UP:
   case R_PEN_LEAVE:
      game_pen_up(c.g, now);
      game_set_brush(c.g, c.color, brush_sizes[c.size_index]);
      break;
   case R_PEN_HOVER:
      return R_FALSE;
   }
   Rwidget_invalidate(widget);
   return R_TRUE;
}

/* The mouse (and touch emulating it): flat width. */
static int on_mouse(Rid widget, const REvent *e, void *user_data) {
   (void)user_data;
   float x, y;
   to_game(e->local_x, e->local_y, &x, &y);
   u64 now = c.now_ms();
   switch (e->type) {
   case R_EVENT_MOUSE_DOWN:
      if (e->button != R_MOUSE_LEFT)
         return R_FALSE;
      c.mouse_down = true;
      game_pen_down(c.g, x, y, -1.0f, now);
      break;
   case R_EVENT_MOUSE_MOVE:
      if (!c.mouse_down)
         return R_FALSE;
      game_pen_move(c.g, x, y, -1.0f, now);
      break;
   case R_EVENT_MOUSE_UP:
   case R_EVENT_MOUSE_LEAVE:
      if (!c.mouse_down)
         return R_FALSE;
      c.mouse_down = false;
      game_pen_up(c.g, now);
      break;
   default:
      return R_FALSE;
   }
   Rwidget_invalidate(widget);
   return R_TRUE;
}

/* The palette: 16 colours, then the brush sizes. */

static float size_x(int i) {
   return GAME_PALETTE_SIZE * (SWATCH + SWATCH_GAP) + 18.0f +
          (float)i * (SWATCH + 10.0f);
}

static void paint_palette(Rid widget, RPainter *painter, float width,
                          float height, void *user_data) {
   (void)widget;
   (void)width;
   (void)user_data;
   float y = (height - SWATCH) * 0.5f;
   for (int i = 0; i < GAME_PALETTE_SIZE; i++) {
      float x = (float)i * (SWATCH + SWATCH_GAP), r, g, b;
      rgb(game_palette[i], &r, &g, &b);
      if (i == c.color)
         Rdraw_rect(painter, x - 3, y - 3, SWATCH + 6, SWATCH + 6, 9.0f, 0.35f,
                    0.6f, 1.0f, 1.0f);
      Rdraw_rect(painter, x, y, SWATCH, SWATCH, 7.0f, r, g, b, 1.0f);
      if (i == 0) // the eraser: white with a cross
         Rdraw_line(painter, x + 8, y + 8, x + SWATCH - 8, y + SWATCH - 8,
                    2.0f, 0.6f, 0.6f, 0.6f, 1.0f);
   }
   float r, g, b;
   rgb(game_palette[c.color == 0 ? 1 : c.color], &r, &g, &b);
   for (int i = 0; i < BRUSH_COUNT; i++) {
      float x = size_x(i);
      if (i == c.size_index)
         Rdraw_rect(painter, x - 3, y - 3, SWATCH + 6, SWATCH + 6, 9.0f, 0.35f,
                    0.6f, 1.0f, 1.0f);
      Rdraw_rect(painter, x, y, SWATCH, SWATCH, 7.0f, 0.2f, 0.22f, 0.27f,
                 1.0f);
      float radius = 2.0f + (float)i * 3.2f;
      Rdraw_circle(painter, x + SWATCH * 0.5f, y + SWATCH * 0.5f, radius, r, g,
                   b, 1.0f);
   }
}

static void measure_palette(Rid widget, float *width, float *height,
                            void *user_data) {
   (void)widget;
   (void)user_data;
   *width = size_x(BRUSH_COUNT);
   *height = PALETTE_HEIGHT;
}

static void pick(float x) {
   for (int i = 0; i < GAME_PALETTE_SIZE; i++) {
      float sx = (float)i * (SWATCH + SWATCH_GAP);
      if (x >= sx && x < sx + SWATCH + SWATCH_GAP)
         c.color = i;
   }
   for (int i = 0; i < BRUSH_COUNT; i++) {
      if (x >= size_x(i) && x < size_x(i) + SWATCH + 10.0f)
         c.size_index = i;
   }
   game_set_brush(c.g, c.color, brush_sizes[c.size_index]);
   Rwidget_invalidate(c.palette);
}

static int on_palette_mouse(Rid widget, const REvent *e, void *user_data) {
   (void)widget;
   (void)user_data;
   if (e->type != R_EVENT_MOUSE_DOWN || e->button != R_MOUSE_LEFT)
      return R_FALSE;
   pick(e->local_x);
   return R_TRUE;
}

static int on_palette_pen(Rid widget, const RPenEvent *e, void *user_data) {
   (void)widget;
   (void)user_data;
   if (e->phase != R_PEN_DOWN)
      return R_FALSE;
   pick(e->local_x);
   return R_TRUE;
}

void canvas_init(Rid canvas, Rid palette, game *g, u64 (*now_ms)(void)) {
   c.canvas = canvas;
   c.palette = palette;
   c.g = g;
   c.now_ms = now_ms;
   game_set_brush(g, c.color, brush_sizes[c.size_index]);

   Rcustom_set_paint_callback(canvas, paint_canvas, NULL);
   Rwidget_set_event_callback(canvas, on_mouse, NULL);
   Rwidget_set_pen_callback(canvas, on_pen, NULL);
   Rwidget_set_pen_mouse(canvas, R_FALSE);
   Rwidget_set_touch_mode(canvas, R_TOUCH_MODE_NONE);
   Rwidget_set_touch_gestures(canvas, R_FALSE);
   Rwidget_set_cursor(canvas, R_CURSOR_CROSSHAIR);

   Rcustom_set_paint_callback(palette, paint_palette, NULL);
   Rcustom_set_measure_callback(palette, measure_palette, NULL);
   Rwidget_set_event_callback(palette, on_palette_mouse, NULL);
   Rwidget_set_pen_callback(palette, on_palette_pen, NULL);
   Rwidget_set_pen_mouse(palette, R_FALSE);
}

void canvas_invalidate(void) {
   Rwidget_invalidate(c.canvas);
}
