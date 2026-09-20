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

/* A host application that owns the window and loads libemacs.dll
   exports w32_host_get_api from its executable, and Emacs asks for it
   once, at startup.  A host that does not, and emacs.exe, leave Emacs
   as it is.

   Messages both ways are UTF-8 strings, with nothing said here about
   what is in them: that is between the host and the Lisp it runs.
   Only C types cross, because the host may be built with another
   compiler and another C runtime than Emacs.  */

#ifndef W32HOST_H
#define W32HOST_H

#define W32_HOST_API_VERSION 1

/* Called by the host with one message, from a thread of its own.  The
   message belongs to the host and is not kept.  */
typedef void (*w32_host_event_fn) (void *data, const char *message);

struct w32_host_api
{
  unsigned version;

  /* Emacs to the host.  Called on Emacs's thread.  */
  void (*post) (const char *message);

  /* The host to Emacs.  Called once, with the function that takes the
     messages of the host and the data to pass back to it.  */
  void (*on_event) (w32_host_event_fn fn, void *data);
};

/* What the host exports.  Returns null if it does not speak VERSION.  */
typedef const struct w32_host_api *(*w32_host_get_api_fn) (unsigned version);

/* Ask the host for its interface.  Does nothing if there is no host.  */
extern void init_w32host (void);
extern void syms_of_w32host (void);

#endif /* W32HOST_H */
