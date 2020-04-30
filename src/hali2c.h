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

#ifndef HALI2C_H_
#define HALI2C_H_

#include <stddef.h>
#include <errno.h>

struct hali2c_dev {
	int (*read)(struct hali2c_dev* device, unsigned char* buf, size_t len);
	int (*write)(struct hali2c_dev* device, const unsigned char* buf, size_t len);
	void (*close)(struct hali2c_dev* device);
};

/*
 * Read from I2C device.
 *
 * Return 0 on success, or -ve on error,
 * or n<len if not all bytes have been read.
 */
static inline int hali2c_read(struct hali2c_dev* dev,
	unsigned char* buf, size_t len)
{
	if (!dev)
		return 0;
	if (!dev->read)
		return -ENODEV;
	return dev->read(dev, buf, len);
}

/*
 * Write to I2C device.
 *
 * Return 0 on success, or -ve on error,
 * or n<len if not all bytes have been written.
 */
static inline int hali2c_write(struct hali2c_dev* dev,
	const unsigned char* buf, size_t len)
{
	if (!dev)
		return 0;
	if (!dev->write)
		return -ENODEV;
	return dev->write(dev, buf, len);
}

/*
 * Close the I2C and free all allocated resources.
 */
static inline void hali2c_close(struct hali2c_dev* dev)
{
	if (dev && dev->close)
		dev->close(dev);
}

/*
 * Read with retry on NACK.
 * This will call read up to max_attempts times with a delay
 * of guard_time_us in between.
 *
 * Returns 0 on success, -ETIMEDOUT if timed out, or -ve on error,
 * or n<len if not all bytes have been read.
 */
int hali2c_read_with_retry(struct hali2c_dev* dev,
	unsigned char* buf, size_t len,
	size_t max_attempts, size_t guard_time_us);

/*
 * Write with retry on NACK.
 * This will call write up to max_attempts times with a delay
 * of guard_time_us in between.
 *
 * Returns 0 on success, -ETIMEDOUT if timed out, or -ve on error,
 * or n<len if not all bytes have been written.
 */
int hali2c_write_with_retry(struct hali2c_dev* dev,
	const unsigned char* buf, size_t len,
	size_t max_attempts, size_t guard_time_us);

/*
 * Create a new hali2c_dev device based the configuration string.
 * Returns the new object on success, or NULL otherwise.
 */
struct hali2c_dev* hali2c_open(char* config);

#endif /* HALI2C_H_ */
