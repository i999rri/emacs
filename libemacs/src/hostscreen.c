/* Reading back the screen Emacs drew, for a host application.

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

/* A host application draws the screen itself, in whatever it draws
   with, and Lisp tells it what to draw.  Lisp could work that out from
   the buffer, but it would be working out again what redisplay has
   just finished working out, and would reach a different answer: where
   the lines were broken, what an overlay or a display property put
   there instead of the text, which font a character was found in, what
   is hidden.  Redisplay decided all of it and left the decision in the
   glyph matrix of each window.  This reads it back.  */

#include <config.h>

#include "lisp.h"
#include "character.h"
#include "buffer.h"
#include "frame.h"
#include "window.h"
#include "dispextern.h"
#include "font.h"
#include "hostlib.h"

/* For what the pointer is over, which each window system keeps in the
   display it belongs to.  A frame on no window system keeps it in its
   terminal, and MOUSE_HL_INFO reaches for both.  */
#include "termchar.h"
#ifdef HAVE_WINDOW_SYSTEM
#include TERM_HEADER
#endif

/* The face a run was drawn in, as the attributes that decide how it
   looks.  The family and the size are of the font redisplay settled
   on, which is not the family of the face when the face has no glyph
   for the character and the fontset found one elsewhere.  */

static Lisp_Object
host_run_face (struct frame *f, int face_id)
{
  struct face *face = FACE_FROM_ID_OR_NULL (f, face_id);
  Lisp_Object family = Qnil, size = Qnil;

  if (!face)
    return Qnil;

  if (face->font)
    {
      Lisp_Object name = face->font->props[FONT_FAMILY_INDEX];

      if (SYMBOLP (name))
	family = SYMBOL_NAME (name);
      size = make_fixnum (face->font->pixel_size);
    }

  return list (QCforeground, face->lface[LFACE_FOREGROUND_INDEX],
	       QCbackground, face->lface[LFACE_BACKGROUND_INDEX],
	       QCweight, face->lface[LFACE_WEIGHT_INDEX],
	       QCslant, face->lface[LFACE_SLANT_INDEX],
	       QCunderline, face->lface[LFACE_UNDERLINE_INDEX],
	       QCfamily, family,
	       QCsize, size);
}

/* Which of the glyphs of a row the pointer is over, as columns from
   FROM up to but not including TO, and the face they are drawn in.

   Redisplay does not put that face in the matrix: it draws the glyphs
   again with it when the pointer arrives and puts the old ones back
   when it leaves, which a host drawing from the matrix never sees.  */

struct host_mouse_face
{
  int from, to, face_id;
};

static struct host_mouse_face
host_row_mouse_face (struct frame *f, Lisp_Object window, int vpos, int used)
{
  Mouse_HLInfo *hlinfo = MOUSE_HL_INFO (f);
  struct host_mouse_face over = { -1, -1, DEFAULT_FACE_ID };

  if (hlinfo->mouse_face_hidden
      || !EQ (hlinfo->mouse_face_window, window)
      || vpos < hlinfo->mouse_face_beg_row
      || vpos > hlinfo->mouse_face_end_row)
    return over;

  over.from = (vpos == hlinfo->mouse_face_beg_row
	       ? hlinfo->mouse_face_beg_col : 0);
  over.to = (vpos == hlinfo->mouse_face_end_row && !hlinfo->mouse_face_past_end
	     ? hlinfo->mouse_face_end_col : used);
  over.face_id = hlinfo->mouse_face_face_id;
  return over;
}

/* The face glyph AT of a row is drawn in, which is the mouse face
   where the pointer is over it.  */

static int
host_glyph_face (struct glyph *glyphs, int at, struct host_mouse_face over)
{
  return (over.from >= 0 && at >= over.from && at < over.to
	  ? over.face_id : glyphs[at].face_id);
}

/* One run of text drawn at X, WIDTH pixels wide, in the face FACE_ID.
   TEXT is NULL for a run that is a blank of that width, which is what
   a stretch of space or a character with no glyph comes to until there
   is something better to do with them.  */

static Lisp_Object
host_run (struct frame *f, int face_id, int x, int width,
	  char const *text, ptrdiff_t nchars, ptrdiff_t nbytes)
{
  return nconc2 (list (QCtext,
		       text ? make_multibyte_string (text, nchars, nbytes)
			    : Qnil,
		       QCx, make_fixnum (x),
		       QCwidth, make_fixnum (width)),
		 host_run_face (f, face_id));
}

/* The image GLYPH shows, as a run of its own: the image's spec, which
   says where its data is, and how far its top is above the baseline
   and how tall it is, which say where on the line it goes.  */

static Lisp_Object
host_image_run (struct frame *f, struct glyph *glyph, int x)
{
  Lisp_Object run = host_run (f, glyph->face_id, x, glyph->pixel_width,
			      NULL, 0, 0);
#ifdef HAVE_WINDOW_SYSTEM
  struct image *img = IMAGE_OPT_FROM_ID (f, glyph->u.img_id);

  if (img)
    run = nconc2 (run, list (QCimage, img->spec,
			     QCascent, make_fixnum (glyph->ascent),
			     QCheight, make_fixnum (glyph->ascent
						    + glyph->descent)));
#endif
  return run;
}

/* What kind of line ROW is, the Ith of MATRIX.  The mode line and the
   lines above the text are not text, and a host may well want to put
   them somewhere else entirely.  */

static Lisp_Object
host_row_kind (struct glyph_matrix *matrix, struct glyph_row *row)
{
  if (!row->mode_line_p)
    return Qtext;
  if (matrix->tab_line_p && row == MATRIX_TAB_LINE_ROW (matrix))
    return Qtab_line;
  if (row == MATRIX_MODE_LINE_ROW (matrix))
    return Qmode_line;

  return Qheader_line;
}

DEFUN ("window-screen-rows", Fwindow_screen_rows, Swindow_screen_rows,
       0, 1, 0,
       doc: /* Return the rows WINDOW is showing, as Emacs drew them.

Redisplay decides what every line of a window holds: where the text was
broken, what it was drawn in, what an overlay or a display property put
there instead of the text, which font each character was found in.
This is that decision, read back, so that something other than Emacs
can draw the same screen.

The value is a list of lines, from the top of WINDOW down.  Each line
is a plist:

  :y       where it is, in pixels below the top of WINDOW
  :height  how tall it is
  :ascent  how far its baseline is below its top
  :line-spacing        how much of the height is space between lines
  :line-spacing-above  how much of that space is above the text; the
                       rest is below it
  :kind    `text', `mode-line', `header-line' or `tab-line'
  :start   where in the buffer it begins, or nil if it is not text
  :runs    what is on it

Each run is a plist of :text, :x and :width, followed by the face it was
drawn in as :foreground, :background, :weight, :slant, :underline,
:family and :size.  X and Y both count from the top left corner of
WINDOW.  A run whose :text is nil is a blank that wide, which is what a
stretch of space or a character the font has no glyph for comes to.

An image is a run of its own, with :image, the spec it was made from,
and :ascent and :height, how far its top is above the baseline and how
tall it is.

WINDOW defaults to the selected one.  The value is nil if Emacs has not
drawn WINDOW yet.  */)
  (Lisp_Object window)
{
  struct window *w = decode_live_window (window);
  struct frame *f = XFRAME (w->frame);
  struct glyph_matrix *matrix = w->current_matrix;
  Lisp_Object window_object = Qnil;
  Lisp_Object lines = Qnil;

  XSETWINDOW (window_object, w);
  char *text;
  USE_SAFE_ALLOCA;

  if (!matrix || !matrix->rows)
    return Qnil;

  /* A line holds no more characters than it has glyphs, and no row has
     more glyphs than the longest.  The matrix says how wide it is, but
     says it of the frame it was made for rather than of the rows it
     ended up with, so the rows are asked instead.  */
  int widest = 0;

  for (int i = 0; i < matrix->nrows; ++i)
    {
      int used = MATRIX_ROW (matrix, i)->used[TEXT_AREA];

      if (used > widest)
	widest = used;
    }
  SAFE_NALLOCA (text, MAX_MULTIBYTE_LENGTH, widest + 1);

  for (int i = 0; i < matrix->nrows; ++i)
    {
      struct glyph_row *row = MATRIX_ROW (matrix, i);
      struct glyph *glyphs = row->glyphs[TEXT_AREA];
      int used = row->used[TEXT_AREA];
      Lisp_Object runs = Qnil;
      /* A line of text begins where the text of the window begins,
	 past the margin and the fringe; a mode line is as wide as the
	 window and begins at its edge.  Both are given from the edge,
	 so that what draws them has one origin to draw from.  */
      int x = row->x + (row->full_width_p
			? 0 : window_box_left_offset (w, TEXT_AREA));
      int start = 0;
      struct host_mouse_face over;

      if (!row->enabled_p)
	continue;

      over = host_row_mouse_face (f, window_object, i, used);

      /* A run ends where the face changes, where text gives way to what
	 is not text or the other way about, and where the characters
	 stop being the same width: what draws the run has to space its
	 characters the way Emacs spaced them, and can only do that for
	 one width at a time.  */
      while (start < used)
	{
	  int face_id = host_glyph_face (glyphs, start, over);
	  bool texts = glyphs[start].type == CHAR_GLYPH;

	  if (glyphs[start].type == IMAGE_GLYPH)
	    {
	      runs = Fcons (host_image_run (f, &glyphs[start], x), runs);
	      x += glyphs[start].pixel_width;
	      start++;
	      continue;
	    }

	  short advance = glyphs[start].pixel_width;
	  ptrdiff_t nchars = 0, nbytes = 0;
	  int width = 0;
	  int end = start;

	  while (end < used
		 && host_glyph_face (glyphs, end, over) == face_id
		 && glyphs[end].type != IMAGE_GLYPH
		 && (glyphs[end].type == CHAR_GLYPH) == texts
		 && (!texts || glyphs[end].pixel_width == advance))
	    {
	      /* A padding glyph is the rest of a character that is
		 already there, and takes no room of its own.  */
	      if (texts && !glyphs[end].padding_p)
		{
		  nbytes += CHAR_STRING (glyphs[end].u.ch,
					 (unsigned char *) text + nbytes);
		  nchars++;
		}
	      width += glyphs[end].pixel_width;
	      end++;
	    }

	  runs = Fcons (host_run (f, face_id, x, width,
				  texts ? text : NULL, nchars, nbytes),
			runs);
	  x += width;
	  start = end;
	}

      lines = Fcons (list (QCy, make_fixnum (row->y),
			   QCheight, make_fixnum (row->height),
			   QCascent, make_fixnum (row->ascent),
			   QCline_spacing, make_fixnum (row->extra_line_spacing),
			   QCline_spacing_above,
			   make_fixnum (row->extra_line_spacing_above),
			   QCkind, host_row_kind (matrix, row),
			   /* Which line of the buffer this is, so that
			      one that has only scrolled is known to be
			      the line it was.  */
			   QCstart, (row->mode_line_p
				     ? Qnil
				     : make_int (MATRIX_ROW_START_CHARPOS (row))),
			   QCruns, Fnreverse (runs)),
		     lines);
    }

  SAFE_FREE ();
  return Fnreverse (lines);
}

DEFUN ("window-screen-cursor", Fwindow_screen_cursor, Swindow_screen_cursor,
       0, 1, 0,
       doc: /* Return where the cursor is in WINDOW, as Emacs drew it.

The value is a plist of :x, :y, :width and :height, in the pixels and
from the corner `window-screen-rows' counts in, or nil if the cursor
is not in WINDOW.  :height is the whole line's, and :line-spacing and
:line-spacing-above say how much of it is space between lines, as they
do for a row, for a cursor as tall as the text and not the line.

WINDOW defaults to the selected one.  */)
  (Lisp_Object window)
{
  struct window *w = decode_live_window (window);
  struct frame *f = XFRAME (w->frame);
  struct glyph_matrix *matrix = w->current_matrix;
  struct glyph_row *row;
  int width = FRAME_COLUMN_WIDTH (f);

  if (!matrix || !matrix->rows
      || w->cursor.vpos < 0 || w->cursor.vpos >= matrix->nrows)
    return Qnil;

  row = MATRIX_ROW (matrix, w->cursor.vpos);
  if (!row->enabled_p)
    return Qnil;

  /* As wide as what it sits on, so that it covers a wide character the
     way Emacs covers one.  */
  if (w->cursor.hpos >= 0 && w->cursor.hpos < row->used[TEXT_AREA])
    {
      int glyph_width = row->glyphs[TEXT_AREA][w->cursor.hpos].pixel_width;

      if (glyph_width > 0)
	width = glyph_width;
    }

  return list (QCx, make_fixnum (w->cursor.x
				 + window_box_left_offset (w, TEXT_AREA)),
	       QCy, make_fixnum (w->cursor.y),
	       QCwidth, make_fixnum (width),
	       QCheight, make_fixnum (row->height),
	       QCline_spacing, make_fixnum (row->extra_line_spacing),
	       QCline_spacing_above,
	       make_fixnum (row->extra_line_spacing_above));
}

DEFUN ("frame-screen-pointer", Fframe_screen_pointer, Sframe_screen_pointer,
       0, 1, 0,
       doc: /* Return the shape the pointer is to take over FRAME.

Redisplay chooses it as the pointer moves: a hand over something that
can be clicked, a bar over text, an arrow elsewhere.  Drawing it is the
host's, which has the pointer; this is what to tell it.

One of `arrow', `text', `hand', `busy', `horizontal-drag',
`vertical-drag', or the name of an edge or a corner.  FRAME defaults to
the selected one.  */)
  (Lisp_Object frame)
{
#ifdef HAVE_HOST
  struct frame *f = decode_live_frame (frame);

  if (!FRAME_HOST_P (f))
    return Qnil;

  switch (HOST_POINTER_OF (FRAME_OUTPUT_DATA (f)->current_cursor))
    {
    case HOST_POINTER_ARROW: return Qarrow;
    case HOST_POINTER_TEXT: return Qtext;
    case HOST_POINTER_HAND: return Qhand;
    case HOST_POINTER_BUSY: return Qbusy;
    case HOST_POINTER_HORIZONTAL_DRAG: return Qhorizontal_drag;
    case HOST_POINTER_VERTICAL_DRAG: return Qvertical_drag;
    case HOST_POINTER_LEFT_EDGE: return Qleft_edge;
    case HOST_POINTER_TOP_LEFT_CORNER: return Qtop_left_corner;
    case HOST_POINTER_TOP_EDGE: return Qtop_edge;
    case HOST_POINTER_TOP_RIGHT_CORNER: return Qtop_right_corner;
    case HOST_POINTER_RIGHT_EDGE: return Qright_edge;
    case HOST_POINTER_BOTTOM_RIGHT_CORNER: return Qbottom_right_corner;
    case HOST_POINTER_BOTTOM_EDGE: return Qbottom_edge;
    case HOST_POINTER_BOTTOM_LEFT_CORNER: return Qbottom_left_corner;
    default: return Qnil;
    }
#else
  return Qnil;
#endif
}

void
syms_of_hostscreen (void)
{
  DEFSYM (QCtext, ":text");
  /* Also image.c's, which only a build with a window system has.  */
  DEFSYM (QCascent, ":ascent");
  DEFSYM (QCx, ":x");
  DEFSYM (QCy, ":y");
  DEFSYM (QCruns, ":runs");
  DEFSYM (QCkind, ":kind");
  DEFSYM (QCstart, ":start");
  DEFSYM (Qarrow, "arrow");
  DEFSYM (Qtext, "text");
  DEFSYM (Qhand, "hand");
  DEFSYM (Qbusy, "busy");
  DEFSYM (Qhorizontal_drag, "horizontal-drag");
  DEFSYM (Qvertical_drag, "vertical-drag");
  DEFSYM (Qleft_edge, "left-edge");
  DEFSYM (Qtop_left_corner, "top-left-corner");
  DEFSYM (Qtop_edge, "top-edge");
  DEFSYM (Qtop_right_corner, "top-right-corner");
  DEFSYM (Qright_edge, "right-edge");
  DEFSYM (Qbottom_right_corner, "bottom-right-corner");
  DEFSYM (Qbottom_edge, "bottom-edge");
  DEFSYM (Qbottom_left_corner, "bottom-left-corner");
  DEFSYM (QCline_spacing, ":line-spacing");
  DEFSYM (QCline_spacing_above, ":line-spacing-above");

  defsubr (&Swindow_screen_rows);
  defsubr (&Swindow_screen_cursor);
  defsubr (&Sframe_screen_pointer);
}
