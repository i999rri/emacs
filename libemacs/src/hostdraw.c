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

/* Take in that the box X, Y, WIDTH by HEIGHT of F's picture was drawn
   into, so that the host is given it.  */

static void
host_drawn (struct frame *f, int x, int y, int width, int height)
{
  struct host_picture *picture = &FRAME_OUTPUT_DATA (f)->picture;

  if (width <= 0 || height <= 0)
    return;

  if (picture->drawn_width <= 0 || picture->drawn_height <= 0)
    {
      picture->drawn_x = x;
      picture->drawn_y = y;
      picture->drawn_width = width;
      picture->drawn_height = height;
      return;
    }

  {
    int left = min (picture->drawn_x, x);
    int top = min (picture->drawn_y, y);
    int right = max (picture->drawn_x + picture->drawn_width, x + width);
    int bottom = max (picture->drawn_y + picture->drawn_height, y + height);

    picture->drawn_x = left;
    picture->drawn_y = top;
    picture->drawn_width = right - left;
    picture->drawn_height = bottom - top;
  }
}

void
host_forget_drawn (struct frame *f)
{
  struct host_picture *picture = &FRAME_OUTPUT_DATA (f)->picture;

  picture->drawn_x = picture->drawn_y = 0;
  picture->drawn_width = picture->drawn_height = 0;
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

static char *
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

/* Give the host the part of F's picture drawn into since it was last
   given any, and take that there is now nothing to give.

   Said to the host rather than to Lisp: it is a picture, and there is
   nothing in it for Lisp to decide.  Only the frame being drawn is
   sent, which is the one the host shows; a child frame has its own
   picture and is still to be sent.  */

void
host_show_picture (struct frame *f)
{
  struct host_picture *picture = &FRAME_OUTPUT_DATA (f)->picture;
  const struct host_api *api = host_current_api ();
  int x = picture->drawn_x, y = picture->drawn_y;
  int width = picture->drawn_width, height = picture->drawn_height;
  ptrdiff_t bytes = (ptrdiff_t) width * height * sizeof *picture->cells;
  unsigned char *box;
  char *message, *at;
  int row;

  if (!api || !picture->cells || width <= 0 || height <= 0)
    return;

  /* The box on its own, since base64 has to read it a row at a time
     and the picture is wider than the box.  */
  box = xmalloc (bytes);
  for (row = 0; row < height; row++)
    memcpy (box + (ptrdiff_t) row * width * sizeof *picture->cells,
	    picture->cells + (ptrdiff_t) (y + row) * picture->width + x,
	    (ptrdiff_t) width * sizeof *picture->cells);

  message = xmalloc (128 + 4 * ((bytes + 2) / 3) + 4);
  at = message + sprintf (message,
			  "{\"type\":\"picture\",\"width\":%d,\"height\":%d,"
			  "\"drawn\":{\"x\":%d,\"y\":%d,"
			  "\"width\":%d,\"height\":%d},\"cells\":\"",
			  picture->width, picture->height,
			  x, y, width, height);
  at = host_base64 (at, box, bytes);
  strcpy (at, "\"}");

  api->post (message);

  xfree (message);
  xfree (box);
  host_forget_drawn (f);
}
