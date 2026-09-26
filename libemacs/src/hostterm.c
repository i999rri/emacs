/* Frames that a host application draws: the terminal.

Copyright (C) 2026 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

/* The terminal of the `host' window system, and its redisplay
   interface.  Redisplay lays out a host frame as it lays out any
   other, and leaves what it decided in the glyph matrices; then it
   asks the window system to draw it, and here nothing does, because
   the host draws the frame from the matrices itself (hostscreen.c).
   What is left for this file is what shared code needs of a window
   system to get that far: the display, the frame's font and size,
   colors, and the hooks that may not be null.

   The host's input is read in hostinput.c.  */

#include <config.h>

#include <stdlib.h>

#include "lisp.h"
#include "blockinput.h"
#include "keyboard.h"
#include "termhooks.h"
#include "window.h"
#include "buffer.h"
#include "fontset.h"
/* For MOUSE_HL_INFO, which reaches into a terminal that is not one of
   these but has to compile all the same.  */
#include "termchar.h"
#include "hostterm.h"
#include "hostlib.h"

/* The one display there is, once `x-open-connection' has opened it.
   frame.c and keyboard.c look at it by this name, whatever the window
   system is.  */
struct host_display_info *x_display_list;

char *
get_keysym_name (int keysym)
{
  static char value[16];
  sprintf (value, "%d", keysym);
  return value;
}

/* Colors.  */

/* Look up the color NAME, a "#rrggbb" or other spec, or a name in
   rgb.txt, and store it in COLOR.  Return whether it was found.  */

bool
host_get_color (const char *name, Emacs_Color *color)
{
  unsigned short r16, g16, b16;
  Lisp_Object tail;

  if (parse_color_spec (name, &r16, &g16, &b16))
    {
      color->pixel = RGB_TO_ULONG (r16 / 256, g16 / 256, b16 / 256);
      color->red = r16;
      color->green = g16;
      color->blue = b16;
      return true;
    }

  if (!x_display_list)
    return false;

  for (tail = x_display_list->color_map; CONSP (tail); tail = XCDR (tail))
    {
      Lisp_Object entry = XCAR (tail);

      if (CONSP (entry) && STRINGP (XCAR (entry))
	  && !xstrcasecmp (SSDATA (XCAR (entry)), name))
	{
	  unsigned long pixel = XFIXNUM (XCDR (entry));

	  color->pixel = pixel;
	  color->red = RED16_FROM_ULONG (pixel);
	  color->green = GREEN16_FROM_ULONG (pixel);
	  color->blue = BLUE16_FROM_ULONG (pixel);
	  return true;
	}
    }

  return false;
}

static bool
host_defined_color (struct frame *f, const char *name,
		    Emacs_Color *color, bool alloc, bool make_index)
{
  bool found = host_get_color (name, color);

  if (found && f && f->gamma && alloc)
    gamma_correct (f, color);

  return found;
}

static void
host_query_frame_background_color (struct frame *f, Emacs_Color *bgcolor)
{
  unsigned long pixel = FRAME_BACKGROUND_PIXEL (f);

  bgcolor->pixel = pixel;
  bgcolor->red = RED16_FROM_ULONG (pixel);
  bgcolor->green = GREEN16_FROM_ULONG (pixel);
  bgcolor->blue = BLUE16_FROM_ULONG (pixel);
}

/* There are no X resources: every parameter comes from Lisp.  */

static const char *
host_get_string_resource (void *rdb, const char *name, const char *class)
{
  return NULL;
}

/* Fonts and sizes.  */

/* Give frame F the font FONT_OBJECT as its default font, and FONTSET,
   or a fontset made from the font if FONTSET is negative.  The frame's
   character cell is the font's, so its size in pixels changes with
   it.  */

static Lisp_Object
host_new_font (struct frame *f, Lisp_Object font_object, int fontset)
{
  struct font *font = XFONT_OBJECT (font_object);
  int ascent, descent, unit;

  if (fontset < 0)
    fontset = fontset_from_font (font_object);
  FRAME_FONTSET (f) = fontset;

  if (FRAME_FONT (f) == font)
    return font_object;

  FRAME_FONT (f) = font;
  FRAME_BASELINE_OFFSET (f) = font->baseline_offset;
  FRAME_COLUMN_WIDTH (f) = font->average_width;
  get_font_ascent_descent (font, &ascent, &descent);
  FRAME_LINE_HEIGHT (f) = ascent + descent;
  FRAME_TAB_BAR_HEIGHT (f) = FRAME_TAB_BAR_LINES (f) * FRAME_LINE_HEIGHT (f);

  unit = FRAME_COLUMN_WIDTH (f);
  if (FRAME_CONFIG_SCROLL_BAR_WIDTH (f) > 0)
    FRAME_CONFIG_SCROLL_BAR_COLS (f)
      = (FRAME_CONFIG_SCROLL_BAR_WIDTH (f) + unit - 1) / unit;
  else
    FRAME_CONFIG_SCROLL_BAR_COLS (f) = (14 + unit - 1) / unit;

  /* Keep the same number of lines and columns, as other window
     systems do; the host sizes the frame again when it has room for
     something else.  */
  if (f->can_set_window_size)
    adjust_frame_size (f, FRAME_COLS (f) * FRAME_COLUMN_WIDTH (f),
		       FRAME_LINES (f) * FRAME_LINE_HEIGHT (f),
		       3, false, Qfont);

  return font_object;
}

/* Make frame F WIDTH by HEIGHT pixels, as Lisp asked.  Another window
   system would resize the window and change the frame's size once
   told that it had; a host frame has no window, so its size is what
   was asked, at once.  The host is not told: it gave the size in the
   first place (`resize'), and follows the frame's size in the
   screens it is sent.  */

static void
host_set_window_size (struct frame *f, bool change_gravity,
		      int width, int height)
{
  change_frame_size (f, width, height, false, true, false);
  do_pending_window_change (false);
}

/* Put frame F at X, Y, as Lisp asked: within its parent for a child
   frame, which is the only kind whose place means anything, since the
   host puts the root frame where its window is.  There is no window to
   move, so the place is the frame's own, and the host draws the frame
   there the next time it is sent the screen.  */

static void
host_set_offset (struct frame *f, int x, int y, int change_gravity)
{
  if (change_gravity > 0)
    {
      f->left_pos = x;
      f->top_pos = y;
      f->size_hint_flags &= ~(XNegative | YNegative);
      if (x < 0)
	f->size_hint_flags |= XNegative;
      if (y < 0)
	f->size_hint_flags |= YNegative;
      f->win_gravity = NorthWestGravity;
    }

  fset_redisplay (f);
  if (FRAME_PARENT_FRAME (f))
    fset_redisplay (FRAME_PARENT_FRAME (f));
}

/* Give frame F the keys, as `select-frame-set-input-focus' asks for a
   child frame that takes input, a minibuffer that floats over the
   frame for one.  The keys come from the host to the root frame, and
   are sent on to F the way the keys of a frame with no minibuffer are
   sent to the frame of its minibuffer, so that Emacs reads them as
   F's without switching frames to do it.  */

static void
host_focus_frame (struct frame *f, bool noactivate)
{
  struct frame *root = root_frame (f);
  Lisp_Object root_frame_object, focus;

  XSETFRAME (root_frame_object, root);
  XSETFRAME (focus, f);
  Fredirect_frame_focus (root_frame_object, root == f ? Qnil : focus);
}

void
host_set_frame_visible_invisible (struct frame *f, bool visible)
{
  if (visible)
    {
      if (!FRAME_VISIBLE_P (f))
	SET_FRAME_GARBAGED (f);
      SET_FRAME_VISIBLE (f, true);
      SET_FRAME_ICONIFIED (f, false);
    }
  else
    SET_FRAME_VISIBLE (f, false);
}

static void
host_iconify_frame (struct frame *f)
{
  SET_FRAME_VISIBLE (f, false);
  SET_FRAME_ICONIFIED (f, true);
}

static void
host_delete_frame (struct frame *f)
{
  /* TODO: tell the host the frame is gone (`frame-deleted'), once it
     is told of frames at all.  */
  xfree (f->output_data.host);
  f->output_data.host = NULL;
}

/* Menus.  The host is to show them, through `call'; until it does, a
   popup menu is one nothing was chosen from, rather than a crash on a
   null hook.  */

static Lisp_Object
host_menu_show (struct frame *f, int x, int y, int menuflags,
		Lisp_Object title, const char **error_name)
{
  /* TODO: ask the host to show the menu.  */
  return Qnil;
}

/* The redisplay interface.  Everything that would draw draws nothing.
   Those that keep account of where things are on the screen are the
   generic ones, so that the cursor, the mouse face and the rows in the
   matrices come out the same as on any other window system.  */

/* Move what is drawn for the rows of W that RUN says have moved.

   Redisplay moves the rows it keeps within the matrix and draws only
   the ones that were not there before, so the picture has to move with
   them: what was drawn in the rows that moved is still what they show.  */

static void
host_scroll_run (struct window *w, struct run *run)
{
  struct frame *f = XFRAME (w->frame);
  int x, y, width, height, from_y, to_y, bottom_y;

  window_box (w, ANY_AREA, &x, &y, &width, &height);

  from_y = WINDOW_TO_FRAME_PIXEL_Y (w, run->current_y);
  to_y = WINDOW_TO_FRAME_PIXEL_Y (w, run->desired_y);
  bottom_y = y + height;

  /* Not as far as the mode line below the text, which is not the
     window's to scroll.  */
  if (to_y < from_y)
    height = (from_y + run->height > bottom_y
	      ? bottom_y - from_y : run->height);
  else
    height = (to_y + run->height > bottom_y
	      ? bottom_y - to_y : run->height);

  /* The cursor is drawn again by `gui_update_window_end'; moved with
     the rows it would be drawn twice.  */
  gui_clear_cursor (w);

  host_move_area (f, x, from_y, width, height, to_y);
}

/* Take that ROW of W has been drawn again, so that what goes in the
   fringe beside it is drawn again with it.  */

static void
host_after_update_window_line (struct window *w, struct glyph_row *desired_row)
{
  eassert (w);

  if (!desired_row->mode_line_p && !w->pseudo_window_p)
    desired_row->redraw_fringe_bitmaps_p = true;
}

/* Draw the bitmap P says goes in the fringe of W beside ROW, which
   marks a line as continued, as truncated, as empty, and whatever else
   a bitmap has been put there for.  */

static void
host_draw_fringe_bitmap (struct window *w, struct glyph_row *row,
			 struct draw_fringe_bitmap_params *p)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct face *face = p->face;
  int row_y;

  /* Within the row, so that a bitmap taller than the row it belongs to
     does not reach into the next.  */
  row_y = max (WINDOW_TO_FRAME_PIXEL_Y (w, max (0, row->y)),
	       WINDOW_TOP_EDGE_Y (w));
  host_set_clip (f, min (p->x, p->bx >= 0 ? p->bx : p->x), row_y,
		 max (p->wd, p->bx >= 0 ? p->nx : 0)
		 + (p->bx >= 0 ? abs (p->x - p->bx) : 0),
		 row->visible_height);

  /* The part of the fringe with no bitmap on it, cleared to the
     fringe's own background.  */
  if (p->bx >= 0 && !p->overlay_p)
    host_fill_area (f, p->bx, p->by, p->nx, p->ny, face->background);

  if (p->which && p->bits)
    {
      unsigned short *bits = p->bits + p->dh;
      /* Laid over a cursor already drawn in the fringe, the bitmap is
	 what is behind it: it is the hole the cursor is seen through
	 rather than anything drawn in its own right.  */
      unsigned long color = (p->cursor_p
			     ? (p->overlay_p
				? face->background
				: FRAME_OUTPUT_DATA (f)->cursor_pixel)
			     : face->foreground);
      int line, column;

      /* A bitmap is a bit to a pixel and a row of it to a short, with
	 the leftmost pixel in the highest bit.  */
      for (line = 0; line < p->h; line++)
	for (column = 0; column < p->wd; column++)
	  if (bits[line] & (1 << (p->wd - 1 - column)))
	    host_fill_area (f, p->x + column, p->y + line, 1, 1, color);
    }

  host_reset_clip (f);
}

/* The colors S is to be drawn in, which is what a graphics context
   holds on the window systems that have one: for most text the face's
   own, and for the cursor a pair that stands out against it.  */

static void
host_set_glyph_string_colors (struct glyph_string *s)
{
  prepare_face_for_display (s->f, s->face);

  s->foreground = s->face->foreground;
  s->background = s->face->background;
  s->stippled_p = s->face->stipple != 0;

  if (s->hl != DRAW_CURSOR)
    return;

  s->stippled_p = false;
  s->background = FRAME_OUTPUT_DATA (s->f)->cursor_pixel;
  s->foreground = s->face->background;

  /* The glyph would be invisible drawn in the color behind it, so
     something else is tried until one of them tells apart.  */
  if (s->foreground == s->background)
    s->foreground = s->face->foreground;
  if (s->foreground == s->background)
    s->foreground = FRAME_FOREGROUND_PIXEL (s->f);

  /* Nothing would mark the cursor out from the text it is on, so the
     face is turned about instead.  */
  if (s->background == s->face->background
      && s->foreground == s->face->foreground)
    {
      s->background = s->face->foreground;
      s->foreground = s->face->background;
    }
}

/* Fill in what is behind S, which is the face's background over the
   whole width the string was given.

   Only where it is needed: the glyphs are drawn over their own
   background where the font is as tall as the line, so filling it
   first would be drawing the same pixels twice.  FORCE_P asks for it
   anyway, which is what drawing a box around the text and drawing
   over the overhang of the string before it both need.  */

static void
host_draw_glyph_string_background (struct glyph_string *s, bool force_p)
{
  int box = max (s->face->box_horizontal_line_width, 0);

  if (s->background_filled_p)
    return;

  /* A stipple is a pattern this does not draw; the background is
     filled with the color behind it, which is what it comes to
     without the pattern.  */
  if (s->stippled_p
      || FONT_HEIGHT (s->font) < s->height - 2 * box
      || FONT_TOO_HIGH (s->font)
      || s->font_not_found_p
      || s->extends_to_end_of_line_p
      || force_p)
    {
      host_fill_area (s->f, s->x, s->y + box, s->background_width,
		      s->height - 2 * box, s->background);
      s->background_filled_p = true;
    }
}

/* Keep drawing within S itself, rather than within the whole of what
   it is a part of: a string whose neighbour is drawn in another face
   may reach into it, and that much is drawn when the neighbour is.  */

static void
host_set_glyph_string_clipping_exactly (struct glyph_string *s)
{
  host_set_clip (s->f, s->x, s->y, s->width, s->height);
}

/* How far S reaches past where it was given room for, which is how
   far the strings beside it have to be drawn again when it is.

   A glyph may be drawn wider than the room it was given: an italic
   leans past it, and some letters are drawn with a flourish.  Without
   this Emacs takes every glyph to stay within its room, and what is
   drawn past it is left behind when the text beside it changes.  */

static void
host_compute_glyph_string_overhangs (struct glyph_string *s)
{
  if (!s->cmp
      && (s->first_glyph->type == CHAR_GLYPH
	  || s->first_glyph->type == COMPOSITE_GLYPH))
    {
      struct font_metrics metrics;

      if (s->first_glyph->type == CHAR_GLYPH)
	s->font->driver->text_extents (s->font, s->char2b, s->nchars,
				       &metrics);
      else
	composition_gstring_width (composition_gstring_from_id (s->cmp_id),
				   s->cmp_from, s->cmp_to, &metrics);

      s->right_overhang = (metrics.rbearing > metrics.width
			   ? metrics.rbearing - metrics.width : 0);
      s->left_overhang = metrics.lbearing < 0 ? -metrics.lbearing : 0;
    }
  else if (s->cmp)
    {
      s->right_overhang = s->cmp->rbearing - s->cmp->pixel_width;
      s->left_overhang = -s->cmp->lbearing;
    }
}

/* Keep drawing of S within the part of the window it may reach, which
   is the row it is in, cut down by whatever `draw_glyphs' asked for.  */

static void
host_set_glyph_string_clipping (struct glyph_string *s)
{
  NativeRectangle box;

  get_glyph_string_clip_rect (s, &box);
  host_set_clip (s->f, box.x, box.y, box.width, box.height);
}

/* Return COLOR lightened or darkened by FACTOR, which is what a raised
   or sunken box is drawn with: a face names one color for its box, and
   the two edges are that color moved towards white and towards black.  */

static unsigned long
host_shaded (unsigned long color, double factor)
{
  int part[3], i;

  part[0] = RED_FROM_ULONG (color);
  part[1] = GREEN_FROM_ULONG (color);
  part[2] = BLUE_FROM_ULONG (color);

  for (i = 0; i < 3; i++)
    {
      double moved = part[i] * factor;

      /* A color that is already at one end cannot be moved further
	 that way, so it is moved towards the middle instead and the
	 edge is still told apart from the face.  */
      if (moved > 255)
	moved = 255;
      if (factor > 1 && part[i] < 16)
	moved = 32;
      part[i] = (int) moved;
    }

  return RGB_TO_ULONG (part[0], part[1], part[2]);
}

/* Draw the box of S between LEFT_X, TOP_Y and RIGHT_X, BOTTOM_Y, whose
   lines are HWIDTH pixels high and VWIDTH wide.  LEFT_P and RIGHT_P
   say whether the box is closed at each end: a box around text that
   runs on into the next glyph string is not.  */

static void
host_draw_box_rect (struct glyph_string *s, int left_x, int top_y,
		    int right_x, int bottom_y, int hwidth, int vwidth,
		    bool left_p, bool right_p)
{
  unsigned long color = s->face->box_color;
  struct host_clip was = host_clip_now (s->f);

  host_set_glyph_string_clipping (s);

  host_fill_area (s->f, left_x, top_y, right_x - left_x + 1, hwidth, color);
  if (left_p)
    host_fill_area (s->f, left_x, top_y, vwidth, bottom_y - top_y + 1, color);
  host_fill_area (s->f, left_x, bottom_y - hwidth + 1, right_x - left_x + 1,
		  hwidth, color);
  if (right_p)
    host_fill_area (s->f, right_x - vwidth + 1, top_y, vwidth,
		    bottom_y - top_y + 1, color);

  host_clip_again (s->f, was);
}

/* Draw the box of S as one standing out of the screen, or sunk into
   it, which is the same box with its edges in two colors: the light
   falls on the top and the left of a raised box and on the bottom and
   the right of a sunken one.  */

static void
host_draw_relief_rect (struct glyph_string *s, int left_x, int top_y,
		       int right_x, int bottom_y, int hwidth, int vwidth,
		       bool raised_p, bool left_p, bool right_p)
{
  unsigned long color = (s->face->use_box_color_for_shadows_p
			 ? s->face->box_color
			 : s->face->background);
  unsigned long lit = host_shaded (color, 1.35);
  unsigned long shade = host_shaded (color, 0.6);
  unsigned long top = raised_p ? lit : shade;
  unsigned long bottom = raised_p ? shade : lit;
  struct host_clip was = host_clip_now (s->f);

  host_set_glyph_string_clipping (s);

  host_fill_area (s->f, left_x, top_y, right_x - left_x + 1, hwidth, top);
  if (left_p)
    host_fill_area (s->f, left_x, top_y, vwidth, bottom_y - top_y + 1, top);
  host_fill_area (s->f, left_x, bottom_y - hwidth + 1, right_x - left_x + 1,
		  hwidth, bottom);
  if (right_p)
    host_fill_area (s->f, right_x - vwidth + 1, top_y, vwidth,
		    bottom_y - top_y + 1, bottom);

  host_clip_again (s->f, was);
}

/* Draw the box the face of S asks for around it.  */

static void
host_draw_glyph_string_box (struct glyph_string *s)
{
  int hwidth, vwidth, left_x, right_x, top_y, bottom_y, last_x;
  bool raised_p, left_p, right_p;
  struct glyph *last_glyph;

  last_x = (s->row->full_width_p && !s->w->pseudo_window_p
	    ? WINDOW_RIGHT_EDGE_X (s->w)
	    : window_box_right (s->w, s->area));

  /* Which glyph may carry the right line of the box: the first, for a
     composition or an image, and the last for anything else.  */
  if (s->cmp || s->img)
    last_glyph = s->first_glyph;
  else if (s->first_glyph->type == COMPOSITE_GLYPH
	   && s->first_glyph->u.cmp.automatic)
    {
      struct glyph *end = s->row->glyphs[s->area] + s->row->used[s->area];
      struct glyph *g = s->first_glyph;

      for (last_glyph = g++;
	   g < end && g->u.cmp.automatic && g->u.cmp.id == s->cmp_id
	     && g->slice.cmp.to < s->cmp_to;
	   last_glyph = g++)
	;
    }
  else
    last_glyph = s->first_glyph + s->nchars - 1;

  vwidth = eabs (s->face->box_vertical_line_width);
  hwidth = eabs (s->face->box_horizontal_line_width);
  raised_p = s->face->box == FACE_RAISED_BOX;
  left_x = s->x;
  right_x = (s->row->full_width_p && s->extends_to_end_of_line_p
	     ? last_x - 1
	     : min (last_x, s->x + s->background_width) - 1);
  top_y = s->y;
  bottom_y = top_y + s->height - 1;

  left_p = (s->first_glyph->left_box_line_p
	    || (s->hl == DRAW_MOUSE_FACE
		&& (s->prev == NULL || s->prev->hl != s->hl)));
  right_p = (last_glyph->right_box_line_p
	     || (s->hl == DRAW_MOUSE_FACE
		 && (s->next == NULL || s->next->hl != s->hl)));

  if (s->face->box == FACE_SIMPLE_BOX)
    host_draw_box_rect (s, left_x, top_y, right_x, bottom_y, hwidth, vwidth,
			left_p, right_p);
  else
    host_draw_relief_rect (s, left_x, top_y, right_x, bottom_y, hwidth,
			   vwidth, raised_p, left_p, right_p);
}

/* Draw a line of dashes SEGMENT pixels long and as far apart under S,
   WIDTH pixels of it, OFFSET below the baseline and THICKNESS thick.  */

static void
host_draw_dash (struct glyph_string *s, int width, int segment, int offset,
		int thickness, unsigned long color)
{
  int at;

  /* From where the pattern would have fallen had the line been drawn
     in one piece, so that a run split in two by the cursor or by what
     lights up under the pointer is still one dashed line.  */
  for (at = -(s->x % (segment * 2)); at < width; at += segment * 2)
    {
      int from = max (at, 0);

      if (from < at + segment)
	host_fill_area (s->f, s->x + from, s->ybase + offset,
			min (at + segment, width) - from, thickness, color);
    }
}

/* Draw the underline of S in the style the face asks for, POSITION
   below the baseline, WIDTH pixels of it and THICKNESS thick.  */

static void
host_fill_underline (struct glyph_string *s, enum face_underline_type style,
		     int position, int width, int thickness,
		     unsigned long color)
{
  switch (style)
    {
      /* A double line is two of these, drawn one call after another.  */
    case FACE_UNDERLINE_SINGLE:
    case FACE_UNDERLINE_DOUBLE_LINE:
      host_fill_area (s->f, s->x, s->ybase + position, width, thickness,
		      color);
      break;

    case FACE_UNDERLINE_DOTS:
      host_draw_dash (s, width, thickness, position, thickness, color);
      break;

    case FACE_UNDERLINE_DASHES:
      host_draw_dash (s, width, thickness * 3, position, thickness, color);
      break;

    case FACE_NO_UNDERLINE:
    case FACE_UNDERLINE_WAVE:
    default:
      emacs_abort ();
    }
}

/* Draw the wave under S, WIDTH pixels of it, which is what marks
   misspelt text and the like.  */

static void
host_draw_underwave (struct glyph_string *s, int width, unsigned long color)
{
  int height = 3, length = 2;
  int dy = height - 1;
  int x0 = s->x, y0 = s->ybase + height / 2;
  int xmax = x0 + width;
  int x1, x2, y1, y2;
  bool odd;
  struct host_clip was = host_clip_now (s->f);
  NativeRectangle within;

  /* Within the string as well as within itself: a wave under the last
     line of a window, which is only half shown, would otherwise be
     drawn into the mode line below it.  */
  get_glyph_string_clip_rect (s, &within);
  {
    int left = max (x0, within.x);
    int top = max (y0, within.y);
    int right = min (x0 + width, within.x + within.width);
    int bottom = min (y0 + height, within.y + within.height);

    if (right <= left || bottom <= top)
      return;
    host_set_clip (s->f, left, top, right - left, bottom - top);
  }

  /* From the last turn of the wave before the string, so that a wave
     under text drawn in more than one piece is one wave.  */
  x1 = x0 - x0 % length;
  x2 = x1 + length;
  odd = (x1 / length) & 1;
  y1 = y2 = y0;

  if (odd)
    y1 += dy;
  else
    y2 += dy;

  while (x1 <= xmax)
    {
      host_draw_line (s->f, x1, y1, x2, y2, color);
      x1 = x2, y1 = y2;
      x2 += length, y2 = y0 + (odd ? 0 : dy);
      odd = !odd;
    }

  host_clip_again (s->f, was);
}

/* Draw the glyphs of S.

   With their own background where nothing else has filled it, which
   is how the background of a face is drawn at all where the font is
   as tall as the line: filling it first and drawing over it would be
   the same pixels twice, so only one of the two is done.  */

static void
host_draw_glyph_string_foreground (struct glyph_string *s)
{
  int x, i;

  /* Past the line of a box on the left, where the face has one.  */
  x = (s->face->box != FACE_NO_BOX && s->first_glyph->left_box_line_p
       ? s->x + max (s->face->box_vertical_line_width, 0)
       : s->x);

  /* A box to a character, where the font it wanted could not be read:
     what is there is known, and that it could not be drawn.  */
  if (s->font_not_found_p)
    {
      for (i = 0; i < s->nchars; i++)
	{
	  struct glyph *glyph = s->first_glyph + i;

	  host_draw_rectangle (s->f, x, s->y, glyph->pixel_width - 1,
			       s->height - 1, s->foreground);
	  x += glyph->pixel_width;
	}
      return;
    }

  {
    struct font *font = s->font;
    int boff = font->baseline_offset;
    int y;

    if (font->vertical_centering)
      boff = VCENTER_BASELINE_OFFSET (font, s->f) - boff;
    y = s->ybase - boff;

    font->driver->draw (s, 0, s->nchars, x, y,
			!(s->for_overlaps
			  || (s->background_filled_p && s->hl != DRAW_CURSOR)));

    /* Drawn again a pixel over, which is how a face asks for a weight
       the font it is drawn in has none of.  */
    if (s->face->overstrike)
      font->driver->draw (s, 0, s->nchars, x + 1, y, false);
  }
}

/* Draw the glyphs of S, which is a composition: several characters
   drawn as one, each where the composition says to put it.

   A static composition holds the offsets itself; an automatic one is
   a gstring, whose glyphs are drawn in runs, with one of its own
   wherever a glyph was adjusted.  */

static void
host_draw_composite_glyph_string_foreground (struct glyph_string *s)
{
  struct font *font = s->font;
  int i, j, x;

  x = (s->face && s->face->box != FACE_NO_BOX && s->first_glyph->left_box_line_p
       ? s->x + max (s->face->box_vertical_line_width, 0)
       : s->x);

  /* A box around the whole of it, where the font of its first
     character could not be read.  */
  if (s->font_not_found_p)
    {
      if (s->cmp_from == 0)
	host_draw_rectangle (s->f, x, s->y, s->width - 1, s->height - 1,
			     s->foreground);
      return;
    }

  if (!s->first_glyph->u.cmp.automatic)
    {
      int y = s->ybase;

      for (i = 0, j = s->cmp_from; i < s->nchars; i++, j++)
	/* A tab in a composition is room kept to one side of it
	   rather than anything to draw.  */
	if (COMPOSITION_GLYPH (s->cmp, j) != '\t')
	  {
	    int xx = x + s->cmp->offsets[j * 2];
	    int yy = y - s->cmp->offsets[j * 2 + 1];

	    font->driver->draw (s, j, j + 1, xx, yy, false);
	    if (s->face->overstrike)
	      font->driver->draw (s, j, j + 1, xx + 1, yy, false);
	  }
      return;
    }

  {
    Lisp_Object gstring = composition_gstring_from_id (s->cmp_id);
    int y = s->ybase;
    int width = 0;

    for (i = j = s->cmp_from; i < s->cmp_to; i++)
      {
	Lisp_Object glyph = LGSTRING_GLYPH (gstring, i);

	if (NILP (LGLYPH_ADJUSTMENT (glyph)))
	  {
	    width += LGLYPH_WIDTH (glyph);
	    continue;
	  }

	/* One that was moved is drawn on its own, and what came
	   before it in one go.  */
	if (j < i)
	  {
	    font->driver->draw (s, j, i, x, y, false);
	    if (s->face->overstrike)
	      font->driver->draw (s, j, i, x + 1, y, false);
	    x += width;
	  }

	font->driver->draw (s, i, i + 1, x + LGLYPH_XOFF (glyph),
			    y + LGLYPH_YOFF (glyph), false);
	if (s->face->overstrike)
	  font->driver->draw (s, i, i + 1, x + LGLYPH_XOFF (glyph) + 1,
			      y + LGLYPH_YOFF (glyph), false);
	x += LGLYPH_WADJUST (glyph);
	j = i + 1;
	width = 0;
      }

    if (j < i)
      {
	font->driver->draw (s, j, i, x, y, false);
	if (s->face->overstrike)
	  font->driver->draw (s, j, i, x + 1, y, false);
      }
  }
}

/* Draw the glyphs of S, which stand for characters the font has none
   of: a box with what is known of the character written in it, in two
   halves, one above the other.  */

static void
host_draw_glyphless_glyph_string_foreground (struct glyph_string *s)
{
  struct glyph *glyph = s->first_glyph;
  unsigned char2b[8];
  int x, i, j;

  x = (s->face && s->face->box != FACE_NO_BOX && s->first_glyph->left_box_line_p
       ? s->x + max (s->face->box_vertical_line_width, 0)
       : s->x);

  s->char2b = char2b;

  for (i = 0; i < s->nchars; i++, glyph++)
    {
      char buf[7];
      char *str = NULL;
      int len = glyph->u.glyphless.len;

      if (glyph->u.glyphless.method == GLYPHLESS_DISPLAY_ACRONYM)
	{
	  if (len > 0
	      && CHAR_TABLE_P (Vglyphless_char_display)
	      && (CHAR_TABLE_EXTRA_SLOTS (XCHAR_TABLE (Vglyphless_char_display))
		  >= 1))
	    {
	      Lisp_Object acronym
		= (!glyph->u.glyphless.for_no_font
		   ? CHAR_TABLE_REF (Vglyphless_char_display,
				     glyph->u.glyphless.ch)
		   : XCHAR_TABLE (Vglyphless_char_display)->extras[0]);

	      if (CONSP (acronym))
		acronym = XCAR (acronym);
	      if (STRINGP (acronym))
		str = SSDATA (acronym);
	    }
	}
      else if (glyph->u.glyphless.method == GLYPHLESS_DISPLAY_HEX_CODE)
	{
	  unsigned int ch = glyph->u.glyphless.ch;

	  eassume (ch <= MAX_CHAR);
	  sprintf (buf, "%0*X", ch < 0x10000 ? 4 : 6, ch);
	  str = buf;
	}

      if (str)
	{
	  int upper_len = (len + 1) / 2;

	  /* What is written in the box is ASCII throughout.  */
	  for (j = 0; j < len; j++)
	    char2b[j] = s->font->driver->encode_char (s->font, str[j]) & 0xFFFF;
	  s->font->driver->draw (s, 0, upper_len,
				 x + glyph->slice.glyphless.upper_xoff,
				 s->ybase + glyph->slice.glyphless.upper_yoff,
				 false);
	  s->font->driver->draw (s, upper_len, len,
				 x + glyph->slice.glyphless.lower_xoff,
				 s->ybase + glyph->slice.glyphless.lower_yoff,
				 false);
	}

      if (glyph->u.glyphless.method != GLYPHLESS_DISPLAY_THIN_SPACE)
	host_draw_rectangle (s->f, x, s->ybase - glyph->ascent,
			     glyph->pixel_width - 1,
			     glyph->ascent + glyph->descent - 1, s->foreground);
      x += glyph->pixel_width;
    }

  /* Nothing is to read this again once it is what was on the stack
     here.  */
  s->char2b = NULL;
}

/* Draw S, which is a stretch of blank as wide as it was given room
   for: a tab, or what a `display' property of (space . ...) asks for.

   The cursor on one is drawn as wide as a character rather than as
   wide as the stretch, unless `x-stretch-cursor' says otherwise: a
   cursor on a tab would otherwise be a block eight columns wide.  */

static void
host_draw_stretch_glyph_string (struct glyph_string *s)
{
  eassert (s->first_glyph->type == STRETCH_GLYPH);

  if (s->hl == DRAW_CURSOR && !x_stretch_cursor_p)
    {
      int background_width = s->background_width;
      int x = s->x, width;

      if (!s->row->reversed_p)
	{
	  int left_x = window_box_left_offset (s->w, TEXT_AREA);

	  if (x < left_x)
	    {
	      background_width -= left_x - x;
	      x = left_x;
	    }
	}
      else
	{
	  /* Read right to left, the cursor is at the right edge.  */
	  int right_x = window_box_right (s->w, TEXT_AREA);

	  if (x + background_width > right_x)
	    background_width -= x - right_x;
	  x += background_width;
	}

      width = min (FRAME_COLUMN_WIDTH (s->f), background_width);
      if (s->row->reversed_p)
	x -= width;

      host_fill_area (s->f, x, s->y, width, s->height, s->background);

      /* The rest of the stretch is not the cursor, and is filled in
	 the color it would have had without one.  */
      if (width < background_width)
	{
	  struct face *face = s->face;
	  unsigned long color;

	  if (!s->row->reversed_p)
	    x += width;
	  else
	    x = s->x;

	  color = (s->row->mouse_face_p && cursor_in_mouse_face_p (s->w)
		   ? face->background : face->background);
	  host_set_glyph_string_clipping (s);
	  host_fill_area (s->f, x, s->y, background_width - width, s->height,
			  color);
	  host_reset_clip (s->f);
	}
    }
  else if (!s->background_filled_p)
    {
      int background_width = s->background_width;
      int x = s->x, text_left_x = window_box_left (s->w, TEXT_AREA);

      /* Not into the fringe or the margin on the left, which are not
	 the text's to draw in; a mode line has neither.  */
      if (s->area == TEXT_AREA && x < text_left_x && !s->row->mode_line_p)
	{
	  background_width -= text_left_x - x;
	  x = text_left_x;
	}

      if (background_width > 0)
	host_fill_area (s->f, x, s->y, background_width, s->height,
			s->background);
    }

  s->background_filled_p = true;
}

/* Draw what the face of S puts around and through its text, once the
   text itself is drawn.  */

static void
host_draw_glyph_string_decorations (struct glyph_string *s, bool box_drawn_p)
{
  int area_x, area_y, area_width, area_height, area_max_x, width;

  /* Not past the area the text is in, nor into the fringe.  */
  window_box (s->w, s->area, &area_x, &area_y, &area_width, &area_height);
  area_max_x = area_x + area_width - 1;

  width = s->width;
  if (!s->row->mode_line_p && !s->row->tab_line_p
      && area_max_x < s->x + width - 1)
    width -= (s->x + width - 1) - area_max_x;

  if (!box_drawn_p && s->face->box != FACE_NO_BOX)
    host_draw_glyph_string_box (s);

  if (s->face->underline)
    {
      unsigned long color = (s->face->underline_defaulted_p
			     ? s->foreground
			     : s->face->underline_color);

      if (s->face->underline == FACE_UNDERLINE_WAVE)
	host_draw_underwave (s, width, color);
      else if (s->face->underline >= FACE_UNDERLINE_SINGLE)
	{
	  unsigned long thickness, position;

	  /* Drawn the same as the piece before it, so that an
	     underline under text drawn in several pieces is one
	     line.  */
	  if (s->prev
	      && s->prev->face->underline != FACE_UNDERLINE_WAVE
	      && s->prev->face->underline >= FACE_UNDERLINE_SINGLE
	      && (s->prev->face->underline_at_descent_line_p
		  == s->face->underline_at_descent_line_p)
	      && (s->prev->face->underline_pixels_above_descent_line
		  == s->face->underline_pixels_above_descent_line))
	    {
	      thickness = s->prev->underline_thickness;
	      position = s->prev->underline_position;
	    }
	  else
	    {
	      struct font *font = font_for_underline_metrics (s);
	      unsigned long minimum_offset;
	      bool at_descent_line, from_the_font;
	      Lisp_Object val;

	      val = WINDOW_BUFFER_LOCAL_VALUE (Qunderline_minimum_offset, s->w);
	      minimum_offset = FIXNUMP (val) ? max (0, XFIXNUM (val)) : 1;

	      val = WINDOW_BUFFER_LOCAL_VALUE (Qx_underline_at_descent_line,
					       s->w);
	      at_descent_line = (!(NILP (val) || BASE_EQ (val, Qunbound))
				 || s->face->underline_at_descent_line_p);

	      val = WINDOW_BUFFER_LOCAL_VALUE
		(Qx_use_underline_position_properties, s->w);
	      from_the_font = !(NILP (val) || BASE_EQ (val, Qunbound));

	      thickness = (font && font->underline_thickness > 0
			   ? font->underline_thickness : 1);

	      if (at_descent_line)
		position = ((s->height - thickness) - (s->ybase - s->y)
			    - s->face->underline_pixels_above_descent_line);
	      else if (from_the_font && font && font->underline_position >= 0)
		position = font->underline_position;
	      else if (font)
		position = (font->descent + 1) / 2;
	      else
		position = minimum_offset;

	      /* How far down was asked for in pixels, and then it is
		 not to be moved.  */
	      if (!s->face->underline_pixels_above_descent_line)
		position = max (position, minimum_offset);
	    }

	  /* Within the line: an underline below it would be drawn over
	     the line under this one.  */
	  if (s->y + s->height <= s->ybase + position)
	    position = (s->height - 1) - (s->ybase - s->y);
	  if (s->y + s->height < s->ybase + position + thickness)
	    thickness = (s->y + s->height) - (s->ybase + position);

	  s->underline_thickness = thickness;
	  s->underline_position = position;

	  host_fill_underline (s, s->face->underline, position, width,
			       thickness, color);

	  if (s->face->underline == FACE_UNDERLINE_DOUBLE_LINE)
	    host_fill_underline (s, s->face->underline,
				 position - thickness - 1, width, thickness,
				 color);
	}
    }

  if (s->face->overline_p)
    host_fill_area (s->f, s->x, s->y, width, 1,
		    (s->face->overline_color_defaulted_p
		     ? s->foreground : s->face->overline_color));

  if (s->face->strike_through_p)
    {
      /* Across the glyph rather than the string: the line the string
	 is in may be taller than the text, where something else in it
	 is drawn in a larger font.  */
      int glyph_y = s->ybase - s->first_glyph->ascent;
      int glyph_height = s->first_glyph->ascent + s->first_glyph->descent;

      host_fill_area (s->f, s->x, glyph_y + (glyph_height - 1) / 2, width, 1,
		      (s->face->strike_through_color_defaulted_p
		       ? s->foreground : s->face->strike_through_color));
    }
}

/* Draw the part of OTHER that lies over S, in S's colors: OTHER was
   drawn in its own and kept to its own room, so what it reaches into
   S with is still the wrong color.  */

static void
host_draw_glyph_string_over (struct glyph_string *other,
			     struct glyph_string *s)
{
  enum draw_glyphs_face was = other->hl;
  struct host_clip clip = host_clip_now (s->f);

  other->hl = s->hl;
  host_set_glyph_string_colors (other);
  host_set_clip (s->f, s->x, s->y, s->width, s->height);

  if (other->first_glyph->type == CHAR_GLYPH)
    host_draw_glyph_string_foreground (other);
  else
    host_draw_composite_glyph_string_foreground (other);

  host_clip_again (s->f, clip);
  other->hl = was;
}

static void
host_draw_glyph_string (struct glyph_string *s)
{
  bool box_drawn_p = false;

  /* The room for a tab line is Emacs's to keep and another's to draw
     in, where whatever draws the frame says so: it puts something of
     its own there, and a tab line drawn under that would be a line
     sent every time it changed and never seen.  */
  if (!host_draw_tab_lines && s->row->tab_line_p)
    return;

  host_set_glyph_string_colors (s);

  /* What this string reaches into is drawn again first, so that the
     glyphs of it are not left over what is drawn here.  */
  if (s->next && s->right_overhang && !s->for_overlaps)
    {
      struct glyph_string *next;
      int width;

      for (width = 0, next = s->next;
	   next && width < s->right_overhang;
	   width += next->width, next = next->next)
	if (next->first_glyph->type != IMAGE_GLYPH)
	  {
	    struct host_clip was = host_clip_now (s->f);

	    host_set_glyph_string_colors (next);
	    host_set_glyph_string_clipping (next);
	    if (next->first_glyph->type == STRETCH_GLYPH)
	      host_fill_area (next->f, next->x, next->y,
			      next->background_width, next->height,
			      next->background);
	    else
	      host_draw_glyph_string_background (next, true);
	    host_clip_again (s->f, was);
	  }

      /* The colors are this string's again, which telling the strings
	 beside it apart took away.  */
      host_set_glyph_string_colors (s);
    }

  /* A box around text is drawn first, so that the text is drawn over
     it rather than cut short by it.  */
  if (!s->for_overlaps && s->face->box != FACE_NO_BOX
      && (s->first_glyph->type == CHAR_GLYPH
	  || s->first_glyph->type == COMPOSITE_GLYPH))
    {
      host_set_glyph_string_clipping (s);
      host_draw_glyph_string_background (s, true);
      host_draw_glyph_string_box (s);
      box_drawn_p = true;
    }
  else if (!s->clip_head && !s->clip_tail
	   && ((s->prev && s->prev->hl != s->hl && s->left_overhang)
	       || (s->next && s->next->hl != s->hl && s->right_overhang)))
    /* What this reaches into is drawn in another face, and was drawn
       or is to be drawn with the overhang that belongs to it.  */
    host_set_glyph_string_clipping_exactly (s);
  else
    host_set_glyph_string_clipping (s);

  if (box_drawn_p)
    host_set_glyph_string_clipping (s);

  switch (s->first_glyph->type)
    {
    case STRETCH_GLYPH:
      host_draw_stretch_glyph_string (s);
      break;

    case CHAR_GLYPH:
      /* Drawn over what is already there when it is only the part of
	 a glyph that reaches into another row.  */
      if (s->for_overlaps)
	s->background_filled_p = true;
      else
	host_draw_glyph_string_background (s, false);

      if (s->font)
	{
	  /* The host is to have the file these glyphs are looked for
	     in, against the day it draws them itself: a glyph is
	     numbered by the file it is in and nothing else.  */
	  host_font_id (s);
	  host_draw_glyph_string_foreground (s);
	}
      break;

    case COMPOSITE_GLYPH:
      /* The pieces of a static composition after the first are drawn
	 over what the ones before them put there.  */
      if (s->for_overlaps
	  || (s->cmp_from > 0 && !s->first_glyph->u.cmp.automatic))
	s->background_filled_p = true;
      else
	host_draw_glyph_string_background (s, true);

      if (s->font)
	{
	  host_font_id (s);
	  host_draw_composite_glyph_string_foreground (s);
	}
      break;

    case GLYPHLESS_GLYPH:
      if (s->for_overlaps)
	s->background_filled_p = true;
      else
	host_draw_glyph_string_background (s, true);

      if (s->font)
	{
	  host_font_id (s);
	  host_draw_glyphless_glyph_string_foreground (s);
	}
      break;

    default:
      /* Images and xwidgets are still to be drawn; the room kept for
	 them comes out as their background until they are.  */
      host_draw_glyph_string_background (s, true);
      break;
    }

  if (!s->for_overlaps)
    {
      struct glyph_string *other;

      host_draw_glyph_string_decorations (s, box_drawn_p);

      /* What the strings beside this one reach into it with was drawn
	 in their own colors and kept to their own room; the part of
	 it that lies over this one is drawn again in these.  */
      for (other = s->prev; other; other = other->prev)
	if (other->hl != s->hl
	    && other->x + other->width + other->right_overhang > s->x)
	  host_draw_glyph_string_over (other, s);

      for (other = s->next; other; other = other->next)
	if (other->hl != s->hl
	    && other->x - other->left_overhang < s->x + s->width)
	  {
	    host_draw_glyph_string_over (other, s);
	    other->clip_head = s->next;
	  }
    }

  host_reset_clip (s->f);
}

/* The shape the pointer is to take over F, which redisplay chose as it
   worked out what the pointer is over.

   Kept rather than shown: what draws the window has the pointer, and is
   told the name of the shape with the screen it is drawn over.  */

static void
host_define_frame_cursor (struct frame *f, Emacs_Cursor cursor)
{
  static const char *const names[] = {
    [HOST_POINTER_NONE] = "none",
    [HOST_POINTER_ARROW] = "arrow",
    [HOST_POINTER_TEXT] = "text",
    [HOST_POINTER_HAND] = "hand",
    [HOST_POINTER_BUSY] = "busy",
    [HOST_POINTER_HORIZONTAL_DRAG] = "horizontal-drag",
    [HOST_POINTER_VERTICAL_DRAG] = "vertical-drag",
    [HOST_POINTER_LEFT_EDGE] = "left-edge",
    [HOST_POINTER_TOP_LEFT_CORNER] = "top-left-corner",
    [HOST_POINTER_TOP_EDGE] = "top-edge",
    [HOST_POINTER_TOP_RIGHT_CORNER] = "top-right-corner",
    [HOST_POINTER_RIGHT_EDGE] = "right-edge",
    [HOST_POINTER_BOTTOM_RIGHT_CORNER] = "bottom-right-corner",
    [HOST_POINTER_BOTTOM_EDGE] = "bottom-edge",
    [HOST_POINTER_BOTTOM_LEFT_CORNER] = "bottom-left-corner",
  };
  const struct host_api *api = host_current_api ();
  enum host_pointer shape = HOST_POINTER_OF (cursor);
  char message[96];

  if (FRAME_OUTPUT_DATA (f)->current_cursor == cursor)
    return;

  FRAME_OUTPUT_DATA (f)->current_cursor = cursor;

  /* Said to the host rather than to Lisp.  What lights up under the
     pointer is drawn and handed over as the pointer arrives, and the
     shape it takes is settled in the same breath; going by way of
     Lisp, which looks at what it has been sent on a timer, would have
     the two disagree for as long as that timer takes.  */
  if (!api || shape < 0 || shape >= ARRAYELTS (names) || !names[shape])
    return;

  sprintf (message, "{\"type\":\"pointer\",\"shape\":\"%s\"}",
	   names[shape]);
  api->post (message);
}

static void
host_clear_frame_area (struct frame *f, int x, int y, int width, int height)
{
  host_fill_area (f, x, y, width, height, FRAME_BACKGROUND_PIXEL (f));
}

/* Keep drawing within the part of AREA of W that ROW is drawn in, as
   the cursor is drawn.  */

static void
host_clip_to_row (struct window *w, struct glyph_row *row,
		  enum glyph_row_area area)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  int window_x, window_y, window_width;
  int y;

  window_box (w, area, &window_x, &window_y, &window_width, 0);

  y = max (WINDOW_TO_FRAME_PIXEL_Y (w, max (0, row->y)), window_y);
  host_set_clip (f, window_x, y, window_width, row->visible_height);
}

/* Draw the box a hollow cursor is around the glyph of W it is on, in
   ROW.  */

static void
host_draw_hollow_cursor (struct window *w, struct glyph_row *row)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct glyph *glyph = get_phys_cursor_glyph (w);
  int x, y, width, height;

  /* The matrix may say nothing usable about where the cursor is, and
     then there is nothing to draw around.  */
  if (!glyph)
    return;

  get_phys_cursor_geometry (w, row, glyph, &x, &y, &height);
  width = w->phys_cursor_width - 1;

  /* A character read right to left has the cursor at its right edge,
     unless the box is as wide as the glyph or wider, which is what
     `x-stretch-cursor' makes it.  */
  if ((glyph->resolved_level & 1) != 0 && glyph->pixel_width > width)
    {
      x += glyph->pixel_width - width;
      if (width > 0)
	width -= 1;
    }

  host_clip_to_row (w, row, TEXT_AREA);
  /* In the color of the cursor itself: what a filled cursor draws the
     text in is the color behind it, which would leave no box.  */
  host_draw_rectangle (f, x, y, width, height - 1,
		       FRAME_OUTPUT_DATA (f)->cursor_pixel);
  host_reset_clip (f);
}

/* Draw the bar a cursor of KIND is, WIDTH pixels of it, on the glyph
   of W it is on in ROW.  */

static void
host_draw_bar_cursor (struct window *w, struct glyph_row *row, int width,
		      enum text_cursor_kinds kind)
{
  struct frame *f = XFRAME (w->frame);
  struct glyph *glyph = get_phys_cursor_glyph (w);
  struct face *face;
  unsigned long color;
  int x;

  /* Out of the window, as it is while the minibuffer and the echo area
     change places; there would be nothing but garbage to draw.  */
  if (!glyph || glyph->type == XWIDGET_GLYPH)
    return;

  /* On an image a bar may fall outside the window altogether, and a
     cursor drawn as the glyph is seen wherever the image is.  */
  if (glyph->type == IMAGE_GLYPH)
    {
      draw_phys_cursor_glyph (w, MATRIX_ROW (w->current_matrix,
					     w->phys_cursor.vpos),
			      DRAW_CURSOR);
      return;
    }

  /* A bar in the color of the cursor is invisible on text drawn on
     that color; the text is legible on what is behind it, so its own
     color stands out there too.  */
  face = FACE_FROM_ID (f, glyph->face_id);
  color = (face->background == FRAME_OUTPUT_DATA (f)->cursor_pixel
	   ? face->foreground
	   : FRAME_OUTPUT_DATA (f)->cursor_pixel);

  host_clip_to_row (w, row, TEXT_AREA);
  x = WINDOW_TEXT_TO_FRAME_PIXEL_X (w, w->phys_cursor.x);

  if (kind == BAR_CURSOR)
    {
      if (width < 0)
	width = FRAME_CURSOR_WIDTH (f);
      width = min (glyph->pixel_width, width);
      w->phys_cursor_width = width;

      /* A character read right to left has the bar at its right edge.  */
      if ((glyph->resolved_level & 1) != 0)
	x += glyph->pixel_width - width;

      host_fill_area (f, x, WINDOW_TO_FRAME_PIXEL_Y (w, w->phys_cursor.y),
		      width, row->height, color);
    }
  else
    {
      int dummy_x, dummy_y, dummy_height;

      if (width < 0)
	width = row->height;
      width = min (row->height, width);

      get_phys_cursor_geometry (w, row, glyph, &dummy_x, &dummy_y,
				&dummy_height);

      if ((glyph->resolved_level & 1) != 0
	  && glyph->pixel_width > w->phys_cursor_width - 1)
	x += glyph->pixel_width - w->phys_cursor_width + 1;

      host_fill_area (f, x,
		      WINDOW_TO_FRAME_PIXEL_Y (w, (w->phys_cursor.y
						   + row->height - width)),
		      w->phys_cursor_width - 1, width, color);
    }

  host_reset_clip (f);
}

static void
host_draw_window_cursor (struct window *w, struct glyph_row *row,
			 int x, int y, enum text_cursor_kinds cursor_type,
			 int cursor_width, bool on_p, bool active_p)
{
  if (!on_p)
    return;

  w->phys_cursor_type = cursor_type;
  w->phys_cursor_on_p = true;

  /* The cursor is past the last glyph of a line that filled the window
     exactly, where there is no glyph to draw it on; the fringe shows
     it instead.  */
  if (row->exact_window_width_line_p
      && (row->reversed_p
	  ? w->phys_cursor.hpos < 0
	  : w->phys_cursor.hpos >= row->used[TEXT_AREA]))
    {
      row->cursor_in_fringe_p = true;
      draw_fringe_bitmap (w, row, row->reversed_p);
      return;
    }

  switch (cursor_type)
    {
    case HOLLOW_BOX_CURSOR:
      host_draw_hollow_cursor (w, row);
      break;

    case FILLED_BOX_CURSOR:
      draw_phys_cursor_glyph (w, row, DRAW_CURSOR);
      break;

    case BAR_CURSOR:
    case HBAR_CURSOR:
      host_draw_bar_cursor (w, row, cursor_width, cursor_type);
      break;

    case NO_CURSOR:
      w->phys_cursor_width = 0;
      break;

    default:
      emacs_abort ();
    }
}

static void
host_draw_vertical_window_border (struct window *w, int x, int y_0, int y_1)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct face *face = FACE_FROM_ID_OR_NULL (f, VERTICAL_BORDER_FACE_ID);

  host_draw_line (f, x, y_0, x, y_1,
		  face ? face->foreground : FRAME_FOREGROUND_PIXEL (f));
}

static void
host_draw_window_divider (struct window *w, int x_0, int x_1,
			  int y_0, int y_1)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct face *face = FACE_FROM_ID_OR_NULL (f, WINDOW_DIVIDER_FACE_ID);
  struct face *first
    = FACE_FROM_ID_OR_NULL (f, WINDOW_DIVIDER_FIRST_PIXEL_FACE_ID);
  struct face *last
    = FACE_FROM_ID_OR_NULL (f, WINDOW_DIVIDER_LAST_PIXEL_FACE_ID);
  unsigned long color = face ? face->foreground : FRAME_FOREGROUND_PIXEL (f);
  unsigned long color_first
    = first ? first->foreground : FRAME_FOREGROUND_PIXEL (f);
  unsigned long color_last
    = last ? last->foreground : FRAME_FOREGROUND_PIXEL (f);

  /* The pixels at the edges are drawn in faces of their own where
     there is room for them, which is what gives the divider an edge to
     be seen against.  */
  if (y_1 - y_0 > x_1 - x_0 && x_1 - x_0 >= 3)
    {
      host_fill_area (f, x_0, y_0, 1, y_1 - y_0, color_first);
      host_fill_area (f, x_0 + 1, y_0, x_1 - x_0 - 2, y_1 - y_0, color);
      host_fill_area (f, x_1 - 1, y_0, 1, y_1 - y_0, color_last);
    }
  else if (x_1 - x_0 > y_1 - y_0 && y_1 - y_0 >= 3)
    {
      host_fill_area (f, x_0, y_0, x_1 - x_0, 1, color_first);
      host_fill_area (f, x_0, y_0 + 1, x_1 - x_0, y_1 - y_0 - 2, color);
      host_fill_area (f, x_0, y_1 - 1, x_1 - x_0, 1, color_last);
    }
  else
    host_fill_area (f, x_0, y_0, x_1 - x_0, y_1 - y_0, color);
}

/* The font a frame starts with, when its parameters name none: the
   `font' parameter, or else the first of the monospace fonts that
   the font files found have.  */

static void
host_default_font_parameter (struct frame *f, Lisp_Object parms)
{
  struct host_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  Lisp_Object font = gui_display_get_arg (dpyinfo, parms, Qfont,
					  "font", "Font", RES_TYPE_STRING);

  if (BASE_EQ (font, Qunbound))
    font = Qnil;

  if (!FONTP (font) && !STRINGP (font))
    {
      static const char *const names[] =
	{
	  "Monospace-11",
	  "DejaVu Sans Mono-11",
	  "Noto Sans Mono-11",
	  "Liberation Mono-11",
	  "Cascadia Mono-11",
	  "Consolas-11",
	};

      for (int i = 0; i < ARRAYELTS (names); i++)
	{
	  font = font_open_by_name (f, build_unibyte_string (names[i]));
	  if (!NILP (font))
	    break;
	}

      /* None of those, on a system that has other fonts, as NixOS
	 does with only what the user asked for: whatever monospace font
	 the files found have.  */
      if (NILP (font))
	font = font_open_by_spec (f, CALLN (Ffont_spec, QCspacing,
					    make_fixnum (FONT_SPACING_MONO)));

      if (NILP (font))
	error ("No font was found in `host-font-directories'");
    }

  gui_default_parameter (f, parms, Qfont, font, "font", "Font",
			 RES_TYPE_STRING);
}

/* Let what the pointer is over light up again.  Redisplay draws the
   glyphs it lights up along with the rest, so while an update is going
   on the highlight is left alone, and dispnew.c asks for that by
   setting this on the way in; nothing but the port takes it off.  */

static void
host_update_end (struct frame *f)
{
  MOUSE_HL_INFO (f)->mouse_face_defer = false;
}

/* Everything redisplay had to draw is drawn.  What the pointer is over
   may have changed under it while that went on -- either because it
   moved and the highlight was deferred, or because the text moved under
   it -- and nothing else looks again once it has stopped moving.  */

static void
host_frame_up_to_date (struct frame *f)
{
  eassert (FRAME_HOST_P (f));

  if (!host_highlight_after_update)
    return;

  block_input ();
  FRAME_MOUSE_UPDATE (f);
  /* Whatever lit up is drawn into the picture and nothing else would
     hand it over: the update it belongs to is over.  This costs nothing
     where nothing was drawn.  */
  flush_frame (f);
  unblock_input ();
}

static struct redisplay_interface host_redisplay_interface =
  {
    host_frame_parm_handlers,
    gui_produce_glyphs,
    gui_write_glyphs,
    NULL, /* insert_glyphs, which window-based update does not use.  */
    gui_clear_end_of_line,
    host_scroll_run,
    host_after_update_window_line,
    NULL, /* update_window_begin */
    NULL, /* update_window_end */
    host_show_picture,
    gui_clear_window_mouse_face,
    gui_get_glyph_overhangs,
    gui_fix_overlapping_area,
    host_draw_fringe_bitmap,
    NULL, /* define_fringe_bitmap */
    NULL, /* destroy_fringe_bitmap */
    host_compute_glyph_string_overhangs,
    host_draw_glyph_string,
    host_define_frame_cursor,
    host_clear_frame_area,
    NULL, /* clear_under_internal_border */
    host_draw_window_cursor,
    host_draw_vertical_window_border,
    host_draw_window_divider,
    NULL, /* shift_glyphs_for_insert */
    NULL, /* show_hourglass */
    NULL, /* hide_hourglass */
    host_default_font_parameter,
  };

/* The display.  */

static struct terminal *
host_create_terminal (struct host_display_info *dpyinfo)
{
  struct terminal *terminal
    = create_terminal (output_host, &host_redisplay_interface);

  terminal->display_info.host = dpyinfo;
  dpyinfo->terminal = terminal;
  terminal->kboard = allocate_kboard (Qhost);

  terminal->update_end_hook = host_update_end;
  terminal->frame_up_to_date_hook = host_frame_up_to_date;
  terminal->defined_color_hook = host_defined_color;
  terminal->query_frame_background_color = host_query_frame_background_color;
  terminal->get_string_resource_hook = host_get_string_resource;
  terminal->set_new_font_hook = host_new_font;
  terminal->set_window_size_hook = host_set_window_size;
  terminal->frame_visible_invisible_hook = host_set_frame_visible_invisible;
  terminal->set_frame_offset_hook = host_set_offset;
  terminal->focus_frame_hook = host_focus_frame;
  terminal->iconify_frame_hook = host_iconify_frame;
  terminal->delete_frame_hook = host_delete_frame;
  terminal->menu_show_hook = host_menu_show;
  terminal->read_socket_hook = host_read_socket;
  terminal->mouse_position_hook = host_mouse_position;
  terminal->get_focus_frame = host_get_focus_frame;
  terminal->frame_rehighlight_hook = host_frame_rehighlight;
  /* TODO: asking the host to bring its window to the front, which
     focus_frame_hook does not: it only sends the keys on to a child
     frame, since the host has the focus and Emacs follows it.  */

  return terminal;
}

/* Open the display, once: there is one host, and one display.  */

struct host_display_info *
host_term_init (void)
{
  struct host_display_info *dpyinfo;
  struct terminal *terminal;
  Lisp_Object color_file, color_map;

  if (x_display_list)
    return x_display_list;

  block_input ();

  color_file = Fexpand_file_name (build_string ("rgb.txt"),
				  Fsymbol_value (intern ("data-directory")));
  color_map = Fx_load_color_file (color_file);
  if (NILP (color_map))
    fatal ("Could not read %s.\n", SDATA (color_file));

  dpyinfo = xzalloc (sizeof *dpyinfo);
  dpyinfo->color_map = color_map;
  dpyinfo->n_planes = 24;
  /* What a host's fonts are sized for, before a host says otherwise.
     TODO: take it from the host's scale.  */
  dpyinfo->resx = 96;
  dpyinfo->resy = 96;
  dpyinfo->name_list_element = Fcons (build_string ("host"), Qnil);
  dpyinfo->smallest_font_height = 1;
  dpyinfo->smallest_char_width = 1;
  dpyinfo->next = x_display_list;
  x_display_list = dpyinfo;

  terminal = host_create_terminal (dpyinfo);
  if (current_kboard == initial_kboard)
    current_kboard = terminal->kboard;
  terminal->kboard->reference_count++;
  /* The display is never closed, so it is never deleted.  */
  terminal->reference_count++;
  terminal->name = xstrdup ("host");

  gui_init_fringe (terminal->rif);
  host_input_init ();
  unblock_input ();

  host_enumerate_fonts ();

  return dpyinfo;
}

void
mark_host_display (void)
{
  if (x_display_list)
    {
      mark_object (x_display_list->name_list_element);
      mark_object (x_display_list->color_map);
    }

  mark_sfntfont ();
}

void
syms_of_hostterm (void)
{
  /* That this is a host build, for loadup.el to load host-win.el.  */
  Fprovide (Qhost, Qnil);

  syms_of_hostfont ();

  /* Every window system has these, and cus-start.el expects them of
     one that has `x-create-frame'.  */
  DEFVAR_BOOL ("host-highlight-after-update", host_highlight_after_update,
	       doc: /* Whether to look under the pointer again after drawing.
The text under a pointer that has not moved changes all the same, as
when the window scrolls, and nothing else would light up what is under
it then.  Looking costs a little of every redisplay, and drawing what
it finds costs a picture handed to the host.  */);
  host_highlight_after_update = true;

  DEFVAR_BOOL ("host-draw-tab-lines", host_draw_tab_lines,
    doc: /* Whether Emacs draws the tab line of a window.
Nil leaves the room it keeps for one alone, for a host that draws
something of its own there.  Emacs keeps the room either way: how much
there is is the face `tab-line''s to say.  */);
  host_draw_tab_lines = true;

  DEFVAR_BOOL ("x-use-underline-position-properties",
	       x_use_underline_position_properties,
     doc: /* SKIP: real doc in xterm.c.  */);
  x_use_underline_position_properties = true;
  DEFSYM (Qx_use_underline_position_properties,
	  "x-use-underline-position-properties");

  DEFVAR_BOOL ("x-underline-at-descent-line",
	       x_underline_at_descent_line,
     doc: /* SKIP: real doc in xterm.c.  */);
  x_underline_at_descent_line = false;
  DEFSYM (Qx_underline_at_descent_line, "x-underline-at-descent-line");
}
