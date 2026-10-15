/*
 * font5x7.h - 5x7 bitmap text for the host renderer.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lives in tools/ rather than src/ on purpose: a font is a renderer concern,
 * not part of the QuickDraw core. The engine has no idea text exists; a real
 * port would hand glyphs to it as a QuickDraw 'w' (bitmap) pattern.
 */

#ifndef FONT5X7_H
#define FONT5X7_H

#include "qd/qd.h"

void draw_text(qd_grafport_t *p, int x, int y, const char *s, int scale);

#endif /* FONT5X7_H */
