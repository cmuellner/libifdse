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

#include <debuglog.h>

#include "utils.h"
#include "kerkey.h"

#define KERKEY_CMD_TIMEOUT 0x75
#define KERKEY_CMD_ATR 0x76

struct kerkey_dev
{
	/* parsed data */
	char *i2c_device; /* I2C device (e.g. "/dev/i2c-0") */
	int i2c_addr; /* I2C slave addr (e.g. 0x20) */
	int reset_gpio; /* GPIO of reset line (e.g. 16) */
	char *reader_name; /* I2C device name (e.g. "Kerkey") */
	int i2c_fd; /* File descriptor to I2C device */
	int gpio_fd; /* File descriptor to GPIO */
	unsigned char *atr;
	size_t atr_len;
	size_t timeout_ms;
};

static int validate_reader_name(char *reader_name)
{
	if (strcmp(reader_name, "Kerkey") == 0)
		return 0;
	return -1;
}

/*
 * Note, that all provided strings are allocated on the heap
 * and need to be freed later.
 */
static int parse_device_string(char *device, char **i2c_device, int *i2c_addr,
	int *reset_gpio, char **reader_name)
{
	char *p = device;

	/* Advance p after the first ':' in
	 * the pattern "<i2c_device>:<i2c_addr>:<reset_pin>:<name>" */
	p = strchr(p, ':');
	if (!p) {
		Log2(PCSC_LOG_ERROR, "No I2C slave address defined in '%s'", device);
		return -1;
	}
	*i2c_device = strndup(device, p - device);
	Log2(PCSC_LOG_DEBUG, "i2c_device: %s", *i2c_device);
	p++;

	/* parse i2c_addr from the pattern "<i2c_addr>:<reset_pin>:<name>" */
	*i2c_addr = (int)strtol(p, NULL, 0);
	Log2(PCSC_LOG_DEBUG, "i2c_addr: %d", *i2c_addr);

	/* Advance p after the first ':' in
	 * the pattern "<i2c_addr>:<reset_pin>:<name>" */
	p = strchr(p, ':');
	if (!p) {
		free(*i2c_device);
		*i2c_device = NULL;
		Log2(PCSC_LOG_ERROR, "No reset pin defined in '%s'", device);
		return -1;
	}
	p++;

	/* parse reset_pin from the pattern "<reset_pin>:<name>" */
	*reset_gpio = (size_t)strtol(p, NULL, 0);
	Log2(PCSC_LOG_DEBUG, "reset_gpio: %d", *reset_gpio);

	/* Advance p after the first ':' in
	 * the pattern "<reset_pin>:<name>" */
	p = strchr(p, ':');
	if (!p) {
		*reader_name = strdup("Kerkey");
	} else {
		p++;
		*reader_name = strdup(p);
	}
	Log2(PCSC_LOG_DEBUG, "reader_name: %s", *reader_name);

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
		} else if (sret == -1 && errno == ENXIO) {
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
			Log1(PCSC_LOG_ERROR, "Could not read length from I2C device");
			return -1;
		}
		counter++;
	} while(counter < max_attempts);

	Log1(PCSC_LOG_ERROR, "Read timed out");
	return -1;
}

static int kerkey_write_i2c(struct kerkey_dev *dev, const unsigned char *buf, size_t len)
{
	ssize_t sret = write(dev->i2c_fd, buf, len);
	if (sret == -1) {
		Log2(PCSC_LOG_ERROR, "Writing to I2C device failed (%s)",
			strerror(errno));
		return -1;
	} else if (sret != (ssize_t)len) {
		Log3(PCSC_LOG_ERROR, "Wrote only %zi of %zu bytes", sret, len);
		return -1;
	}
	return 0;
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
	const char* up_string = "1";
	ssize_t sret = write(dev->gpio_fd, up_string, strlen(up_string));
	if (sret == -1) {
		Log2(PCSC_LOG_ERROR, "Could not write to value file (%s)",
			strerror(errno));
		return -1;
	}
	return 0;
}

static int kerkey_power_down_gpio(struct kerkey_dev *dev)
{
	const char* off_string = "0";
	ssize_t sret = write(dev->gpio_fd, off_string, strlen(off_string));
	if (sret == -1) {
		Log2(PCSC_LOG_ERROR, "Could not write to value file (%s)",
			strerror(errno));
		return -1;
	}
	return 0;
}

static int open_kerkey_gpio(struct kerkey_dev *dev)
{
	/* Prepare gpio direction filename */
	char direction_filename[512];
	int ret = snprintf(direction_filename, sizeof(direction_filename),
		"/sys/class/gpio/gpio%d/direction", dev->reset_gpio);
	if (ret >= (int)sizeof(direction_filename)) {
		Log1(PCSC_LOG_ERROR, "Could not prepare GPIO direction filename!");
		return -1;
	}

	/* Prepare gpio value filename */
	char value_filename[512];
	ret = snprintf(value_filename, sizeof(value_filename),
		"/sys/class/gpio/gpio%d/value", dev->reset_gpio);
	if (ret >= (int)sizeof(value_filename)) {
		Log1(PCSC_LOG_ERROR, "Could not prepare GPIO value filename!");
		return -1;
	}

	/* Prepare export string */
	char export_string[5];
	ret = snprintf(export_string, sizeof(export_string),
		"%d", dev->reset_gpio);
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

	/* Open the value file */
	dev->gpio_fd = open(value_filename, O_RDWR);
	if (dev->gpio_fd < 0) {
		Log2(PCSC_LOG_ERROR, "Could not open value file (%s)",
			strerror(errno));
		return -1;
	}

	ret = kerkey_power_down_gpio(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not power down Kerkey!");
		close(dev->gpio_fd);
		dev->gpio_fd = -1;
		return -1;
	}

	ret = usleep(200*1000);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
		close(dev->gpio_fd);
		dev->gpio_fd = -1;
		return -1;
	}

	ret = kerkey_power_up_gpio(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not power up Kerkey!");
		close(dev->gpio_fd);
		dev->gpio_fd = -1;
		return -1;
	}

	ret = usleep(200*1000);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
		close(dev->gpio_fd);
		dev->gpio_fd = -1;
		return -1;
	}

	return 0;
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

	/* Initialize I2C */
	ret = open_kerkey_i2c(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not open I2C!");
		return -1;
	}

	/* Initialize GPIO */
	ret = open_kerkey_gpio(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not open GPIO!");
		close_kerkey_i2c(dev);
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
	ret = kerkey_read_i2c(dev, res, 2);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Reading response failed!");
		return -1;
	}

	bool chain = (res[0] & 0x80) ? 1 : 0;
	short rlen = ((res[0] << 8) | res[1]) & 0x00ff;

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
	int reset_gpio;
	char *reader_name = NULL;
	struct kerkey_dev *dev;

	Log2(PCSC_LOG_DEBUG, "device: %s", device);

	dev = malloc(sizeof(*dev));
	if (!dev) {
		Log1(PCSC_LOG_ERROR, "Not enough memory!");
		return -1;
	}

	/* Parse device string from reader.conf */
	int ret = parse_device_string(device, &i2c_device, &i2c_addr,
			&reset_gpio, &reader_name);
	if (ret) {
		Log2(PCSC_LOG_ERROR, "device string can't be parsed: %s", device);
		goto fail;
	}

	ret = validate_reader_name(reader_name);
	if (ret) {
		Log2(PCSC_LOG_ERROR, "Reader name not supported: %s", reader_name);
		goto fail;
	}

	/* Initialize I2C device */
	dev->i2c_device = i2c_device;
	dev->i2c_addr = i2c_addr;
	dev->reset_gpio = reset_gpio;
	dev->reader_name = reader_name;
	dev->atr = NULL;
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
	free(reader_name);
	free(dev);
	return -1;
}

int kerkey_close(struct reader *r)
{
	struct kerkey_dev *dev = get_reader_prv(r);

	close_kerkey_dev(dev);

	free(dev->i2c_device);
	free(dev->reader_name);
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
	return kerkey_power_up_gpio(dev);
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

	Log2(PCSC_LOG_DEBUG, "tx_len: %zu", tx_len);

	*rx_len = 0;

send:
	len = tx_len > 254 ? 254 : tx_len;

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
		/* waiting time extension */
		ret = usleep(1000);
		if (ret) {
			Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
			return -1;
		}
		goto read_res;
	}

	if (tx_len != 0) {
		if (!chain || rlen != 0) {
			Log1(PCSC_LOG_ERROR, "Communication error!");
			return -1;
		}
		goto send;
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

	*rx_len += rlen;

	if (chain)
		goto read_res;

	return 0;
}
