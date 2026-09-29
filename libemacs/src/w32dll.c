/* Entry point of libemacs.dll, Emacs built as a DLL for a host application.

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

/* The host loads libemacs.dll and calls w32_emacs_init on a thread it
   creates, with the same arguments emacs.exe would get.  That thread
   becomes Emacs's main thread.  It needs the 8 MB stack emacs.exe is
   linked with, and w32_emacs_init does not return until Emacs exits;
   kill-emacs still ends the whole process.  */

#include <config.h>

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include "w32common.h"

extern BOOL ctrl_c_handler (unsigned long);

__declspec (dllexport) int w32_emacs_init (int, char **);

/* Give Emacs HANDLE as file descriptor FD, opened with FLAGS.

   A host application has no console, so the C runtime Emacs is linked
   against started with nothing at descriptors 0, 1 and 2: what Emacs
   writes to its standard output goes nowhere, and what it would read
   from its standard input cannot be read.  The host puts handles of
   its own in place before Emacs starts; this is what binds them to the
   descriptors the runtime counts in, which SetStdHandle alone does
   not.

   The handle is duplicated because the descriptor owns what it is
   given: closing it would close the host's, and the host goes on using
   it after Emacs has finished with it.  */

static void
open_standard_stream (HANDLE handle, int fd, int flags)
{
  HANDLE own;
  int opened;

  if (!handle || handle == INVALID_HANDLE_VALUE)
    return;

  if (!DuplicateHandle (GetCurrentProcess (), handle,
			GetCurrentProcess (), &own,
			0, FALSE, DUPLICATE_SAME_ACCESS))
    return;

  opened = _open_osfhandle ((intptr_t) own, flags);
  if (opened < 0)
    {
      CloseHandle (own);
      return;
    }

  if (opened != fd)
    {
      _dup2 (opened, fd);
      _close (opened);
    }
}

int
w32_emacs_init (int argc, char **argv)
{
  /* The same setup _start in w32proc.c does in emacs.exe before it
     hands control to the CRT.  Here the CRT is already running and main
     is the host's, so do it here and call Emacs's main directly.

     malloc, realloc and free call through these pointers, which stay
     null until init_heap sets them, so catch any use before that.  */
  the_malloc_fn = malloc_before_init;
  the_realloc_fn = realloc_before_init;
  the_free_fn = free_before_init;

  cache_system_info ();

  /* Before anything of Emacs runs, since what Emacs says goes through
     these.

     Standard output is taken from standard error when it cannot be
     read: a process the shell starts can have the monitor to open its
     window on kept where its standard output would be, and then asking
     for it answers nothing whatever was put there (see create_child).
     Standard error is never used that way, and the host puts the same
     pipe in both.  */
  HANDLE input = GetStdHandle (STD_INPUT_HANDLE);
  HANDLE output = GetStdHandle (STD_OUTPUT_HANDLE);
  HANDLE error = GetStdHandle (STD_ERROR_HANDLE);

  open_standard_stream (input, 0, _O_RDONLY | _O_BINARY);
  open_standard_stream (output ? output : error, 1, _O_WRONLY | _O_BINARY);
  open_standard_stream (error, 2, _O_WRONLY | _O_BINARY);

  /* And the process is left with the standard handles emacs.exe has,
     which is to say none: the host's are Emacs's descriptors now, and
     have no business being handed on to anything else.  */
  SetStdHandle (STD_INPUT_HANDLE, NULL);
  SetStdHandle (STD_OUTPUT_HANDLE, NULL);
  SetStdHandle (STD_ERROR_HANDLE, NULL);

  /* Keep Ctrl-C in the console from ending the process, and do not let
     a missing removable drive stop Emacs with a system dialog.  Both
     apply to the whole host process.  */
  SetConsoleCtrlHandler ((PHANDLER_ROUTINE) ctrl_c_handler, TRUE);
  SetErrorMode (SEM_FAILCRITICALERRORS);

  return w32_emacs_main (argc, argv);
}
