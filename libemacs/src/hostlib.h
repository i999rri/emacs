/* What libemacs's files share with each other, and with Emacs.

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

#ifndef HOSTLIB_H
#define HOSTLIB_H

#include <stdbool.h>
#include <stddef.h>

#include "host.h"

/* The host's table, or null when there is no host.  */
extern const struct host_api *host_current_api (void);

/* Memory a thread of the host's may use: not Emacs's own, which
   belongs to Emacs's thread on some systems.  */
extern void *host_alloc (size_t);
extern void *host_realloc (void *, size_t);
extern void host_free (void *);

/* Have the messages IS_INPUT says are input kept apart from those
   Lisp takes, for host_take_input, and WAKEUP called each time any
   message comes, of either kind.  Both are called on the host's
   thread, and may use nothing of Emacs.  Does nothing when there is
   no host.  */
extern void host_claim_input (bool (*is_input) (const char *),
			      void (*wakeup) (void));

/* The oldest input message not yet taken, to be freed with host_free,
   or null if there is none.  */
extern char *host_take_input (void);

/* Whether there is a message waiting for Lisp to take.  */
extern bool host_lisp_pending_p (void);

/* Whether Lisp has been told of the messages waiting, and saying that
   it has been.  Told once for all of them, since being told is an event
   in the keyboard buffer and the keys wait there too.  Forgotten as
   Lisp takes the messages, so whoever is told must take them.  */
extern bool host_lisp_told_p (void);
extern void host_lisp_told (void);

/* Say to Lisp what C has done, as though the host had said it.  */
extern void host_notify (const char *message);

/* Find the host as Emacs starts, and give Lisp what it has of it.
   Called from emacs.c on every system.  */
extern void init_host (void);
extern void syms_of_host (void);
extern void syms_of_hostscreen (void);

#endif /* HOSTLIB_H */
