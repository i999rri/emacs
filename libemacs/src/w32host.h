/* Interface to a host application that loads Emacs as libemacs.dll.

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

/* What of the host is particular to Windows: the frames that are
   windows of Emacs's on no screen, which the host draws.  The table
   the host gives Emacs is the same on every system, in host.h.  */

#ifndef W32HOST_H
#define W32HOST_H

#include <windows.h>

#include "host.h"

/* Ask the host for its interface.  Does nothing if there is no host.  */
extern void init_w32host (void);
extern void syms_of_w32host (void);

/* The window the host wants frames in, or null when Emacs makes its
   own windows, which is whenever there is no host.  */
extern HWND w32_host_window (void);

/* The window a dialog for frame F is to belong to.  A dialog stays
   in front of the window it belongs to and keeps it from being used
   until it is answered; a frame the host draws has a window nobody
   sees, and a dialog that belonged to it would open behind whatever
   the person was looking at, with nothing to say it was waiting.  */
struct frame;
extern HWND w32_dialog_owner (struct frame *);

/* Tell the host about a frame's window, once it has been made.  */
extern void w32_host_frame_created (HWND);

#endif /* W32HOST_H */
