/* What Windows itself gives Emacs, apart from any window system.

TAKEN FROM src/w32term.h, which declares these beside the w32 window
system's frames, displays and scroll bars.  That file is still there and
is still that window system's; nothing here is cut out of it, and a w32
build reads it rather than this.  What was left behind is everything
that names a w32 frame or display.  Read the two against each other when
upstream moves w32term.h.

This is what w32base.c defines and what w32.c, w32console.c, w32inevt.c,
w32notify.c, w32proc.c and emacs.c ask of it: Windows's threads, its
message queue, its system version, its keyboard.  None of it is a window
system's, and a build whose frames are drawn by a host application needs
it all the same.

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

#ifndef EMACS_W32BASE_H
#define EMACS_W32BASE_H

#include <windows.h>

#include "systime.h"	/* for Time, which w32gui.h brings a w32 build */
#include "w32common.h"

/* The two threads every Emacs on Windows has: the one that reads
   messages and the one that runs Lisp.  */
extern DWORD dwWindowsThreadId;
extern HANDLE hWindowsThread;
extern DWORD dwMainThreadId;
extern HANDLE hMainThread;

extern void w32_init_main_thread (void);

/* The messages Emacs posts to itself.  Only these three are wanted
   without a window system; the rest ask a w32 frame for something.  */
#define WM_EMACS_START                 (WM_USER + 1)
#define WM_EMACS_INPUT_READY           (WM_EMACS_START + 24)
#define WM_EMACS_FILENOTIFY            (WM_EMACS_START + 25)

typedef struct W32Msg {
    MSG msg;
    DWORD dwModifiers;
    RECT rect;
} W32Msg;

/* The critical section the threads are guarded with, and the queue
   they pass messages through.  */
extern CRITICAL_SECTION critsect;

#define enter_crit() EnterCriticalSection (&critsect)
#define leave_crit() LeaveCriticalSection (&critsect)

extern void init_crit (void);
extern void delete_crit (void);
extern void signal_quit (void);

extern BOOL get_next_msg (W32Msg *, BOOL);
extern BOOL post_msg (W32Msg *);
extern BOOL prepend_msg (W32Msg *);
extern int drain_message_queue (void);

/* The system, as the heap, the dynamic loader and the registry see
   it.  The version numbers themselves are in w32common.h.  */
extern char *w32_strerror (int error_no);
extern LPBYTE w32_get_resource (const char *, const char *, LPDWORD);
extern void w32_reset_stack_overflow_guard (void);
extern void w32_sys_ring_bell (struct frame *f);

/* The low level keyboard hook, which the console terminal sets up as
   much as a window system frame does.  */
#ifdef WINDOWSNT
extern void setup_w32_kbdhook (HWND);
extern void remove_w32_kbdhook (void);
extern void reset_w32_kbdhook_state (void);
extern int check_w32_winkey_state (int);
#define w32_kbdhook_active (os_subtype != OS_SUBTYPE_9X)
#else
#define w32_kbdhook_active 0
#endif

/* Keypad command key support.  W32 doesn't have virtual keys defined
   for the function keys on the keypad (they are mapped to the standard
   function keys), so we define our own.  */
#define VK_NUMPAD_BEGIN		0x92
#define VK_NUMPAD_CLEAR		(VK_NUMPAD_BEGIN + 0)
#define VK_NUMPAD_ENTER		(VK_NUMPAD_BEGIN + 1)
#define VK_NUMPAD_PRIOR		(VK_NUMPAD_BEGIN + 2)
#define VK_NUMPAD_NEXT		(VK_NUMPAD_BEGIN + 3)
#define VK_NUMPAD_END		(VK_NUMPAD_BEGIN + 4)
#define VK_NUMPAD_HOME		(VK_NUMPAD_BEGIN + 5)
#define VK_NUMPAD_LEFT		(VK_NUMPAD_BEGIN + 6)
#define VK_NUMPAD_UP		(VK_NUMPAD_BEGIN + 7)
#define VK_NUMPAD_RIGHT		(VK_NUMPAD_BEGIN + 8)
#define VK_NUMPAD_DOWN		(VK_NUMPAD_BEGIN + 9)
#define VK_NUMPAD_INSERT	(VK_NUMPAD_BEGIN + 10)
#define VK_NUMPAD_DELETE	(VK_NUMPAD_BEGIN + 11)

#ifndef VK_LWIN
/* Older compiler environments don't have these defined.  */
#define VK_LWIN			0x5B
#define VK_RWIN			0x5C
#define VK_APPS			0x5D
#endif

/* Support for treating Windows and Apps keys as modifiers.  These
   constants must not overlap with any of the dwControlKeyState flags in
   KEY_EVENT_RECORD.  */
#define LEFT_WIN_PRESSED       0x8000
#define RIGHT_WIN_PRESSED      0x4000
#define APPS_PRESSED           0x2000

/* What the console input in w32inevt.c makes of a key.  */
extern unsigned int map_keypad_keys (unsigned int, unsigned int);
extern unsigned int w32_key_to_modifier (int key);
extern int w32_kbd_mods_to_emacs (DWORD mods, WORD key);
extern int w32_kbd_patch_key (KEY_EVENT_RECORD *event, int cpId);

extern void syms_of_w32base (void);
extern void globals_of_w32base (void);

#endif /* EMACS_W32BASE_H */
