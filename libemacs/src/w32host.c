/* What of the host is particular to Windows.

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

/* A host on Windows may give Emacs a window to make its frames in:
   windows of Emacs's own on no screen, which the host draws.  The
   rest of talking to the host is the same on every system, in
   host.c.  */

#include <config.h>

#include <windows.h>

#include "lisp.h"
#include "frame.h"
#include "w32common.h"
#include "w32term.h"
#include "w32host.h"
#include "hostlib.h"

HWND
w32_host_window (void)
{
  const struct host_api *api = host_current_api ();

  return api && api->window ? (HWND) api->window () : NULL;
}

HWND
w32_dialog_owner (struct frame *f)
{
  HWND host = w32_host_window ();

  if (host && FRAME_W32_P (f) && f->output_data.w32->host_drawn)
    return host;
  return FRAME_W32_WINDOW (f);
}

void
w32_host_frame_created (HWND window)
{
  const struct host_api *api = host_current_api ();
  char message[128];

  if (!api)
    return;

  /* A window handle fits in a JSON number: Windows keeps them small
     enough that no precision is lost.  */
  sprintf (message, "{\"type\":\"frame\",\"window\":%"PRIdMAX"}",
	   (intmax_t) (INT_PTR) window);
  api->post (message);
}
