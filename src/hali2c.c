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

#include <unistd.h>
#include <errno.h>

#include <debuglog.h>

#include "hali2c.h"
#include "helpers.h"
#include "hali2c_kernel.h"

const char* hali2c_kernel_id = "kernel";

static int is_nack(int v)
{
	/*
	 * The Linux kernel I2C userspace API requires drivers to return
	 * -ENXIO on NACK. However, several drivers don't do that, but
	 * return -ETIMEDOUT or -EREMOTEIO. Let's cover all this cases.
	 */
	return (v == -ENXIO || v == -ETIMEDOUT || v == -EREMOTEIO);
}

int hali2c_read_with_retry(struct hali2c_dev* dev,
		unsigned char* buf, size_t len,
		size_t max_attempts, size_t guard_time_us)
{
	size_t counter = 0;

	if (!dev)
		return 0;

	do {
		int ret = dev->read(dev, buf, len);
		if (ret == (int)len) {
			/* Done */
			return 0;
		} else if (is_nack(ret)) {
			ret = usleep(guard_time_us);
			if (ret) {
				Log2(PCSC_LOG_ERROR, "Calling usleep failed: %d", ret);
				return ret;
			}
		} else if (ret < 0) {
			Log2(PCSC_LOG_ERROR, "Reading from I2C device failed: %d", ret);
			return ret;
		} else {
			Log3(PCSC_LOG_ERROR, "Read only %i of %zu bytes", ret, len);
			return ret;
		}
		counter++;
	} while (counter < max_attempts);

	Log1(PCSC_LOG_ERROR, "Read timed out");

	return -ETIMEDOUT;
}

int hali2c_write_with_retry(struct hali2c_dev* dev,
	const unsigned char* buf, size_t len,
	size_t max_attempts, size_t guard_time_us)
{
	size_t counter = 0;

	if (!dev)
		return 0;

	do {
		int ret = dev->write(dev, buf, len);
		if (ret == (int)len) {
			/* Done */
			return 0;
		} else if (is_nack(ret)) {
			ret = usleep(guard_time_us);
			if (ret) {
				Log2(PCSC_LOG_ERROR, "Calling usleep failed: %d", ret);
				return errno;
			}
		} else if (ret < 0) {
			Log2(PCSC_LOG_ERROR, "Writing to I2C device failed: %d", ret);
			return ret;
		} else {
			Log3(PCSC_LOG_ERROR, "Wrote only %i of %zu bytes", ret, len);
			return ret;
		}
		counter++;
	} while (counter < max_attempts);

	Log1(PCSC_LOG_ERROR, "Write timed out");

	return -ETIMEDOUT;
}


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

