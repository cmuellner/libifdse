/*
 * Copyright (C) 2020 Christoph Muellner
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

#include <debuglog.h>

#include "halse.h"
#include "helpers.h"
#include "halse_kerkey.h"

static const char* halse_kerkey_id = "kerkey";

struct lun_se {
	bool in_use;
	DWORD lun;
	struct halse_dev *dev;
};

static struct lun_se lun_se_array[MAX_SE_DEVICES];

static struct halse_dev* halse_parse(char* config)
{
	char *p = config;

	/* Sanity check and advance to se driver. */
	if (!starts_with("se:", p)) {
		Log2(PCSC_LOG_ERROR, "Invalid config: '%s'!", config);
		return NULL;
	}
	p = strchr(p, ':');
	p++;

	/* Prepare pointer to args. */
	char *args = strchr(config, '@');
	if (args)
		args++;

	if (starts_with(halse_kerkey_id, p))
		return halse_open_kerkey(args);

	Log2(PCSC_LOG_ERROR, "Unknown SE provider: '%s'!", p);
	return NULL;
}

bool halse_exists(DWORD lun)
{
	size_t i;

	for (i=0; i<MAX_SE_DEVICES; i++) {
		struct lun_se* ls = &lun_se_array[i];
		if (ls->in_use && ls->lun == lun)
			return true;
	}

	return false;
}

struct halse_dev* halse_open(DWORD lun, char* config)
{
	size_t i;

	if (!config)
		return NULL;

	for (i=0; i<MAX_SE_DEVICES; i++) {
		struct lun_se* ls = &lun_se_array[i];
		if (!ls->in_use) {
			ls->in_use = 1;
			ls->lun = lun;
			ls->dev = halse_parse(config);
			return ls->dev;
		}
	}

	return NULL;
}

struct halse_dev* halse_get(DWORD lun)
{
	size_t i;

	for (i=0; i<MAX_SE_DEVICES; i++) {
		struct lun_se* ls = &lun_se_array[i];
		if (ls->in_use && ls->lun == lun)
			return ls->dev;
	}

	return NULL;
}

void halse_free(DWORD lun)
{
	size_t i;

	for (i=0; i<MAX_SE_DEVICES; i++) {
		struct lun_se* ls = &lun_se_array[i];
		if (ls->lun == lun) {
			ls->in_use = 0;
			return;
		}
	}
}

