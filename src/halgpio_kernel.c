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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <debuglog.h>

#include "ext/gpio.h"
#include "helpers.h"
#include "halgpio.h"

struct halgpio_kernel_dev
{
	/* Embed halgpio device */
	struct halgpio_dev device;
	/* GPIO related state. */
	int gpiochip; /* GPIO chip of reset line (e.g. 0) */
	int gpioline; /* GPIO of reset line (e.g. 16) */
	int gpio_active_low; /* Reset line is active low */
	int gpio_fd; /* File descriptor to GPIO */
};

/*
 * Parse the information encoded in a string with
 * the following pattern: "<gpiochip>:<[n]gpioline>"
 */
static int halgpio_kernel_parse(struct halgpio_kernel_dev *dev, char* config)
{
	char *p = config;
	char *endptr;

	/* parse the gpiochip */
	errno = 0;
	dev->gpiochip = (int)strtol(p, &endptr, 0);
	if (errno != 0 || p == endptr) {
		Log2(PCSC_LOG_ERROR, "Parser error: invalid GPIOCHIP in '%s'", p);
		return -1;
	}

	Log2(PCSC_LOG_DEBUG, "gpiochip: %d", dev->gpiochip);

	/* advance p after the first ':' in
	 * the pattern "<gpiochip>:<[n]gpioline>" */
	p = strchr(p, ':');
	if (!p)
		return -1;
	p++;

	/* parse an optional 'n' as the active_low indicator */
	if (*p == 'n') {
		dev->gpio_active_low = 1;
		p++; /* advance */
	} else {
		dev->gpio_active_low = 0;
	}
	Log2(PCSC_LOG_DEBUG, "gpio_active_low: %d", dev->gpio_active_low);

	/* parse reset_pin from the pattern "<gpioline>" */
	errno = 0;
	dev->gpioline = (size_t)strtol(p, &endptr, 0);
	if (errno != 0 || p == endptr) {
		Log2(PCSC_LOG_ERROR, "Parser error: invalid GPIO line in '%s'", p);
		return -1;
	}
	Log2(PCSC_LOG_DEBUG, "gpioline: %d", dev->gpioline);

	return 0;
}

static int halgpio_kernel_open(struct halgpio_kernel_dev *dev)
{
	int ret = 0;
	char *chrdev_name;
	struct gpiohandle_request req;
	int fd;

	ret = asprintf(&chrdev_name, "/dev/gpiochip%d", dev->gpiochip);
	if (ret < 0)
		return -ENOMEM;

	fd = open(chrdev_name, 0);
	if (fd == -1) {
		Log3(PCSC_LOG_ERROR, "Could not open GPIO chip file %s (%s)",
			chrdev_name, strerror(errno));
		ret = -1;
		goto err;
	}

	req.lineoffsets[0] = dev->gpioline;
	req.flags = GPIOHANDLE_REQUEST_OUTPUT;
	strcpy(req.consumer_label, "libifdse");
	req.lines = 1;
	req.default_values[0] = 0;

	if (dev->gpio_active_low)
		req.flags |= GPIOHANDLE_REQUEST_ACTIVE_LOW;

	ret = ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req);
	if (ret) {
		Log2(PCSC_LOG_ERROR, "Could not get GPIO lines (%s)",
			strerror(errno));
		goto end;
	}

	dev->gpio_fd = req.fd;
	if (dev->gpio_fd == -1) {
		Log3(PCSC_LOG_ERROR, "Could not open GPIO file %s (%s)",
			chrdev_name, strerror(errno));
		ret = -1;
		goto err;
	}

err:
	close(dev->gpio_fd);
	dev->gpio_fd = -1;
end:
	close(fd);
	free(chrdev_name);

	return ret;
}

static int halgpio_kernel_enable(struct halgpio_dev *device)
{
	struct halgpio_kernel_dev *dev = container_of(device, struct halgpio_kernel_dev, device);

	if (dev->gpio_fd == -1)
		return 0;

	struct gpiohandle_data data;
	data.values[0] = 1;

	int ret = ioctl(dev->gpio_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
	if (ret == -1) {
		Log2(PCSC_LOG_ERROR, "Could not set GPIO value (%s)",
			strerror(errno));
		return -1;
	}

	return 0;
}

static int halgpio_kernel_disable(struct halgpio_dev *device)
{
	struct halgpio_kernel_dev *dev = container_of(device, struct halgpio_kernel_dev, device);

	if (dev->gpio_fd == -1)
		return 0;

	struct gpiohandle_data data;
	data.values[0] = 0;

	int ret = ioctl(dev->gpio_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
	if (ret == -1) {
		Log2(PCSC_LOG_ERROR, "Could not set GPIO value (%s)",
			strerror(errno));
		return -1;
	}

	return 0;
}

static void halgpio_kernel_close(struct halgpio_dev *device)
{
	struct halgpio_kernel_dev *dev = container_of(device, struct halgpio_kernel_dev, device);

	if (dev->gpio_fd >= 0) {
		close(dev->gpio_fd);
		dev->gpio_fd = -1;
	}
}

struct halgpio_dev* halgpio_open_kernel(char* config)
{
	int ret;
	struct halgpio_kernel_dev *dev;

	if (!config)
		return NULL;

	Log2(PCSC_LOG_DEBUG, "Trying to create device with config: '%s'", config);

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		Log1(PCSC_LOG_ERROR, "Not enough memory!");
		return NULL;
	}

	/* Parse device string from reader.conf */
	ret = halgpio_kernel_parse(dev, config);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "device string can't be parsed!");
		free(dev);
		return NULL;
	}

	ret = halgpio_kernel_open(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "device can't be opened!");
		free(dev);
		return NULL;
	}

	dev->device.enable = halgpio_kernel_enable;
	dev->device.disable = halgpio_kernel_disable;
	dev->device.close = halgpio_kernel_close;

	return &dev->device;
}

