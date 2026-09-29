/* Frames that a host application draws: what was drawn, said rather
   than drawn.

Copyright (C) 2026 i999rri

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

/* hostdraw.c hands the host the pixels redisplay drew.  This says what
   redisplay drew instead: the areas it filled, the lines it drew, and
   which glyphs of which font it put where.  A screen of text comes to
   a few hundred of those, against the megabyte its pixels come to, and
   a host that draws the glyphs draws them with the same letters it
   draws everything else with.

   What is said is a line of JSON for each thing to do, in the order
   redisplay did them, between a `begin' and an `end'.  Order is the
   whole of it: the text is drawn over the background that was filled
   before it, so a host that read them in any other order would show
   something else.  Emacs writes the lines of a frame in one go all the
   same, so that a screen is one write rather than a few hundred.

   Which glyph is which is settled by the font file: Emacs reads the
   files itself and numbers the glyphs as they are numbered there, and
   hostfont.c hands the host the same file, so that both are numbering
   the glyphs of the same font (`host-send-font').  */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lisp.h"
#include "frame.h"
#include "dispextern.h"
#include "hostterm.h"
#include "hostlib.h"

/* `host-draw-commands': whether the host is to be told what to draw
   rather than handed the pixels.  The variable itself is globals.h's,
   as every DEFVAR's is.  */

bool
host_recording_p (void)
{
  return host_draw_commands;
}

/* Room for one more command, and the one it made room for.  */

static struct host_command *
host_next_command (struct frame *f, enum host_op op)
{
  struct host_record *record = &FRAME_OUTPUT_DATA (f)->record;
  struct host_command *command;

  if (record->count == record->room)
    {
      int room = record->room ? record->room * 2 : 256;

      record->commands = xrealloc (record->commands,
				   room * sizeof *record->commands);
      record->room = room;
    }

  command = &record->commands[record->count++];
  memset (command, 0, sizeof *command);
  command->op = op;
  return command;
}

void
host_record_fill (struct frame *f, int x, int y, int width, int height,
		  unsigned long color)
{
  struct host_command *command = host_next_command (f, HOST_OP_FILL);

  command->x = x;
  command->y = y;
  command->width = width;
  command->height = height;
  command->color = color;
}

void
host_record_rectangle (struct frame *f, int x, int y, int width, int height,
		       unsigned long color)
{
  struct host_command *command = host_next_command (f, HOST_OP_RECTANGLE);

  command->x = x;
  command->y = y;
  command->width = width;
  command->height = height;
  command->color = color;
}

/* A line from X0,Y0 to X1,Y1, which are kept in the box's four numbers
   since a line has no size of its own.  */

void
host_record_line (struct frame *f, int x0, int y0, int x1, int y1,
		  unsigned long color)
{
  struct host_command *command = host_next_command (f, HOST_OP_LINE);

  command->x = x0;
  command->y = y0;
  command->width = x1;
  command->height = y1;
  command->color = color;
}

/* The image of S, drawn within the box X,Y by WIDTH,HEIGHT.

   The pixels are not here: the host asks for them once by the number,
   and draws them as often as it is told to, the way it does with the
   file of a font.  Nor is the size it is drawn at: the matrix says
   that, and says which part of the image this row shows, by carrying
   the image's corner to where that part goes.  A host that is told the
   matrix has nothing to work out.  */

void
host_record_image (struct frame *f, struct glyph_string *s,
		   int x, int y, int width, int height)
{
  struct host_command *command = host_next_command (f, HOST_OP_IMAGE);
  struct image *img = s->img;

  command->image = host_image_id (img->pixmap, img->mask);
  command->original_width = img->original_width;
  command->original_height = img->original_height;
  command->x = x;
  command->y = y;
  command->width = width;
  command->height = height;
  command->smooth = img->use_bilinear_filtering;

  /* The image's own transformation, and after it the move that takes
     the corner of the image to where the corner of this slice goes.  */
  command->matrix[0] = img->transform[0][0];
  command->matrix[1] = img->transform[1][0];
  command->matrix[2] = img->transform[0][1];
  command->matrix[3] = img->transform[1][1];
  command->matrix[4] = img->transform[0][2] + x - s->slice.x;
  command->matrix[5] = img->transform[1][2] + y - s->slice.y;
}

void
host_record_copy (struct frame *f, int x, int y, int width, int height,
		  int to_y)
{
  struct host_command *command = host_next_command (f, HOST_OP_COPY);

  command->x = x;
  command->y = y;
  command->width = width;
  command->height = height;
  command->to_y = to_y;
}

void
host_record_clip (struct frame *f, bool clipped, int x, int y,
		  int width, int height)
{
  struct host_command *command
    = host_next_command (f, clipped ? HOST_OP_CLIP : HOST_OP_UNCLIP);

  command->x = x;
  command->y = y;
  command->width = width;
  command->height = height;
}

/* The glyphs of S from FROM to TO, drawn on the baseline Y with the
   Xth glyph at X_COORDS[X].  */

void
host_record_glyphs (struct glyph_string *s, int from, int to, int y,
		    int count, int const *x_coords)
{
  struct host_command *command;
  int i;

  if (from == to)
    return;

  command = host_next_command (s->f, HOST_OP_GLYPHS);
  command->y = y;
  command->color = s->foreground;
  command->font = host_font_id (s);
  command->size = s->font->pixel_size;
  command->count = count;
  command->ids = xmalloc (count * sizeof *command->ids);
  command->xs = xmalloc (count * sizeof *command->xs);

  for (i = 0; i < count; i++)
    {
      command->ids[i] = s->char2b[from + i];
      command->xs[i] = x_coords[i];
    }
}

void
host_forget_commands (struct frame *f)
{
  struct host_record *record = &FRAME_OUTPUT_DATA (f)->record;
  int i;

  for (i = 0; i < record->count; i++)
    if (record->commands[i].op == HOST_OP_GLYPHS)
      {
	xfree (record->commands[i].ids);
	xfree (record->commands[i].xs);
      }
  record->count = 0;
}

/* Saying it.  */

/* Room enough for one command's line, whatever it is: the header, and
   for a run of glyphs two numbers apiece.  */

static ptrdiff_t
host_command_room (struct host_command const *command)
{
  return 192 + (ptrdiff_t) command->count * 16;
}

static char *
host_say_color (char *at, unsigned long color)
{
  return at + sprintf (at, "\"color\":\"#%06lx\"", color & 0xffffff);
}

static char *
host_say_command (char *at, struct host_command const *command)
{
  int i;

  switch (command->op)
    {
    case HOST_OP_FILL:
    case HOST_OP_RECTANGLE:
      at += sprintf (at, "{\"type\":\"draw\",\"op\":\"%s\",\"x\":%d,\"y\":%d,"
		     "\"width\":%d,\"height\":%d,",
		     command->op == HOST_OP_FILL ? "fill" : "rectangle",
		     command->x, command->y, command->width, command->height);
      at = host_say_color (at, command->color);
      break;

    case HOST_OP_LINE:
      at += sprintf (at, "{\"type\":\"draw\",\"op\":\"line\",\"x0\":%d,"
		     "\"y0\":%d,\"x1\":%d,\"y1\":%d,",
		     command->x, command->y, command->width, command->height);
      at = host_say_color (at, command->color);
      break;

    case HOST_OP_COPY:
      at += sprintf (at, "{\"type\":\"draw\",\"op\":\"copy\",\"x\":%d,"
		     "\"y\":%d,\"width\":%d,\"height\":%d,\"toY\":%d",
		     command->x, command->y, command->width, command->height,
		     command->to_y);
      break;

    case HOST_OP_CLIP:
      at += sprintf (at, "{\"type\":\"draw\",\"op\":\"clip\",\"x\":%d,"
		     "\"y\":%d,\"width\":%d,\"height\":%d",
		     command->x, command->y, command->width, command->height);
      break;

    case HOST_OP_UNCLIP:
      at += sprintf (at, "{\"type\":\"draw\",\"op\":\"unclip\"");
      break;

    case HOST_OP_IMAGE:
      at += sprintf (at, "{\"type\":\"draw\",\"op\":\"image\",\"image\":%d,"
		     "\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,"
		     "\"imageWidth\":%d,\"imageHeight\":%d,\"smooth\":%s,"
		     "\"matrix\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]",
		     command->image, command->x, command->y,
		     command->width, command->height,
		     command->original_width, command->original_height,
		     command->smooth ? "true" : "false",
		     command->matrix[0], command->matrix[1],
		     command->matrix[2], command->matrix[3],
		     command->matrix[4], command->matrix[5]);
      break;

    case HOST_OP_GLYPHS:
      at += sprintf (at, "{\"type\":\"draw\",\"op\":\"glyphs\",\"font\":%d,"
		     "\"size\":%.2f,\"y\":%d,",
		     command->font, command->size, command->y);
      at = host_say_color (at, command->color);
      at += sprintf (at, ",\"ids\":[");
      for (i = 0; i < command->count; i++)
	at += sprintf (at, "%s%u", i ? "," : "",
		       (unsigned) command->ids[i]);
      at += sprintf (at, "],\"xs\":[");
      for (i = 0; i < command->count; i++)
	at += sprintf (at, "%s%d", i ? "," : "", command->xs[i]);
      at += sprintf (at, "]");
      break;
    }

  return at + sprintf (at, "}\n");
}

/* Hand the host what redisplay drew, and forget it.  */

void
host_send_commands (struct frame *f)
{
  struct host_record *record = &FRAME_OUTPUT_DATA (f)->record;
  struct host_picture *picture = &FRAME_OUTPUT_DATA (f)->picture;
  const struct host_api *api = host_current_api ();
  char name[32];
  ptrdiff_t room = 256;
  char *message, *at;
  int i;

  if (!api || record->count <= 0)
    return;

  host_frame_name (f, name, sizeof name);

  for (i = 0; i < record->count; i++)
    room += host_command_room (&record->commands[i]);

  message = xmalloc (room);
  at = message + sprintf (message,
			  "{\"type\":\"draw\",\"op\":\"begin\",\"frame\":\"%s\","
			  "\"width\":%d,\"height\":%d}\n",
			  name, picture->width, picture->height);

  for (i = 0; i < record->count; i++)
    at = host_say_command (at, &record->commands[i]);

  /* The last line has no end of its own: the host puts one there when
     it takes the message.  */
  sprintf (at, "{\"type\":\"draw\",\"op\":\"end\",\"frame\":\"%s\"}", name);
  api->post (message);

  xfree (message);
  host_forget_commands (f);
}

void
syms_of_hostrecord (void)
{
  DEFVAR_BOOL ("host-draw-commands", host_draw_commands,
	       doc: /* Whether to tell the host what to draw.
The host is handed the pixels redisplay drew while this is nil, which
asks nothing of it but to show them.  Told instead, it draws the text
itself, with the fonts Emacs read and the glyphs Emacs chose, and the
screen costs a hundredth of what its pixels cost to send.

A host that cannot draw them shows nothing at all, so this is for one
that says it can.  */);
  host_draw_commands = false;
}
