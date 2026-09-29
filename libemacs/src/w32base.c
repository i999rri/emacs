/* What Windows itself does for Emacs, apart from any window system.

TAKEN FROM src/w32xfns.c, src/w32fns.c and src/w32term.c, which hold
what Windows does for Emacs and what the w32 window system does
together.  All three are still there and are still that window system's
to build; nothing here is cut out of them, and each section below says
what it was taken from and what was left behind.  Read this against
those three when upstream moves them.

Copyright (C) 2026 Free Software Foundation, Inc.
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

/* The critical section Emacs guards its threads with, and the queue
   they pass messages through.  Both are Windows's and neither is a
   window system's: a build whose frames are drawn by a host application
   has threads and a message queue like any other Emacs on Windows.

   Taken from src/w32xfns.c, which holds these and the palette and
   device context of a w32 frame together; that file is the w32 window
   system's to build and cannot be built without it.  What is here is
   the other half of it, unchanged, so that the two can be read against
   each other when upstream moves.  */

#include <config.h>
#include <signal.h>
#include <stdio.h>
#include <windows.h>
#include <windowsx.h>

#ifdef WINDOWSNT
/* Override API version to get the required functionality.  */
# undef _WIN32_WINNT
# define _WIN32_WINNT 0x0501
/* mingw.org's MinGW headers mistakenly omit this enumeration: */
# ifndef MINGW_W64
typedef enum _WTS_VIRTUAL_CLASS {
  WTSVirtualClientData,
  WTSVirtualFileHandle
} WTS_VIRTUAL_CLASS;
# endif
#include <wtsapi32.h>	/* for WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK */
#endif	/* WINDOWSNT */

#include <objbase.h>	/* for CoCreateGuid, which names the hook's window */

#include "lisp.h"
#include "frame.h"
#include "keyboard.h"	/* for the modifier bits a key carries */
/* Not w32term.h, which is the w32 window system's: w32base.h is what it
   declares of Windows itself.  */
#include "w32base.h"

/* The module handle of Emacs itself, which cache_system_info caches.
   A w32 build has this in w32fns.c, where the window class wants it.  */
HINSTANCE hinst = NULL;

/* Set from w32console.c, where the console's input codepage is.  */
extern int w32_console_unicode_input;

#define myalloc(cb) GlobalAllocPtr (GPTR, cb)
#define myfree(lp) GlobalFreePtr (lp)

CRITICAL_SECTION critsect;

#ifdef WINDOWSNT
extern HANDLE keyboard_handle;
#endif /* WINDOWSNT */

static HANDLE input_available = NULL;
extern HANDLE interrupt_handle;
HANDLE interrupt_handle = NULL;

void
init_crit (void)
{
  InitializeCriticalSection (&critsect);

  /* For safety, input_available should only be reset by get_next_msg
     when the input queue is empty, so make it a manual reset event. */
  input_available = CreateEvent (NULL, TRUE, FALSE, NULL);

#if HAVE_W32NOTIFY
  /* Initialize the linked list of notifications sets that will be
     used to communicate between the watching worker threads and the
     main thread.  */
  notifications_set_head = malloc (sizeof(struct notifications_set));
  if (notifications_set_head)
    {
      memset (notifications_set_head, 0, sizeof(struct notifications_set));
      notifications_set_head->next
	= notifications_set_head->prev = notifications_set_head;
    }
  else
    DebPrint(("Out of memory: can't initialize notifications sets."));
#endif

#ifdef WINDOWSNT
  keyboard_handle = input_available;
#endif /* WINDOWSNT */

  /* interrupt_handle is signaled when quit (C-g) is detected, so that
     blocking system calls can be interrupted.  We make it a manual
     reset event, so that if we should ever have multiple threads
     performing system calls, they will all be interrupted (I'm guessing
     that would the right response).  Note that we use PulseEvent to
     signal this event, so that it never remains signaled.  */
  interrupt_handle = CreateEvent (NULL, TRUE, FALSE, NULL);
}

void
delete_crit (void)
{
  DeleteCriticalSection (&critsect);

  if (input_available)
    {
      CloseHandle (input_available);
      input_available = NULL;
    }
  if (interrupt_handle)
    {
      CloseHandle (interrupt_handle);
      interrupt_handle = NULL;
    }

#if HAVE_W32NOTIFY
  if (notifications_set_head)
    {
      /* Free any remaining notifications set that could be left over.  */
      while (notifications_set_head->next != notifications_set_head)
	{
	  struct notifications_set *ns = notifications_set_head->next;
	  notifications_set_head->next = ns->next;
	  ns->next->prev = notifications_set_head;
	  if (ns->notifications)
	    free (ns->notifications);
	  free (ns);
	}
    }
  free (notifications_set_head);
#endif
}

void
signal_quit (void)
{
  /* Make sure this event never remains signaled; if the main thread
     isn't in a blocking call, then this should do nothing.  */
  PulseEvent (interrupt_handle);
}

typedef struct int_msg
{
  W32Msg                w32msg;
  struct int_msg *lpNext;
} int_msg;

static int_msg *lpHead = NULL;
static int_msg *lpTail = NULL;
static int nQueue = 0;

BOOL
get_next_msg (W32Msg * lpmsg, BOOL bWait)
{
  BOOL bRet = FALSE;

  enter_crit ();

  /* The while loop takes care of multiple sets */

  while (!nQueue && bWait)
    {
      leave_crit ();
      WaitForSingleObject (input_available, INFINITE);
      enter_crit ();
    }

  if (nQueue)
    {
      memcpy (lpmsg, &lpHead->w32msg, sizeof (W32Msg));

      {
	int_msg * lpCur = lpHead;

	lpHead = lpHead->lpNext;

	myfree (lpCur);
      }

      nQueue--;
      /* Consolidate WM_PAINT messages to optimize redrawing.  */
      if (lpmsg->msg.message == WM_PAINT && nQueue)
        {
          int_msg * lpCur = lpHead;
          int_msg * lpPrev = NULL;
          int_msg * lpNext = NULL;

          while (lpCur && nQueue)
            {
              lpNext = lpCur->lpNext;
              if (lpCur->w32msg.msg.message == WM_PAINT)
                {
                  /* Remove this message from the queue.  */
                  if (lpPrev)
                    lpPrev->lpNext = lpNext;
                  else
                    lpHead = lpNext;

                  if (lpCur == lpTail)
                    lpTail = lpPrev;

                  /* Adjust clip rectangle to cover both.  */
                  if (!UnionRect (&(lpmsg->rect), &(lpmsg->rect),
                                  &(lpCur->w32msg.rect)))
                    {
                      SetRectEmpty (&(lpmsg->rect));
                    }

                  myfree (lpCur);

                  nQueue--;

                  lpCur = lpNext;
                }
              else
                {
                  lpPrev = lpCur;
                  lpCur = lpNext;
                }
            }
        }

      bRet = TRUE;
    }

  if (nQueue == 0)
    ResetEvent (input_available);

  leave_crit ();

  return (bRet);
}

extern char * w32_strerror (int error_no);

/* Tell the main thread that we have input available; if the main
   thread is blocked in select(), we wake it up here.  */
static void
notify_msg_ready (void)
{
  SetEvent (input_available);

#ifdef CYGWIN
  /* Wakes up the main thread, which is blocked select()ing for /dev/windows,
     among other files.  */
  (void) PostThreadMessage (dwMainThreadId, WM_EMACS_INPUT_READY, 0, 0);
#endif /* CYGWIN */
}

BOOL
post_msg (W32Msg * lpmsg)
{
  int_msg * lpNew = (int_msg *) myalloc (sizeof (int_msg));

  if (!lpNew)
    return (FALSE);

  memcpy (&lpNew->w32msg, lpmsg, sizeof (W32Msg));
  lpNew->lpNext = NULL;

  enter_crit ();

  if (nQueue++)
    {
      lpTail->lpNext = lpNew;
    }
  else
    {
      lpHead = lpNew;
    }

  lpTail = lpNew;
  notify_msg_ready ();
  leave_crit ();

  return (TRUE);
}

BOOL
prepend_msg (W32Msg *lpmsg)
{
  int_msg * lpNew = (int_msg *) myalloc (sizeof (int_msg));

  if (!lpNew)
    return (FALSE);

  memcpy (&lpNew->w32msg, lpmsg, sizeof (W32Msg));

  enter_crit ();

  nQueue++;
  lpNew->lpNext = lpHead;
  lpHead = lpNew;
  notify_msg_ready ();
  leave_crit ();

  return (TRUE);
}

/* Process all messages in the current thread's queue.  Value is 1 if
   one of these messages was WM_EMACS_FILENOTIFY, zero otherwise.  */
int
drain_message_queue (void)
{
  MSG msg;
  int retval = 0;

  while (PeekMessage (&msg, NULL, 0, 0, PM_REMOVE))
    {
      switch (msg.message)
	{
#ifdef WINDOWSNT
	case WM_WTSSESSION_CHANGE:
	  if (msg.wParam == WTS_SESSION_LOCK)
	    reset_w32_kbdhook_state ();
	  break;
#endif
	case WM_EMACS_FILENOTIFY:
	  retval = 1;
	  break;
	}
      TranslateMessage (&msg);
      DispatchMessage (&msg);
    }
  return retval;
}


/* ------------------------------------------------------------------

TAKEN FROM src/w32fns.c, unchanged but for the comment above each part
saying what wants it: the definitions below are that file's, and that
file is still there and is still the w32 window system's to build.
What was left behind is everything with a w32 frame, menu, dialog or
display in it.  Read the two against each other when upstream moves
w32fns.c.

   ------------------------------------------------------------------  */

/* The system version and its build number: w32heap.c wants the page
   size of it, dynlib.c the OS, editfns.c the version string, and
   systhread.c the two entry points looked up beside them.  */

typedef BOOL (WINAPI *IsDebuggerPresent_Proc) (void);
typedef HRESULT (WINAPI *SetThreadDescription_Proc)
  (HANDLE hThread, PCWSTR lpThreadDescription);

extern IsDebuggerPresent_Proc is_debugger_present;
IsDebuggerPresent_Proc is_debugger_present = NULL;
extern SetThreadDescription_Proc set_thread_description;
SetThreadDescription_Proc set_thread_description = NULL;

/* This gives us the page size and the size of the allocation unit on NT.  */
SYSTEM_INFO sysinfo_cache;

/* This gives us version, build, and platform identification.  */
OSVERSIONINFO osinfo_cache;

DWORD_PTR syspage_mask = 0;

/* The major and minor versions of NT.  */
int w32_major_version;
int w32_minor_version;
int w32_build_number;

/* Distinguish between Windows NT and Windows 95.  */
int os_subtype;

/* Cache information describing the NT system for later use.  */
void
cache_system_info (void)
{
  union
    {
      struct info
	{
	  char  major;
	  char  minor;
	  short platform;
	} info;
      DWORD data;
    } version;

  /* Cache the module handle of Emacs itself.  */
  hinst = GetModuleHandle (NULL);

  /* Cache the version of the operating system.  */
  version.data = GetVersion ();
  w32_major_version = version.info.major;
  w32_minor_version = version.info.minor;

  if (version.info.platform & 0x8000)
    os_subtype = OS_SUBTYPE_9X;
  else
    os_subtype = OS_SUBTYPE_NT;

  /* Cache page size, allocation unit, processor type, etc.  */
  GetSystemInfo (&sysinfo_cache);
  syspage_mask = (DWORD_PTR)sysinfo_cache.dwPageSize - 1;

  /* Cache os info.  */
  osinfo_cache.dwOSVersionInfoSize = sizeof (OSVERSIONINFO);
  GetVersionEx (&osinfo_cache);

  w32_build_number = osinfo_cache.dwBuildNumber;
  if (os_subtype == OS_SUBTYPE_9X)
    w32_build_number &= 0xffff;

  w32_num_mouse_buttons = GetSystemMetrics (SM_CMOUSEBUTTONS);
}


#ifdef WINDOWSNT
char *
w32_version_string (void)
{
  /* NNN.NNN.NNNNNNNNNN */
  static char version_string[3 + 1 + 3 + 1 + 10 + 1];
  _snprintf (version_string, sizeof version_string, "%d.%d.%d",
	     w32_major_version, w32_minor_version, w32_build_number);
  return version_string;
}
#endif

/* What strerror is for the error codes Windows returns.  */

/* Equivalent of strerror for W32 error codes.  */
char *
w32_strerror (int error_no)
{
  static char buf[500];
  DWORD ret;

  if (error_no == 0)
    error_no = GetLastError ();

  ret = FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM |
		       FORMAT_MESSAGE_IGNORE_INSERTS,
		       NULL,
		       error_no,
		       0, /* choose most suitable language */
		       buf, sizeof (buf), NULL);

  while (ret > 0 && (buf[ret - 1] == '\n' ||
		     buf[ret - 1] == '\r' ))
      --ret;
  buf[ret] = '\0';
  if (!ret)
    sprintf (buf, "w32 error %d", error_no);

  return buf;
}

/* The registry, which w32.c reads the environment out of.  */

/* Query a value from the Windows Registry (under HKCU and HKLM),
   where `key' is the registry key, `name' is the name, and `lpdwtype'
   is a pointer to the return value's type. `lpwdtype' can be NULL if
   you do not care about the type.

   Returns: pointer to the value, or null pointer if the key/name does
   not exist. */
LPBYTE
w32_get_resource (const char *key, const char *name, LPDWORD lpdwtype)
{
  LPBYTE lpvalue;
  HKEY hrootkey = NULL;
  DWORD cbData;

  /* Check both the current user and the local machine to see if
     we have any resources.  */

  if (RegOpenKeyEx (HKEY_CURRENT_USER, key, 0, KEY_READ, &hrootkey) == ERROR_SUCCESS)
    {
      lpvalue = NULL;

      if (RegQueryValueEx (hrootkey, name, NULL, NULL, NULL, &cbData) == ERROR_SUCCESS
	  && (lpvalue = xmalloc (cbData)) != NULL
	  && RegQueryValueEx (hrootkey, name, NULL, lpdwtype, lpvalue, &cbData) == ERROR_SUCCESS)
	{
          RegCloseKey (hrootkey);
	  return (lpvalue);
	}

      xfree (lpvalue);

      RegCloseKey (hrootkey);
    }

  if (RegOpenKeyEx (HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &hrootkey) == ERROR_SUCCESS)
    {
      lpvalue = NULL;

      if (RegQueryValueEx (hrootkey, name, NULL, NULL, NULL, &cbData) == ERROR_SUCCESS
	  && (lpvalue = xmalloc (cbData)) != NULL
	  && RegQueryValueEx (hrootkey, name, NULL, lpdwtype, lpvalue, &cbData) == ERROR_SUCCESS)
	{
          RegCloseKey (hrootkey);
	  return (lpvalue);
	}

      xfree (lpvalue);

      RegCloseKey (hrootkey);
    }

  return (NULL);
}

/* The low level keyboard hook, which the console terminal sets up
   as much as a window system frame does.  */

/* Session notifications, which tell the hook that the computer was
   locked.  Loaded in globals_of_w32base, as w32fns.c loads them in
   globals_of_w32fns.  */
typedef BOOL (WINAPI * WTSRegisterSessionNotification_Proc)
  (HWND hwnd, DWORD dwFlags);
typedef BOOL (WINAPI * WTSUnRegisterSessionNotification_Proc) (HWND hwnd);

static WTSRegisterSessionNotification_Proc WTSRegisterSessionNotification_fn;
static WTSUnRegisterSessionNotification_Proc WTSUnRegisterSessionNotification_fn;

/* Special virtual key code for indicating "any" key.  */
#define VK_ANY 0xFF

/* Keyboard hook state data.  */
static struct
{
  int hook_count; /* counter, if several windows are created */
  HHOOK hook;     /* hook handle */
  HWND console;   /* console window handle */
  HWND notified_wnd; /* window that receives session notifications */

  int lwindown;      /* Left Windows key currently pressed (and hooked) */
  int rwindown;      /* Right Windows key currently pressed (and hooked) */
  int winsdown;      /* Number of handled keys currently pressed */
  int send_win_up;   /* Pass through the keyup for this Windows key press? */
  int suppress_lone; /* Suppress simulated Windows keydown-keyup for this press? */
  int winseen;       /* Windows keys seen during this press? */

  char alt_hooked[256];  /* hook Alt+[this key]? */
  char lwin_hooked[256]; /* hook left Win+[this key]? */
  char rwin_hooked[256]; /* hook right Win+[this key]? */
} kbdhook;

typedef HWND (WINAPI *GetConsoleWindow_Proc) (void);

#ifdef WINDOWSNT

/* The Windows keyboard hook callback.  */
static LRESULT CALLBACK
funhook (int code, WPARAM w, LPARAM l)
{
  INPUT inputs[2];
  HWND focus = GetFocus ();
  int console = 0;
  KBDLLHOOKSTRUCT const *hs = (KBDLLHOOKSTRUCT*)l;

  if (code < 0 || (hs->flags & LLKHF_INJECTED))
    return CallNextHookEx (0, code, w, l);

  /* The keyboard hook sees keyboard input on all processes (except
     elevated ones, when Emacs itself is not elevated).  As such,
     care must be taken to only filter out keyboard input when Emacs
     itself is on the foreground.

     GetFocus returns a non-NULL window if another application is active,
     and always for a console Emacs process.  For a console Emacs, determine
     focus by checking if the current foreground window is the process's
     console window.  */
  if (focus == NULL && kbdhook.console != NULL)
    {
      if (GetForegroundWindow () == kbdhook.console)
	{
	  focus = kbdhook.console;
	  console = 1;
	}
    }

  /* First, check hooks for the left and right Windows keys.  */
  if (hs->vkCode == VK_LWIN || hs->vkCode == VK_RWIN)
    {
      if (focus != NULL && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN))
	{
	  /* The key is being pressed in an Emacs window.  */
	  if (hs->vkCode == VK_LWIN && !kbdhook.lwindown)
	    {
	      kbdhook.lwindown = 1;
	      kbdhook.winseen = 1;
	      kbdhook.winsdown++;
	    }
	  else if (hs->vkCode == VK_RWIN && !kbdhook.rwindown)
	    {
	      kbdhook.rwindown = 1;
	      kbdhook.winseen = 1;
	      kbdhook.winsdown++;
	    }
	  /* Returning 1 here drops the keypress without further processing.
	     If the keypress was allowed to go through, the normal Windows
	     hotkeys would take over.  */
	  return 1;
	}
      else if (kbdhook.winsdown > 0 && (w == WM_KEYUP || w == WM_SYSKEYUP))
	{
	  /* A key that has been captured earlier is being released now.  */
	  if (hs->vkCode == VK_LWIN && kbdhook.lwindown)
	    {
	      kbdhook.lwindown = 0;
	      kbdhook.winsdown--;
	    }
	  else if (hs->vkCode == VK_RWIN && kbdhook.rwindown)
	    {
	      kbdhook.rwindown = 0;
	      kbdhook.winsdown--;
	    }
	  if (kbdhook.winsdown == 0 && kbdhook.winseen)
	    {
	      if (!kbdhook.suppress_lone)
	        {
		  /* The Windows key was pressed, then released,
		     without any other key pressed simultaneously.
		     Normally, this opens the Start menu, but the user
		     can prevent this by setting the
		     w32-pass-[lr]window-to-system variable to
		     NIL.  */
		  if ((hs->vkCode == VK_LWIN && !NILP (Vw32_pass_lwindow_to_system))
		       || (hs->vkCode == VK_RWIN && !NILP (Vw32_pass_rwindow_to_system)))
		    {
		      /* Not prevented - Simulate the keypress to the system.  */
		      memset (inputs, 0, sizeof (inputs));
		      inputs[0].type = INPUT_KEYBOARD;
		      inputs[0].ki.wVk = hs->vkCode;
		      inputs[0].ki.wScan = hs->vkCode;
		      inputs[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
		      inputs[0].ki.time = 0;
		      inputs[1].type = INPUT_KEYBOARD;
		      inputs[1].ki.wVk = hs->vkCode;
		      inputs[1].ki.wScan = hs->vkCode;
		      inputs[1].ki.dwFlags
			= KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
		      inputs[1].ki.time = 0;
		      SendInput (2, inputs, sizeof (INPUT));
		    }
		  else if (focus != NULL)
		    {
		      /* When not passed to system, must simulate privately to Emacs.	 */
		      PostMessage (focus, WM_SYSKEYDOWN, hs->vkCode, 0);
		      PostMessage (focus, WM_SYSKEYUP, hs->vkCode, 0);
		    }
		}
	    }
	  if (kbdhook.winsdown == 0)
	    {
	      /* No Windows keys pressed anymore - clear the state flags.  */
	      kbdhook.suppress_lone = 0;
	      kbdhook.winseen = 0;
	    }
	  if (!kbdhook.send_win_up)
	    {
	      /* Swallow this release message, as not to confuse
		 applications who did not get to see the original
		 WM_KEYDOWN message either.  */
	      return 1;
	    }
	  kbdhook.send_win_up = 0;
	}
    }
  else if (kbdhook.winsdown > 0)
    {
      /* Some other key was pressed while a captured Win key is down.
	 This is either an Emacs registered hotkey combination, or a
	 system hotkey.	 */
      if ((kbdhook.lwindown && kbdhook.lwin_hooked[hs->vkCode]) ||
	  (kbdhook.rwindown && kbdhook.rwin_hooked[hs->vkCode]))
	{
	  /* Hooked Win-x combination, do not pass the keypress to Windows.  */
	  kbdhook.suppress_lone = 1;
	}
      else if (!kbdhook.suppress_lone)
	{
	  /* Unhooked S-x combination; simulate the combination now
	     (will be seen by the system).  */
	  memset (inputs, 0, sizeof (inputs));
	  inputs[0].type = INPUT_KEYBOARD;
	  inputs[0].ki.wVk = kbdhook.lwindown ? VK_LWIN : VK_RWIN;
	  inputs[0].ki.wScan = kbdhook.lwindown ? VK_LWIN : VK_RWIN;
	  inputs[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
	  inputs[0].ki.time = 0;
	  inputs[1].type = INPUT_KEYBOARD;
	  inputs[1].ki.wVk = hs->vkCode;
	  inputs[1].ki.wScan = hs->scanCode;
	  inputs[1].ki.dwFlags =
	    (hs->flags & LLKHF_EXTENDED) ? KEYEVENTF_EXTENDEDKEY : 0;
	  inputs[1].ki.time = 0;
	  SendInput (2, inputs, sizeof (INPUT));
	  /* Stop processing of this Win sequence here; the
	     corresponding keyup messages will come through the normal
	     channel when the keys are released.  */
	  kbdhook.suppress_lone = 1;
	  kbdhook.send_win_up = 1;
	  /* Swallow the original keypress (as we want the Win key
	     down message simulated above to precede this real message).  */
	  return 1;
	}
    }

  /* Next, handle the registered Alt-* combinations.  */
  if ((w == WM_SYSKEYDOWN || w == WM_KEYDOWN)
      && kbdhook.alt_hooked[hs->vkCode]
      && focus != NULL
      && (GetAsyncKeyState (VK_MENU) & 0x8000))
    {
      /* Prevent the system from getting this Alt-* key - suppress the
	 message and post as a normal keypress to Emacs.  */
      if (console)
	{
	  INPUT_RECORD rec;
	  DWORD n;
	  rec.EventType = KEY_EVENT;
	  rec.Event.KeyEvent.bKeyDown = TRUE;
	  rec.Event.KeyEvent.wVirtualKeyCode = hs->vkCode;
	  rec.Event.KeyEvent.wVirtualScanCode = hs->scanCode;
	  rec.Event.KeyEvent.uChar.UnicodeChar = 0;
	  rec.Event.KeyEvent.dwControlKeyState =
	    ((GetAsyncKeyState (VK_LMENU) & 0x8000) ? LEFT_ALT_PRESSED : 0)
	    | ((GetAsyncKeyState (VK_RMENU) & 0x8000) ? RIGHT_ALT_PRESSED : 0)
	    | ((GetAsyncKeyState (VK_LCONTROL) & 0x8000) ? LEFT_CTRL_PRESSED : 0)
	    | ((GetAsyncKeyState (VK_RCONTROL) & 0x8000) ? RIGHT_CTRL_PRESSED : 0)
	    | ((GetAsyncKeyState (VK_SHIFT) & 0x8000) ? SHIFT_PRESSED : 0)
	    | ((hs->flags & LLKHF_EXTENDED) ? ENHANCED_KEY : 0);
	  if (w32_console_unicode_input)
	    WriteConsoleInputW (keyboard_handle, &rec, 1, &n);
	  else
	    WriteConsoleInputA (keyboard_handle, &rec, 1, &n);
	}
      else
	PostMessage (focus, w, hs->vkCode, 1 | (1<<29));
      return 1;
    }

  /* The normal case - pass the message through.  */
  return CallNextHookEx (0, code, w, l);
}

/* Set up the hook; can be called several times, with matching
   remove_w32_kbdhook calls.  */
void
setup_w32_kbdhook (HWND hwnd)
{
  kbdhook.hook_count++;

  /* This hook gets in the way of debugging, since when Emacs stops,
     its input thread stops, and there's nothing to process keyboard
     events, whereas this hook is global, and is invoked in the
     context of the thread that installed it.  So we don't install the
     hook if the process is being debugged. */
  if (w32_kbdhook_active)
    {
      if (is_debugger_present && is_debugger_present ())
	return;
    }

  /* Hooking is only available on NT architecture systems, as
     indicated by the w32_kbdhook_active variable.  */
  if (kbdhook.hook_count == 1 && w32_kbdhook_active)
    {
      /* Get the handle of the Emacs console window.  As the
	 GetConsoleWindow function is only available on Win2000+, a
	 hackish workaround described in Microsoft KB article 124103
	 (https://support.microsoft.com/en-us/kb/124103) is used for
	 NT 4 systems.  */
      GetConsoleWindow_Proc get_console = (GetConsoleWindow_Proc)
	get_proc_addr (GetModuleHandle ("kernel32.dll"), "GetConsoleWindow");

      if (get_console != NULL)
	kbdhook.console = get_console ();
      else
        {
	  GUID guid;
	  wchar_t *oldTitle = malloc (1024 * sizeof(wchar_t));
	  wchar_t newTitle[64];
	  int i;

	  CoCreateGuid (&guid);
	  if (oldTitle != NULL && StringFromGUID2 (&guid, newTitle, 64))
	    {
	      GetConsoleTitleW (oldTitle, 1024);
	      SetConsoleTitleW (newTitle);
	      for (i = 0; i < 25; i++)
	        {
		  Sleep (40);
		  kbdhook.console = FindWindowW (NULL, newTitle);
		  if (kbdhook.console != NULL)
		    break;
		}
	      SetConsoleTitleW (oldTitle);
	    }
	  free (oldTitle);
	}

      /* Set the hook.  */
      kbdhook.hook = SetWindowsHookEx (WH_KEYBOARD_LL, funhook,
				       GetModuleHandle (NULL), 0);

      /* Register session notifications so we get notified about the
	 computer being locked.  */
      kbdhook.notified_wnd = NULL;
      if (hwnd != NULL && WTSRegisterSessionNotification_fn != NULL)
	{
	  WTSRegisterSessionNotification_fn (hwnd, NOTIFY_FOR_THIS_SESSION);
	  kbdhook.notified_wnd = hwnd;
	}
    }
}

/* Remove the hook.  */
void
remove_w32_kbdhook (void)
{
  kbdhook.hook_count--;
  if (kbdhook.hook_count == 0 && w32_kbdhook_active)
    {
      UnhookWindowsHookEx (kbdhook.hook);
      if (kbdhook.notified_wnd != NULL
	  && WTSUnRegisterSessionNotification_fn != NULL)
	  WTSUnRegisterSessionNotification_fn (kbdhook.notified_wnd);
      kbdhook.hook = NULL;
      kbdhook.notified_wnd = NULL;
    }
}
#endif	/* WINDOWSNT */

/* Mark a specific key combination as hooked, preventing it to be
   handled by the system.  */
static void
hook_w32_key (int hook, int modifier, int vkey)
{
  char *tbl = NULL;

  switch (modifier)
    {
    case VK_MENU:
      tbl = kbdhook.alt_hooked;
      break;
    case VK_LWIN:
      tbl = kbdhook.lwin_hooked;
      break;
    case VK_RWIN:
      tbl = kbdhook.rwin_hooked;
      break;
    }

  if (tbl != NULL && vkey >= 0 && vkey <= 255)
    {
       /* VK_ANY hooks all keys for this modifier */
       if (vkey == VK_ANY)
	 memset (tbl, (char)hook, 256);
       else
	 tbl[vkey] = (char)hook;
       /* Alt-<modifier>s should go through */
       kbdhook.alt_hooked[VK_MENU] = 0;
       kbdhook.alt_hooked[VK_LMENU] = 0;
       kbdhook.alt_hooked[VK_RMENU] = 0;
       kbdhook.alt_hooked[VK_CONTROL] = 0;
       kbdhook.alt_hooked[VK_LCONTROL] = 0;
       kbdhook.alt_hooked[VK_RCONTROL] = 0;
       kbdhook.alt_hooked[VK_SHIFT] = 0;
       kbdhook.alt_hooked[VK_LSHIFT] = 0;
       kbdhook.alt_hooked[VK_RSHIFT] = 0;
    }
}

#ifdef WINDOWSNT
/* Check the current Win key pressed state.  */
int
check_w32_winkey_state (int vkey)
{
  /* The hook code handles grabbing of the Windows keys and Alt-* key
     combinations reserved by the system.  Handling Alt is a bit
     easier, as Windows intends Alt-* shortcuts for application use in
     Windows; hotkeys such as Alt-tab and Alt-escape are special
     cases.  Win-* hotkeys, on the other hand, are primarily meant for
     system use.

     As a result, when we want Emacs to be able to grab the Win-*
     keys, we must swallow all Win key presses in a low-level keyboard
     hook.  Unfortunately, this means that the Emacs window procedure
     (and console input handler) never see the keypresses either.
     Thus, to check the modifier states properly, Emacs code must use
     the check_w32_winkey_state function that uses the flags directly
     updated by the hook callback.  */
  switch (vkey)
    {
    case VK_LWIN:
      return kbdhook.lwindown;
    case VK_RWIN:
      return kbdhook.rwindown;
    }
  return 0;
}

/* Reset the keyboard hook state.  Locking the workstation with Win-L
   leaves the Win key(s) "down" from the hook's point of view - the
   keyup event is never seen.  Thus, this function must be called when
   the system is locked.  */
void
reset_w32_kbdhook_state (void)
{
  kbdhook.lwindown = 0;
  kbdhook.rwindown = 0;
  kbdhook.winsdown = 0;
  kbdhook.send_win_up = 0;
  kbdhook.suppress_lone = 0;
  kbdhook.winseen = 0;
}
#endif	/* WINDOWSNT */

/* What the console input in w32inevt.c makes of a key: which
   modifiers it carries, which keypad key it is, and what it means
   in the console codepage.  The bell is here too, being the
   console terminal's as much as a frame's.  */

/* Which of MessageBeep's sounds the bell is.  A w32 build lets Lisp
   choose; without that window system it stays the plain Beep.  */
static unsigned int sound_type = 0xFFFFFFFF;
#define MB_EMACS_SILENT (0xFFFFFFFF - 1)

unsigned int
w32_key_to_modifier (int key)
{
  Lisp_Object key_mapping;

  switch (key)
    {
    case VK_LWIN:
      key_mapping = Vw32_lwindow_modifier;
      break;
    case VK_RWIN:
      key_mapping = Vw32_rwindow_modifier;
      break;
    case VK_APPS:
      key_mapping = Vw32_apps_modifier;
      break;
    case VK_SCROLL:
      key_mapping = Vw32_scroll_lock_modifier;
      break;
    default:
      key_mapping = Qnil;
    }

  /* NB. This code runs in the input thread, asynchronously to the lisp
     thread, so we must be careful to ensure access to lisp data is
     thread-safe.  The following code is safe because the modifier
     variable values are updated atomically from lisp and symbols are
     not relocated by GC.  Also, we don't have to worry about seeing GC
     markbits here.  */
  if (EQ (key_mapping, Qhyper))
    return hyper_modifier;
  if (EQ (key_mapping, Qsuper))
    return super_modifier;
  if (EQ (key_mapping, Qmeta))
    return meta_modifier;
  if (EQ (key_mapping, Qalt))
    return alt_modifier;
  if (EQ (key_mapping, Qctrl))
    return ctrl_modifier;
  if (EQ (key_mapping, Qcontrol)) /* synonym for ctrl */
    return ctrl_modifier;
  if (EQ (key_mapping, Qshift))
    return shift_modifier;

  /* Don't generate any modifier if not explicitly requested.  */
  return 0;
}

unsigned int
map_keypad_keys (unsigned int virt_key, unsigned int extended)
{
  if (virt_key < VK_CLEAR || virt_key > VK_DELETE)
    return virt_key;

  if (virt_key == VK_RETURN)
    return (extended ? VK_NUMPAD_ENTER : VK_RETURN);

  if (virt_key >= VK_PRIOR && virt_key <= VK_DOWN)
    return (!extended ? (VK_NUMPAD_PRIOR + (virt_key - VK_PRIOR)) : virt_key);

  if (virt_key == VK_INSERT || virt_key == VK_DELETE)
    return (!extended ? (VK_NUMPAD_INSERT + (virt_key - VK_INSERT)) : virt_key);

  if (virt_key == VK_CLEAR)
    return (!extended ? VK_NUMPAD_CLEAR : virt_key);

  return virt_key;
}

/* Translate console modifiers to emacs modifiers.
   German keyboard support (Kai Morgan Zeise 2/18/95).  */
int
w32_kbd_mods_to_emacs (DWORD mods, WORD key)
{
  int retval = 0;

  /* If we recognize right-alt and left-ctrl as AltGr, and it has been
     pressed, first remove those modifiers.  */
  if (!NILP (Vw32_recognize_altgr)
      && (mods & (RIGHT_ALT_PRESSED | LEFT_CTRL_PRESSED))
      == (RIGHT_ALT_PRESSED | LEFT_CTRL_PRESSED))
    mods &= ~ (RIGHT_ALT_PRESSED | LEFT_CTRL_PRESSED);

  if (mods & (RIGHT_ALT_PRESSED | LEFT_ALT_PRESSED))
    retval = ((NILP (Vw32_alt_is_meta)) ? alt_modifier : meta_modifier);

  if (mods & (RIGHT_CTRL_PRESSED | LEFT_CTRL_PRESSED))
    {
      retval |= ctrl_modifier;
      if ((mods & (RIGHT_CTRL_PRESSED | LEFT_CTRL_PRESSED))
	  == (RIGHT_CTRL_PRESSED | LEFT_CTRL_PRESSED))
	retval |= meta_modifier;
    }

  if (mods & LEFT_WIN_PRESSED)
    retval |= w32_key_to_modifier (VK_LWIN);
  if (mods & RIGHT_WIN_PRESSED)
    retval |= w32_key_to_modifier (VK_RWIN);
  if (mods & APPS_PRESSED)
    retval |= w32_key_to_modifier (VK_APPS);
  if (mods & SCROLLLOCK_ON)
    retval |= w32_key_to_modifier (VK_SCROLL);

  /* Just in case someone wanted the original behavior, make it
     optional by setting w32-capslock-is-shiftlock to t.  */
  if (NILP (Vw32_capslock_is_shiftlock)
      /* Keys that should _not_ be affected by CapsLock.  */
      && (    (key == VK_BACK)
	   || (key == VK_TAB)
	   || (key == VK_CLEAR)
	   || (key == VK_RETURN)
	   || (key == VK_ESCAPE)
	   || ((key >= VK_SPACE) && (key <= VK_HELP))
	   || ((key >= VK_NUMPAD0) && (key <= VK_F24))
	   || ((key >= VK_NUMPAD_CLEAR) && (key <= VK_NUMPAD_DELETE))
	 ))
    {
      /* Only consider shift state.  */
      if ((mods & SHIFT_PRESSED) != 0)
	retval |= shift_modifier;
    }
  else
    {
      /* Ignore CapsLock state if not enabled.  */
      if (NILP (Vw32_enable_caps_lock))
	mods &= ~CAPSLOCK_ON;
      if ((mods & (SHIFT_PRESSED | CAPSLOCK_ON)) != 0)
	retval |= shift_modifier;
    }

  return retval;
}

/* The return code indicates key code size.  cpID is the codepage to
   use for translation to Unicode; -1 means use the current console
   input codepage.  */
int
w32_kbd_patch_key (KEY_EVENT_RECORD *event, int cpId)
{
  unsigned int key_code = event->wVirtualKeyCode;
  unsigned int mods = event->dwControlKeyState;
  BYTE keystate[256];
  static BYTE ansi_code[4];
  static int isdead = 0;

  if (isdead == 2)
    {
      event->uChar.AsciiChar = ansi_code[2];
      isdead = 0;
      return 1;
    }
  if (event->uChar.AsciiChar != 0)
    return 1;

  memset (keystate, 0, sizeof (keystate));
  keystate[key_code] = 0x80;
  if (mods & SHIFT_PRESSED)
    keystate[VK_SHIFT] = 0x80;
  if (mods & CAPSLOCK_ON)
    keystate[VK_CAPITAL] = 1;
  /* If we recognize right-alt and left-ctrl as AltGr, set the key
     states accordingly before invoking ToAscii.  */
  if (!NILP (Vw32_recognize_altgr)
      && (mods & LEFT_CTRL_PRESSED) && (mods & RIGHT_ALT_PRESSED))
    {
      keystate[VK_CONTROL] = 0x80;
      keystate[VK_LCONTROL] = 0x80;
      keystate[VK_MENU] = 0x80;
      keystate[VK_RMENU] = 0x80;
    }

#if 0
  /* Because of an OS bug, ToAscii corrupts the stack when called to
     convert a dead key in console mode on NT4.  Unfortunately, trying
     to check for dead keys using MapVirtualKey doesn't work either -
     these functions apparently use internal information about keyboard
     layout which doesn't get properly updated in console programs when
     changing layout (though apparently it gets partly updated,
     otherwise ToAscii wouldn't crash).  */
  if (is_dead_key (event->wVirtualKeyCode))
    return 0;
#endif

  /* On NT, call ToUnicode instead and then convert to the current
     console input codepage.  */
  if (os_subtype == OS_SUBTYPE_NT)
    {
      WCHAR buf[128];

      isdead = ToUnicode (event->wVirtualKeyCode, event->wVirtualScanCode,
			  keystate, buf, 128, 0);
      if (isdead > 0)
	{
	  /* When we are called from the GUI message processing code,
	     we are passed the current keyboard codepage, a positive
	     number, to use below.  */
	  if (cpId == -1)
	    cpId = GetConsoleCP ();

	  event->uChar.UnicodeChar = buf[isdead - 1];
	  isdead = WideCharToMultiByte (cpId, 0, buf, isdead,
					(LPSTR)ansi_code, 4, NULL, NULL);
	}
      else
	isdead = 0;
    }
  else
    {
      isdead = ToAscii (event->wVirtualKeyCode, event->wVirtualScanCode,
			keystate, (LPWORD) ansi_code, 0);
    }

  if (isdead == 0)
    return 0;
  event->uChar.AsciiChar = ansi_code[0];
  return isdead;
}


void
w32_sys_ring_bell (struct frame *f)
{
  if (sound_type == 0xFFFFFFFF)
    {
      Beep (666, 100);
    }
  else if (sound_type == MB_EMACS_SILENT)
    {
      /* Do nothing.  */
    }
  else
    MessageBeep (sound_type);
}

/* The guard page at the stack limit, which keyboard.c puts back
   after a stack overflow.  */

/* MinGW headers don't declare this (should be in malloc.h).  Also,
   the function is not present pre-W2K, so make the call through
   a function pointer.  */
typedef int (__cdecl *_resetstkoflw_proc) (void);
static _resetstkoflw_proc resetstkoflw;

/* Re-establish the guard page at stack limit.  This is needed because
   when a stack overflow is detected, Windows removes the guard bit
   from the guard page, so if we don't re-establish that protection,
   the next stack overflow will cause a crash.  */
void
w32_reset_stack_overflow_guard (void)
{
  if (resetstkoflw == NULL)
    resetstkoflw = (_resetstkoflw_proc)
      get_proc_addr (GetModuleHandle ("msvcrt.dll"), "_resetstkoflw");
  /* We ignore the return value.  If _resetstkoflw fails, the next
     stack overflow will crash the program.  */
  if (resetstkoflw != NULL)
    (void)resetstkoflw ();
}


/* ------------------------------------------------------------------

TAKEN FROM src/w32term.c, unchanged but for the comment above each
part: the definitions below are that file's, and that file is still
there and is still the w32 window system's to build.  What was left
behind is everything that draws, scrolls or decorates a w32 frame.
Read the two against each other when upstream moves w32term.c.

   ------------------------------------------------------------------  */

/* The two threads, which w32proc.c and w32notify.c both reach for.
   Emacs on Windows has them whatever draws the frames; only the
   message thread the w32 window system starts on one of them is that
   window system's.  */

DWORD dwWindowsThreadId = 0;
HANDLE hWindowsThread = NULL;
DWORD dwMainThreadId = 0;
HANDLE hMainThread = NULL;

void
w32_init_main_thread (void)
{
  dwMainThreadId = GetCurrentThreadId ();
  DuplicateHandle (GetCurrentProcess (), GetCurrentThread (),
		   GetCurrentProcess (), &hMainThread, 0, TRUE,
		   DUPLICATE_SAME_ACCESS);


}


/* What w32inevt.c calls a change the file system reported.  */

Lisp_Object
w32_lispy_file_action (DWORD action)
{
  static char unknown_fmt[] = "unknown-action(%d)";
  Lisp_Object retval;

  switch (action)
    {
    case FILE_ACTION_ADDED:
      retval = Qadded;
      break;
    case FILE_ACTION_REMOVED:
      retval = Qremoved;
      break;
    case FILE_ACTION_MODIFIED:
      retval = Qmodified;
      break;
    case FILE_ACTION_RENAMED_OLD_NAME:
      retval = Qrenamed_from;
      break;
    case FILE_ACTION_RENAMED_NEW_NAME:
      retval = Qrenamed_to;
      break;
    default:
      {
	char buf[sizeof(unknown_fmt) - 1 + INT_STRLEN_BOUND (DWORD)];

	sprintf (buf, unknown_fmt, action);
	retval = intern (buf);
      }
      break;
    }

  return retval;
}

/* What globals_of_w32fns and syms_of_w32fns do for the definitions
   above.  Emacs calls these instead where there is no w32 window
   system to call those.  */

void
globals_of_w32base (void)
{
  HMODULE hm_kernel32 = GetModuleHandle ("kernel32.dll");
  is_debugger_present = (IsDebuggerPresent_Proc)
    get_proc_addr (hm_kernel32, "IsDebuggerPresent");
  set_thread_description = (SetThreadDescription_Proc)
    get_proc_addr (hm_kernel32, "SetThreadDescription");

#ifdef WINDOWSNT
  HMODULE wtsapi32_lib = LoadLibrary ("wtsapi32.dll");
  WTSRegisterSessionNotification_fn = (WTSRegisterSessionNotification_Proc)
    get_proc_addr (wtsapi32_lib, "WTSRegisterSessionNotification");
  WTSUnRegisterSessionNotification_fn = (WTSUnRegisterSessionNotification_Proc)
    get_proc_addr (wtsapi32_lib, "WTSUnRegisterSessionNotification");
#endif /* WINDOWSNT */
}

void
syms_of_w32base (void)
{
  /* The modifiers a Windows or Apps key can be made to mean, which
     w32-lwindow-modifier and its fellows in w32console.c are set to.  */
  DEFSYM (Qhyper, "hyper");
  DEFSYM (Qsuper, "super");
  DEFSYM (Qmeta, "meta");
  DEFSYM (Qalt, "alt");
  DEFSYM (Qctrl, "ctrl");
  DEFSYM (Qcontrol, "control");
  DEFSYM (Qshift, "shift");

  /* The two the key translation above reads that a w32 build defines
     in w32fns.c and w32term.c.  The rest of that set is in
     w32console.c, which is built whatever draws the frames.  */

  DEFVAR_LISP ("w32-alt-is-meta", Vw32_alt_is_meta,
	       doc: /* Non-nil if the Alt key is to be considered the same as the META key.
When nil, Emacs will translate the Alt key to the ALT modifier, not to META.  */);
  Vw32_alt_is_meta = Qt;

  DEFVAR_LISP ("w32-capslock-is-shiftlock",
	       Vw32_capslock_is_shiftlock,
	       doc: /* Apply CapsLock state to non character input keys.
When nil, CapsLock only affects normal character input keys.  */);
  Vw32_capslock_is_shiftlock = Qnil;

  /* What a change the file system reported is called, which
     w32_lispy_file_action above answers with.  */
  DEFSYM (Qadded, "added");
  DEFSYM (Qremoved, "removed");
  DEFSYM (Qmodified, "modified");
  DEFSYM (Qrenamed_from, "renamed-from");
  DEFSYM (Qrenamed_to, "renamed-to");
}
