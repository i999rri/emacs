/* Talking to a host application, on any system.

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

/* The host's table, found as Emacs starts, and the messages the host
   has sent, kept until Lisp takes them.  Nothing here knows what a
   message says, or what system it runs on but for where to find the
   table and whose memory to keep the messages in.  */

#include <config.h>

#include <stdlib.h>
#include <string.h>

#ifdef WINDOWSNT
#include <windows.h>
#include "w32common.h"
#else
#include <dlfcn.h>
#endif

#include "lisp.h"
#include "coding.h"
#include "systhread.h"
#include "hostlib.h"
#include "hostpipe.h"

/* The host's interface, or null when Emacs runs on its own.  */
static const struct host_api *host_api;

/* Messages the host has sent and Lisp has not taken yet, oldest
   first.  The host writes them from a thread of its own, so
   everything here is under event_lock.  */
struct host_event
{
  struct host_event *next;
  char *message;
};

static struct host_event *event_head;
static struct host_event *event_tail;
static sys_mutex_t event_lock;

/* Stop the queue from growing without bound if Lisp never takes the
   events, as when the host sends to an Emacs that is busy or wedged.
   The oldest go first: the newest are the ones still worth acting
   on.  */
#define MAX_PENDING_EVENTS 4096
static int pending_events;

void *
host_alloc (size_t size)
{
#ifdef WINDOWSNT
  return HeapAlloc (GetProcessHeap (), 0, size);
#else
  return malloc (size);
#endif
}

void *
host_realloc (void *block, size_t size)
{
#ifdef WINDOWSNT
  return block ? HeapReAlloc (GetProcessHeap (), 0, block, size)
	       : HeapAlloc (GetProcessHeap (), 0, size);
#else
  return realloc (block, size);
#endif
}

void
host_free (void *block)
{
#ifdef WINDOWSNT
  HeapFree (GetProcessHeap (), 0, block);
#else
  free (block);
#endif
}

const struct host_api *
host_current_api (void)
{
  return host_api;
}

/* Take one message from the host.  This runs on the host's thread,
   where none of Emacs may be touched, so it only copies the message
   and queues it.  */
static void
receive_host_event (void *data, const char *message)
{
  size_t size = message ? strlen (message) + 1 : 0;
  struct host_event *event;
  char *copy;

  if (!message)
    return;

  event = host_alloc (sizeof *event);
  copy = host_alloc (size);
  if (!event || !copy)
    {
      if (event)
	host_free (event);
      if (copy)
	host_free (copy);
      return;
    }

  memcpy (copy, message, size);
  event->next = NULL;
  event->message = copy;

  sys_mutex_lock (&event_lock);

  if (event_tail)
    event_tail->next = event;
  else
    event_head = event;
  event_tail = event;
  pending_events++;

  while (MAX_PENDING_EVENTS < pending_events)
    {
      struct host_event *oldest = event_head;

      event_head = oldest->next;
      if (!event_head)
	event_tail = NULL;
      pending_events--;
      host_free (oldest->message);
      host_free (oldest);
    }

  sys_mutex_unlock (&event_lock);
}

DEFUN ("host-available-p", Fhost_available_p, Shost_available_p,
       0, 0, 0,
       doc: /* Return t if Emacs runs inside a host application.
That is the case when Emacs was loaded as a library by a program that
draws the user interface itself, rather than started as a program of
its own, or was started by one to talk to over a pipe.  */)
  (void)
{
  return host_api ? Qt : Qnil;
}

DEFUN ("host-post", Fhost_post, Shost_post, 1, 1, 0,
       doc: /* Send MESSAGE, a string, to the host application.
Return t if it was sent, nil if there is no host.  What a message may
say is up to the host.  */)
  (Lisp_Object message)
{
  CHECK_STRING (message);

  if (!host_api)
    return Qnil;

  host_api->post (SSDATA (ENCODE_UTF_8 (message)));
  return Qt;
}

DEFUN ("host-take-events", Fhost_take_events, Shost_take_events,
       0, 0, 0,
       doc: /* Return the messages the host application has sent since the
last call, as a list of strings, oldest first, and forget them.  */)
  (void)
{
  Lisp_Object events = Qnil;
  struct host_event *taken = NULL;

  if (!host_api)
    return Qnil;

  /* Take the whole queue at once, so that decoding the messages, which
     may signal, does not hold the lock against the host.  */
  sys_mutex_lock (&event_lock);
  taken = event_head;
  event_head = event_tail = NULL;
  pending_events = 0;
  sys_mutex_unlock (&event_lock);

  while (taken)
    {
      struct host_event *event = taken;

      taken = event->next;
      events = Fcons (build_string_from_utf8 (event->message), events);
      host_free (event->message);
      host_free (event);
    }

  return Fnreverse (events);
}

/* What the program Emacs runs in exports for Emacs, or null.  */
static host_get_api_fn
exported_get_api (void)
{
#ifdef WINDOWSNT
  return (host_get_api_fn) get_proc_addr (GetModuleHandle (NULL), HOST_GET_API);
#else
  return (host_get_api_fn) dlsym (RTLD_DEFAULT, HOST_GET_API);
#endif
}

void
init_host (void)
{
  host_get_api_fn get_api = exported_get_api ();
  /* No host exports this, and neither does Emacs started on its own;
     a host that only started Emacs may still be at the other end of
     its pipes.  */
  const struct host_api *api = get_api ? get_api (HOST_API_VERSION) : host_pipe_api ();

  if (!api || api->version != HOST_API_VERSION
      || !api->post || !api->on_event)
    return;

  sys_mutex_init (&event_lock);
  host_api = api;

  /* Last, because the host may send from this call onwards.  */
  api->on_event (receive_host_event, NULL);
}

void
syms_of_host (void)
{
  defsubr (&Shost_available_p);
  defsubr (&Shost_post);
  defsubr (&Shost_take_events);

  /* The names these had while there was only Windows, for a while.  */
  Ffset (intern_c_string ("w32-host-available-p"), intern_c_string ("host-available-p"));
  Ffset (intern_c_string ("w32-host-post"), intern_c_string ("host-post"));
  Ffset (intern_c_string ("w32-host-take-events"), intern_c_string ("host-take-events"));
}
