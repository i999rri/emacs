/* Talking to a host application that loads Emacs as libemacs.dll.

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

#include <config.h>

#include <windows.h>

#include "lisp.h"
#include "coding.h"
#include "w32common.h"
#include "w32host.h"

/* The host's interface, or null when Emacs runs on its own.  */
static const struct w32_host_api *host_api;

/* Messages the host has sent and Lisp has not taken yet, oldest
   first.  The host writes them from a thread of its own, so
   everything here is under event_lock.

   They live in the heap of the process, not Emacs's: Emacs's malloc
   is its own and belongs to its own thread, and free in Emacs's
   sources is a macro for it, which would not match what another
   thread allocated.  */
struct host_event
{
  struct host_event *next;
  char *message;
};

static struct host_event *event_head;
static struct host_event *event_tail;
static CRITICAL_SECTION event_lock;

/* Stop the queue from growing without bound if Lisp never takes the
   events, as when the host sends to an Emacs that is busy or wedged.
   The oldest go first: the newest are the ones still worth acting
   on.  */
#define MAX_PENDING_EVENTS 4096
static int pending_events;

/* Take one message from the host.  This runs on the host's thread,
   where none of Emacs may be touched, so it only copies the message
   and queues it.  */
static void
receive_host_event (void *data, const char *message)
{
  HANDLE heap = GetProcessHeap ();
  size_t size = message ? strlen (message) + 1 : 0;
  struct host_event *event;
  char *copy;

  if (!message)
    return;

  event = HeapAlloc (heap, 0, sizeof *event);
  copy = HeapAlloc (heap, 0, size);
  if (!event || !copy)
    {
      if (event)
	HeapFree (heap, 0, event);
      if (copy)
	HeapFree (heap, 0, copy);
      return;
    }

  memcpy (copy, message, size);
  event->next = NULL;
  event->message = copy;

  EnterCriticalSection (&event_lock);

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
      HeapFree (heap, 0, oldest->message);
      HeapFree (heap, 0, oldest);
    }

  LeaveCriticalSection (&event_lock);
}

DEFUN ("w32-host-available-p", Fw32_host_available_p, Sw32_host_available_p,
       0, 0, 0,
       doc: /* Return t if Emacs runs inside a host application.
That is the case when Emacs was loaded as a library by a program that
draws the user interface itself, rather than started as a program of
its own.  */)
  (void)
{
  return host_api ? Qt : Qnil;
}

DEFUN ("w32-host-post", Fw32_host_post, Sw32_host_post, 1, 1, 0,
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

DEFUN ("w32-host-take-events", Fw32_host_take_events, Sw32_host_take_events,
       0, 0, 0,
       doc: /* Return the messages the host application has sent since the
last call, as a list of strings, oldest first, and forget them.  */)
  (void)
{
  HANDLE heap = GetProcessHeap ();
  Lisp_Object events = Qnil;
  struct host_event *taken = NULL;

  if (!host_api)
    return Qnil;

  /* Take the whole queue at once, so that decoding the messages, which
     may signal, does not hold the lock against the host.  */
  EnterCriticalSection (&event_lock);
  taken = event_head;
  event_head = event_tail = NULL;
  pending_events = 0;
  LeaveCriticalSection (&event_lock);

  while (taken)
    {
      struct host_event *event = taken;

      taken = event->next;
      events = Fcons (build_string_from_utf8 (event->message), events);
      HeapFree (heap, 0, event->message);
      HeapFree (heap, 0, event);
    }

  return Fnreverse (events);
}

void
init_w32host (void)
{
  w32_host_get_api_fn get_api
    = (w32_host_get_api_fn) get_proc_addr (GetModuleHandle (NULL),
					   "w32_host_get_api");
  const struct w32_host_api *api;

  /* No host exports this, and neither does emacs.exe.  */
  if (!get_api)
    return;

  api = get_api (W32_HOST_API_VERSION);
  if (!api || api->version != W32_HOST_API_VERSION
      || !api->post || !api->on_event)
    return;

  InitializeCriticalSection (&event_lock);
  host_api = api;

  /* Last, because the host may send from this call onwards.  */
  api->on_event (receive_host_event, NULL);
}

void
syms_of_w32host (void)
{
  defsubr (&Sw32_host_available_p);
  defsubr (&Sw32_host_post);
  defsubr (&Sw32_host_take_events);
}
