/* Frames that a host application draws: making them, and their
   parameters.

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

/* `make-frame' on the `host' window system, and what Lisp asks of a
   window system's display under the names X gave it (`x-open-connection',
   `xw-color-defined-p' and the rest), which faces.el and frame.el call
   whenever `display-graphic-p' is true.  A frame here is only its
   parameters and its layout: there is no window to make, to place or
   to show.  */

#include <config.h>

#include <math.h>

#include "lisp.h"
#include "blockinput.h"
#include "keyboard.h"
#include "termhooks.h"
#include "window.h"
#include "buffer.h"
#include "fontset.h"
#include "hostterm.h"

/* The display OBJECT names: a terminal, a frame, a display name or
   nil for the selected frame's.  There is only one, and it is opened
   on first use.  */

struct host_display_info *
check_x_display_info (Lisp_Object object)
{
  if (NILP (object) || STRINGP (object))
    return host_term_init ();

  if (TERMINALP (object))
    {
      struct terminal *t = decode_live_terminal (object);

      if (t->type != output_host)
	error ("Terminal %d is not a host display", t->id);

      return t->display_info.host;
    }

  return FRAME_DISPLAY_INFO (decode_window_system_frame (object));
}

void
frame_set_mouse_pixel_position (struct frame *f, int pix_x, int pix_y)
{
  /* The pointer is the host's, and Emacs cannot move it.  */
}

void
gamma_correct (struct frame *f, Emacs_Color *color)
{
  if (f->gamma)
    {
      color->red = pow (color->red / 65535.0, f->gamma) * 65535.0 + 0.5;
      color->green = pow (color->green / 65535.0, f->gamma) * 65535.0 + 0.5;
      color->blue = pow (color->blue / 65535.0, f->gamma) * 65535.0 + 0.5;
      color->pixel = RGB_TO_ULONG (color->red / 256, color->green / 256,
				   color->blue / 256);
    }
}

/* Images.  image.c reads and writes pixels through these, but nothing
   makes a host pixmap yet.  TODO: images, which the host is to draw
   (see image_create_x_image_and_pixmap_1 in image.c).  */

unsigned long
host_get_pixel (struct host_pixmap *pixmap, int x, int y)
{
  return pixmap->pixels[y * pixmap->width + x];
}

void
host_put_pixel (struct host_pixmap *pixmap, int x, int y,
		unsigned long pixel)
{
  pixmap->pixels[y * pixmap->width + x] = pixel;
}

/* Frame parameters.  */

static unsigned long
host_decode_color (struct frame *f, Lisp_Object color_name)
{
  Emacs_Color color;

  CHECK_STRING (color_name);

  if (host_get_color (SSDATA (color_name), &color))
    return color.pixel;

  signal_error ("Undefined color", color_name);
}

static void
host_set_foreground_color (struct frame *f, Lisp_Object arg,
			   Lisp_Object oldval)
{
  unsigned long old_fg = FRAME_FOREGROUND_PIXEL (f);

  FRAME_FOREGROUND_PIXEL (f) = host_decode_color (f, arg);
  if (FRAME_OUTPUT_DATA (f)->cursor_pixel == old_fg)
    FRAME_OUTPUT_DATA (f)->cursor_pixel = FRAME_FOREGROUND_PIXEL (f);

  update_face_from_frame_parameter (f, Qforeground_color, arg);
  if (FRAME_VISIBLE_P (f))
    redraw_frame (f);
}

static void
host_set_background_color (struct frame *f, Lisp_Object arg,
			   Lisp_Object oldval)
{
  FRAME_BACKGROUND_PIXEL (f) = host_decode_color (f, arg);

  update_face_from_frame_parameter (f, Qbackground_color, arg);
  if (FRAME_VISIBLE_P (f))
    redraw_frame (f);
}

static void
host_set_cursor_color (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  FRAME_OUTPUT_DATA (f)->cursor_pixel = host_decode_color (f, arg);

  update_face_from_frame_parameter (f, Qcursor_color, arg);
  if (FRAME_VISIBLE_P (f))
    {
      gui_update_cursor (f, false);
      gui_update_cursor (f, true);
    }
}

static void
host_set_cursor_type (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  set_frame_cursor_types (f, arg);
}

static void
host_set_internal_border_width (struct frame *f, Lisp_Object arg,
				Lisp_Object oldval)
{
  int border = check_int_nonnegative (arg);

  if (border != FRAME_INTERNAL_BORDER_WIDTH (f))
    {
      f->internal_border_width = border;
      adjust_frame_size (f, -1, -1, 3, false, Qinternal_border_width);
    }
}

/* The menu bar is Emacs's own, laid out in the frame's menu bar
   window as on a text terminal, so that the host reads it from the
   matrices like the rest of the frame.  */

static void
host_set_menu_bar_lines (struct frame *f, Lisp_Object value,
			 Lisp_Object oldval)
{
  int olines = FRAME_MENU_BAR_LINES (f);
  int nlines;

  if (FRAME_MINIBUF_ONLY_P (f) || FRAME_PARENT_FRAME (f))
    return;

  nlines = TYPE_RANGED_FIXNUMP (int, value) ? XFIXNUM (value) : 0;

  fset_redisplay (f);
  FRAME_MENU_BAR_LINES (f) = nlines;
  FRAME_MENU_BAR_HEIGHT (f) = nlines * FRAME_LINE_HEIGHT (f);
  if (nlines != olines)
    adjust_frame_size (f, -1, -1, 3, true, Qmenu_bar_lines);
}

/* Set the name of frame F to NAME.  EXPLICIT means that Lisp asked for
   it, rather than redisplay suggesting it from `frame-title-format',
   and a name Lisp asked for is kept until Lisp says otherwise.  */

static void
host_set_name (struct frame *f, Lisp_Object name, bool explicit)
{
  if (explicit)
    {
      if (f->explicit_name && NILP (name))
	update_mode_lines = 37;
      f->explicit_name = !NILP (name);
    }
  else if (f->explicit_name)
    return;

  if (NILP (name))
    name = Vinvocation_name;
  CHECK_STRING (name);

  if (!NILP (Fstring_equal (name, f->name)))
    return;

  fset_name (f, name);
  /* TODO: tell the host the title, when it is told of frames.  */
}

static void
host_explicitly_set_name (struct frame *f, Lisp_Object arg,
			  Lisp_Object oldval)
{
  host_set_name (f, arg, true);
}

static void
host_set_title (struct frame *f, Lisp_Object name, Lisp_Object old_name)
{
  if (EQ (name, f->title))
    return;

  update_mode_lines = 38;
  fset_title (f, name);
}

/* In `frame_parms' order (frame.c).  Null entries are parameters a
   host frame does not have yet, or ever: X borders, scroll bars,
   icons, the tool bar and window-manager hints.  */

frame_parm_handler host_frame_parm_handlers[] =
  {
    gui_set_autoraise,
    gui_set_autolower,
    host_set_background_color,
    NULL, /* border-color */
    gui_set_border_width,
    host_set_cursor_color,
    host_set_cursor_type,
    gui_set_font,
    host_set_foreground_color,
    NULL, /* icon-name */
    NULL, /* icon-type */
    NULL, /* child-frame-border-width */
    host_set_internal_border_width,
    gui_set_right_divider_width,
    gui_set_bottom_divider_width,
    host_set_menu_bar_lines,
    NULL, /* mouse-color */
    host_explicitly_set_name,
    gui_set_scroll_bar_width,
    gui_set_scroll_bar_height,
    host_set_title,
    gui_set_unsplittable,
    gui_set_vertical_scroll_bars,
    gui_set_horizontal_scroll_bars,
    gui_set_visibility,
    NULL, /* tab-bar-lines; TODO */
    NULL, /* tool-bar-lines; TODO, with images */
    NULL, /* scroll-bar-foreground */
    NULL, /* scroll-bar-background */
    gui_set_screen_gamma,
    gui_set_line_spacing,
    gui_set_left_fringe,
    gui_set_right_fringe,
    NULL, /* wait-for-wm */
    gui_set_fullscreen,
    gui_set_font_backend,
    NULL, /* alpha; TODO, for the host to draw with */
    NULL, /* sticky */
    NULL, /* tool-bar-position */
    NULL, /* inhibit-double-buffering */
    NULL, /* undecorated */
    NULL, /* parent-frame; TODO, child frames */
    NULL, /* skip-taskbar */
    NULL, /* no-focus-on-map */
    NULL, /* no-accept-focus */
    NULL, /* z-group */
    NULL, /* override-redirect */
    gui_set_no_special_glyphs,
    NULL, /* alpha-background */
    NULL, /* borders-respect-alpha-background */
    NULL, /* use-frame-synchronization */
  };

/* Making a frame.  */

static Lisp_Object
unwind_create_frame (Lisp_Object frame)
{
  struct frame *f = XFRAME (frame);

  /* A frame that did not become official is freed again; one that did
     is deleted as any frame is.  */
  if (FRAME_LIVE_P (f) && NILP (Fmemq (frame, Vframe_list)))
    {
      free_glyphs (f);
      xfree (f->output_data.host);
      f->output_data.host = NULL;
      return Qt;
    }

  return Qnil;
}

static void
do_unwind_create_frame (Lisp_Object frame)
{
  unwind_create_frame (frame);
}

DEFUN ("x-create-frame", Fx_create_frame, Sx_create_frame,
       1, 1, 0,
       doc: /* Make a new frame, which a host application draws.
Return the frame.  PARMS is an alist of frame parameters.  */)
  (Lisp_Object parms)
{
  struct frame *f;
  Lisp_Object frame, tem, name, display;
  bool minibuffer_only = false;
  specpdl_ref count = SPECPDL_INDEX ();
  struct host_display_info *dpyinfo;
  struct kboard *kb;

  parms = Fcopy_alist (parms);
  Vx_resource_name = Vinvocation_name;

  display = gui_display_get_arg (NULL, parms, Qterminal, 0, 0,
				 RES_TYPE_STRING);
  if (BASE_EQ (display, Qunbound))
    display = Qnil;
  dpyinfo = check_x_display_info (display);
  kb = dpyinfo->terminal->kboard;

  if (!dpyinfo->terminal->name)
    error ("Terminal is not live, can't create new frames on it");

  name = gui_display_get_arg (dpyinfo, parms, Qname, 0, 0, RES_TYPE_STRING);
  if (!STRINGP (name) && !BASE_EQ (name, Qunbound) && !NILP (name))
    error ("Invalid frame name--not a string or nil");
  if (STRINGP (name))
    Vx_resource_name = name;

  tem = gui_display_get_arg (dpyinfo, parms, Qminibuffer,
			     "minibuffer", "Minibuffer", RES_TYPE_SYMBOL);
  if (EQ (tem, Qnone) || NILP (tem))
    f = make_frame_without_minibuffer (Qnil, kb, display);
  else if (EQ (tem, Qonly))
    {
      f = make_minibuffer_frame ();
      minibuffer_only = true;
    }
  else if (WINDOWP (tem))
    f = make_frame_without_minibuffer (tem, kb, display);
  else
    f = make_frame (true);

  XSETFRAME (frame, f);
  frame_set_id_from_params (f, parms);

  f->terminal = dpyinfo->terminal;
  f->output_method = output_host;
  f->output_data.host = xzalloc (sizeof *f->output_data.host);
  FRAME_DISPLAY_INFO (f) = dpyinfo;
  FRAME_FONTSET (f) = -1;

  fset_icon_name (f, gui_display_get_arg (dpyinfo, parms, Qicon_name,
					  "iconName", "Title",
					  RES_TYPE_STRING));
  if (!STRINGP (f->icon_name))
    fset_icon_name (f, Qnil);

  /* With the display set up, this unwind-protect is safe.  */
  record_unwind_protect (do_unwind_create_frame, frame);

  if (BASE_EQ (name, Qunbound) || NILP (name) || !STRINGP (name))
    {
      fset_name (f, Vinvocation_name);
      f->explicit_name = false;
    }
  else
    {
      fset_name (f, name);
      f->explicit_name = true;
      specbind (Qx_resource_name, name);
    }

  register_font_driver (&host_sfntfont_driver, f);

  gui_default_parameter (f, parms, Qfont_backend, Qnil,
			 "fontBackend", "FontBackend", RES_TYPE_STRING);

  FRAME_RIF (f)->default_font_parameter (f, parms);
  if (!FRAME_FONT (f))
    {
      delete_frame (frame, Qnoelisp);
      error ("Invalid frame font");
    }

  gui_default_parameter (f, parms, Qborder_width, make_fixnum (0),
			 "borderwidth", "BorderWidth", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qinternal_border_width, make_fixnum (0),
			 "internalBorderWidth", "InternalBorderWidth",
			 RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qright_divider_width, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qbottom_divider_width, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  /* There are no scroll bars to have.  */
  gui_default_parameter (f, parms, Qvertical_scroll_bars, Qnil,
			 "verticalScrollBars", "VerticalScrollBars",
			 RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qhorizontal_scroll_bars, Qnil,
			 "horizontalScrollBars", "HorizontalScrollBars",
			 RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qforeground_color, build_string ("black"),
			 "foreground", "Foreground", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qbackground_color, build_string ("white"),
			 "background", "Background", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qline_spacing, Qnil,
			 "lineSpacing", "LineSpacing", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qleft_fringe, Qnil,
			 "leftFringe", "LeftFringe", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qright_fringe, Qnil,
			 "rightFringe", "RightFringe", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qno_special_glyphs, Qnil,
			 NULL, NULL, RES_TYPE_BOOLEAN);

  init_frame_faces (f);

  tem = gui_display_get_arg (dpyinfo, parms, Qmin_width, NULL, NULL,
			     RES_TYPE_NUMBER);
  if (FIXNUMP (tem))
    store_frame_param (f, Qmin_width, tem);
  tem = gui_display_get_arg (dpyinfo, parms, Qmin_height, NULL, NULL,
			     RES_TYPE_NUMBER);
  if (FIXNUMP (tem))
    store_frame_param (f, Qmin_height, tem);

  adjust_frame_size (f, FRAME_COLS (f) * FRAME_COLUMN_WIDTH (f),
		     FRAME_LINES (f) * FRAME_LINE_HEIGHT (f), 5, true,
		     Qx_create_frame_1);

  /* The menu bar, tab bar and tool bar are set up from their modes at
     startup; these are only what the modes say.  */
  gui_default_parameter (f, parms, Qmenu_bar_lines,
			 NILP (Vmenu_bar_mode)
			 ? make_fixnum (0) : make_fixnum (1),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qtab_bar_lines,
			 NILP (Vtab_bar_mode)
			 ? make_fixnum (0) : make_fixnum (1),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qtool_bar_lines,
			 NILP (Vtool_bar_mode)
			 ? make_fixnum (0) : make_fixnum (1),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qbuffer_predicate, Qnil,
			 "bufferPredicate", "BufferPredicate",
			 RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qtitle, Qnil, "title", "Title",
			 RES_TYPE_STRING);

  gui_figure_window_size (f, parms, false, true);

  tem = gui_display_get_arg (dpyinfo, parms, Qunsplittable, 0, 0,
			     RES_TYPE_BOOLEAN);
  f->no_split = minibuffer_only || (!BASE_EQ (tem, Qunbound) && !NILP (tem));

  f->terminal->reference_count++;
  Vframe_list = Fcons (frame, Vframe_list);

  gui_default_parameter (f, parms, Qauto_raise, Qnil,
			 "autoRaise", "AutoRaiseLower", RES_TYPE_BOOLEAN);
  gui_default_parameter (f, parms, Qauto_lower, Qnil,
			 "autoLower", "AutoLower", RES_TYPE_BOOLEAN);
  gui_default_parameter (f, parms, Qcursor_type, Qbox,
			 "cursorType", "CursorType", RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qscroll_bar_width, Qnil,
			 "scrollBarWidth", "ScrollBarWidth", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qscroll_bar_height, Qnil,
			 "scrollBarHeight", "ScrollBarHeight",
			 RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qfullscreen, Qnil,
			 "fullscreen", "Fullscreen", RES_TYPE_SYMBOL);

  f->can_set_window_size = true;
  adjust_frame_size (f, FRAME_TEXT_WIDTH (f), FRAME_TEXT_HEIGHT (f),
		     0, true, Qx_create_frame_2);

  /* Visible unless asked otherwise.  `x-create-frame-with-faces' asks
     for it invisible and makes it visible once its faces are set.  */
  tem = gui_display_get_arg (dpyinfo, parms, Qvisibility, 0, 0,
			     RES_TYPE_SYMBOL);
  if (BASE_EQ (tem, Qunbound))
    tem = Qt;
  if (!NILP (tem) && !EQ (tem, Qicon))
    host_set_frame_visible_invisible (f, true);
  else
    f->was_invisible = true;

  if (FRAME_HAS_MINIBUF_P (f)
      && (!FRAMEP (KVAR (kb, Vdefault_minibuffer_frame))
	  || !FRAME_LIVE_P (XFRAME (KVAR (kb, Vdefault_minibuffer_frame)))))
    kset_default_minibuffer_frame (kb, frame);

  /* The parameters that were not consumed above are kept, as other
     window systems keep them.  */
  for (tem = parms; CONSP (tem); tem = XCDR (tem))
    if (CONSP (XCAR (tem)) && !NILP (XCAR (XCAR (tem))))
      fset_param_alist (f, Fcons (XCAR (tem), f->param_alist));

  /* TODO: tell the host there is a new frame (`frame').  */

  /* Make sure windows on this frame appear in calls to next-window
     and similar functions.  */
  Vwindow_list = Qnil;

  return unbind_to (count, frame);
}

/* The display.  */

DEFUN ("x-open-connection", Fx_open_connection, Sx_open_connection,
       1, 3, 0,
       doc: /* Open the host's display.
DISPLAY is its name, which is not looked at: there is one host.
XRM-STRING and MUST-SUCCEED are ignored.  */)
  (Lisp_Object display, Lisp_Object xrm_string, Lisp_Object must_succeed)
{
  host_term_init ();
  return Qnil;
}

DEFUN ("x-display-list", Fx_display_list, Sx_display_list, 0, 0, 0,
       doc: /* Return the list of display names that Emacs has connections to.  */)
  (void)
{
  return x_display_list ? list1 (XCAR (x_display_list->name_list_element))
			: Qnil;
}

DEFUN ("xw-display-color-p", Fxw_display_color_p, Sxw_display_color_p,
       0, 1, 0,
       doc: /* Return t if the host's display supports color.  */)
  (Lisp_Object terminal)
{
  check_x_display_info (terminal);
  return Qt;
}

DEFUN ("x-display-grayscale-p", Fx_display_grayscale_p,
       Sx_display_grayscale_p, 0, 1, 0,
       doc: /* Return t if the host's display can show shades of gray.  */)
  (Lisp_Object terminal)
{
  check_x_display_info (terminal);
  return Qt;
}

DEFUN ("x-display-color-cells", Fx_display_color_cells,
       Sx_display_color_cells, 0, 1, 0,
       doc: /* Return the number of colors the host's display can show.  */)
  (Lisp_Object terminal)
{
  check_x_display_info (terminal);
  return make_fixnum (1 << 24);
}

DEFUN ("x-display-planes", Fx_display_planes, Sx_display_planes,
       0, 1, 0,
       doc: /* Return the number of bitplanes of the host's display.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (check_x_display_info (terminal)->n_planes);
}

DEFUN ("xw-color-defined-p", Fxw_color_defined_p, Sxw_color_defined_p,
       1, 2, 0,
       doc: /* Return t if COLOR is a valid color name or spec.  */)
  (Lisp_Object color, Lisp_Object frame)
{
  Emacs_Color unused;

  CHECK_STRING (color);
  decode_window_system_frame (frame);

  return host_get_color (SSDATA (color), &unused) ? Qt : Qnil;
}

DEFUN ("xw-color-values", Fxw_color_values, Sxw_color_values, 1, 2, 0,
       doc: /* Return a description of the color named COLOR.
The value is (RED GREEN BLUE), each from 0 to 65535, or nil if COLOR
is not a color.  */)
  (Lisp_Object color, Lisp_Object frame)
{
  Emacs_Color value;

  CHECK_STRING (color);
  decode_window_system_frame (frame);

  if (!host_get_color (SSDATA (color), &value))
    return Qnil;

  return list3i (value.red, value.green, value.blue);
}

DEFUN ("host-frame-list-z-order", Fhost_frame_list_z_order,
       Shost_frame_list_z_order, 0, 1, 0,
       doc: /* Return the list of Emacs's frames, in Z (stacking) order.
If DISPLAY is a live frame, return the child frames of that frame.

The host stacks the frames and Emacs is not told how, so the selected
frame comes first and the rest are in no particular order.  */)
  (Lisp_Object display)
{
  Lisp_Object frames = Qnil, selected = Qnil;
  Lisp_Object tail, frame;
  bool children = FRAMEP (display) && FRAME_LIVE_P (XFRAME (display));

  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);

      if (!FRAME_HOST_P (f)
	  || (children ? !EQ (display, get_frame_param (f, Qparent_frame))
			: FRAME_PARENT_FRAME (f) != NULL))
	continue;

      if (EQ (frame, selected_frame))
	selected = frame;
      else
	frames = Fcons (frame, frames);
    }

  return NILP (selected) ? Fnreverse (frames)
			 : Fcons (selected, Fnreverse (frames));
}

DEFUN ("x-hide-tip", Fx_hide_tip, Sx_hide_tip, 0, 0, 0,
       doc: /* Hide the current tooltip window, if there is any.
There never is: tooltips on a host frame are not shown yet.  Value is
nil.  */)
  (void)
{
  /* TODO: `x-show-tip', with the host showing the tip.  */
  return Qnil;
}

void
syms_of_hostfns (void)
{
  defsubr (&Sx_create_frame);
  defsubr (&Sx_open_connection);
  defsubr (&Sx_display_list);
  defsubr (&Sxw_display_color_p);
  defsubr (&Sx_display_grayscale_p);
  defsubr (&Sx_display_color_cells);
  defsubr (&Sx_display_planes);
  defsubr (&Sxw_color_defined_p);
  defsubr (&Sxw_color_values);
  defsubr (&Sx_hide_tip);
  defsubr (&Shost_frame_list_z_order);
}
