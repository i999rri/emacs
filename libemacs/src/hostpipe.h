/* The host's table, spoken over a pipe.

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

#ifndef HOSTPIPE_H
#define HOSTPIPE_H

#include "host.h"

/* The environment variable that has Emacs talk to a host over its
   standard input and output, when no host has loaded it.  */
#define HOST_PIPE_VARIABLE "EMACS_HOST_PIPE"

/* A table that sends each message as a line on the standard output,
   and reads the host's as lines from the standard input, or null if
   Emacs was not asked to.  Takes the two for itself, before anything
   else of Emacs uses them.  */
extern const struct host_api *host_pipe_api (void);

#endif /* HOSTPIPE_H */
