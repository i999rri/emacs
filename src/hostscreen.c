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

/* One run of text drawn at X, WIDTH pixels wide, in the face FACE_ID.
   TEXT is NULL for a run that is a blank of that width, which is what
   a stretch of space, an image, or a character with no glyph comes to
   until there is something better to do with them.  */

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
stretch of space, an image, or a character the font has no glyph for
comes to.

WINDOW defaults to the selected one.  The value is nil if Emacs has not
drawn WINDOW yet.  */)
  (Lisp_Object window)
{
  struct window *w = decode_live_window (window);
  struct frame *f = XFRAME (w->frame);
  struct glyph_matrix *matrix = w->current_matrix;
  Lisp_Object lines = Qnil;
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

      if (!row->enabled_p)
	continue;

      /* A run ends where the face changes, where text gives way to what
	 is not text or the other way about, and where the characters
	 stop being the same width: what draws the run has to space its
	 characters the way Emacs spaced them, and can only do that for
	 one width at a time.  */
      while (start < used)
	{
	  int face_id = glyphs[start].face_id;
	  bool texts = glyphs[start].type == CHAR_GLYPH;
	  short advance = glyphs[start].pixel_width;
	  ptrdiff_t nchars = 0, nbytes = 0;
	  int width = 0;
	  int end = start;

	  while (end < used
		 && glyphs[end].face_id == face_id
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
is not in WINDOW.

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
	       QCheight, make_fixnum (row->height));
}

void
syms_of_hostscreen (void)
{
  DEFSYM (QCtext, ":text");
  DEFSYM (QCx, ":x");
  DEFSYM (QCy, ":y");
  DEFSYM (QCruns, ":runs");
  DEFSYM (QCkind, ":kind");
  DEFSYM (QCstart, ":start");
  DEFSYM (QCline_spacing, ":line-spacing");
  DEFSYM (QCline_spacing_above, ":line-spacing-above");

  defsubr (&Swindow_screen_rows);
  defsubr (&Swindow_screen_cursor);
}
