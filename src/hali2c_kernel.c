/*
 * Copyright (C) 2017 - 2020 Christoph Muellner
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <debuglog.h>
#include <linux/i2c-dev.h>

#include "helpers.h"
#include "hali2c.h"

struct hali2c_kernel_dev
{
	/* Embed halgpio device */
	struct hali2c_dev device;
	/* I2C related state. */
	char *i2c_device; /* I2C device (e.g. "/dev/i2c-0") */
	int i2c_addr; /* I2C slave addr (e.g. 0x20) */
	int i2c_fd; /* File descriptor to I2C device */
};

/*
 * Parse the information encoded in a string with
 * the following pattern: "<i2c_device>:<i2c_addr>"
 */
static int hali2c_kernel_parse(struct hali2c_kernel_dev *dev, char* config)
{
	char *p = config;
	char *endptr;

	/* Advance p after the first ':' in
	 * the pattern "<i2c_device>:<i2c_addr>" */
	p = strchr(p, ':');
	if (!p) {
		Log2(PCSC_LOG_ERROR, "No I2C slave address defined in '%s'", config);
		return -1;
	}
	dev->i2c_device = strndup(config, p - config);
	Log2(PCSC_LOG_DEBUG, "i2c_device: %s", dev->i2c_device);
	p++;

	/* Parse i2c_addr from the pattern "<i2c_addr>" */
	errno = 0;
	dev->i2c_addr = (int)strtol(p, &endptr, 0);
	if (errno != 0 || p == endptr) {
		Log2(PCSC_LOG_ERROR, "Parser error: invalid I2C address in '%s'", p);
		return -1;
	}

	Log2(PCSC_LOG_DEBUG, "i2c_addr: %d", dev->i2c_addr);

	return 0;
}

static int hali2c_kernel_open(struct hali2c_kernel_dev *dev)
{
	/* Open I2C device */
	dev->i2c_fd = open(dev->i2c_device, O_RDWR);
	if (dev->i2c_fd == -1) {
		Log3(PCSC_LOG_ERROR, "Could not open I2C device %s (%s)",
			dev->i2c_device, strerror(errno));
		return -1;
	}

	Log3(PCSC_LOG_DEBUG, "I2C fd (%s): %d", dev->i2c_device, dev->i2c_fd);

	/* Set the slave address */
	if (ioctl(dev->i2c_fd, I2C_SLAVE, dev->i2c_addr) < 0) {
		Log3(PCSC_LOG_ERROR, "Could not set I2C address %d (%s)",
			dev->i2c_addr, strerror(errno));
		close(dev->i2c_fd);
		dev->i2c_fd = -1;
		return -1;
	}

	return 0;
}

static int hali2c_kernel_read(struct hali2c_dev* device, unsigned char* buf, size_t len)
{
	struct hali2c_kernel_dev *dev = container_of(device, struct hali2c_kernel_dev, device);

	ssize_t sret = read(dev->i2c_fd, buf, len);
	if (sret < 0)
		return -errno;

	return (int)sret;
}

static int hali2c_kernel_write(struct hali2c_dev* device, const unsigned char* buf, size_t len)
{
	struct hali2c_kernel_dev *dev = container_of(device, struct hali2c_kernel_dev, device);

	ssize_t sret = write(dev->i2c_fd, buf, len);
	if (sret < 0)
		return -errno;

	return (int)sret;
}

void hali2c_kernel_close(struct hali2c_dev* device)
{
	struct hali2c_kernel_dev *dev = container_of(device, struct hali2c_kernel_dev, device);

	if (dev->i2c_fd >= 0) {
		close(dev->i2c_fd);
		dev->i2c_fd = -1;
	}
}

struct hali2c_dev* hali2c_open_kernel(char* config)
{
	int ret;
	struct hali2c_kernel_dev *dev;

	if (!config)
		return NULL;

	Log2(PCSC_LOG_DEBUG, "Trying to create device with config: '%s'", config);

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		Log1(PCSC_LOG_ERROR, "Not enough memory!");
		return NULL;
	}

	/* Parse device string from reader.conf */
	ret = hali2c_kernel_parse(dev, config);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "device string can't be parsed!");
		free(dev);
		return NULL;
	}

	ret = hali2c_kernel_open(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "device can't be opened!");
		free(dev);
		return NULL;
	}

	dev->device.read = hali2c_kernel_read;
	dev->device.write = hali2c_kernel_write;
	dev->device.close = hali2c_kernel_close;

	return &dev->device;
}

