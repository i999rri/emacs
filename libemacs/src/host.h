/* Interface to a host application that loads Emacs as a library.

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

/* A host application that owns the window and loads Emacs as a
   library exports host_get_api from its executable, and Emacs asks
   for it once, at startup.  A host that does not, and Emacs started
   as a program of its own, leave Emacs as it is.

   Messages both ways are UTF-8 strings, each a JSON object, with
   nothing said here about what is in them: that is between the host
   and Emacs, and is the same on every system.  Only C types cross,
   because the host may be built with another compiler and another C
   runtime than Emacs.  Nothing here is particular to one system, so
   that one header serves a host on any of them.  */

#ifndef HOST_H
#define HOST_H

#define HOST_API_VERSION 3

/* The name of what the host exports.  */
#define HOST_GET_API "host_get_api"

/* Called by the host with one message, from a thread of its own.  The
   message belongs to the host and is not kept.  */
typedef void (*host_event_fn) (void *data, const char *message);

struct host_api
{
  unsigned version;

  /* Emacs to the host.  Called on Emacs's thread.  */
  void (*post) (const char *message);

  /* The host to Emacs.  Called once, with the function that takes the
     messages of the host and the data to pass back to it.  */
  void (*on_event) (host_event_fn fn, void *data);

  /* A window of the system's that frames are to be made in, or null to
     leave Emacs its own.  Only a host on Windows gives one today, for
     frames that are windows of Emacs's on no screen, which the host
     draws; the host places and sizes them itself.  May be null.  */
  void *(*window) (void);
};

/* What the host exports.  Returns null if it does not speak VERSION.  */
typedef const struct host_api *(*host_get_api_fn) (unsigned version);

#endif /* HOST_H */
