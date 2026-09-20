/* Entry point of libemacs.dll, Emacs built as a DLL for a host application.

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

/* The host loads libemacs.dll and calls w32_emacs_init on a thread it
   creates, with the same arguments emacs.exe would get.  That thread
   becomes Emacs's main thread.  It needs the 8 MB stack emacs.exe is
   linked with, and w32_emacs_init does not return until Emacs exits;
   kill-emacs still ends the whole process.  */

#include <config.h>

#include <windows.h>

#include "w32common.h"

extern BOOL ctrl_c_handler (unsigned long);

__declspec (dllexport) int w32_emacs_init (int, char **);

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

  /* Keep Ctrl-C in the console from ending the process, and do not let
     a missing removable drive stop Emacs with a system dialog.  Both
     apply to the whole host process.  */
  SetConsoleCtrlHandler ((PHANDLER_ROUTINE) ctrl_c_handler, TRUE);
  SetErrorMode (SEM_FAILCRITICALERRORS);

  return w32_emacs_main (argc, argv);
}
