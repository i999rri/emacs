/* Frames that a host application draws: the fonts it is to draw in.

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

/* The fonts a host is to draw in.

   Emacs reads the font files itself and works out which glyph of a
   file each character comes to (sfntfont-host.c).  A host told to draw
   those glyphs has to look for them in the same file, since a glyph is
   numbered by the file it is in and nothing else; a font it found by
   name might be another file, with other numbers in it.  So the file
   itself is what it is given.

   It is given in two steps.  Which file a font is comes first, as its
   name and what it was when it was read, which is small and is said of
   every font as it is first drawn in.  The file itself follows only
   where the host asks for it: it may be tens of megabytes, it has to
   cross to the host as text, and a host that kept it from a run before
   this one has no need of it again.  */

#include <config.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lisp.h"
#include "frame.h"
#include "sfntfont.h"
#include "hostterm.h"
#include "hostlib.h"

/* A font file the host has been told of.  */

struct host_font
{
  struct host_font *next;

  /* The file it was read from, and which face of that file.  */
  char *path;
  int instance;

  /* What the host knows it by.  */
  int id;
};

static struct host_font *host_fonts;
static int host_fonts_named;

/* Say to the host which file the font FONT is, and what it was when it
   was read, which is how the host tells a file it already has from one
   it does not.  */

static void
host_font_say (const struct host_api *api, struct host_font *font)
{
  struct stat about;
  char *message;

  if (stat (font->path, &about) < 0)
    return;

  message = xmalloc (256 + 2 * strlen (font->path));
  sprintf (message,
	   "{\"type\":\"font\",\"id\":%d,\"instance\":%d,\"file\":\"",
	   font->id, font->instance);

  /* The name as JSON: a path may hold a backslash or a quotation
     mark, and nothing else in it has to be escaped.  */
  {
    char *at = message + strlen (message);
    const char *from;

    for (from = font->path; *from; from++)
      {
	if (*from == '\\' || *from == '"')
	  *at++ = '\\';
	*at++ = *from;
      }
    sprintf (at, "\",\"size\":%lld,\"when\":%lld}",
	     (long long) about.st_size, (long long) about.st_mtime);
  }

  api->post (message);
  xfree (message);
}

/* The number the host knows the font of S by, telling it of the file
   if it has not been told, or -1 where there is no telling it.  */

int
host_font_id (struct glyph_string *s)
{
  const struct host_api *api = host_current_api ();
  struct host_font *font;
  const char *path;
  int instance = -1;

  if (!api || !s->font)
    return -1;

  path = sfntfont_file_name (s->font, &instance);
  if (!path)
    return -1;

  for (font = host_fonts; font; font = font->next)
    if (font->instance == instance && !strcmp (font->path, path))
      return font->id;

  font = xmalloc (sizeof *font);
  font->path = xstrdup (path);
  font->instance = instance;
  font->id = host_fonts_named++;
  font->next = host_fonts;
  host_fonts = font;

  host_font_say (api, font);
  return font->id;
}

/* Send the host the file of the font it knows by ID, which it asks for
   when it has none of that file and something to draw from it.  */

void
host_font_wanted (int id)
{
  const struct host_api *api = host_current_api ();
  struct host_font *font;
  unsigned char *bytes;
  ptrdiff_t length;
  char *message, *at;
  int file;
  off_t size;

  if (!api)
    return;

  for (font = host_fonts; font; font = font->next)
    if (font->id == id)
      break;
  if (!font)
    return;

  file = emacs_open (font->path, O_RDONLY, 0);
  if (file < 0)
    return;

  size = lseek (file, 0, SEEK_END);
  if (size <= 0 || lseek (file, 0, SEEK_SET) < 0)
    {
      emacs_close (file);
      return;
    }

  bytes = xmalloc (size);
  length = emacs_read_quit (file, bytes, size);
  emacs_close (file);

  if (length == size)
    {
      message = xmalloc (128 + 4 * ((length + 2) / 3) + 4);
      at = message + sprintf (message,
			      "{\"type\":\"font\",\"id\":%d,\"bytes\":\"",
			      font->id);
      at = host_base64 (at, bytes, length);
      strcpy (at, "\"}");

      api->post (message);
      xfree (message);
    }

  xfree (bytes);
}

DEFUN ("host-send-font", Fhost_send_font, Shost_send_font, 1, 1, 0,
       doc: /* Send the host the file of the font it knows by ID.
The host asks for one when it has something to draw from a font and
has not the file it is in.  */)
  (Lisp_Object id)
{
  CHECK_FIXNUM (id);
  host_font_wanted (XFIXNUM (id));
  return Qnil;
}

void
syms_of_hostfont (void)
{
  defsubr (&Shost_send_font);
}
