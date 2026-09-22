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

/* A host application loads Emacs and gives it a table of functions to
   call.  A program that only starts Emacs, a host for tests above
   all, cannot: it has Emacs's standard input and output instead.
   This is the same table made out of those, so that the rest of Emacs
   cannot tell the two apart.

   One message is one line: JSON says nothing that needs a newline of
   its own, and Emacs writes none into a message.  */

#include <config.h>

#include <stdlib.h>
#include <string.h>

#ifdef WINDOWSNT
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "lisp.h"
#include "systhread.h"
#include "hostpipe.h"

/* The two ends, taken from Emacs's standard input and output as Emacs
   starts, so that nothing Emacs prints later reaches the host.  */
#ifdef WINDOWSNT
static HANDLE pipe_in = INVALID_HANDLE_VALUE;
static HANDLE pipe_out = INVALID_HANDLE_VALUE;
#else
static int pipe_in = -1;
static int pipe_out = -1;
#endif

static host_event_fn event_fn;
static void *event_data;

/* Memory for the thread that reads: not Emacs's own, which belongs to
   Emacs's thread.  */
static void *
pipe_alloc (size_t size)
{
#ifdef WINDOWSNT
  return HeapAlloc (GetProcessHeap (), 0, size);
#else
  return malloc (size);
#endif
}

static void *
pipe_realloc (void *block, size_t size)
{
#ifdef WINDOWSNT
  return block ? HeapReAlloc (GetProcessHeap (), 0, block, size)
	       : HeapAlloc (GetProcessHeap (), 0, size);
#else
  return realloc (block, size);
#endif
}

static void
pipe_free (void *block)
{
#ifdef WINDOWSNT
  HeapFree (GetProcessHeap (), 0, block);
#else
  free (block);
#endif
}

/* Read up to SIZE bytes into BUFFER.  Return how many, or 0 at the end
   of the input or on an error: the host has gone either way.  */
static size_t
pipe_read (char *buffer, size_t size)
{
#ifdef WINDOWSNT
  DWORD got = 0;
  return ReadFile (pipe_in, buffer, size, &got, NULL) ? got : 0;
#else
  ssize_t got = read (pipe_in, buffer, size);
  return got < 0 ? 0 : got;
#endif
}

static void
pipe_write (const char *bytes, size_t size)
{
  while (size)
    {
#ifdef WINDOWSNT
      DWORD wrote = 0;
      if (!WriteFile (pipe_out, bytes, size, &wrote, NULL) || !wrote)
	return;
#else
      ssize_t wrote = write (pipe_out, bytes, size);
      if (wrote <= 0)
	return;
#endif
      bytes += wrote;
      size -= wrote;
    }
}

/* Emacs to the host: the message and the end of its line.  Called on
   Emacs's thread only, so the two writes are not taken apart.  */
static void
pipe_post (const char *message)
{
  pipe_write (message, strlen (message));
  pipe_write ("\n", 1);
}

/* Read the host's lines, and hand each over as it is finished.  Runs
   on a thread of its own, where none of Emacs may be touched.  */
static void *
read_lines (void *arg)
{
  size_t size = 4096, used = 0;
  char *line = pipe_alloc (size);
  char chunk[4096];

  if (!line)
    return NULL;

  for (;;)
    {
      size_t got = pipe_read (chunk, sizeof chunk);

      if (!got)
	break;

      for (size_t i = 0; i < got; i++)
	{
	  if (chunk[i] == '\n')
	    {
	      /* A line from a host on Windows may end in CRLF.  */
	      if (used && line[used - 1] == '\r')
		used--;
	      line[used] = '\0';
	      if (used)
		event_fn (event_data, line);
	      used = 0;
	      continue;
	    }

	  if (used + 1 >= size)
	    {
	      char *bigger = pipe_realloc (line, size * 2);

	      if (!bigger)
		{
		  pipe_free (line);
		  return NULL;
		}
	      line = bigger;
	      size *= 2;
	    }
	  line[used++] = chunk[i];
	}
    }

  pipe_free (line);
  return NULL;
}

/* The host to Emacs: start reading, now that there is somewhere for
   what is read to go.  */
static void
pipe_on_event (host_event_fn fn, void *data)
{
  sys_thread_t thread;

  event_fn = fn;
  event_data = data;
  if (!sys_thread_create (&thread, read_lines, NULL))
    event_fn = NULL;
}

static const struct host_api pipe_api =
  {
    HOST_API_VERSION,
    pipe_post,
    pipe_on_event,
    /* No window to make frames in: a host over a pipe has none.  */
    NULL,
  };

const struct host_api *
host_pipe_api (void)
{
  const char *wanted = getenv (HOST_PIPE_VARIABLE);

  if (!wanted || !*wanted)
    return NULL;

#ifdef WINDOWSNT
  /* Duplicated, so that Emacs closing or replacing its standard
     handles later leaves these alone.  */
  if (!DuplicateHandle (GetCurrentProcess (), GetStdHandle (STD_INPUT_HANDLE),
			GetCurrentProcess (), &pipe_in, 0, FALSE,
			DUPLICATE_SAME_ACCESS)
      || !DuplicateHandle (GetCurrentProcess (), GetStdHandle (STD_OUTPUT_HANDLE),
			   GetCurrentProcess (), &pipe_out, 0, FALSE,
			   DUPLICATE_SAME_ACCESS))
    return NULL;
#else
  pipe_in = dup (STDIN_FILENO);
  pipe_out = dup (STDOUT_FILENO);
  if (pipe_in < 0 || pipe_out < 0)
    return NULL;
#endif

  return &pipe_api;
}
