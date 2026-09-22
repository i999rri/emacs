/* What libemacs's files share with each other, and with Emacs.

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

#ifndef HOSTLIB_H
#define HOSTLIB_H

#include <stddef.h>

#include "host.h"

/* The host's table, or null when there is no host.  */
extern const struct host_api *host_current_api (void);

/* Memory a thread of the host's may use: not Emacs's own, which
   belongs to Emacs's thread on some systems.  */
extern void *host_alloc (size_t);
extern void *host_realloc (void *, size_t);
extern void host_free (void *);

/* Find the host as Emacs starts, and give Lisp what it has of it.
   Called from emacs.c on every system.  */
extern void init_host (void);
extern void syms_of_host (void);
extern void syms_of_hostscreen (void);

#endif /* HOSTLIB_H */
