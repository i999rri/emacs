/* Frames that a host application draws: its keys, text, pointer and focus.

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

/* The host sends what the person does as messages (docs/protocol.md):
   `key', `text', `pointer' and `focus'.  Here they become the events
   any other window system makes of what its system tells it, read by
   the terminal's read_socket_hook as Emacs reads input, in the order
   they came.  The rest of the host's messages are Lisp's, which takes
   them with `host-take-events'.

   host.c receives the messages on a thread of the host's and asks
   host_input_message_p which of the two queues each goes to; then it
   writes a byte to a pipe that Emacs waits on as it waits on a
   keyboard, so that input is read as soon as Emacs wants input,
   rather than when a timer comes round.  The messages are JSON, and
   this reads the little of JSON they use in C: the thread of the
   host's cannot use Lisp, and read_socket_hook should not have to.

   Named keys.  A key the layout gives no character for comes with
   the name Emacs has for it (`return', `f5', `kp-add').  Such a key is
   made a NON_ASCII_KEYSTROKE_EVENT whose code is the X keysym that
   has that name in keyboard.c's lispy_function_keys, found by name:
   that table is what makes a function key's symbol from its code on
   X, and so on here too, with no table of our own to keep the same
   as it.  A name that is not in it is dropped.

   TODO: `resize' is still Lisp's (urushi-screen--resize), which sizes
   the frame with `set-frame-size'.  Taking it here would size the
   frame in the order of the keys around it.  */

#include <config.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lisp.h"
#include "blockinput.h"
#include "keyboard.h"
#include "termhooks.h"
/* For the terminal a frame on no window system keeps what the
   pointer is over in, which MOUSE_HL_INFO reaches for.  */
#include "termchar.h"
#include "window.h"
#include "systime.h"
#include "systhread.h"	/* for the lock on the wakeup below */
#ifdef WINDOWSNT
/* The message queue Emacs on Windows is woken through.  */
#include "w32base.h"
#endif
#include "hostlib.h"
#include "hostterm.h"

/* X's function keysyms are 0xff00 and up, and lispy_function_keys
   is indexed from there (keyboard.c's FUNCTION_KEY_OFFSET).  */
#define HOST_FUNCTION_KEY_OFFSET 0xff00

/* How the host's thread wakes the one Emacs waits on.

   Everywhere but Windows that is a pipe of our own, waited on beside
   the keyboard.  On Windows it is the message queue every Emacs there
   already has (w32base.c), because a pipe cannot do it: sys_select
   waits on handles, and it passes over a descriptor that is not a
   child's or a socket's, so nothing would wake (w32proc.c).  Emacs
   does not build its own self-pipe for child signals there either
   (process.c).  */

#ifndef WINDOWSNT

static int wakeup_pipe[2] = { -1, -1 };

/* Whether a byte is in that pipe that Emacs has not read yet, and the
   lock the two threads keep it under.  One byte is enough: it says
   there is something to look at, and looking takes everything there
   is.  Keeping the count is what lets the byte be read without
   waiting, a read asking for exactly what is known to be there.  */
static sys_mutex_t wakeup_lock;
static int wakeups_sent;

#endif /* not WINDOWSNT */

/* The wheel's movement not yet made into whole lines, as a touchpad
   moves it a fraction of a line at a time.  */
static double wheel_x, wheel_y;

/* A little JSON.  The messages are one object each, whose members are
   strings, numbers, booleans and arrays of strings; nothing else is
   read, and anything else is passed over.  */

enum json_kind { JSON_STRING, JSON_NUMBER, JSON_TRUE, JSON_FALSE, JSON_NULL };

/* Called for each member of the object, and for each element of a
   member that is an array, with KEY the member's name.  STRING is the
   value of a string, decoded to UTF-8, and NUMBER that of a number.  */
typedef void (*json_member_fn) (void *data, const char *key,
				enum json_kind kind, const char *string,
				double number);

static const char *
json_skip_space (const char *p)
{
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  return p;
}

static int
json_hex (const char *p)
{
  int value = 0;

  for (int i = 0; i < 4; i++)
    {
      int c = p[i], digit;

      if ('0' <= c && c <= '9')
	digit = c - '0';
      else if ('a' <= c && c <= 'f')
	digit = c - 'a' + 10;
      else if ('A' <= c && c <= 'F')
	digit = c - 'A' + 10;
      else
	return -1;
      value = value * 16 + digit;
    }

  return value;
}

static char *
json_put_utf8 (char *out, int c)
{
  if (c < 0x80)
    *out++ = c;
  else if (c < 0x800)
    {
      *out++ = 0xc0 | (c >> 6);
      *out++ = 0x80 | (c & 0x3f);
    }
  else if (c < 0x10000)
    {
      *out++ = 0xe0 | (c >> 12);
      *out++ = 0x80 | ((c >> 6) & 0x3f);
      *out++ = 0x80 | (c & 0x3f);
    }
  else
    {
      *out++ = 0xf0 | (c >> 18);
      *out++ = 0x80 | ((c >> 12) & 0x3f);
      *out++ = 0x80 | ((c >> 6) & 0x3f);
      *out++ = 0x80 | (c & 0x3f);
    }
  return out;
}

/* Read the string that P, just past its opening quote, begins, into
   OUT, which has room for it: a string decoded is never longer than
   it was in JSON.  Return where it ends, past its closing quote, or
   NULL if it does not.  */

static const char *
json_string (const char *p, char *out)
{
  while (*p != '"')
    {
      if (!*p)
	return NULL;
      if (*p != '\\')
	{
	  *out++ = *p++;
	  continue;
	}

      p++;
      switch (*p)
	{
	case 'b': *out++ = '\b'; break;
	case 'f': *out++ = '\f'; break;
	case 'n': *out++ = '\n'; break;
	case 'r': *out++ = '\r'; break;
	case 't': *out++ = '\t'; break;
	case 'u':
	  {
	    int c = json_hex (p + 1);

	    if (c < 0)
	      return NULL;
	    p += 4;
	    /* A character past the first 65536 comes as two halves of
	       UTF-16.  One half alone is no character.  */
	    if (0xd800 <= c && c < 0xdc00
		&& p[1] == '\\' && p[2] == 'u')
	      {
		int low = json_hex (p + 3);

		if (0xdc00 <= low && low < 0xe000)
		  {
		    c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
		    p += 6;
		  }
	      }
	    if (0xd800 <= c && c < 0xe000)
	      c = 0xfffd;
	    out = json_put_utf8 (out, c);
	  }
	  break;
	case '\0':
	  return NULL;
	default:
	  /* \" \\ \/ */
	  *out++ = *p;
	  break;
	}
      p++;
    }

  *out = '\0';
  return p + 1;
}

/* Pass over the value P begins, whatever it is.  Return where it ends,
   or NULL if it does not.  */

static const char *
json_skip (const char *p)
{
  int depth = 0;

  do
    {
      p = json_skip_space (p);
      switch (*p)
	{
	case '\0':
	  return NULL;
	case '"':
	  p++;
	  while (*p != '"')
	    {
	      if (!*p || (*p == '\\' && !*++p))
		return NULL;
	      p++;
	    }
	  p++;
	  break;
	case '{': case '[':
	  depth++;
	  p++;
	  break;
	case '}': case ']':
	  depth--;
	  p++;
	  break;
	default:
	  p++;
	  break;
	}
    }
  while (depth > 0);

  return p;
}

/* Read the value P begins, and give it to FN as KEY's, or each of its
   elements if it is an array.  BUFFER has room for any string in the
   message.  Return where it ends, or NULL if it does not.  */

static const char *
json_value (const char *p, const char *key, char *buffer,
	    json_member_fn fn, void *data)
{
  p = json_skip_space (p);

  switch (*p)
    {
    case '"':
      p = json_string (p + 1, buffer);
      if (p)
	fn (data, key, JSON_STRING, buffer, 0);
      return p;

    case '[':
      p = json_skip_space (p + 1);
      if (*p == ']')
	return p + 1;
      for (;;)
	{
	  if (*p == '[' || *p == '{')
	    p = json_skip (p);
	  else
	    p = json_value (p, key, buffer, fn, data);
	  if (!p)
	    return NULL;
	  p = json_skip_space (p);
	  if (*p == ']')
	    return p + 1;
	  if (*p != ',')
	    return NULL;
	  p++;
	}

    case '{':
      return json_skip (p);

    case 't':
      fn (data, key, JSON_TRUE, NULL, 0);
      return strncmp (p, "true", 4) ? NULL : p + 4;

    case 'f':
      fn (data, key, JSON_FALSE, NULL, 0);
      return strncmp (p, "false", 5) ? NULL : p + 5;

    case 'n':
      fn (data, key, JSON_NULL, NULL, 0);
      return strncmp (p, "null", 4) ? NULL : p + 4;

    default:
      {
	char *end;
	double number = strtod (p, &end);

	if (end == p)
	  return NULL;
	fn (data, key, JSON_NUMBER, NULL, number);
	return end;
      }
    }
}

/* Read MESSAGE, one JSON object, and give FN each of its members.
   Uses only the C library, so that a thread that is not Emacs's may
   call it.  Return false if MESSAGE is not an object.  */

static bool
json_object (const char *message, json_member_fn fn, void *data)
{
  size_t size = strlen (message) + 1;
  /* Keys and values each in a part of their own, since FN is given a
     key and its value together.  */
  char *keys = host_alloc (size * 2);
  char *values = keys + size;
  const char *p = json_skip_space (message);
  bool ok = false;

  if (!keys || *p != '{')
    goto done;

  p = json_skip_space (p + 1);
  if (*p == '}')
    {
      ok = true;
      goto done;
    }

  for (;;)
    {
      if (*p != '"' || !(p = json_string (p + 1, keys)))
	goto done;
      p = json_skip_space (p);
      if (*p != ':')
	goto done;
      if (!(p = json_value (p + 1, keys, values, fn, data)))
	goto done;
      p = json_skip_space (p);
      if (*p == '}')
	break;
      if (*p != ',')
	goto done;
      p = json_skip_space (p + 1);
    }
  ok = true;

 done:
  if (keys)
    host_free (keys);
  return ok;
}

/* Which queue a message goes to.  */

struct message_type
{
  char type[16];
};

static void
take_type (void *data, const char *key, enum json_kind kind,
	   const char *string, double number)
{
  struct message_type *type = data;

  if (kind == JSON_STRING && !strcmp (key, "type"))
    {
      strncpy (type->type, string, sizeof type->type - 1);
      type->type[sizeof type->type - 1] = '\0';
    }
}

/* Whether MESSAGE is one of the host's input messages, which are read
   here rather than by Lisp.  `composition' is not: what the input
   method is still turning over is drawn by Lisp at the cursor
   (urushi-screen.el), and is no input until it comes as `text'.
   Called on the host's thread.  */

static bool
host_input_message_p (const char *message)
{
  struct message_type type = { "" };

  json_object (message, take_type, &type);
  return (!strcmp (type.type, "key")
	  || !strcmp (type.type, "text")
	  || !strcmp (type.type, "pointer")
	  || !strcmp (type.type, "focus"));
}

/* Wake Emacs to read what was queued.  Called on the host's thread,
   and only writes to the pipe.  */

/* Nothing of Emacs's is touched here: this runs on a thread of the
   host's, and Emacs's allocator and its book of descriptors are the
   other thread's alone -- touching them corrupts the heap.  What both
   ways below use is the system's.  */

static void
host_input_wakeup (void)
{
#ifdef WINDOWSNT
  W32Msg msg;

  /* The message itself says nothing; it is posting one that signals
     what Emacs waits on for input.  host_read_socket takes them all
     again, which is what lets that signal fall.  */
  memset (&msg, 0, sizeof msg);
  msg.msg.message = WM_EMACS_INPUT_READY;
  post_msg (&msg);
#else
  char byte = 0;

  sys_mutex_lock (&wakeup_lock);
  /* Written before it is counted, so that what is counted is always
     there to be read.  A wakeup Emacs has not taken yet will bring it
     to this message as well.  */
  if (wakeups_sent == 0 && write (wakeup_pipe[1], &byte, 1) == 1)
    wakeups_sent = 1;
  sys_mutex_unlock (&wakeup_lock);
#endif
}

/* A message, read.  */

struct host_input
{
  char type[16];
  char kind[16];
  char name[32];
  /* The character of a key, or -1.  */
  int ch;
  /* The text of `text', decoded, in memory of the host's.  */
  char *text;
  int modifiers;
  bool down, focused;
  int button, clicks;
  double x, y, dx, dy;
};

static int
host_modifier (const char *name)
{
  static const struct { const char *name; int modifier; } modifiers[] =
    {
      { "ctrl", ctrl_modifier },
      { "meta", meta_modifier },
      { "shift", shift_modifier },
      { "super", super_modifier },
      { "hyper", hyper_modifier },
      { "alt", alt_modifier },
    };

  for (int i = 0; i < ARRAYELTS (modifiers); i++)
    if (!strcmp (name, modifiers[i].name))
      return modifiers[i].modifier;
  return 0;
}

static void
copy_string (char *to, size_t size, const char *from)
{
  strncpy (to, from, size - 1);
  to[size - 1] = '\0';
}

static void
take_member (void *data, const char *key, enum json_kind kind,
	     const char *string, double number)
{
  struct host_input *input = data;

  if (kind == JSON_STRING)
    {
      if (!strcmp (key, "type"))
	copy_string (input->type, sizeof input->type, string);
      else if (!strcmp (key, "kind"))
	copy_string (input->kind, sizeof input->kind, string);
      else if (!strcmp (key, "name"))
	copy_string (input->name, sizeof input->name, string);
      else if (!strcmp (key, "modifiers"))
	input->modifiers |= host_modifier (string);
      else if (!strcmp (key, "char") && *string)
	{
	  int length;

	  input->ch = string_char_and_length ((const unsigned char *) string,
					      &length);
	}
      else if (!strcmp (key, "text"))
	{
	  size_t size = strlen (string) + 1;

	  if (input->text)
	    host_free (input->text);
	  input->text = host_alloc (size);
	  if (input->text)
	    memcpy (input->text, string, size);
	}
    }
  else if (kind == JSON_NUMBER)
    {
      if (!strcmp (key, "char"))
	input->ch = number;
      else if (!strcmp (key, "button"))
	input->button = number;
      else if (!strcmp (key, "clicks"))
	input->clicks = number;
      else if (!strcmp (key, "x"))
	input->x = number;
      else if (!strcmp (key, "y"))
	input->y = number;
      else if (!strcmp (key, "dx"))
	input->dx = number;
      else if (!strcmp (key, "dy"))
	input->dy = number;
    }
  else if (kind == JSON_TRUE || kind == JSON_FALSE)
    {
      if (!strcmp (key, "down"))
	input->down = kind == JSON_TRUE;
      else if (!strcmp (key, "focused"))
	input->focused = kind == JSON_TRUE;
    }
}

/* Making events.  */

/* The frame a message is about.  Frames have no names the host knows
   yet, so this is the root frame of the selected one, or of the first
   host frame there is.  TODO: the message's `frame', once frames
   have ids.  */

static struct frame *
host_input_frame (void)
{
  struct frame *f = SELECTED_FRAME ();
  Lisp_Object tail, frame;

  if (FRAME_LIVE_P (f) && FRAME_HOST_P (f))
    return root_frame (f);

  FOR_EACH_FRAME (tail, frame)
    if (FRAME_HOST_P (XFRAME (frame)) && !FRAME_PARENT_FRAME (XFRAME (frame)))
      return XFRAME (frame);

  return NULL;
}

static Time
host_input_time (void)
{
  struct timespec now = current_timespec ();

  return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Return the function key whose name is NAME, as an offset into
   lispy_function_keys, or -1 if there is none.  */

static int
host_function_key (const char *name)
{
  /* The table is X's, which is long, but a key is pressed at a time,
     and it is read through far faster than a key can be.  */
  for (int i = 0; i < 256; i++)
    if (lispy_function_keys[i] && !strcmp (lispy_function_keys[i], name))
      return i;
  return -1;
}

static void
host_new_focus (struct host_display_info *dpyinfo, struct frame *f)
{
  dpyinfo->focus_frame = f;
  host_frame_rehighlight (f);
}

/* Store EVENT, and count it in *COUNT.  */

static void
store (struct input_event *event, struct input_event *hold_quit, int *count)
{
  kbd_buffer_store_event_hold (event, hold_quit);
  ++*count;
}

/* Put out what lights up under the pointer, where `mouse-highlight'
   asks for typing to put it out: a number there means the highlight is
   in the way of reading what was typed.  It lights up again when the
   pointer next moves.  */

static void
host_typed (struct frame *f)
{
  Mouse_HLInfo *hlinfo = MOUSE_HL_INFO (f);
  struct frame *mouse_frame = hlinfo->mouse_face_mouse_frame;

  if (hlinfo->mouse_face_hidden || !FIXNUMP (Vmouse_highlight)
      || EQ (f->tool_bar_window, hlinfo->mouse_face_window)
      || EQ (f->tab_bar_window, hlinfo->mouse_face_window))
    return;

  clear_mouse_face (hlinfo);
  hlinfo->mouse_face_hidden = true;
  if (mouse_frame)
    flush_frame (mouse_frame);
}

static void
host_key (struct host_input *input, struct frame *f,
	  struct input_event *hold_quit, int *count)
{
  struct input_event ie;

  /* Only a key pressed is a keystroke; letting go of it is nothing
     Emacs has an event for.  */
  if (!input->down)
    return;

  host_typed (f);

  EVENT_INIT (ie);
  XSETFRAME (ie.frame_or_window, f);
  ie.modifiers = input->modifiers;
  ie.timestamp = host_input_time ();

  if (input->ch >= 0)
    {
      if (!CHAR_VALID_P (input->ch))
	return;
      ie.kind = (ASCII_CHAR_P (input->ch)
		 ? ASCII_KEYSTROKE_EVENT : MULTIBYTE_CHAR_KEYSTROKE_EVENT);
      ie.code = input->ch;
    }
  else
    {
      int key = host_function_key (input->name);

      if (key < 0)
	return;
      ie.kind = NON_ASCII_KEYSTROKE_EVENT;
      ie.code = HOST_FUNCTION_KEY_OFFSET + key;
    }

  store (&ie, hold_quit, count);
}

/* What the input method settled on, typed as its characters are.  */

static void
host_text (struct host_input *input, struct frame *f,
	   struct input_event *hold_quit, int *count)
{
  const unsigned char *p = (const unsigned char *) input->text;
  struct input_event ie;

  if (!p)
    return;

  host_typed (f);

  while (*p)
    {
      int length;
      int c = string_char_and_length (p, &length);

      p += length;
      EVENT_INIT (ie);
      XSETFRAME (ie.frame_or_window, f);
      ie.kind = (ASCII_CHAR_P (c)
		 ? ASCII_KEYSTROKE_EVENT : MULTIBYTE_CHAR_KEYSTROKE_EVENT);
      ie.code = c;
      ie.timestamp = host_input_time ();
      store (&ie, hold_quit, count);
    }
}

/* The wheel moved by DELTA lines along one axis, of which *PENDING is
   what was left of the last time: make the whole lines of it into
   events of KIND, TOWARD when positive and AWAY when negative.  */

static void
host_wheel_axis (double delta, double *pending, enum event_kind kind,
		 int toward, int away, struct input_event *model,
		 struct input_event *hold_quit, int *count)
{
  *pending += delta;

  while (*pending >= 1 || *pending <= -1)
    {
      struct input_event ie = *model;

      ie.kind = kind;
      ie.modifiers |= *pending > 0 ? toward : away;
      store (&ie, hold_quit, count);
      *pending -= *pending > 0 ? 1 : -1;
    }
}

static void
host_pointer (struct host_input *input, struct frame *f,
	      struct input_event *hold_quit, int *count, int *help)
{
  struct host_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  Mouse_HLInfo *hlinfo = &dpyinfo->mouse_highlight;
  int x = input->x, y = input->y;
  struct input_event ie;

  EVENT_INIT (ie);
  XSETFRAME (ie.frame_or_window, f);
  XSETINT (ie.x, x);
  XSETINT (ie.y, y);
  ie.modifiers = input->modifiers;
  ie.timestamp = host_input_time ();
  dpyinfo->last_mouse_movement_time = ie.timestamp;

  if (!strcmp (input->kind, "leave"))
    {
      if (f == hlinfo->mouse_face_mouse_frame)
	{
	  clear_mouse_face (hlinfo);
	  hlinfo->mouse_face_mouse_frame = NULL;
	}
      if (dpyinfo->last_mouse_motion_frame == f)
	dpyinfo->last_mouse_motion_frame = NULL;
      if (!NILP (help_echo_string))
	*help = -1;
      return;
    }

  dpyinfo->last_mouse_motion_frame = f;
  dpyinfo->last_mouse_motion_x = x;
  dpyinfo->last_mouse_motion_y = y;

  if (!strcmp (input->kind, "move"))
    {
      /* What the pointer is over, for the mouse face and the help
	 echo, and that it moved, for a drag with `track-mouse'.  */

      previous_help_echo_string = help_echo_string;
      help_echo_string = Qnil;
      f->mouse_moved = true;

      /* Typing hides the pointer, and moving it is what brings it back:
	 what it is over is not looked at while it is hidden, so without
	 this nothing lights up under it once anything has been typed.  */
      frame_make_pointer_visible (f);

      /* What typing put out lights up again, and where it lights up is
	 worked out afresh: the text may have moved under the pointer
	 while it was out.  */
      if (hlinfo->mouse_face_hidden)
	{
	  hlinfo->mouse_face_hidden = false;
	  clear_mouse_face (hlinfo);
	}

      note_mouse_highlight (f, x, y);

      /* What lights up under the pointer is drawn as the pointer
	 arrives, outside any redisplay, and nothing else would hand
	 the picture over until the screen changed for some other
	 reason.  A window system that draws to a screen of its own
	 needs no such thing; one that is handed a picture does.  */
      flush_frame (f);

      if (!NILP (help_echo_string) || !NILP (previous_help_echo_string))
	*help = 1;
      return;
    }

  if (!strcmp (input->kind, "wheel"))
    {
      /* Away from the person is up, and to the right is right.  */
      host_wheel_axis (input->dy, &wheel_y, WHEEL_EVENT,
		       up_modifier, down_modifier, &ie, hold_quit, count);
      host_wheel_axis (input->dx, &wheel_x, HORIZ_WHEEL_EVENT,
		       down_modifier, up_modifier, &ie, hold_quit, count);
      return;
    }

  if (input->button < 1 || input->button > 5)
    return;

  /* Emacs counts double and triple clicks itself, from the time and
     place of each, as it does on X; the host's `clicks' is not
     needed.  */
  if (!strcmp (input->kind, "down"))
    {
      ie.modifiers |= down_modifier;
      dpyinfo->grabbed |= 1 << input->button;
      dpyinfo->last_mouse_frame = f;
    }
  else if (!strcmp (input->kind, "up"))
    {
      /* An up whose down Emacs did not see is not a click.  */
      if (!(dpyinfo->grabbed & (1 << input->button)))
	return;
      ie.modifiers |= up_modifier;
      dpyinfo->grabbed &= ~(1 << input->button);
    }
  else
    return;

  ie.kind = MOUSE_CLICK_EVENT;
  ie.code = input->button - 1;
  f->mouse_moved = false;
  store (&ie, hold_quit, count);
}

static void
host_focus (struct host_input *input, struct frame *f,
	    struct input_event *hold_quit, int *count)
{
  struct host_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct input_event ie;

  EVENT_INIT (ie);
  XSETFRAME (ie.frame_or_window, f);
  ie.timestamp = host_input_time ();

  if (input->focused)
    {
      host_new_focus (dpyinfo, f);
      ie.kind = FOCUS_IN_EVENT;
    }
  else
    {
      host_new_focus (dpyinfo, NULL);
      ie.kind = FOCUS_OUT_EVENT;
    }

  store (&ie, hold_quit, count);
}

/* Read the input the host has sent, and make events of it.  */

int
host_read_socket (struct terminal *terminal, struct input_event *hold_quit)
{
  int count = 0, help = 0;
  char *message;

  /* Take the wakeups first: one that comes after this is for a message
     that comes after it too.  */
#ifdef WINDOWSNT
  {
    W32Msg msg;

    /* All of them, which is what lowers the signal Emacs waits on:
       get_next_msg does that when the queue runs out.  */
    while (get_next_msg (&msg, FALSE))
      continue;
  }
#else
  {
    char drain[1];

    /* Only as much as was counted, so that this never waits for a byte
       that is not coming.  */
    sys_mutex_lock (&wakeup_lock);
    int waiting = wakeups_sent;
    wakeups_sent = 0;
    sys_mutex_unlock (&wakeup_lock);

    if (waiting && read (wakeup_pipe[0], drain, waiting) < 0)
      /* Nothing to do about it: the message is read below all the
	 same, and the next wakeup will find the pipe as it is.  */
      ;
  }
#endif

  block_input ();

  while ((message = host_take_input ()))
    {
      struct host_input input = { .ch = -1 };
      struct frame *f = host_input_frame ();

      json_object (message, take_member, &input);
      host_free (message);

      if (f)
	{
	  if (!strcmp (input.type, "key"))
	    host_key (&input, f, hold_quit, &count);
	  else if (!strcmp (input.type, "text"))
	    host_text (&input, f, hold_quit, &count);
	  else if (!strcmp (input.type, "pointer"))
	    host_pointer (&input, f, hold_quit, &count, &help);
	  else if (!strcmp (input.type, "focus"))
	    host_focus (&input, f, hold_quit, &count);
	}

      if (input.text)
	host_free (input.text);
    }

  /* The messages that are not input are Lisp's to handle, and this is
     where Emacs learns there are any: the host wrote to the pipe for
     these as well as for input.  An event carries the handling into
     the command loop, where Lisp may run.  */
  /* One at a time: the keyboard buffer is where keys and the pointer
     wait too, and it is not endless.  Another event while the first is
     still there would say nothing new and would crowd them out.  The
     telling is forgotten where the messages are taken and not here,
     since Emacs is woken to read this by a message arriving and not by
     Lisp reading one: told from the emptiness of the queue instead, a
     look that found it empty was the only chance to forget, and the
     message after the one Lisp took was announced to nobody.  */
  if (host_lisp_pending_p () && !host_lisp_told_p ()
      && !NILP (Vhost_message_function))
    {
      struct input_event ie;

      EVENT_INIT (ie);
      ie.kind = NOTIFICATION_EVENT;
      ie.arg = Fcons (Vhost_message_function, Qnil);
      kbd_buffer_store_event_hold (&ie, hold_quit);
      host_lisp_told ();
      count++;
    }

  /* The help echo of what the pointer is over, or none any more.  */
  if (help && !(hold_quit && hold_quit->kind != NO_EVENT))
    {
      Lisp_Object frame = Qnil;
      struct frame *f = x_display_list->last_mouse_motion_frame;

      if (f)
	XSETFRAME (frame, f);
      if (help > 0)
	gen_help_event (help_echo_string, frame, help_echo_window,
			help_echo_object, help_echo_pos);
      else
	{
	  help_echo_string = Qnil;
	  gen_help_event (Qnil, frame, Qnil, Qnil, 0);
	}
      count++;
    }

  unblock_input ();
  return count;
}

/* Where the pointer last was, for `mouse-position' and for a drag.  */

void
host_mouse_position (struct frame **fp, int insist, Lisp_Object *bar_window,
		     enum scroll_bar_part *part, Lisp_Object *x,
		     Lisp_Object *y, Time *timestamp)
{
  struct host_display_info *dpyinfo = x_display_list;
  Lisp_Object tail, frame;
  struct frame *f;

  if (!fp || !dpyinfo)
    return;

  block_input ();

  FOR_EACH_FRAME (tail, frame)
    if (FRAME_HOST_P (XFRAME (frame)))
      XFRAME (frame)->mouse_moved = false;

  f = (gui_mouse_grabbed (dpyinfo) ? dpyinfo->last_mouse_frame
       : dpyinfo->last_mouse_motion_frame);

  if (f && FRAME_LIVE_P (f))
    {
      *fp = f;
      *bar_window = Qnil;
      *part = scroll_bar_above_handle;
      XSETINT (*x, dpyinfo->last_mouse_motion_x);
      XSETINT (*y, dpyinfo->last_mouse_motion_y);
      *timestamp = dpyinfo->last_mouse_movement_time;
    }

  unblock_input ();
}

/* The focus.  The host has it, and says where it is with `focus';
   Emacs can only follow.  */

Lisp_Object
host_get_focus_frame (struct frame *f)
{
  struct frame *focus = FRAME_DISPLAY_INFO (f)->focus_frame;
  Lisp_Object frame = Qnil;

  if (focus && FRAME_LIVE_P (focus))
    XSETFRAME (frame, focus);
  return frame;
}

/* Make the frame that has the focus, or the one it redirects its
   focus to, the one whose cursor is shown as having it.  */

void
host_frame_rehighlight (struct frame *ignored)
{
  struct host_display_info *dpyinfo = x_display_list;
  struct frame *old, *f;

  if (!dpyinfo)
    return;

  old = dpyinfo->highlight_frame;
  f = dpyinfo->focus_frame;

  if (f && FRAME_LIVE_P (f))
    {
      Lisp_Object redirect = FRAME_FOCUS_FRAME (f);

      if (FRAMEP (redirect) && FRAME_LIVE_P (XFRAME (redirect)))
	f = XFRAME (redirect);
    }
  else
    f = NULL;

  dpyinfo->highlight_frame = f;

  if (old != f)
    {
      if (old && FRAME_LIVE_P (old))
	gui_update_cursor (old, true);
      if (f)
	gui_update_cursor (f, true);
    }
}

/* Start taking the host's input from Lisp's queue.  */

void
host_input_init (void)
{
#ifdef WINDOWSNT
  /* Emacs waits on what the message queue signals only where the
     descriptor it calls the keyboard is one of the descriptors it is
     waiting on, that being how it waits for a key on Windows
     (sys_select in w32proc.c hands descriptor 0 to that signal).  */
  add_keyboard_wait_descriptor (0);
#else
  if (wakeup_pipe[0] >= 0)
    return;

  if (emacs_pipe (wakeup_pipe) < 0)
    fatal ("Could not make a pipe for the host's input");
  /* Only the writing end, which is the end that can be made not to
     wait everywhere; the reading end is kept from waiting by reading
     no more than wakeups_sent says is there.  */
  fcntl (wakeup_pipe[1], F_SETFL, O_NONBLOCK);
  sys_mutex_init (&wakeup_lock);
  add_keyboard_wait_descriptor (wakeup_pipe[0]);
#endif

  host_claim_input (host_input_message_p, host_input_wakeup);
}

void
syms_of_hostinput (void)
{
  DEFVAR_LISP ("host-message-function", Vhost_message_function,
	       doc: /* Function called with no arguments for the host's messages.
It is called when the host has sent messages that are not input, which
are the ones `host-take-events' returns, and is to take them.  Called
as the messages come, from the command loop.

While this is nil nothing tells Lisp that any came, and whatever wants
them must look for itself.  */);
  Vhost_message_function = Qnil;
}
