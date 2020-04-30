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
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <debuglog.h>

#include "helpers.h"
#include "hali2c.h"
#include "halgpio.h"
#include "halse.h"

#define KERKEY_CMD_TIMEOUT 0x75
#define KERKEY_CMD_ATR 0x76

#define I2C_FRAME_LENGTH_MAX 254

#define GUARD_TIME_US 1000

struct halse_kerkey_dev
{
	/* Embed halse device */
	struct halse_dev device;

	/* Embed I2C and GPIO devices */
	struct hali2c_dev *i2c_dev;
	struct halgpio_dev *gpio_dev;

	/* Cached data from the device. */
	unsigned char *atr;
	size_t atr_len;
	size_t timeout_ms;
};

static inline int halse_kerkey_read_i2c(struct halse_kerkey_dev *dev, unsigned char *buf, size_t len)
{
	return hali2c_read_with_retry(dev->i2c_dev, buf, len, dev->timeout_ms, GUARD_TIME_US);
}

static inline int halse_kerkey_write_i2c(struct halse_kerkey_dev *dev, const unsigned char *buf, size_t len)
{
	return hali2c_write_with_retry(dev->i2c_dev, buf, len, dev->timeout_ms, GUARD_TIME_US);
}

static int halse_kerkey_get_timeout(struct halse_kerkey_dev *dev)
{
	const unsigned char cmd = KERKEY_CMD_TIMEOUT;

	int ret = halse_kerkey_write_i2c(dev, &cmd, 1);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Failed to write command");
		return -1;
	}

	unsigned char res[2];
read_res:
	ret = halse_kerkey_read_i2c(dev, res, 2);
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

	ret = halse_kerkey_read_i2c(dev, res, 2);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Reading ATR failed!");
		return -1;
	}

	dev->timeout_ms = (res[0] << 8) | res[1];

	Log2(PCSC_LOG_DEBUG, "Set card timeout to: %zu", dev->timeout_ms);

	return 0;
}

static int halse_kerkey_warm_reset_dev(struct halse_kerkey_dev *dev)
{
	const unsigned char cmd = KERKEY_CMD_ATR;
	int ret = halse_kerkey_write_i2c(dev, &cmd, 1);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Failed to write command");
		return -1;
	}

	unsigned char res[2];
	ret = halse_kerkey_read_i2c(dev, res, 2);
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

	ret = halse_kerkey_read_i2c(dev, dev->atr, rlen);
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


/*
 * Parse the information encoded in a string with
 * the pattern "i2c:...[@gpio:...]".
 */
static int halse_kerkey_parse(struct halse_kerkey_dev *dev, char* config)
{
	char *p = config;
	const char delimiter[] = "@";
	char *saveptr;

	/* Get first token */
	p = strtok_r(config, delimiter, &saveptr);
	while (p != NULL) {
		if (starts_with("i2c:", p)) {
			p = strchr(p, ':');
			p++;
			dev->i2c_dev = hali2c_open(p);
			if (!dev->i2c_dev) {
				Log2(PCSC_LOG_ERROR, "Failed to parse I2C configuration: '%s'", p);
				return -1;
			}
		} else if (starts_with("gpio:", p)) {
			p = strchr(p, ':');
			p++;
			dev->gpio_dev = halgpio_open(p);
			if (!dev->gpio_dev) {
				Log2(PCSC_LOG_ERROR, "Failed to parse GPIO configuration: '%s'", p);
				return -1;
			}
		} else {
			Log2(PCSC_LOG_ERROR, "Invalid token in config string: '%s'", p);
			return -1;
		}

		/* Get next token. */
		p = strtok_r(NULL, delimiter, &saveptr);
	}

	if (!dev->i2c_dev) {
		if (dev->gpio_dev) {
			dev->gpio_dev->close(dev->gpio_dev);
			dev->gpio_dev = NULL;
		}

		Log1(PCSC_LOG_ERROR, "Missing I2C device!");
		return -1;
	}

	return 0;
}

static int halse_kerkey_open(struct halse_kerkey_dev *dev)
{
	int ret;

	ret = halgpio_disable(dev->gpio_dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not power down Kerkey!");
		return -1;
	}

	ret = usleep(200*1000);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
		return -1;
	}

	ret = halgpio_enable(dev->gpio_dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not power up Kerkey!");
		return -1;
	}

	ret = usleep(200*1000);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
		return -1;
	}

	/* Get kerkey's ATR */
	ret = halse_kerkey_warm_reset_dev(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not reset Kerkey!");
		hali2c_close(dev->i2c_dev);
		halgpio_close(dev->gpio_dev);
		return -1;
	}

	ret = halse_kerkey_get_timeout(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not get timeout!");
		hali2c_close(dev->i2c_dev);
		halgpio_close(dev->gpio_dev);
		return -1;
	}

	return 0;
}

static void halse_kerkey_close(struct halse_dev *device)
{
	struct halse_kerkey_dev *dev = container_of(device, struct halse_kerkey_dev, device);
	hali2c_close(dev->i2c_dev);
	halgpio_close(dev->gpio_dev);
}

static int halse_kerkey_get_atr(struct halse_dev* device, unsigned char *buf, size_t *len)
{
	struct halse_kerkey_dev *dev = container_of(device, struct halse_kerkey_dev, device);

	if (*len < dev->atr_len) {
		Log1(PCSC_LOG_ERROR, "Buffer size too small!");
		return -1;
	}

	memcpy(buf, dev->atr, dev->atr_len);
	*len = dev->atr_len;

	return 0;
}

static int halse_kerkey_power_up(struct halse_dev *device)
{
	struct halse_kerkey_dev *dev = container_of(device, struct halse_kerkey_dev, device);
	int ret = halgpio_enable(dev->gpio_dev);

	usleep(200*1000);

	return ret;
}

static int halse_kerkey_power_down(struct halse_dev *device)
{
	struct halse_kerkey_dev *dev = container_of(device, struct halse_kerkey_dev, device);
	return halgpio_disable(dev->gpio_dev);
}

static int halse_kerkey_warm_reset(struct halse_dev *device)
{
	struct halse_kerkey_dev *dev = container_of(device, struct halse_kerkey_dev, device);
	return halse_kerkey_warm_reset_dev(dev);
}

static int halse_kerkey_xfer(struct halse_dev *device, unsigned char *tx_buf, size_t tx_len, unsigned char *rx_buf, size_t *rx_len)
{
	struct halse_kerkey_dev *dev = container_of(device, struct halse_kerkey_dev, device);
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

	ret = halse_kerkey_write_i2c(dev, tx_buf + tx_off, len);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Writing data failed!");
		return -1;
	}

	tx_off += len;
	tx_len -= len;

read_res:
	ret = halse_kerkey_read_i2c(dev, res, 2);
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

	ret = halse_kerkey_read_i2c(dev, rx_buf + rx_off, rlen);
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

struct halse_dev* halse_open_kerkey(char* config)
{
	int ret;
	struct halse_kerkey_dev *dev;

	if (!config)
		return NULL;

	Log2(PCSC_LOG_DEBUG, "Trying to create device with config: '%s'", config);

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		Log1(PCSC_LOG_ERROR, "Not enough memory!");
		return NULL;
	}

	/* Parse device string from reader.conf */
	ret = halse_kerkey_parse(dev, config);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "device string can't be parsed!");
		free(dev);
		return NULL;
	}

	/* Initialial kerkey timeout */
	dev->timeout_ms = 10000;

	ret = halse_kerkey_open(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "device can't be opened!");
		free(dev);
		return NULL;
	}

	dev->device.close = halse_kerkey_close;
	dev->device.get_atr = halse_kerkey_get_atr;
	dev->device.power_up = halse_kerkey_power_up;
	dev->device.power_down = halse_kerkey_power_down;
	dev->device.warm_reset = halse_kerkey_warm_reset;
	dev->device.xfer = halse_kerkey_xfer;

	return &dev->device;
}

