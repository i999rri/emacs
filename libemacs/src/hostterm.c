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

#include "lisp.h"
#include "blockinput.h"
#include "keyboard.h"
#include "termhooks.h"
#include "window.h"
#include "buffer.h"
#include "fontset.h"
#include "hostterm.h"

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

static void
host_scroll_run (struct window *w, struct run *run)
{
  /* The matrices already hold the rows where they moved to.  */
}

static void
host_after_update_window_line (struct window *w,
			       struct glyph_row *desired_row)
{
}

static void
host_draw_fringe_bitmap (struct window *w, struct glyph_row *row,
			 struct draw_fringe_bitmap_params *p)
{
}

static void
host_draw_glyph_string (struct glyph_string *s)
{
}

static void
host_define_frame_cursor (struct frame *f, Emacs_Cursor cursor)
{
}

static void
host_clear_frame_area (struct frame *f, int x, int y, int width, int height)
{
}

static void
host_draw_window_cursor (struct window *w, struct glyph_row *row,
			 int x, int y, enum text_cursor_kinds cursor_type,
			 int cursor_width, bool on_p, bool active_p)
{
  /* Where the cursor is was recorded in W before this was called,
     which is what the host reads.  */
  if (on_p)
    {
      w->phys_cursor_type = cursor_type;
      w->phys_cursor_on_p = true;
    }
}

static void
host_draw_vertical_window_border (struct window *w, int x, int y_0, int y_1)
{
}

static void
host_draw_window_divider (struct window *w, int x_0, int x_1,
			  int y_0, int y_1)
{
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

      if (NILP (font))
	error ("No font was found in `host-font-directories'");
    }

  gui_default_parameter (f, parms, Qfont, font, "font", "Font",
			 RES_TYPE_STRING);
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
    NULL, /* flush_display */
    gui_clear_window_mouse_face,
    gui_get_glyph_overhangs,
    gui_fix_overlapping_area,
    host_draw_fringe_bitmap,
    NULL, /* define_fringe_bitmap */
    NULL, /* destroy_fringe_bitmap */
    NULL, /* compute_glyph_string_overhangs */
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

  terminal->defined_color_hook = host_defined_color;
  terminal->query_frame_background_color = host_query_frame_background_color;
  terminal->get_string_resource_hook = host_get_string_resource;
  terminal->set_new_font_hook = host_new_font;
  terminal->set_window_size_hook = host_set_window_size;
  terminal->frame_visible_invisible_hook = host_set_frame_visible_invisible;
  terminal->iconify_frame_hook = host_iconify_frame;
  terminal->delete_frame_hook = host_delete_frame;
  terminal->menu_show_hook = host_menu_show;
  terminal->read_socket_hook = host_read_socket;
  terminal->mouse_position_hook = host_mouse_position;
  terminal->get_focus_frame = host_get_focus_frame;
  terminal->frame_rehighlight_hook = host_frame_rehighlight;
  /* TODO: focus_frame_hook, to ask the host for the focus; the host
     has it, and Emacs can only follow where it goes.  */

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

  /* Every window system has these, and cus-start.el expects them of
     one that has `x-create-frame'.  */
  DEFVAR_BOOL ("x-use-underline-position-properties",
	       x_use_underline_position_properties,
     doc: /* SKIP: real doc in xterm.c.  */);
  x_use_underline_position_properties = true;

  DEFVAR_BOOL ("x-underline-at-descent-line",
	       x_underline_at_descent_line,
     doc: /* SKIP: real doc in xterm.c.  */);
  x_underline_at_descent_line = false;
}
