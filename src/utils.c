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

#include <stddef.h>
#include "utils.h"

struct reader
{
	bool in_use;
	DWORD lun;
	void *prv;
};

static struct reader readers[MAX_KERKEY_DEVICES];

bool reader_exists(DWORD lun)
{
	size_t i;

	for (i=0; i<MAX_KERKEY_DEVICES; i++) {
		struct reader* r = &readers[i];
		if (r->in_use && r->lun == lun)
			return true;
	}

	return false;
}

struct reader* create_reader(DWORD lun)
{
	size_t i;

	for (i=0; i<MAX_KERKEY_DEVICES; i++) {
		struct reader* r = &readers[i];
		if (!r->in_use) {
			r->in_use = 1;
			r->lun = lun;
			r->prv = NULL;
			return r;
		}
	}

	return NULL;
}

struct reader* get_reader(DWORD lun)
{
	size_t i;

	for (i=0; i<MAX_KERKEY_DEVICES; i++) {
		struct reader* r = &readers[i];
		if (r->in_use && r->lun == lun)
			return r;
	}

	return NULL;
}

void set_reader_prv(struct reader *r, void* prv)
{
	r->prv = prv;
}

void* get_reader_prv(struct reader *r)
{
	return r->prv;
}

void free_reader(struct reader* r)
{
	r->in_use = 0;
}
