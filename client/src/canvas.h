/* canvas.h — the drawing surface and the drawer's palette, as Raven custom
 * widgets over the game core. The 800x600 game space is letterboxed into the
 * widget; pointer and pen positions are mapped back before reaching the core.
 */

#ifndef CANVAS_H
#define CANVAS_H

#include <Raven.h>

#include "game.h"

// canvas and palette are Custom widgets. now_ms is the clock the game core
// runs on.
void canvas_init(Rid canvas, Rid palette, game *g, u64 (*now_ms)(void));
// Narrow: the palette wraps to width and has undo and clear of its own.
void canvas_set_narrow(bool narrow, float width);
// The game changed what is drawn.
void canvas_invalidate(void);

#endif
