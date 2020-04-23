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

#include <debuglog.h>

#include "halgpio.h"
#include "helpers.h"
#include "halgpio_kernel.h"
#include "halgpio_sysfs.h"

const char* halgpio_kernel_id = "kernel";
const char* halgpio_sysfs_id = "sysfs";

struct halgpio_dev* halgpio_open(char* config)
{
	if (!config)
		return NULL;

	/* Prepare pointer to args. */
	char *args = strchr(config, ':');
	if (args)
		args++;

	if (starts_with(halgpio_kernel_id, config))
		return halgpio_open_kernel(args);
	else if (starts_with(halgpio_sysfs_id, config))
		return halgpio_open_sysfs(args);

	Log2(PCSC_LOG_ERROR, "Unknown GPIO provider: '%s'!", config);
	return NULL;
}

