/* Entry point of emacs.exe on MS-Windows.

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

/* _start lives in its own file, apart from the rest of w32proc.c, so
   that libemacs.dll can leave it out: it hands control to the CRT's
   mainCRTStartup, which only an executable has.  The DLL's entry point
   is w32_emacs_init in w32dll.c.  */

#include <config.h>

#include <windows.h>

#include "w32common.h"

extern BOOL ctrl_c_handler (unsigned long type);

/* MinGW64 doesn't add a leading underscore to external symbols,
   whereas configure.ac sets up LD_SWITCH_SYSTEM_TEMACS to force the
   entry point at __start, with two underscores.  */
#ifdef __MINGW64__
#define _start __start
#endif

extern void mainCRTStartup (void);

/* Startup code for running on NT.  When we are running as the dumped
   version, we need to bootstrap our heap and .bss section into our
   address space before we can actually hand off control to the startup
   code supplied by NT (primarily because that code relies upon malloc ()).  */
void _start (void);

void
_start (void)
{

#if 1
  /* Give us a way to debug problems with crashes on startup when
     running under the MSVC profiler. */
  if (GetEnvironmentVariable ("EMACS_DEBUG", NULL, 0) > 0)
    DebugBreak ();
#endif

  the_malloc_fn = malloc_before_init;
  the_realloc_fn = realloc_before_init;
  the_free_fn = free_before_init;

  /* Cache system info, e.g., the NT page size.  */
  cache_system_info ();

  /* This prevents ctrl-c's in shells running while we're suspended from
     having us exit.  */
  SetConsoleCtrlHandler ((PHANDLER_ROUTINE) ctrl_c_handler, TRUE);

  /* Prevent Emacs from being locked up (eg. in batch mode) when
     accessing devices that aren't mounted (eg. removable media drives).  */
  SetErrorMode (SEM_FAILCRITICALERRORS);
  mainCRTStartup ();
}
