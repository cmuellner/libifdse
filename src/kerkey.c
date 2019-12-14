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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <linux/i2c-dev.h>
#include "ext/gpio.h"

#include <debuglog.h>

#include "utils.h"
#include "kerkey.h"

#define KERKEY_CMD_TIMEOUT 0x75
#define KERKEY_CMD_ATR 0x76

#define I2C_FRAME_LENGTH_MAX 254

struct kerkey_dev
{
	/* parsed data */
	char *i2c_device; /* I2C device (e.g. "/dev/i2c-0") */
	int i2c_addr; /* I2C slave addr (e.g. 0x20) */
	int gpiochip; /* GPIO chip of reset line (e.g. 0) */
	int gpioline; /* GPIO of reset line (e.g. 16) */
	int gpioline_active_low; /* Reset line is active low */
	int i2c_fd; /* File descriptor to I2C device */
	int gpio_fd; /* File descriptor to GPIO */
	unsigned char *atr;
	size_t atr_len;
	size_t timeout_ms;
};

/*
 * Parse the information encoded in a string with the following pattern:
 * 	"<i2c_device>:<i2c_addr>:<gpiochip>:<[n]gpioline>:<name>"
 *
 * Note, that all provided strings are allocated on the heap
 * and need to be freed later.
 */
static int parse_device_string(char *device, char **i2c_device, int *i2c_addr,
	int *gpiochip, int *gpioline,
	int *gpioline_active_low)
{
	char *p = device;

	/* Advance p after the first ':' in
	 * the pattern "<i2c_device>:<i2c_addr>:<gpiochip>:<[n]gpioline>" */
	p = strchr(p, ':');
	if (!p) {
		Log2(PCSC_LOG_ERROR, "No I2C slave address defined in '%s'", device);
		return -1;
	}
	*i2c_device = strndup(device, p - device);
	Log2(PCSC_LOG_DEBUG, "i2c_device: %s", *i2c_device);
	p++;

	/* parse i2c_addr from the pattern "<i2c_addr>:<gpiochip>:<[n]gpioline>" */
	*i2c_addr = (int)strtol(p, NULL, 0);
	Log2(PCSC_LOG_DEBUG, "i2c_addr: %d", *i2c_addr);

	/* Advance p after the first ':' in
	 * the pattern "<i2c_addr>:<gpiochip>:<[n]gpioline>" */
	p = strchr(p, ':');
	if (!p) {
		Log2(PCSC_LOG_INFO, "No reset pin defined for '%s'", device);
		*gpiochip = -1;
		*gpioline = -1;
		*gpioline_active_low = -1;
		return 0;
	}
	p++;

	/* parse the gpiochip from the pattern "<gpiochip>:<[n]gpioline>" */
	*gpiochip = (int)strtol(p, NULL, 0);
	Log2(PCSC_LOG_DEBUG, "gpiochip: %d", *gpiochip);

	/* Advance p after the first ':' in
	 * the pattern "<gpiochip>:<[n]gpioline>" */
	p = strchr(p, ':');
	if (!p) {
		free(*i2c_device);
		*i2c_device = NULL;
		Log2(PCSC_LOG_ERROR, "No reset pin defined in '%s'", device);
		return -1;
	}
	p++;

	/* parse an optional 'n' as the active_low indicator */
	if (*p == 'n') {
		*gpioline_active_low = 1;
		p++; /* advance */
	} else {
		*gpioline_active_low = 0;
	}
	Log2(PCSC_LOG_DEBUG, "gpioline_active_low: %d", *gpioline_active_low);

	/* parse reset_pin from the pattern "<gpioline>" */
	*gpioline = (size_t)strtol(p, NULL, 0);
	Log2(PCSC_LOG_DEBUG, "gpioline: %d", *gpioline);

	return 0;
}

static int kerkey_read_i2c(struct kerkey_dev *dev, unsigned char *buf, size_t len)
{
	const size_t max_attempts = dev->timeout_ms;
	size_t counter = 0;
	do {
		ssize_t sret = read(dev->i2c_fd, buf, len);
		if (sret == (ssize_t)len) {
			/* Done */
			return 0;
		} else if (sret == -1 && (errno == ENXIO || errno == ETIMEDOUT || errno == EREMOTEIO)) {
			/* Kerkey not ready yet, let's wait 1 ms */
			int ret = usleep(1000);
			if (ret) {
				Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
				return -1;
			}
		} else if (sret == -1) {
			Log2(PCSC_LOG_ERROR, "Reading from I2C device failed (%s)",
				strerror(errno));
			return -1;
		} else {
			Log3(PCSC_LOG_ERROR, "Read only %zi of %zu bytes", sret, len);
			return -1;
		}
		counter++;
	} while (counter < max_attempts);

	Log1(PCSC_LOG_ERROR, "Read timed out");
	return -1;
}

static int kerkey_write_i2c(struct kerkey_dev *dev, const unsigned char *buf, size_t len)
{
	const size_t max_attempts = dev->timeout_ms;
	size_t counter = 0;
	do {
		ssize_t sret = write(dev->i2c_fd, buf, len);
		if (sret == (ssize_t)len) {
			/* Done */
			return 0;
		} else if (sret == -1 && (errno == ENXIO || errno == ETIMEDOUT || errno == EREMOTEIO)) {
			/* Kerkey not ready yet, let's wait 1 ms */
			int ret = usleep(1000);
			if (ret) {
				Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
				return -1;
			}
		} else if (sret == -1) {
			Log2(PCSC_LOG_ERROR, "Writing to I2C device failed (%s)",
				strerror(errno));
			return -1;
		} else {
			Log3(PCSC_LOG_ERROR, "Wrote only %zi of %zu bytes", sret, len);
			return -1;
		}
		counter++;
	} while (counter < max_attempts);

	Log1(PCSC_LOG_ERROR, "Write timed out");
	return -1;
}

static int open_kerkey_i2c(struct kerkey_dev *dev)
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

static void close_kerkey_i2c(struct kerkey_dev *dev)
{
	if (dev->i2c_fd >= 0) {
		close(dev->i2c_fd);
		dev->i2c_fd = -1;
	}
}

static int kerkey_power_up_gpio(struct kerkey_dev *dev)
{
	int ret;
	struct gpiohandle_data data;

	if (dev->gpio_fd == -1)
		return 0;

	data.values[0] = 1;

	ret = ioctl(dev->gpio_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
	if (ret == -1) {
		Log2(PCSC_LOG_ERROR, "Could not set GPIO value (%s)",
			strerror(errno));
		return -1;
	}
	return 0;
}

static int kerkey_power_down_gpio(struct kerkey_dev *dev)
{
	int ret;
	struct gpiohandle_data data;

	if (dev->gpio_fd == -1)
		return 0;

	data.values[0] = 0;

	ret = ioctl(dev->gpio_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
	if (ret == -1) {
		Log2(PCSC_LOG_ERROR, "Could not set GPIO value (%s)",
			strerror(errno));
		return -1;
	}
	return 0;
}

static int open_kerkey_gpio(struct kerkey_dev *dev)
{
	int ret = 0;
	char *chrdev_name;
	struct gpiohandle_request req;
	int fd;

	if (dev->gpiochip == -1) {
		dev->gpio_fd = -1;
		return 0;
	}

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
	strcpy(req.consumer_label, "libifdkerkey");
	req.lines = 1;
	req.default_values[0] = 0;

	if (dev->gpioline_active_low)
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

	ret = kerkey_power_down_gpio(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not power down Kerkey!");
		goto err;
	}

	ret = usleep(200*1000);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
		goto err;
	}

	ret = kerkey_power_up_gpio(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not power up Kerkey!");
		goto err;
	}

	ret = usleep(200*1000);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
		goto err;
	}

	goto end;

err:
	close(dev->gpio_fd);
	dev->gpio_fd = -1;
end:
	close(fd);
	free(chrdev_name);

	return ret;
}

static void close_kerkey_gpio(struct kerkey_dev *dev)
{
	if (dev->gpio_fd >= 0) {
		close(dev->gpio_fd);
		dev->gpio_fd = -1;
	}
}

static int open_kerkey_dev(struct kerkey_dev *dev)
{
	int ret;

	/* Initialize GPIO */
	ret = open_kerkey_gpio(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not open GPIO!");
		close_kerkey_i2c(dev);
		return -1;
	}

	/* Initialize I2C */
	ret = open_kerkey_i2c(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not open I2C!");
		return -1;
	}

	return 0;
}

static void close_kerkey_dev(struct kerkey_dev *dev)
{
	close_kerkey_i2c(dev);
	close_kerkey_gpio(dev);
}

static int kerkey_get_timeout_dev(struct kerkey_dev *dev)
{
	const unsigned char cmd = KERKEY_CMD_TIMEOUT;
	int ret = kerkey_write_i2c(dev, &cmd, 1);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Failed to write command");
		return -1;
	}

	unsigned char res[2];
read_res:
	ret = kerkey_read_i2c(dev, res, 2);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Reading response failed!");
		return -1;
	}

	bool chain = (res[0] & 0x80) ? 1 : 0;
	short rlen = ((res[0] << 8) | res[1]) & 0x00ff;

	if (!chain && rlen == 0) {
		Log1(PCSC_LOG_DEBUG, "Received WTX");
		ret = usleep(1000);
		if (ret) {
			Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
			return -1;
		}
		goto read_res;
	}

	if (chain || rlen != 2) {
		Log1(PCSC_LOG_ERROR, "Could not get timeout");
		return -1;
	}

	ret = kerkey_read_i2c(dev, res, 2);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Reading ATR failed!");
		return -1;
	}

	dev->timeout_ms = (res[0] << 8) | res[1];

	Log2(PCSC_LOG_DEBUG, "Set card timeout to: %zu", dev->timeout_ms);

	return 0;
}

static int kerkey_warm_reset_dev(struct kerkey_dev *dev)
{
	const unsigned char cmd = KERKEY_CMD_ATR;
	int ret = kerkey_write_i2c(dev, &cmd, 1);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Failed to write command");
		return -1;
	}

	unsigned char res[2];
	ret = kerkey_read_i2c(dev, res, 2);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Reading response failed!");
		return -1;
	}

	bool chain = (res[0] & 0x80) ? 1 : 0;
	short rlen = ((res[0] << 8) | res[1]) & 0x00ff;

	if (chain || rlen == 0) {
		Log1(PCSC_LOG_ERROR, "Could not trigger warm reset!");
		return -1;
	}

	free(dev->atr);
	dev->atr = malloc(rlen);
	if (!dev->atr) {
		Log1(PCSC_LOG_ERROR, "Could not allocate ATR buffer!");
		return -1;
	}

	dev->atr_len = rlen;

	ret = kerkey_read_i2c(dev, dev->atr, rlen);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Reading ATR failed!");
		return -1;
	}

	/* CMD_ATR triggers a warm reset, which takes some time */
	ret = usleep(200*1000);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
		return -1;
	}

	return 0;
}

int kerkey_open(struct reader *r, char *device)
{
	char *i2c_device = NULL;
	int i2c_addr;
	int gpiochip;
	int gpioline;
	int gpioline_active_low;
	struct kerkey_dev *dev;

	Log2(PCSC_LOG_DEBUG, "device: %s", device);

	dev = malloc(sizeof(*dev));
	if (!dev) {
		Log1(PCSC_LOG_ERROR, "Not enough memory!");
		return -1;
	}

	/* Parse device string from reader.conf */
	int ret = parse_device_string(device, &i2c_device, &i2c_addr, &gpiochip,
			&gpioline, &gpioline_active_low);
	if (ret) {
		Log2(PCSC_LOG_ERROR, "device string can't be parsed: %s", device);
		goto fail;
	}

	/* Initialize I2C device */
	dev->i2c_device = i2c_device;
	dev->i2c_addr = i2c_addr;
	dev->gpiochip = gpiochip;
	dev->gpioline = gpioline;
	dev->gpioline_active_low = gpioline_active_low;
	dev->i2c_fd = -1;
	dev->gpio_fd = -1;
	dev->atr = NULL;
	dev->atr_len = 0;
	dev->timeout_ms = 10000;

	ret = open_kerkey_dev(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not open kerkey!");
		goto fail;
	}

	/* Get kerkey's ATR */
	ret = kerkey_warm_reset_dev(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not reset Kerkey!");
		close_kerkey_dev(dev);
		goto fail;
	}

	ret = kerkey_get_timeout_dev(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not get timeout!");
		close_kerkey_dev(dev);
		goto fail;
	}

	/* Store kerkey dev in reader's private data */
	set_reader_prv(r, dev);

	return 0;
fail:
	free(i2c_device);
	free(dev);
	return -1;
}

int kerkey_close(struct reader *r)
{
	struct kerkey_dev *dev = get_reader_prv(r);

	close_kerkey_dev(dev);

	free(dev->i2c_device);
	free(dev);

	return 0;
}

int kerkey_get_atr(struct reader *r, unsigned char *buf, size_t *len)
{
	struct kerkey_dev *dev = get_reader_prv(r);

	if (*len < dev->atr_len) {
		Log1(PCSC_LOG_ERROR, "Buffer size too small!");
		return -1;
	}

	memcpy(buf, dev->atr, dev->atr_len);
	*len = dev->atr_len;
	return 0;
}

int kerkey_power_up(struct reader *r)
{
	struct kerkey_dev *dev = get_reader_prv(r);
	int ret = kerkey_power_up_gpio(dev);
	usleep(200*1000);
	return ret;
}

int kerkey_power_down(struct reader *r)
{
	struct kerkey_dev *dev = get_reader_prv(r);
	return kerkey_power_down_gpio(dev);
}

int kerkey_warm_reset(struct reader *r)
{
	struct kerkey_dev *dev = get_reader_prv(r);
	return kerkey_warm_reset_dev(dev);
}

int kerkey_xfer(struct reader *r, unsigned char *tx_buf, size_t tx_len, unsigned char *rx_buf, size_t *rx_len)
{
	struct kerkey_dev *dev = get_reader_prv(r);
	size_t tx_off = 0;
	size_t rx_off = 0;
	size_t rx_buf_len = *rx_len;
	size_t len;
	int ret;
	unsigned char res[2];

	*rx_len = 0;

send:
	Log2(PCSC_LOG_DEBUG, "tx_len: %zu", tx_len);

	len = tx_len > I2C_FRAME_LENGTH_MAX ? I2C_FRAME_LENGTH_MAX : tx_len;

	ret = kerkey_write_i2c(dev, tx_buf + tx_off, len);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Writing data failed!");
		return -1;
	}

	tx_off += len;
	tx_len -= len;

read_res:
	ret = kerkey_read_i2c(dev, res, 2);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Reading response failed!");
		return -1;
	}

	int chain = (res[0] & 0x80) ? 1 : 0;
	short rlen = ((res[0] << 8) | res[1]) & 0x00ff;

	if (!chain && rlen == 0) {
		Log1(PCSC_LOG_DEBUG, "Received WTX");
		ret = usleep(1000);
		if (ret) {
			Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
			return -1;
		}
		goto read_res;
	}

	if (chain && rlen == 0x00) {
		if (tx_len != 0)
			goto send;
		else {
			Log1(PCSC_LOG_ERROR, "Communication error!");
			return -1;
		}
	}

	if (rx_off + rlen > rx_buf_len) {
		Log1(PCSC_LOG_ERROR, "Receive buffer too small!");
		return -1;
	}

	ret = kerkey_read_i2c(dev, rx_buf + rx_off, rlen);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Reading data failed!");
		return -1;
	}

	rx_off += rlen;
	*rx_len += rlen;

	if (chain)
		goto read_res;

	return 0;
}
