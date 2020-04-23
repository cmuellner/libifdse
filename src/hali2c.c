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

#include "hali2c.h"
#include "helpers.h"
#include "hali2c_kernel.h"

const char* hali2c_kernel_id = "kernel";

struct hali2c_dev* hali2c_open(char* config)
{
	if (!config)
		return NULL;

	/* Prepare pointer to args. */
	char *args = strchr(config, ':');
	if (args)
		args++;

	if (starts_with(hali2c_kernel_id, config)) {
		return hali2c_open_kernel(args);
	}

	Log2(PCSC_LOG_ERROR, "Unknown I2C provider: '%s'!", config);
	return NULL;
}

