/* The window system types of frames that a host application draws.

Copyright (C) 2026 Free Software Foundation, Inc.
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

/* What dispextern.h asks of every window system: the types of a
   pixmap, a window and a pointer shape, a rectangle, and the names X
   gave to the parts of a geometry.  A host frame has no window and no
   pointer of its own, so the handles are only pointers that nothing
   here looks into; they are what haikugui.h has, for the same
   reason.  */

#ifndef HOST_GUI_H
#define HOST_GUI_H

typedef void *Emacs_Pixmap;
typedef void *Emacs_Window;
typedef void *Emacs_Cursor;
typedef void *Window;
typedef int Display;

typedef struct host_char_struct
{
  int rbearing;
  int lbearing;
  int width;
  int ascent;
  int descent;
} XCharStruct;

struct host_rect
{
  int x, y;
  int width, height;
};

#define NativeRectangle struct host_rect
#define CONVERT_TO_EMACS_RECT(xr, nr)	\
  ((xr).x     = (nr).x,			\
   (xr).y     = (nr).y,			\
   (xr).width = (nr).width,		\
   (xr).height = (nr).height)

#define CONVERT_FROM_EMACS_RECT(xr, nr)	\
  ((nr).x    = (xr).x,			\
   (nr).y    = (xr).y,			\
   (nr).width  = (xr).width,		\
   (nr).height = (xr).height)

#define STORE_NATIVE_RECT(nr, px, py, pwidth, pheight)	\
  ((nr).x    = (px),					\
   (nr).y    = (py),					\
   (nr).width  = (pwidth),				\
   (nr).height = (pheight))

/* The gravities and geometry masks of X, which `x-parse-geometry' and
   the frame code use whatever the window system is.  */

#define ForgetGravity		0
#define NorthWestGravity	1
#define NorthGravity		2
#define NorthEastGravity	3
#define WestGravity		4
#define CenterGravity		5
#define EastGravity		6
#define SouthWestGravity	7
#define SouthGravity		8
#define SouthEastGravity	9
#define StaticGravity		10

#define NoValue		0x0000
#define XValue  	0x0001
#define YValue		0x0002
#define WidthValue  	0x0004
#define HeightValue  	0x0008
#define AllValues 	0x000F
#define XNegative 	0x0010
#define YNegative 	0x0020

#define USPosition	(1L << 0) /* user specified x, y */
#define USSize		(1L << 1) /* user specified width, height */
#define PPosition	(1L << 2) /* program specified position */
#define PSize		(1L << 3) /* program specified size */
#define PMinSize	(1L << 4) /* program specified minimum size */
#define PMaxSize	(1L << 5) /* program specified maximum size */
#define PResizeInc	(1L << 6) /* program specified resize increments */
#define PAspect		(1L << 7) /* program specified min, max aspect ratios */
#define PBaseSize	(1L << 8) /* program specified base for incrementing */
#define PWinGravity	(1L << 9) /* program specified window gravity */

/* A pixel is 0xRRGGBB, as the host is told colors.  */
#define RGB_TO_ULONG(r, g, b) (((r) << 16) | ((g) << 8) | (b))
#define RED_FROM_ULONG(color)	(((color) >> 16) & 0xff)
#define GREEN_FROM_ULONG(color)	(((color) >> 8) & 0xff)
#define BLUE_FROM_ULONG(color)	((color) & 0xff)
#define RED16_FROM_ULONG(color)		(RED_FROM_ULONG (color) * 0x101)
#define GREEN16_FROM_ULONG(color)	(GREEN_FROM_ULONG (color) * 0x101)
#define BLUE16_FROM_ULONG(color)	(BLUE_FROM_ULONG (color) * 0x101)

#endif /* HOST_GUI_H */
