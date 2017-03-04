/*
 * Copyright (C) 2017 Christoph Muellner
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UTILS_H_
#define UTILS_H_

#include <stdbool.h>
#include <wintypes.h>

#define MAX_KERKEY_DEVICES 16

/* opaque pointer */
struct reader;

/* Check if reader with given lun exists */
bool reader_exists(DWORD lun);

/* Creates a new reader with given lun */
struct reader* create_reader(DWORD lun);

/* Gets (existing) reader with the given lun. */
struct reader* get_reader(DWORD lun);

/* Set prv pointer for reader */
void set_reader_prv(struct reader *r, void* prv);

/* Get prv pointer from reader */
void* get_reader_prv(struct reader *r);

/* Free the reader */
void free_reader(struct reader *r);

#endif /* UTILS_H_ */
