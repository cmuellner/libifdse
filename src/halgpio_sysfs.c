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

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <debuglog.h>

#include "helpers.h"
#include "halgpio.h"

struct halgpio_sysfs_dev
{
	/* Embed halgpio device */
	struct halgpio_dev device;
	/* GPIO related state. */
	int gpionum; /* sysfs GPIO number (e.g. 16) */
	int gpio_active_low; /* Reset line is active low */
	int gpio_fd; /* File descriptor to GPIO */
};

/*
 * Parse the information encoded in a string with
 * the following pattern: "[n]<gpionum>"
 */
static int halgpio_sysfs_parse(struct halgpio_sysfs_dev *dev, char* config)
{
	char *p = config;
	char *endptr;

	/* parse an optional 'n' as the active_low indicator */
	if (*p == 'n') {
		dev->gpio_active_low = 1;
		p++; /* advance */
	} else {
		dev->gpio_active_low = 0;
	}
	Log2(PCSC_LOG_DEBUG, "gpio_active_low: %d", dev->gpio_active_low);

	/* parse the gpionum */
	errno = 0;
	dev->gpionum = (int)strtol(p, &endptr, 0);
	if (errno != 0 || p == endptr) {
		Log2(PCSC_LOG_ERROR, "Parser error: invalid GPIO '%s'", p);
		return -1;
	}
	Log2(PCSC_LOG_DEBUG, "gpio: %d", dev->gpionum);

	return 0;
}

static int halgpio_sysfs_open(struct halgpio_sysfs_dev *dev)
{
	int ret;

	/* Prepare export string */
	char export_string[5];
	ret = snprintf(export_string, sizeof(export_string),
		"%d", dev->gpionum);
	if (ret >= (int)sizeof(export_string)) {
		Log1(PCSC_LOG_ERROR, "Could not prepare export string!");
		return -1;
	}

	/* Open export file */
	const char *export_filename = "/sys/class/gpio/export";
	int export_fd = open(export_filename, O_WRONLY);
	if (export_fd == -1) {
		Log2(PCSC_LOG_ERROR, "Could not open export file (%s)",
			strerror(errno));
		return -1;
	}

	/* Export the GPIO */
	ssize_t sret = write(export_fd, export_string, strlen(export_string));
	if (sret == -1 && errno != EBUSY) {
		Log2(PCSC_LOG_ERROR, "Could not write to export file (%s)",
			strerror(errno));
		close(export_fd);
		return -1;
	}

	if (sret == -1 && errno == EBUSY)
		Log1(PCSC_LOG_INFO, "Reset GPIO was already exported");

	/* Close export file */
	close(export_fd);

	/* Prepare gpio active_low filename */
	char active_low_filename[512];
	ret = snprintf(active_low_filename, sizeof(active_low_filename),
		"/sys/class/gpio/gpio%d/active_low", dev->gpionum);
	if (ret >= (int)sizeof(active_low_filename)) {
		Log1(PCSC_LOG_ERROR, "Could not prepare GPIO active_low filename!");
		return -1;
	}

	/* Open active_low file */
	int active_low_fd = open(active_low_filename, O_RDWR);
	if (active_low_fd < 0) {
		Log2(PCSC_LOG_ERROR, "Could not open active_low file (%s)",
			strerror(errno));
		return -1;
	}

	/* Set active_low of GPIO to output */
	const char* active_low_string = "0";
	if (dev->gpio_active_low) {
		active_low_string = "1";
	}
	sret = write(active_low_fd, active_low_string, strlen(active_low_string));
	if (sret == -1) {
		Log2(PCSC_LOG_ERROR, "Could not write to active_low file (%s)",
			strerror(errno));
		close(active_low_fd);
		return -1;
	}

	/* Close the active_low file */
	close(active_low_fd);

	/* Prepare gpio direction filename */
	char direction_filename[512];
	ret = snprintf(direction_filename, sizeof(direction_filename),
		"/sys/class/gpio/gpio%d/direction", dev->gpionum);
	if (ret >= (int)sizeof(direction_filename)) {
		Log1(PCSC_LOG_ERROR, "Could not prepare GPIO direction filename!");
		return -1;
	}

	/* Open direction file */
	int direction_fd = open(direction_filename, O_RDWR);
	if (direction_fd < 0) {
		Log2(PCSC_LOG_ERROR, "Could not open direction file (%s)",
			strerror(errno));
		return -1;
	}

	/* Set direction of GPIO to output */
	const char* direction_string = "out";
	sret = write(direction_fd, direction_string, strlen(direction_string));
	if (sret == -1) {
		Log2(PCSC_LOG_ERROR, "Could not write to direction file (%s)",
			strerror(errno));
		close(direction_fd);
		return -1;
	}

	/* Close the direction file */
	close(direction_fd);

	/* Prepare gpio value filename */
	char value_filename[512];
	ret = snprintf(value_filename, sizeof(value_filename),
		"/sys/class/gpio/gpio%d/value", dev->gpionum);
	if (ret >= (int)sizeof(value_filename)) {
		Log1(PCSC_LOG_ERROR, "Could not prepare GPIO value filename!");
		return -1;
	}

	/* Open the value file */
	dev->gpio_fd = open(value_filename, O_RDWR);
	if (dev->gpio_fd < 0) {
		Log2(PCSC_LOG_ERROR, "Could not open value file (%s)",
			strerror(errno));
		return -1;
	}

	return 0;
}

static int halgpio_sysfs_enable(struct halgpio_dev *device)
{
	struct halgpio_sysfs_dev *dev = container_of(device, struct halgpio_sysfs_dev, device);

	const char* up_string = "1";
	ssize_t sret = write(dev->gpio_fd, up_string, strlen(up_string));
	if (sret == -1) {
		Log2(PCSC_LOG_ERROR, "Could not write to value file (%s)",
			strerror(errno));
		return -1;
	}

	return 0;
}

static int halgpio_sysfs_disable(struct halgpio_dev *device)
{
	struct halgpio_sysfs_dev *dev = container_of(device, struct halgpio_sysfs_dev, device);

	const char* off_string = "0";
	ssize_t sret = write(dev->gpio_fd, off_string, strlen(off_string));
	if (sret == -1) {
		Log2(PCSC_LOG_ERROR, "Could not write to value file (%s)",
			strerror(errno));
		return -1;
	}

	return 0;
}

static void halgpio_sysfs_close(struct halgpio_dev *device)
{
	struct halgpio_sysfs_dev *dev = container_of(device, struct halgpio_sysfs_dev, device);

	if (dev->gpio_fd >= 0) {
		close(dev->gpio_fd);
		dev->gpio_fd = -1;
	}
}

struct halgpio_dev* halgpio_open_sysfs(char* config)
{
	int ret;
	struct halgpio_sysfs_dev *dev;

	if (!config)
		return NULL;

	Log2(PCSC_LOG_DEBUG, "Trying to create device with config: '%s'", config);

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		Log1(PCSC_LOG_ERROR, "Not enough memory!");
		return NULL;
	}

	/* Parse device string from reader.conf */
	ret = halgpio_sysfs_parse(dev, config);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "device string can't be parsed!");
		free(dev);
		return NULL;
	}

	ret = halgpio_sysfs_open(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "device can't be opened!");
		free(dev);
		return NULL;
	}

	dev->device.enable = halgpio_sysfs_enable;
	dev->device.disable = halgpio_sysfs_disable;
	dev->device.close = halgpio_sysfs_close;

	return &dev->device;
}

