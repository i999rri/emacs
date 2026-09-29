/* What sfnt.c asks of the host window system.

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

#ifndef SFNTHOST_H
#define SFNTHOST_H

#include <stdbool.h>

#include "sfnt.h"

/* Find the record of CODE in NAME that is in English, and store it in
   *RECORD.  Return whether there is one.  */
extern bool host_find_english_name (struct sfnt_name_table *name,
				    enum sfnt_name_identifier_code code,
				    struct sfnt_name_record *record);

#endif /* SFNTHOST_H */
