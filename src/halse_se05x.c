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
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <debuglog.h>

#include "helpers.h"
#include "hali2c.h"
#include "halgpio.h"
#include "halse.h"

#define SEGT_us 10 /* SE05x guard time between I2C transactions. */
#define MPOT_ms 1 /* Minimum polling time. */
#define BWT_ms 1000 /* Block waiting time. */
#define PWT_ms 5 /* Power-wakeup time. */
#define US_PER_MS 1000

#define SE05X_NAD 0x5A
#define HOST_NAD 0xA5

#define SIZE_PROLOGUE 3
#define SIZE_INF_MAX 254
#define SIZE_EPILOGUE 2

/*
 * I-Block has the form:
 *   0 N(S) M 0 0 0 0 0
 */
#define I_BLOCK 0x00
#define I_BLOCK_MASK 0x80

/*
 * R-Block has the form:
 *   1 0 0 N(R) 0 0 E1 E0
 */
#define R_BLOCK 0x80
#define R_BLOCK_MASK 0xC0

/*
 * S-Block has the form:
 *   1 1 R5 R4 R3 R2 R1 R0
 */
#define S_BLOCK 0xC0
#define S_BLOCK_MASK 0xC0

enum cmd_dir {
	CMD_REQ = 0,
	CMD_RES = 1<<5,
};
#define CMD_REQRES_MASK (1<<5)

enum cmd_type {
	CMD_RESYNC = 0x00, /* Reset sequence number to zero. */
	CMD_SET_IFC = 0x01, /* Set INF field size. */
	CMD_ABORT = 0x02, /* Abort chain. */
	CMD_WTX = 0x03, /* Waiting time extension. */
	CMD_SOFT_RESET = 0x0F, /* Soft reset. */
	CMD_EOA = 0x05, /* End of APDU (enter power-save mode). */
	CMD_RESET = 0x06, /* Chip reset. */
	CMD_ATR = 0x07, /* Get ATR without reset. */
};
#define CMD_TYPE_MASK 0x1F

enum cmd_error {
	EE_NO_ERROR = 0x00,
	EE_CRC_ERROR = 0x01,
	EE_OTHER_ERROR = 0x02,
};
#define CMD_ERROR_MASK 0x03

struct halse_se05x_dev
{
	/* Embed halse device */
	struct halse_dev device;

	/* Embed I2C and GPIO devices */
	struct hali2c_dev *i2c_dev;
	struct halgpio_dev *gpio_dev;

	/* Cached data from the device. */
	unsigned char *atr;
	size_t atr_len;
	size_t timeout_us;
	size_t guard_time_us;
	size_t max_retries;

	/* Transfer state. */
	int n_s;
	int n_r;

	/*
	 * Exchange buffers for blocks.
	 * Note, that we use two buffers here so that we can cache
	 * the last transmit block for retransmission.
	 */
	unsigned char txbuf[SIZE_PROLOGUE + SIZE_INF_MAX + SIZE_EPILOGUE];
	size_t txlen;
	bool txretransmit;
	unsigned char rxbuf[SIZE_PROLOGUE + SIZE_INF_MAX + SIZE_EPILOGUE];
};

static int halse_se05x_recv_block(struct halse_se05x_dev *dev, size_t *len);
static int halse_se05x_power_up(struct halse_dev *device);
static int halse_se05x_power_down(struct halse_dev *device);
static void halse_se05x_close(struct halse_dev *device);
static int halse_se05x_warm_reset(struct halse_dev *device);

static inline int halse_se05x_read_i2c(struct halse_se05x_dev *dev, unsigned char *buf, size_t len)
{
	/*
	 * We need to wait between two I2C transactions.
	 * As this guard time is so short, we simply do that always.
	 */
	usleep(dev->guard_time_us);

	return hali2c_read_with_retry(dev->i2c_dev, buf, len, dev->max_retries, dev->timeout_us);
}

static inline int halse_se05x_write_i2c(struct halse_se05x_dev *dev, const unsigned char *buf, size_t len)
{
	/*
	 * We need to wait between two I2C transactions.
	 * As this guard time is so short, we simply do that always.
	 */
	usleep(dev->guard_time_us);

	return hali2c_write_with_retry(dev->i2c_dev, buf, len, dev->max_retries, dev->timeout_us);
}

static inline int is_i_block(uint8_t pcb)
{
	return ((pcb & I_BLOCK_MASK) == I_BLOCK);
}

static inline int is_r_block(uint8_t pcb)
{
	return ((pcb & R_BLOCK_MASK) == R_BLOCK);
}

static inline int is_r_block_with_error(uint8_t pcb)
{
	return (is_r_block(pcb) && (pcb & CMD_ERROR_MASK));
}

static inline int is_s_block(uint8_t pcb)
{
	return ((pcb & S_BLOCK_MASK) == S_BLOCK);
}

static inline int is_s_block_request(uint8_t pcb)
{
	return (is_s_block(pcb) && (pcb & CMD_REQRES_MASK) == CMD_REQ);
}

static inline int is_s_block_response(uint8_t pcb)
{
	return (is_s_block(pcb) && (pcb & CMD_REQRES_MASK) == CMD_RES);
}

static inline void halse_se05x_clear_state(struct halse_se05x_dev *dev)
{
	/* Reset the sequence numbers. */
	dev->n_s = 0;
	dev->n_r = 0;
}

static inline void halse_se05x_clear_buf(struct halse_se05x_dev *dev)
{
	/* Clear all data in the tx and rx buffers. */
	memset(dev->txbuf, 0, sizeof(dev->txbuf));
	dev->txlen = 0;
	dev->txretransmit = false;
	memset(dev->rxbuf, 0, sizeof(dev->rxbuf));
}

/*
 * CRC16 algorithmus for T=1 blocks.
 */
static uint16_t halse_se05x_calculate_crc(unsigned char* buf, size_t len)
{
	uint16_t crc = 0xFFFF;

	for (size_t i = 0; i < len; i++) {
		crc ^= buf[i];
		for (size_t b = 8; b > 0; --b) {
			if ((crc & 0x0001) == 0x0001) {
				crc = (uint16_t)((crc >> 1) ^ 0x8408);
			} else {
				crc >>= 1;
			}
		}
	}

	crc ^=0xFFFF;
	return swap_uint16(crc);
}

/*
 * TCK (checksum) algorithmus for ISO 7816 ATR.
 */
static uint8_t halse_se05x_calculate_xor(unsigned char* buf, size_t len)
{
	uint8_t v = 0;

	for (size_t i = 0; i < len; i++) {
		v ^= buf[i];
	}

	return v;
}

/*
 * Calculate and append the CRC and send the block.
 */
static int halse_se05x_crc_and_send(struct halse_se05x_dev *dev, size_t len)
{
	/* Calculate and append CRC */
	uint16_t crc = halse_se05x_calculate_crc(dev->txbuf, len);
	dev->txbuf[len] = crc >> 8;
	dev->txbuf[len+1] = crc & 0xff;
	dev->txlen = len + SIZE_EPILOGUE;

	/* Send block */
	return halse_se05x_write_i2c(dev, dev->txbuf, dev->txlen);
}

/*
 * Retransmit the block, if retransmit limit is not exhausted.
 */
static int halse_se05x_resend(struct halse_se05x_dev *dev)
{
	if (dev->txretransmit == true)
		return -ETIMEDOUT;

	dev->txretransmit = true;

	/* Simply re-send */
	return halse_se05x_write_i2c(dev, dev->txbuf, dev->txlen);
}

/*
 * Prepare prologue, copy data and call halse_se05x_crc_and_send().
 */
static int halse_se05x_send_s_block(struct halse_se05x_dev *dev,
		enum cmd_dir d, enum cmd_type t,
		unsigned char* buf, size_t len)
{
	if (len > SIZE_INF_MAX) {
		Log2(PCSC_LOG_ERROR, "Trying to send too much data bytes: %zu", len);
		return -1;
	}

	/* Prepare block prologue. */
	dev->txbuf[0] = SE05X_NAD; /* NAD */
	dev->txbuf[1] = S_BLOCK | d | t; /* PCB */
	dev->txbuf[2] = len; /* LEN */

	/* Copy over payload. */
	memmove(&dev->txbuf[3], buf, len);

	/* Ship it. */
	return halse_se05x_crc_and_send(dev, SIZE_PROLOGUE + len);
}

/*
 * Prepare prologue and call halse_se05x_crc_and_send().
 */
static int halse_se05x_send_s_block_noinf(struct halse_se05x_dev *dev,
		enum cmd_dir d, enum cmd_type t)
{
	return halse_se05x_send_s_block(dev, d, t, NULL, 0);
}

/*
 * Send the given data to the SE.
 * If chain is set, then consume the R-block.
 */
static int halse_se05x_send_i_block(struct halse_se05x_dev *dev,
		unsigned char* buf, size_t len, bool chain)
{
	int ret;

	if (len > SIZE_INF_MAX) {
		Log2(PCSC_LOG_ERROR, "Trying to send too much data bytes: %zu", len);
		return -1;
	}

	/* Prepare block prologue. */
	int ns_field = dev->n_s ? (1<<6) : 0;
	int chain_field = chain ? (1<<5) : 0;
	dev->txbuf[0] = SE05X_NAD; /* NAD */
	dev->txbuf[1] = I_BLOCK | ns_field | chain_field; /* PCB */
	dev->txbuf[2] = len; /* LEN */

	/* Update internal state */
	dev->n_s ^= 1;

	/* Copy over payload. */
	memcpy(&dev->txbuf[3], buf, len);

	/* Ship it. */
	ret = halse_se05x_crc_and_send(dev, SIZE_PROLOGUE + len);
	if (ret) {
		Log2(PCSC_LOG_ERROR, "Sending block failed: %d", ret);
		return -1;
	}

	if (chain) {
		/* In case of chaining, let's consume the token passing. */
		ret = halse_se05x_recv_block(dev, &len);
		if (ret) {
			Log2(PCSC_LOG_ERROR, "Receiving block failed: %d", ret);
			return ret;
		}

		uint8_t pcb = dev->rxbuf[1];
		if (!is_r_block(pcb)) {
			Log2(PCSC_LOG_ERROR, "Received block is not R-block (PCB: 0x%hhx)", pcb);
			return -1;
		}

		uint8_t ee = pcb & CMD_ERROR_MASK;
		if (ee) {
			Log2(PCSC_LOG_ERROR, "Received R-block with error (0x%hhx)", ee);
			return -1;
		}

		uint8_t n_r = (pcb>>4) & 0x01;
		if (n_r != dev->n_s) {
			Log2(PCSC_LOG_ERROR, "Received R-block with wrong N(R) (0x%hhx)", n_r);
			return -1;
		}
	}

	return 0;
}

/*
 * Send an R-block with the given parameters.
 */
static int halse_se05x_send_r_block(struct halse_se05x_dev *dev,
		uint8_t n_r, uint8_t ee)
{
	/* Prepare block prologue. */
	int nr_field = n_r<<4;
	dev->txbuf[0] = SE05X_NAD; /* NAD */
	dev->txbuf[1] = R_BLOCK | nr_field | ee; /* PCB */
	dev->txbuf[2] = 0; /* LEN */

	/* Ship it. */
	return halse_se05x_crc_and_send(dev, SIZE_PROLOGUE);
}

/*
 * Read a block from the SE05x.
 * This function transparently handles WTX requests and
 * does CRC checking.
 *
 * @dev Device to read from.
 * @len Location where the length of the INF field will be stored.
 *
 * @return 0 on success, or -ve on error.
 */
static int halse_se05x_recv_block(struct halse_se05x_dev *dev, size_t *len)
{
	int ret;

	ret = halse_se05x_read_i2c(dev, dev->rxbuf, SIZE_PROLOGUE + SIZE_EPILOGUE);
	if (ret) {
		Log2(PCSC_LOG_ERROR, "Read from I2C failed: %d", ret);
		return -1;
	}

	if (dev->rxbuf[2] > SIZE_INF_MAX) {
		Log3(PCSC_LOG_ERROR, "Invalid LEN received: (%d > %d)", dev->rxbuf[2], SIZE_INF_MAX);
		return -1;
	}

	*len = dev->rxbuf[2];
	if (*len) {
		size_t off = SIZE_PROLOGUE + SIZE_EPILOGUE;
		ret = halse_se05x_read_i2c(dev, dev->rxbuf + off, *len);
		if (ret) {
			Log2(PCSC_LOG_ERROR, "Read from I2C failed: %d", ret);
			return -1;
		}
	}

	if (dev->rxbuf[0] != HOST_NAD) {
		Log2(PCSC_LOG_ERROR, "Invalid NAD received: 0x%hx", dev->rxbuf[0]);
	}

	uint16_t exp_crc = halse_se05x_calculate_crc(dev->rxbuf, SIZE_PROLOGUE + *len);
	uint16_t act_crc = dev->rxbuf[SIZE_PROLOGUE + *len];
	act_crc <<= 8;
	act_crc |= dev->rxbuf[SIZE_PROLOGUE + *len + 1];

	/* Check for CRC errors. */
	if (exp_crc != act_crc) {
		Log3(PCSC_LOG_ERROR, "act_crc (0x%hx) != exp_crc (0x%hx)", act_crc, exp_crc);
		return -1;
	}

	uint8_t pcb = dev->rxbuf[1];
	/* Check if we got an S-Block with a request. */
	if (is_s_block_request(pcb)) {
		switch (pcb & CMD_TYPE_MASK) {
			case CMD_WTX:
				Log1(PCSC_LOG_ERROR, "Received WTX");

				/* Got a waiting time extension, let's ack that. */
				ret = halse_se05x_send_s_block(dev, CMD_RES, CMD_WTX, &dev->rxbuf[3], 1);
				if (ret) {
					Log2(PCSC_LOG_ERROR, "Sending WTX response failed: %d", ret);
					return -1;
				}
				/* Let's do a tail call. */
				return halse_se05x_recv_block(dev, len);
			default:
				Log2(PCSC_LOG_ERROR, "Received unsupported command: 0x%hhx", pcb);
				return -1;
		}
	}

	/* Check if we got an error */
	if (is_r_block_with_error(pcb)) {
		Log2(PCSC_LOG_ERROR, "Received R-block with error (PCB: 0x%hhx) -> retransmit", pcb);
		ret = halse_se05x_resend(dev);
		if (ret) {
			Log2(PCSC_LOG_ERROR, "Retransmit failed: %d", ret);
			return ret;
		}
		/* Let's do a tail call. */
		return halse_se05x_recv_block(dev, len);
	}

	return 0;
}

/*
 * Do a warm reset to the SE (via CMD_SOFT_RESET).
 * After the reset the ATR will be cached.
 */
static int halse_se05x_warm_reset_dev(struct halse_se05x_dev *dev)
{
	int ret;

	ret = halse_se05x_send_s_block_noinf(dev, CMD_REQ, CMD_SOFT_RESET);
	if (ret) {
		Log2(PCSC_LOG_ERROR, "Sending SOFT_RESET command failed: %d", ret);
		return -1;
	}

	size_t len;
	ret = halse_se05x_recv_block(dev, &len);
	if (ret) {
		Log2(PCSC_LOG_ERROR, "Receiving response block failed: %d", ret);
		return -1;
	}

	/* Sanity checks. */
	if (dev->rxbuf[1] != (S_BLOCK | CMD_RES | CMD_SOFT_RESET)) {
		Log2(PCSC_LOG_ERROR, "Receiving unexpected PCB: 0x%hx", dev->rxbuf[1]);
		return -1;
	}

	free(dev->atr);
	dev->atr = malloc(len);
	memcpy(dev->atr, &dev->rxbuf[3], len);
	dev->atr_len = len;

	return 0;
}

/*
 * Do a reset to the SE (via CMD_RESET).
 */
static int halse_se05x_hard_reset_dev(struct halse_se05x_dev *dev)
{
	int ret;

	ret = halse_se05x_send_s_block_noinf(dev, CMD_REQ, CMD_RESET);
	if (ret) {
		Log2(PCSC_LOG_ERROR, "Sending RESET command failed: %d", ret);
		return -1;
	}

	size_t len;
	ret = halse_se05x_recv_block(dev, &len);
	if (ret) {
		Log2(PCSC_LOG_ERROR, "Receiving response block failed: %d", ret);
		return -1;
	}

	/* Sanity checks. */
	if (dev->rxbuf[1] != (S_BLOCK | CMD_RES | CMD_RESET)) {
		Log2(PCSC_LOG_ERROR, "Receiving unexpected PCB: 0x%hx", dev->rxbuf[1]);
		return -1;
	}

	return 0;
}

/*
 * Parse the information encoded in a string with
 * the pattern "i2c:...[@gpio:...]".
 */
static int halse_se05x_parse(struct halse_se05x_dev *dev, char* config)
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
			halgpio_close(dev->gpio_dev);
			dev->gpio_dev = NULL;
		}

		Log1(PCSC_LOG_ERROR, "Missing I2C device!");
		return -1;
	}

	return 0;
}

static int halse_se05x_open(struct halse_dev *dev)
{
	int ret;

	ret = halse_se05x_power_down(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not power down SE05x!");
		halse_se05x_close(dev);
		return -1;
	}

	ret = usleep(PWT_ms * US_PER_MS);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
		halse_se05x_close(dev);
		return -1;
	}

	ret = halse_se05x_power_up(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not power up SE05x!");
		halse_se05x_close(dev);
		return -1;
	}


	/* Get SE05x's ATR */
	ret = halse_se05x_warm_reset(dev);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Could not get ATR from SE05x!");
		halse_se05x_close(dev);
		return -1;
	}

	return 0;
}

static void halse_se05x_close(struct halse_dev *device)
{
	struct halse_se05x_dev *dev = container_of(device, struct halse_se05x_dev, device);
	hali2c_close(dev->i2c_dev);
	dev->i2c_dev = NULL;
	halgpio_close(dev->gpio_dev);
	dev->gpio_dev = NULL;
}

/*
 * Get the ATR of the SE.
 */
static int halse_se05x_get_atr(struct halse_dev* device, unsigned char *buf, size_t *len)
{
	struct halse_se05x_dev *dev = container_of(device, struct halse_se05x_dev, device);
	(void) dev;

	/*
	 * The SE05x has a non-standard ATR (see UM11225),
	 * that is longer than the maximum allowed ATR length
	 * according to ISO7816-3 (which is 32 bytes).
	 *
	 * This function will most likely be called with a buffer of
	 * size 32 bytes (obvious as the standard guarantees that
	 * size to be sufficient). So we need to make somethink up.
	 *
	 * Let's use a fixed artificial ATR with the actual
	 * real historical bytes.
	 */

	Log1(PCSC_LOG_INFO, "SE05x has non-conforming ATR, need to adjust.");
	LogXxd(PCSC_LOG_INFO, "Real ATR from SE05x: ", dev->atr, dev->atr_len);

	/* Let's create an artifical ATR prologue... */
	const unsigned char atr_prologue[] = {
		0x3B, /* TS = 3B --> Direct Convention */
		0xF0, /* T0 = F0, Y(1): 1111, K: 0 (historical bytes) */
		0x96, /* TA(1) = 96 --> Fi=512, Di=32, 16 cycles/ETU */
		      /* 250000 bits/s at 4 MHz, fMax for Fi = 5 MHz => 312500 bits/s */
		0x00, /* TB(1) = 00 --> VPP is not electrically connected */
		0x00, /* TC(1) = 00 --> Extra guard time: 0 */
		0x80, /* TD(1) = 80 --> Y(i+1) = 1000, Protocol T = 0 */
		0x11, /* TD(2) = 11 --> Y(i+1) = 0001, Protocol T = 1 */
		0xFE, /* TA(3) = FE --> IFSC: 254 */
	};

	/*
	 * The SE05x ATR looks as follows:
	 * - PVER(1)
	 * - VID(5)
	 * - DLLP_LEN(1)
	 * - DLLP(DLLP_LEN)
	 * - PLID (1)
	 * - PLP_LEN (1)
	 * - PLP(PLP_LEN)
	 * - HB_LEN (1)
	 * - HB(HB_LEN)
	 */

	size_t offset_hb = 0;
	size_t len_hb;
	offset_hb += 1 + 5; /* PVER, VID */
	offset_hb += 1 + dev->atr[offset_hb]; /* DLLP_LEN + DLLP */
	offset_hb += 1; /* PLID */
	offset_hb += 1 + dev->atr[offset_hb]; /* PLP_LEN + PLP */
	len_hb = dev->atr[offset_hb];
	offset_hb += 1; /* HB_LEN */

	/* Sanity check (HB can't be longer than 15) */
	if (len_hb > 15) {
		Log2(PCSC_LOG_ERROR, "ATR's HB have %zu characters, but only 15 are allowed!", len_hb);
		return -1;
	}

	/* Compose our ATR */
	size_t i = 0;
	memcpy(buf + i, atr_prologue, sizeof(atr_prologue));
	i += sizeof(atr_prologue);
	buf[1] |= len_hb; /* ATR len fixup (K in T0) */
	memcpy(buf + i, &dev->atr[offset_hb], len_hb);
	i += len_hb;
	buf[i] = halse_se05x_calculate_xor(&buf[1], i-1); /* TCK */
	i++;
	*len = i;

	return 0;
}

static int halse_se05x_power_up(struct halse_dev *device)
{
	int ret;
	struct halse_se05x_dev *dev = container_of(device, struct halse_se05x_dev, device);

	if (dev->gpio_dev) {
		ret = halgpio_enable(dev->gpio_dev);
		if (ret) {
			Log2(PCSC_LOG_ERROR, "Enabling SE05x failed: %d", ret);
			return ret;
		}
	} else {
		ret = halse_se05x_hard_reset_dev(dev);
		if (ret) {
			Log2(PCSC_LOG_ERROR, "Reset of SE05x failed: %d", ret);
			return ret;
		}
	}

	halse_se05x_clear_state(dev);

	ret = usleep(PWT_ms * US_PER_MS);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "Calling usleep failed!");
		return -1;
	}

	return 0;
}

static int halse_se05x_power_down(struct halse_dev *device)
{
	struct halse_se05x_dev *dev = container_of(device, struct halse_se05x_dev, device);
	return halgpio_disable(dev->gpio_dev);
}

static int halse_se05x_warm_reset(struct halse_dev *device)
{
	struct halse_se05x_dev *dev = container_of(device, struct halse_se05x_dev, device);
	halse_se05x_clear_state(dev);
	return halse_se05x_warm_reset_dev(dev);
}

static int halse_se05x_xfer(struct halse_dev *device, unsigned char *tx_buf, size_t tx_len, unsigned char *rx_buf, size_t *rx_len)
{
	int ret = 0;
	struct halse_se05x_dev *dev = container_of(device, struct halse_se05x_dev, device);
	size_t tx_off = 0;
	size_t rx_off = 0;
	bool chain;

	//LogXxd(PCSC_LOG_INFO, "tx_buf: ", tx_buf, tx_len);

	/*
	 * This is an unspecified delay.
	 *
	 * Under high-load scenarios it was observed, that
	 * certain devices get into a state, in which they
	 * respond with EE_OTHER_ERROR and only a reset can
	 * get them out of this state.
	 * This delay reliably helped to address this issue.
	 */
	usleep(1 * US_PER_MS);

	/* Sanity checks */
	if (!tx_buf || !tx_len || !rx_buf || !rx_len) {
		ret = -1;
		goto end;
	}

	/* Write loop */
	do {
		size_t len = SIZE_INF_MAX;
		size_t left = tx_len - tx_off;
		if (len > left) {
			len = left;
		}

		if ((left - len) > 0) {
			chain = true;
		} else {
			chain = false;
		}

		ret = halse_se05x_send_i_block(dev, tx_buf + tx_off, len, chain);
		if (ret) {
			Log2(PCSC_LOG_ERROR, "Sending I-block failed: %d", ret);
			goto end;
		}

		tx_off += len;
	} while (chain);

	/* Read loop */
	do {
		size_t len;
		ret = halse_se05x_recv_block(dev, &len);
		if (ret) {
			Log2(PCSC_LOG_ERROR, "Receiving block failed: %d", ret);
			goto end;
		}

		uint8_t pcb = dev->rxbuf[1];
		if (!is_i_block(pcb)) {
			Log2(PCSC_LOG_ERROR, "Received block is not I-block (PCB: 0x%hhx)", pcb);
			goto end;
		}

		if ((rx_off + len) > *rx_len) {
			Log3(PCSC_LOG_ERROR, "Receive buffer too small (buffer size: %zu, data size: %zu) -> Truncating",
					*rx_len, rx_off + len);
			len = *rx_len - rx_off;
		}

		memcpy(rx_buf + rx_off, &dev->rxbuf[3], len);
		rx_off += len;

		chain = (pcb >> 5) & 0x01;
		if (chain) {
			uint8_t ee = 0;
			int n_s = (pcb >> 6) & 1;
			uint8_t n_r = n_s ^ 1;
			ret = halse_se05x_send_r_block(dev, n_r, ee);
			if (ret) {
				Log2(PCSC_LOG_ERROR, "Sending R-block failed: %d", ret);
				goto end;
			}
		}
	} while(chain);

	*rx_len = rx_off;

	//LogXxd(PCSC_LOG_INFO, "rx_buf: ", rx_buf, *rx_len);

end:
	halse_se05x_clear_buf(dev);
	return ret;
}

struct halse_dev* halse_open_se05x(char* config)
{
	int ret;
	struct halse_se05x_dev *dev;

	if (!config)
		return NULL;

	Log2(PCSC_LOG_DEBUG, "Trying to create device with config: '%s'", config);

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		Log1(PCSC_LOG_ERROR, "Not enough memory!");
		return NULL;
	}

	/* Parse device string from reader.conf */
	ret = halse_se05x_parse(dev, config);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "device string can't be parsed!");
		free(dev);
		return NULL;
	}

	/* Initialial se05x timeout */
	dev->timeout_us = MPOT_ms * US_PER_MS;
	dev->guard_time_us = SEGT_us;
	dev->max_retries = BWT_ms * US_PER_MS / dev->timeout_us;

	ret = halse_se05x_open(&dev->device);
	if (ret) {
		Log1(PCSC_LOG_ERROR, "device can't be opened!");
		free(dev);
		return NULL;
	}

	dev->device.close = halse_se05x_close;
	dev->device.get_atr = halse_se05x_get_atr;
	dev->device.power_up = halse_se05x_power_up;
	dev->device.power_down = halse_se05x_power_down;
	dev->device.warm_reset = halse_se05x_warm_reset;
	dev->device.xfer = halse_se05x_xfer;

	return &dev->device;
}

