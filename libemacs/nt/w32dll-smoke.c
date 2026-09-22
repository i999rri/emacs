/* Smoke test for libemacs.dll.

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

/* Loads libemacs.dll from its own directory and runs Emacs on a new
   thread, passing its command line through unchanged, for example:

     w32dll-smoke --batch --eval "(message \"hi\")"

   It is also a host in the sense of w32host.h, the least one that can
   be: it prints what Emacs posts and sends the same text back as an
   event, so that both directions can be seen from a command line.

   Build in the MSYS2 mingw64 shell and place next to libemacs.dll:

     gcc -O2 -I../src -o w32dll-smoke.exe w32dll-smoke.c \
       -Wl,--stack,0x00800000

   The --stack option matters too: threads Emacs creates for Lisp take
   their stack size from the executable.  */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32host.h"

typedef int (*w32_emacs_init_fn) (int, char **);

static host_event_fn event_sink;
static void *event_data;

static void
host_post (const char *message)
{
  printf ("host: %s\n", message);
  fflush (stdout);

  /* Send it straight back, which is all a host of this size can think
     of to do with it.  */
  if (event_sink)
    event_sink (event_data, message);
}

static void
host_on_event (host_event_fn fn, void *data)
{
  event_sink = fn;
  event_data = data;
}

/* No window: a host of this size has none, and Emacs makes its own as
   it does when it runs on its own.  */
/* The window frames go in, when asked for with --host-window.  It is
   the plainest one there is, so that what Emacs does inside a window
   of a host's can be seen without a host to speak of.  */
static HWND host_hwnd;

static void *
host_window (void)
{
  return host_hwnd;
}

static LRESULT CALLBACK
host_wndproc (HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
  switch (message)
    {
    case WM_SIZE:
      {
	/* Keep the frame filling the window, as a host does.  */
	HWND child = GetWindow (hwnd, GW_CHILD);

	if (child)
	  SetWindowPos (child, HWND_TOP, 0, 0, LOWORD (lparam),
			HIWORD (lparam),
			SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS);
	return 0;
      }

    case WM_DESTROY:
      PostQuitMessage (0);
      return 0;
    }

  return DefWindowProc (hwnd, message, wparam, lparam);
}

static void
make_host_window (void)
{
  WNDCLASS class = { 0 };

  class.lpfnWndProc = host_wndproc;
  class.hInstance = GetModuleHandle (NULL);
  class.hCursor = LoadCursor (NULL, IDC_ARROW);
  class.hbrBackground = (HBRUSH) (COLOR_APPWORKSPACE + 1);
  class.lpszClassName = "w32dll-smoke-host";
  RegisterClass (&class);

  /* WS_CLIPCHILDREN, or this window's background is painted over the
     frame, which only paints again when something invalidates it.  */
  host_hwnd = CreateWindow ("w32dll-smoke-host", "w32dll-smoke host",
			    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
			    CW_USEDEFAULT, CW_USEDEFAULT,
			    1000, 700, NULL, NULL, GetModuleHandle (NULL),
			    NULL);
  ShowWindow (host_hwnd, SW_SHOW);
}

static const struct host_api host_api =
  {
    HOST_API_VERSION,
    host_post,
    host_on_event,
    host_window
  };

__declspec (dllexport) const struct host_api *host_get_api (unsigned);

const struct host_api *
host_get_api (unsigned version)
{
  return version == HOST_API_VERSION ? &host_api : NULL;
}

struct emacs_run
{
  w32_emacs_init_fn init;
  int argc;
  char **argv;
  int status;
};

static DWORD WINAPI
run_emacs (LPVOID param)
{
  struct emacs_run *run = param;
  run->status = run->init (run->argc, run->argv);
  return 0;
}

int
main (int argc, char **argv)
{
  /* A real host sits in its own installation, away from Emacs, so let
     EMACS_DLL say where the DLL is and test that case too.  */
  const char *dll_name = getenv ("EMACS_DLL");
  if (!dll_name)
    dll_name = "libemacs.dll";

  HMODULE dll = LoadLibraryA (dll_name);
  if (!dll)
    {
      fprintf (stderr, "cannot load %s: error %lu\n", dll_name,
	       GetLastError ());
      return 1;
    }

  w32_emacs_init_fn init
    = (w32_emacs_init_fn) GetProcAddress (dll, "w32_emacs_init");
  if (!init)
    {
      fprintf (stderr, "libemacs.dll has no w32_emacs_init\n");
      return 1;
    }

  /* --host-window, if it is the first argument, says to be a host with
     a window and keep the rest of the command line for Emacs.  */
  if (1 < argc && strcmp (argv[1], "--host-window") == 0)
    {
      make_host_window ();
      argv[1] = argv[0];
      argv++;
      argc--;
    }

  /* Emacs relies on the 8 MB stack emacs.exe is linked with.  */
  struct emacs_run run = { init, argc, argv, 0 };
  HANDLE thread = CreateThread (NULL, 8 << 20, run_emacs, &run,
				STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  if (!thread)
    {
      fprintf (stderr, "cannot create the Emacs thread: error %lu\n",
	       GetLastError ());
      return 1;
    }

  /* With a window there is a message loop to run, until Emacs is done
     or the window is closed.  */
  if (host_hwnd)
    {
      MSG message;

      while (WaitForSingleObject (thread, 0) == WAIT_TIMEOUT)
	{
	  if (GetMessage (&message, NULL, 0, 0) <= 0)
	    break;
	  TranslateMessage (&message);
	  DispatchMessage (&message);
	}
    }

  WaitForSingleObject (thread, INFINITE);
  return run.status;
}
