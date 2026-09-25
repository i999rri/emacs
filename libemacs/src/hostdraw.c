/* Frames that a host application draws: the picture drawn for it.

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

/* The pixels of a host frame, which the redisplay interface draws
   into (hostterm.c) and the host is then handed to show.

   Redisplay draws through this rather than saying what it would have
   drawn, because it already knows: the font engine reads the font
   files itself and rasterizes the glyphs (sfntfont-host.c), so the
   text is laid out and drawn by the same measurements.  A host asked
   to draw the text instead would have to arrive at those
   measurements again, and be wrong wherever it did not.  */

#include <config.h>

#include <math.h>
#include <stdlib.h>

#include "lisp.h"
#include "frame.h"
#include "hostterm.h"
#include "hostlib.h"

/* A pixel of a picture, from the `unsigned long' Emacs keeps a color
   in, which holds it as 0x00RRGGBB.  The picture has an alpha channel
   because what a host shows it in may expect one; everything drawn is
   opaque.  */

static unsigned int
host_pixel (unsigned long color)
{
  return 0xff000000u | (unsigned int) (color & 0x00ffffffu);
}

/* How much light a color component stands for, and which component
   stands for that much light.

   A component is not the amount of light but a curve of it, so mixing
   two of them as they are stored is not mixing the light they stand
   for: text drawn that way comes out thin on a dark background and
   heavy on a light one, and a gray edge takes on the cast of whatever
   it lies between.  These turn one into the other so that what is
   mixed is the light.  */

#define HOST_LIGHT_MAX 4095

static unsigned short host_light_of[256];
static unsigned char host_color_of[HOST_LIGHT_MAX + 1];
static bool host_light_known;

/* The curve sRGB stores a component by, and its inverse: a straight
   line at the bottom, where the curve would be too steep to hold, and
   a power of 2.4 above it.  */

static double
host_light (double color)
{
  return (color <= 0.04045
	  ? color / 12.92
	  : pow ((color + 0.055) / 1.055, 2.4));
}

static double
host_color (double light)
{
  return (light <= 0.0031308
	  ? light * 12.92
	  : 1.055 * pow (light, 1 / 2.4) - 0.055);
}

static void
host_learn_light (void)
{
  int i;

  for (i = 0; i < 256; i++)
    host_light_of[i] = lround (host_light (i / 255.0) * HOST_LIGHT_MAX);
  for (i = 0; i <= HOST_LIGHT_MAX; i++)
    host_color_of[i] = lround (host_color ((double) i / HOST_LIGHT_MAX) * 255);

  host_light_known = true;
}

/* Blend the color OVER, an 0xAARRGGBB pixel, onto UNDER by COVERAGE
   parts in 255.  */

static unsigned int
host_blend (unsigned int over, unsigned int under, unsigned char coverage)
{
  unsigned int result = 0xff000000u;
  int shift;

  if (coverage == 255)
    return over;
  if (coverage == 0)
    return under;

  if (!host_light_known)
    host_learn_light ();

  for (shift = 0; shift < 24; shift += 8)
    {
      unsigned int a = host_light_of[(over >> shift) & 0xff];
      unsigned int b = host_light_of[(under >> shift) & 0xff];
      /* Rounded, so that full coverage comes out as the color itself
	 rather than a shade beside it.  */
      unsigned int mixed = (a * coverage + b * (255 - coverage) + 127) / 255;

      result |= (unsigned int) host_color_of[mixed] << shift;
    }

  return result;
}

/* The picture of F, large enough for the frame as it is now.

   Made when first asked for and made again when the frame is a
   different size, which is where the frame is drawn again in full, so
   there is nothing in it worth keeping across the change.  */

struct host_picture *
host_frame_picture (struct frame *f)
{
  struct host_picture *picture = &FRAME_OUTPUT_DATA (f)->picture;
  int width = FRAME_PIXEL_WIDTH (f);
  int height = FRAME_PIXEL_HEIGHT (f);

  if (width <= 0 || height <= 0)
    return NULL;

  if (picture->cells && picture->width == width && picture->height == height)
    return picture;

  xfree (picture->cells);
  picture->cells = xzalloc ((ptrdiff_t) width * height * sizeof *picture->cells);
  picture->width = width;
  picture->height = height;
  host_forget_drawn (f);
  return picture;
}

void
host_free_picture (struct frame *f)
{
  struct host_picture *picture = &FRAME_OUTPUT_DATA (f)->picture;

  xfree (picture->cells);
  picture->cells = NULL;
  picture->width = picture->height = 0;
  host_forget_drawn (f);
}

/* The area of the box that holds both A and B, which is what putting
   the two together would come to.  */

static double
host_box_together (struct host_box const *a, struct host_box const *b)
{
  double left = min (a->x, b->x);
  double top = min (a->y, b->y);
  double right = max (a->x + a->width, b->x + b->width);
  double bottom = max (a->y + a->height, b->y + b->height);

  return (right - left) * (bottom - top);
}

/* Put B into A.  */

static void
host_box_absorb (struct host_box *a, struct host_box const *b)
{
  int left = min (a->x, b->x);
  int top = min (a->y, b->y);
  int right = max (a->x + a->width, b->x + b->width);
  int bottom = max (a->y + a->height, b->y + b->height);

  a->x = left;
  a->y = top;
  a->width = right - left;
  a->height = bottom - top;
}

/* Take in that the box X, Y, WIDTH by HEIGHT of F's picture was drawn
   into, so that the host is given it.  */

static void
host_drawn (struct frame *f, int x, int y, int width, int height)
{
  struct host_picture *picture = &FRAME_OUTPUT_DATA (f)->picture;
  struct host_box box = { x, y, width, height };
  int i, waste_at = 0, waste_with = 0;
  double least = 0;

  if (width <= 0 || height <= 0)
    return;

  /* Into a box it already touches, which is the usual way of it: a
     line is drawn a glyph string at a time, and the whole of it is one
     box by the end.  */
  for (i = 0; i < picture->drawn_count; i++)
    {
      struct host_box *kept = &picture->drawn[i];

      if (box.x <= kept->x + kept->width && kept->x <= box.x + box.width
	  && box.y <= kept->y + kept->height && kept->y <= box.y + box.height)
	{
	  host_box_absorb (kept, &box);
	  return;
	}
    }

  if (picture->drawn_count < HOST_DRAWN_BOXES)
    {
      picture->drawn[picture->drawn_count++] = box;
      return;
    }

  /* No room for another, so two of them are put together: whichever
     two waste least between them, the new box among them.  */
  picture->drawn[picture->drawn_count] = box;
  for (i = 0; i <= picture->drawn_count; i++)
    {
      int j;

      for (j = i + 1; j <= picture->drawn_count; j++)
	{
	  double area = host_box_together (&picture->drawn[i],
					   &picture->drawn[j]);

	  if ((i == 0 && j == 1) || area < least)
	    {
	      least = area;
	      waste_at = i;
	      waste_with = j;
	    }
	}
    }

  host_box_absorb (&picture->drawn[waste_at], &picture->drawn[waste_with]);
  picture->drawn[waste_with] = picture->drawn[picture->drawn_count];
}

void
host_forget_drawn (struct frame *f)
{
  FRAME_OUTPUT_DATA (f)->picture.drawn_count = 0;
  FRAME_OUTPUT_DATA (f)->picture.moved_count = 0;
}

/* Keep drawing on F within the box X, Y, WIDTH by HEIGHT, which is
   what redisplay narrows to a row or to what a glyph string may
   reach, and let it out again.  */

void
host_set_clip (struct frame *f, int x, int y, int width, int height)
{
  struct host_picture *picture = &FRAME_OUTPUT_DATA (f)->picture;

  picture->clipped = true;
  picture->clip_x = x;
  picture->clip_y = y;
  picture->clip_width = width;
  picture->clip_height = height;
}

void
host_reset_clip (struct frame *f)
{
  FRAME_OUTPUT_DATA (f)->picture.clipped = false;
}

struct host_clip
host_clip_now (struct frame *f)
{
  struct host_picture *picture = &FRAME_OUTPUT_DATA (f)->picture;

  return (struct host_clip) { picture->clipped, picture->clip_x,
			      picture->clip_y, picture->clip_width,
			      picture->clip_height };
}

void
host_clip_again (struct frame *f, struct host_clip was)
{
  struct host_picture *picture = &FRAME_OUTPUT_DATA (f)->picture;

  picture->clipped = was.clipped;
  picture->clip_x = was.x;
  picture->clip_y = was.y;
  picture->clip_width = was.width;
  picture->clip_height = was.height;
}

/* Cut the box X, Y, WIDTH by HEIGHT down to what is inside PICTURE and
   within what is being kept to, and say whether anything is left.  */

static bool
host_clip (struct host_picture *picture, int *x, int *y,
	   int *width, int *height)
{
  int left = max (*x, 0);
  int top = max (*y, 0);
  int right = min (*x + *width, picture->width);
  int bottom = min (*y + *height, picture->height);

  if (picture->clipped)
    {
      left = max (left, picture->clip_x);
      top = max (top, picture->clip_y);
      right = min (right, picture->clip_x + picture->clip_width);
      bottom = min (bottom, picture->clip_y + picture->clip_height);
    }

  *x = left;
  *y = top;
  *width = right - left;
  *height = bottom - top;
  return *width > 0 && *height > 0;
}

/* Fill the box X, Y, WIDTH by HEIGHT of F's picture with COLOR.  */

void
host_fill_area (struct frame *f, int x, int y, int width, int height,
		unsigned long color)
{
  struct host_picture *picture = host_frame_picture (f);
  unsigned int pixel;
  int row, column;

  if (!picture || !host_clip (picture, &x, &y, &width, &height))
    return;

  pixel = host_pixel (color);
  for (row = y; row < y + height; row++)
    {
      unsigned int *cells = picture->cells + (ptrdiff_t) row * picture->width;

      for (column = x; column < x + width; column++)
	cells[column] = pixel;
    }

  host_drawn (f, x, y, width, height);
}

/* Draw the outline of the box X, Y, WIDTH by HEIGHT of F's picture in
   COLOR, a pixel thick, which is the box a hollow cursor is.  */

void
host_draw_rectangle (struct frame *f, int x, int y, int width, int height,
		     unsigned long color)
{
  if (width < 0 || height < 0)
    return;

  host_fill_area (f, x, y, width + 1, 1, color);
  host_fill_area (f, x, y + height, width + 1, 1, color);
  host_fill_area (f, x, y, 1, height + 1, color);
  host_fill_area (f, x + width, y, 1, height + 1, color);
}

/* Draw a line of F's picture in COLOR from X0, Y0 to X1, Y1.

   Whole pixels, with no softening of the edges: the lines drawn are
   the borders between windows and the wave under misspelt text, and
   both are of a width Emacs chose in pixels.  */

void
host_draw_line (struct frame *f, int x0, int y0, int x1, int y1,
		unsigned long color)
{
  int dx = abs (x1 - x0), dy = -abs (y1 - y0);
  int step_x = x0 < x1 ? 1 : -1, step_y = y0 < y1 ? 1 : -1;
  int error = dx + dy;

  while (true)
    {
      host_fill_area (f, x0, y0, 1, 1, color);
      if (x0 == x1 && y0 == y1)
	return;

      {
	int twice = 2 * error;

	if (twice >= dy)
	  {
	    error += dy;
	    x0 += step_x;
	  }
	if (twice <= dx)
	  {
	    error += dx;
	    y0 += step_y;
	  }
      }
    }
}

/* Move the box X, FROM_Y, WIDTH by HEIGHT of F's picture to X, TO_Y.

   This is what a window scrolling comes to: redisplay moves the rows
   it keeps within the matrix and draws only the ones that were not
   there before, so a picture that did not move with them would keep
   what was drawn in the rows that moved.  */

void
host_move_area (struct frame *f, int x, int from_y, int width, int height,
		int to_y)
{
  struct host_picture *picture = host_frame_picture (f);
  int left = max (x, 0);
  int right = min (x + width, picture ? picture->width : 0);
  int row;

  if (!picture || right <= left || height <= 0 || from_y == to_y)
    return;

  /* Cut to what is in the picture at both ends, keeping the two the
     same height so that what is moved is what was there.  */
  {
    int above = max (0, max (-from_y, -to_y));
    int below = max (0, max (from_y + height - picture->height,
			     to_y + height - picture->height));

    from_y += above;
    to_y += above;
    height -= above + below;
    if (height <= 0)
      return;
  }

  /* From the end the rows are moving towards, so that a box moved on
     top of itself is not read after it has been written.  */
  for (row = 0; row < height; row++)
    {
      int at = to_y < from_y ? row : height - 1 - row;

      memmove (picture->cells + (ptrdiff_t) (to_y + at) * picture->width + left,
	       picture->cells + (ptrdiff_t) (from_y + at) * picture->width + left,
	       (ptrdiff_t) (right - left) * sizeof *picture->cells);
    }

  /* Said to have moved rather than to have been drawn: the host has
     what it holds already and moves its own.  Where there is no room
     to say so it is sent as pixels, which is what saying nothing
     comes to.  */
  if (picture->moved_count < HOST_DRAWN_BOXES)
    {
      struct host_move *move = &picture->moved[picture->moved_count++];

      move->x = left;
      move->y = from_y;
      move->width = right - left;
      move->height = height;
      move->to_y = to_y;
    }
  else
    host_drawn (f, left, to_y, right - left, height);
}

/* Draw COLOR onto F's picture through CELLS, WIDTH by HEIGHT parts in
   255 of it laid out STRIDE bytes to the row, with its top left
   corner at X, Y.

   This is a rasterized glyph: the font engine gives its coverage of
   each pixel, and the color is laid on by that much.  */

void
host_blend_coverage (struct frame *f, unsigned char const *cells, int stride,
		     int width, int height, int x, int y, unsigned long color)
{
  struct host_picture *picture = host_frame_picture (f);
  unsigned int pixel;
  int box_x = x, box_y = y, box_width = width, box_height = height;
  int row, column;

  if (!picture || !host_clip (picture, &box_x, &box_y, &box_width, &box_height))
    return;

  pixel = host_pixel (color);
  for (row = box_y; row < box_y + box_height; row++)
    {
      unsigned int *into = picture->cells + (ptrdiff_t) row * picture->width;
      unsigned char const *from = cells + (ptrdiff_t) (row - y) * stride;

      for (column = box_x; column < box_x + box_width; column++)
	into[column] = host_blend (pixel, into[column], from[column - x]);
    }

  host_drawn (f, box_x, box_y, box_width, box_height);
}

/* Base64, which is how the picture travels: the host is spoken to in
   JSON, and pixels are not text.  */

static const char host_base64_digits[]
  = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Write LENGTH bytes of FROM to TO as base64, and return where the
   next character would go.  TO must have room for four characters per
   three bytes, rounded up.  */

char *
host_base64 (char *to, unsigned char const *from, ptrdiff_t length)
{
  ptrdiff_t i;

  for (i = 0; i + 2 < length; i += 3)
    {
      unsigned int three = from[i] << 16 | from[i + 1] << 8 | from[i + 2];

      *to++ = host_base64_digits[(three >> 18) & 63];
      *to++ = host_base64_digits[(three >> 12) & 63];
      *to++ = host_base64_digits[(three >> 6) & 63];
      *to++ = host_base64_digits[three & 63];
    }

  if (i < length)
    {
      bool pair = i + 1 < length;
      unsigned int rest = from[i] << 16 | (pair ? from[i + 1] << 8 : 0);

      *to++ = host_base64_digits[(rest >> 18) & 63];
      *to++ = host_base64_digits[(rest >> 12) & 63];
      *to++ = pair ? host_base64_digits[(rest >> 6) & 63] : '=';
      *to++ = '=';
    }

  return to;
}

/* The name Lisp knows F by, which is what names the element its
   picture is shown in (`urusi-screen--frame-name').  */

static void
host_frame_name (struct frame *f, char *name, size_t room)
{
  Lisp_Object frame;

  XSETFRAME (frame, f);
  snprintf (name, room, "%lx",
	    (unsigned long) XUFIXNUM (Fsxhash_eq (frame)));
}

/* Give the host the part of F's picture drawn into since it was last
   given any, and take that there is now nothing to give.

   Said to the host rather than to Lisp: it is a picture, and there is
   nothing in it for Lisp to decide.  Every frame has a picture of its
   own and is named in what is sent, so that a child frame floating
   over another is shown over it.  */

void
host_show_picture (struct frame *f)
{
  struct host_picture *picture = &FRAME_OUTPUT_DATA (f)->picture;
  const struct host_api *api = host_current_api ();
  char name[32];
  ptrdiff_t room = 0;
  unsigned char *box;
  char *message, *at;
  int i, row;

  if (!api || !picture->cells
      || (picture->drawn_count <= 0 && picture->moved_count <= 0))
    return;

  host_frame_name (f, name, sizeof name);

  /* Room for the largest box and for the whole message: a header, and
     for each box its own header and its pixels as base64.  */
  for (i = 0; i < picture->drawn_count; i++)
    {
      ptrdiff_t bytes = ((ptrdiff_t) picture->drawn[i].width
			 * picture->drawn[i].height * sizeof *picture->cells);

      room = max (room, bytes);
    }

  box = xmalloc (room);
  message = xmalloc (192 + (ptrdiff_t) HOST_DRAWN_BOXES * 96
		     + (ptrdiff_t) picture->drawn_count
		     * (96 + 4 * ((room + 2) / 3)));
  at = message + sprintf (message,
			  "{\"type\":\"picture\",\"frame\":\"%s\","
			  "\"width\":%d,\"height\":%d,\"moved\":[",
			  name, picture->width, picture->height);

  for (i = 0; i < picture->moved_count; i++)
    {
      struct host_move *move = &picture->moved[i];

      at += sprintf (at, "%s{\"x\":%d,\"y\":%d,\"width\":%d,"
		     "\"height\":%d,\"toY\":%d}", i ? "," : "",
		     move->x, move->y, move->width, move->height, move->to_y);
    }

  at += sprintf (at, "],\"drawn\":[");

  for (i = 0; i < picture->drawn_count; i++)
    {
      struct host_box *drawn = &picture->drawn[i];
      ptrdiff_t stride = (ptrdiff_t) drawn->width * sizeof *picture->cells;

      /* The box on its own, since base64 reads it a row at a time and
	 the picture is wider than the box.  */
      for (row = 0; row < drawn->height; row++)
	memcpy (box + (ptrdiff_t) row * stride,
		picture->cells + (ptrdiff_t) (drawn->y + row) * picture->width
		+ drawn->x,
		stride);

      at += sprintf (at, "%s{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,"
		     "\"cells\":\"", i ? "," : "",
		     drawn->x, drawn->y, drawn->width, drawn->height);
      at = host_base64 (at, box, (ptrdiff_t) drawn->height * stride);
      at += sprintf (at, "\"}");
    }

  strcpy (at, "]}");
  api->post (message);

  /* The first of them say what a picture costs, as the screens do:
     long enough to see what a keystroke sends, and then quiet.  */
  {
    static int told;

    if (told < 30)
      {
	ptrdiff_t drawn = 0;

	for (i = 0; i < picture->drawn_count; i++)
	  drawn += ((ptrdiff_t) picture->drawn[i].width
		    * picture->drawn[i].height);
	fprintf (stderr,
		 "picture %d: %d moved, %d boxes, %ld pixels, %ld bytes\n",
		 ++told, picture->moved_count, picture->drawn_count, (long) drawn,
		 (long) (at - message + 2));
      }
  }

  xfree (message);
  xfree (box);
  host_forget_drawn (f);
}
