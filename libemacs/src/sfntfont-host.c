/* The sfnt font driver for frames that a host application draws.

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

/* The fonts of a host frame are the font engine Emacs has for Android
   (sfnt.c, sfntfont.c), which reads TrueType files itself and needs
   nothing of the system.  Here it only measures: the host draws the
   text in its own fonts, and Emacs has to lay the text out in the
   same widths, so the glyphs it would draw are never drawn.  This is
   sfntfont-android.c without the drawing, and with the font files
   looked for where the systems a host runs on keep them.  */

#include <config.h>

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "lisp.h"
#include "blockinput.h"
#include "coding.h"
#include "font.h"
#include "sfntfont.h"
#include "pdumper.h"
#include "hostterm.h"
#include "sfnthost.h"

/* How deep below a font directory to look: fonts are kept a directory
   or two down, by foundry and by format, and a directory that is a
   whole tree of something else should not be read to its bottom.  */
#define MAX_FONT_DIRECTORY_DEPTH 4

/* The font cache.  */
static Lisp_Object font_cache;

static Lisp_Object
sfntfont_host_get_cache (struct frame *f)
{
  return font_cache;
}

/* Nothing draws: the host draws the text.  This is only called for a
   glyph string that redisplay draws, and the redisplay interface
   draws none (hostterm.c).  */

static void
sfntfont_host_put_glyphs (struct glyph_string *s, int from, int to,
			  int x, int y, bool with_background,
			  struct sfnt_raster **rasters, int *x_coords)
{
}

const struct font_driver host_sfntfont_driver =
  {
    .type = LISPSYM_INITIALLY (Qsfnt_host),
    .case_sensitive = true,
    .get_cache = sfntfont_host_get_cache,
    .list = sfntfont_list,
    .match = sfntfont_match,
    .draw = sfntfont_draw,
    .open_font = sfntfont_open,
    .close_font = sfntfont_close,
    .encode_char = sfntfont_encode_char,
    .text_extents = sfntfont_text_extents,
    .list_family = sfntfont_list_family,
    .get_variation_glyphs = sfntfont_get_variation_glyphs,
#ifdef HAVE_HARFBUZZ
    .begin_hb_font = sfntfont_begin_hb_font,
    .combining_capability = hbfont_combining_capability,
    .shape = hbfont_shape,
    .otf_capability = hbfont_otf_capability,
#endif /* HAVE_HARFBUZZ */
  };

/* Whether NAME is that of a font file sfnt reads.  */

static bool
font_file_name_p (const char *name)
{
  size_t length = strlen (name);

  return (length > 4
	  && (!c_strcasecmp (name + length - 4, ".ttf")
	      || !c_strcasecmp (name + length - 4, ".ttc")));
}

/* Enumerate the font files in DIRECTORY, a file name in the system's
   encoding, and in the directories below it to DEPTH levels.  */

static void
enumerate_directory (const char *directory, int depth)
{
  DIR *dir = opendir (directory);
  struct dirent *entry;

  if (!dir)
    return;

  while ((entry = readdir (dir)))
    {
      struct stat st;
      char *name;

      if (entry->d_name[0] == '.')
	continue;

      name = xmalloc (strlen (directory) + strlen (entry->d_name) + 2);
      sprintf (name, "%s/%s", directory, entry->d_name);

      /* stat rather than d_type, which says nothing of where a
	 symbolic link goes, and the fonts of a Nix profile are all
	 links.  */
      if (!stat (name, &st))
	{
	  if (S_ISDIR (st.st_mode))
	    {
	      if (depth > 0)
		enumerate_directory (name, depth - 1);
	    }
	  else if (font_file_name_p (entry->d_name))
	    sfnt_enum_font (name);
	}

      xfree (name);
    }

  closedir (dir);
}

/* The English name of CODE in NAME, which is what a font is to be
   known by here: the host draws the text with the system's own fonts
   and finds them by the family Emacs gives it, and a font whose names
   are in many languages lists them by the number of the language,
   before English as often as not.  The Windows record for American
   English is taken first, then the Macintosh one for English, which
   is language 0 there.  */

bool
host_find_english_name (struct sfnt_name_table *name,
			enum sfnt_name_identifier_code code,
			struct sfnt_name_record *record)
{
  int best = -1, best_rank = 2;

  for (int i = 0; i < name->count; ++i)
    {
      struct sfnt_name_record *candidate = &name->name_records[i];
      int rank;

      if (candidate->name_id != code)
	continue;

      if (candidate->platform_id == SFNT_PLATFORM_MICROSOFT
	  && candidate->language_id == 0x0409)
	rank = 0;
      else if (candidate->platform_id == SFNT_PLATFORM_MACINTOSH
	       && candidate->language_id == 0)
	rank = 1;
      else
	continue;

      if (rank < best_rank)
	{
	  best = i;
	  best_rank = rank;
	}
    }

  if (best < 0)
    return false;

  /* The offsets within have already been validated.  */
  *record = name->name_records[best];
  return true;
}

/* Read the font files in `host-font-directories'.  Done once, when the
   display is opened rather than as Emacs starts: reading the names in
   a font needs coding systems that are only there once Lisp is.  */

void
host_enumerate_fonts (void)
{
  static bool enumerated;
  Lisp_Object tail;

  if (enumerated)
    return;
  enumerated = true;

  for (tail = Vhost_font_directories; CONSP (tail); tail = XCDR (tail))
    {
      Lisp_Object directory = XCAR (tail);

      if (!STRINGP (directory))
	continue;

      directory = ENCODE_FILE (Fexpand_file_name (directory, Qnil));
      block_input ();
      enumerate_directory (SSDATA (directory), MAX_FONT_DIRECTORY_DEPTH);
      unblock_input ();
    }
}

/* The directories the systems a host runs on keep fonts in, and those
   of the XDG data directories, which is where Nix and Flatpak put the
   fonts that they install.  */

static Lisp_Object
default_font_directories (void)
{
  static const char *const directories[] =
    {
#ifdef WINDOWSNT
      "C:/Windows/Fonts",
      "~/AppData/Local/Microsoft/Windows/Fonts",
#elif defined DARWIN_OS
      "/System/Library/Fonts",
      "/Library/Fonts",
      "~/Library/Fonts",
#else
      "/usr/share/fonts",
      "/usr/local/share/fonts",
      "~/.local/share/fonts",
      "~/.fonts",
      "~/.nix-profile/share/fonts",
      "/run/current-system/sw/share/X11/fonts",
#endif
    };
  Lisp_Object list = Qnil;
  const char *xdg = getenv ("XDG_DATA_DIRS");

  for (int i = ARRAYELTS (directories) - 1; i >= 0; i--)
    list = Fcons (build_string (directories[i]), list);

  if (xdg && *xdg)
    {
      Lisp_Object extra = Qnil;
      const char *start = xdg;

      while (*start)
	{
	  const char *end = strchr (start, SEPCHAR);
	  ptrdiff_t length = end ? end - start : strlen (start);

	  if (length > 0)
	    extra = Fcons (concat2 (make_unibyte_string (start, length),
				    build_string ("/fonts")),
			   extra);
	  if (!end)
	    break;
	  start = end + 1;
	}

      list = nconc2 (list, Fnreverse (extra));
    }

  return list;
}

static void
syms_of_sfntfont_host_for_pdumper (void)
{
  init_sfntfont_vendor (Qsfnt_host, &host_sfntfont_driver,
			sfntfont_host_put_glyphs);
  register_font_driver (&host_sfntfont_driver, NULL);
}

void
init_sfntfont_host (void)
{
  /* What is there depends on the system Emacs runs on, not the one it
     was built on.  */
  Vhost_font_directories = default_font_directories ();

  /* Much of Emacs asks for "Monospace" and "Sans Serif", which no
     font is called: these are the families that stand for them, the
     first of each that is there.  */
  Vsfnt_default_family_alist
    = list4 (Fcons (build_string ("Monospace"),
		    build_string ("DejaVu Sans Mono")),
	     Fcons (build_string ("Monospace Serif"),
		    build_string ("DejaVu Sans Mono")),
	     Fcons (build_string ("Sans Serif"),
		    build_string ("DejaVu Sans")),
	     Fcons (build_string ("Serif"),
		    build_string ("DejaVu Serif")));
}

void
syms_of_sfntfont_host (void)
{
  DEFSYM (Qsfnt_host, "sfnt-host");

  DEFVAR_LISP ("host-font-directories", Vhost_font_directories,
    doc: /* Directories to read font files from, for frames a host draws.
Each is searched a few directories deep for TrueType files (.ttf and
.ttc).  They are read once, when the host's display is opened, which
is before the init file is loaded but after the early init file.  */);
  Vhost_font_directories = Qnil;

  font_cache = list (Qnil);
  staticpro (&font_cache);

  pdumper_do_now_and_after_load (syms_of_sfntfont_host_for_pdumper);
}
