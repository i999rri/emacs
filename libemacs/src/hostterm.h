/* Frames that a host application draws: the terminal and its frames.

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

/* The `host' window system has frames with no window at all.  Emacs
   lays them out as it would any other frame, and a host application
   draws them from what redisplay left in the glyph matrices, which
   hostscreen.c reads back.  This is its TERM_HEADER: what the rest of
   Emacs expects of a window system's display and frames.  */

#ifndef HOST_TERM_H
#define HOST_TERM_H

/* sfntfont.c calls lseek without including this itself; on Android
   it comes with the TERM_HEADER, and this is the TERM_HEADER here.  */
#include <unistd.h>

#include "hostgui.h"
#include "frame.h"
#include "character.h"
#include "dispextern.h"
#include "font.h"
#include "sfntfont.h"
#include "systime.h"

/* An image, as pixels in memory.  Nothing draws it here; it is kept
   so that the host can be given it.  A pixmap and the image it was
   made from are the same object, as on Haiku.  */
struct host_pixmap
{
  int width, height, depth;
  unsigned long *pixels;
};

struct host_bitmap_record
{
  struct host_pixmap *img;
  char *file;
  int refcount;
  int height, width, depth;
};

struct host_display_info
{
  /* Chain of all host_display_info structures.  There is only ever
     one.  */
  struct host_display_info *next;
  struct terminal *terminal;

  /* The name of the display and the font cache, as font.c wants
     them: (NAME . FONT-LIST-CACHE).  */
  Lisp_Object name_list_element;

  /* The colors rgb.txt names, as ("name" . pixel).  */
  Lisp_Object color_map;

  int n_fonts;
  int smallest_char_width;
  int smallest_font_height;

  struct host_bitmap_record *bitmaps;
  ptrdiff_t bitmaps_size;
  ptrdiff_t bitmaps_last;

  int n_planes;
  int grabbed;

  /* Dots per inch.  */
  double resx, resy;

  Mouse_HLInfo mouse_highlight;

  struct frame *highlight_frame;
  struct frame *focus_frame;
  struct frame *last_mouse_frame;
  struct frame *last_mouse_motion_frame;
  int last_mouse_motion_x;
  int last_mouse_motion_y;
  Time last_mouse_movement_time;

  Window root_window;

  /* What frame.c passes to get_string_resource_hook, which finds no
     resources in it: there are none.  */
  void *rdb;

  /* Pointer shapes.  The host has its own pointer and chooses its
     shapes itself; these are only what redisplay asks for.  */
  Emacs_Cursor vertical_scroll_bar_cursor;
  Emacs_Cursor horizontal_scroll_bar_cursor;
};

/* The shapes the pointer takes over a frame.

   Redisplay chooses between the shapes a frame was given and hands the
   one it chose back; a host has its own pointer and is told the name of
   the shape rather than anything of Windows's or X's, so these are what
   the frame is given.  Emacs_Cursor is a pointer here, and these stand
   in it: none of them is one, and none is ever followed.  */

enum host_pointer
{
  HOST_POINTER_NONE,
  HOST_POINTER_ARROW,
  HOST_POINTER_TEXT,
  HOST_POINTER_HAND,
  HOST_POINTER_BUSY,
  HOST_POINTER_HORIZONTAL_DRAG,
  HOST_POINTER_VERTICAL_DRAG,
  HOST_POINTER_LEFT_EDGE,
  HOST_POINTER_TOP_LEFT_CORNER,
  HOST_POINTER_TOP_EDGE,
  HOST_POINTER_TOP_RIGHT_CORNER,
  HOST_POINTER_RIGHT_EDGE,
  HOST_POINTER_BOTTOM_RIGHT_CORNER,
  HOST_POINTER_BOTTOM_EDGE,
  HOST_POINTER_BOTTOM_LEFT_CORNER,
};

#define HOST_CURSOR(shape) ((Emacs_Cursor) (intptr_t) (shape))
#define HOST_POINTER_OF(cursor) ((enum host_pointer) (intptr_t) (cursor))

/* The pixels of a frame, which redisplay draws into and the host is
   handed to show (hostdraw.c).  */

/* How many boxes of what was drawn are kept apart before any are put
   together.  A line and the mode line make two; a line, the mode line
   and the echo area make three; the rest is room to spare.  */
#define HOST_DRAWN_BOXES 8

struct host_picture
{
  /* WIDTH by HEIGHT pixels, a row at a time, each 0xAARRGGBB.  */
  unsigned int *cells;
  int width, height;

  /* The boxes drawn into since the host was last given the picture,
     which is all that is worth handing over.

     Several, because what a keystroke draws is the line being typed in
     and the mode line below it, and one box around both is the whole
     frame between them.  Where there are more than there is room for,
     the two that waste least between them are put together, so that
     what is sent is at worst the box around everything.  */
  /* One more than are kept: putting two together needs the new one
     among them, and it is laid at the end while that is worked out.  */
  struct host_box
  {
    int x, y, width, height;
  } drawn[HOST_DRAWN_BOXES + 1];
  int drawn_count;

  /* The boxes that moved within the picture since then, and how far
     down each went, which is what a window scrolling comes to.  The
     host has what they hold already and moves its own; sending the
     pixels instead would be sending most of the frame for every line
     scrolled.  They are moved before anything drawn is put in.  */
  struct host_move
  {
    int x, y, width, height, to_y;
  } moved[HOST_DRAWN_BOXES];
  int moved_count;

  /* The box drawing is kept within, which redisplay narrows to a row
     or to what a glyph string may reach; the whole picture when
     CLIPPED is false.  */
  bool clipped;
  int clip_x, clip_y, clip_width, clip_height;
};

extern struct host_picture *host_frame_picture (struct frame *);
extern void host_free_picture (struct frame *);
extern void host_forget_drawn (struct frame *);
extern void host_fill_area (struct frame *, int, int, int, int, unsigned long);
extern void host_draw_rectangle (struct frame *, int, int, int, int,
				 unsigned long);
extern void host_draw_line (struct frame *, int, int, int, int, unsigned long);
extern void host_move_area (struct frame *, int, int, int, int, int);
/* What drawing is being kept within, to put back after narrowing it
   further: one that draws a part of a glyph string is drawn within
   what the string is, and leaves that as it found it.  */
struct host_clip
{
  bool clipped;
  int x, y, width, height;
};

extern void host_set_clip (struct frame *, int, int, int, int);
extern void host_reset_clip (struct frame *);
extern struct host_clip host_clip_now (struct frame *);
extern void host_clip_again (struct frame *, struct host_clip);
extern void host_blend_coverage (struct frame *, unsigned char const *, int,
				 int, int, int, int, unsigned long);
extern void host_show_picture (struct frame *);

extern char *host_base64 (char *, unsigned char const *, ptrdiff_t);
extern void host_frame_name (struct frame *, char *, size_t);

/* What was drawn, said rather than drawn (hostrecord.c).  A picture
   costs the whole of what changed in pixels; the same screen said as
   what to draw is two orders of magnitude smaller, and a host that
   draws the text itself draws it with the same letters as everything
   else it draws.  */

enum host_op
  {
    HOST_OP_FILL,
    HOST_OP_RECTANGLE,
    HOST_OP_LINE,
    HOST_OP_COPY,
    HOST_OP_CLIP,
    HOST_OP_UNCLIP,
    HOST_OP_GLYPHS,
    HOST_OP_IMAGE
  };

struct host_command
{
  enum host_op op;

  /* The box it is in.  A line runs from X,Y to WIDTH,HEIGHT, which are
     the far corner rather than a size; a copy goes to TO_Y.  */
  int x, y, width, height, to_y;
  unsigned long color;

  /* A run of glyphs: the font as hostfont.c numbers it, how big it is
     drawn, and for each glyph its number in that font and where it
     goes.  Y is the baseline.  */
  int font, count;
  double size;
  unsigned short *ids;
  int *xs;

  /* An image: which one, as hostfns.c numbers it, and where in the
     image the part being drawn begins.  The box is where that part
     goes, so a row that shows a slice of a tall image says the same
     image with another corner of it.  */
  int image, from_x, from_y;
};

struct host_record
{
  struct host_command *commands;
  int count, room;
};

extern bool host_recording_p (void);
extern void host_record_fill (struct frame *, int, int, int, int,
			      unsigned long);
extern void host_record_rectangle (struct frame *, int, int, int, int,
				   unsigned long);
extern void host_record_line (struct frame *, int, int, int, int,
			      unsigned long);
extern void host_record_copy (struct frame *, int, int, int, int, int);
extern void host_record_clip (struct frame *, bool, int, int, int, int);
extern void host_record_glyphs (struct glyph_string *, int, int, int, int,
				int const *);
extern void host_record_image (struct frame *, int, int, int, int, int,
			       int, int);
extern void host_send_commands (struct frame *);
extern void host_forget_commands (struct frame *);
extern void syms_of_hostrecord (void);

/* The fonts a host is to draw in (hostfont.c).  */
extern int host_font_id (struct glyph_string *);
extern void host_font_wanted (int);
extern void syms_of_hostfont (void);

struct host_output
{
  struct host_display_info *display_info;

  /* What redisplay drew, for the host to show.  */
  struct host_picture picture;

  /* What redisplay drew, said rather than drawn, where the host is to
     draw it itself.  */
  struct host_record record;

  /* The pointer shapes redisplay chooses between as the pointer moves
     over the frame, and the one it last chose.  */
  Emacs_Cursor text_cursor;
  Emacs_Cursor nontext_cursor;
  Emacs_Cursor modeline_cursor;
  Emacs_Cursor hand_cursor;
  Emacs_Cursor hourglass_cursor;
  Emacs_Cursor horizontal_drag_cursor;
  Emacs_Cursor vertical_drag_cursor;
  Emacs_Cursor left_edge_cursor;
  Emacs_Cursor top_left_corner_cursor;
  Emacs_Cursor top_edge_cursor;
  Emacs_Cursor top_right_corner_cursor;
  Emacs_Cursor right_edge_cursor;
  Emacs_Cursor bottom_right_corner_cursor;
  Emacs_Cursor bottom_edge_cursor;
  Emacs_Cursor bottom_left_corner_cursor;
  Emacs_Cursor current_cursor;

  Window parent_desc;

  /* Always null: a host frame has no window of the system's.  It is
     here for FRAME_NATIVE_WINDOW.  */
  Emacs_Window window;

  struct font *font;
  int fontset;
  int baseline_offset;

  unsigned long cursor_pixel;
};

/* xterm.h has this, and shared code names it whatever the window
   system is.  */
struct x_output
{
  int unused;
};

#define FRAME_OUTPUT_DATA(f)		((f)->output_data.host)
#define FRAME_DISPLAY_INFO(f)		(FRAME_OUTPUT_DATA (f)->display_info)
#define FRAME_FONT(f)			(FRAME_OUTPUT_DATA (f)->font)
#define FRAME_FONTSET(f)		(FRAME_OUTPUT_DATA (f)->fontset)
#define FRAME_BASELINE_OFFSET(f)	(FRAME_OUTPUT_DATA (f)->baseline_offset)
#define FRAME_NATIVE_WINDOW(f)		(FRAME_OUTPUT_DATA (f)->window)

#define BLACK_PIX_DEFAULT(f) 0
#define WHITE_PIX_DEFAULT(f) 0xffffff

extern struct host_display_info *x_display_list;
extern frame_parm_handler host_frame_parm_handlers[];

/* hostterm.c */
extern struct host_display_info *host_term_init (void);
extern void mark_host_display (void);
extern void host_set_frame_visible_invisible (struct frame *, bool);
extern bool host_get_color (const char *, Emacs_Color *);
extern void syms_of_hostterm (void);
extern void syms_of_hostinput (void);

/* hostfns.c */
extern struct host_pixmap *host_make_pixmap (int, int, int);
extern void host_free_pixmap (struct frame *, Emacs_Pixmap);
extern ptrdiff_t host_pixmap_size (Emacs_Pixmap);

/* The images a host is to draw, numbered so that its pixels are sent
   once and drawn as often as they are wanted, as a font's file is.  An
   image and the mask that says which of its pixels show are one image
   to the host, since what it is given is what it draws.  */
extern int host_image_id (struct host_pixmap *, struct host_pixmap *);
extern void host_image_wanted (int);

extern unsigned long host_get_pixel (struct host_pixmap *, int, int);
extern void host_put_pixel (struct host_pixmap *, int, int, unsigned long);
extern void syms_of_hostfns (void);

/* hostinput.c */
extern void host_input_init (void);
extern int host_read_socket (struct terminal *, struct input_event *);
extern void host_mouse_position (struct frame **, int, Lisp_Object *,
				 enum scroll_bar_part *, Lisp_Object *,
				 Lisp_Object *, Time *);
extern Lisp_Object host_get_focus_frame (struct frame *);
extern void host_frame_rehighlight (struct frame *);

/* sfntfont-host.c */
extern const struct font_driver host_sfntfont_driver;
extern void host_enumerate_fonts (void);
extern void init_sfntfont_host (void);
extern void syms_of_sfntfont_host (void);

#endif /* HOST_TERM_H */
