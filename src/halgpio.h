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

#ifndef HALGPIO_H_
#define HALGPIO_H_

#include <stddef.h>
#include <errno.h>

struct halgpio_dev {
	int (*enable)(struct halgpio_dev* device);
	int (*disable)(struct halgpio_dev* device);
	void (*close)(struct halgpio_dev* device);
};

/*
 * Enable the GPIO line.
 *
 * Return 0 on success, or -ve on error.
 */
static inline int halgpio_enable(struct halgpio_dev *dev)
{
	if (!dev)
		return 0;
	if (!dev->enable)
		return -ENODEV;
	return dev->enable(dev);
}

/*
 * Disable the GPIO line.
 *
 * Return 0 on success, or -ve on error.
 */
static inline int halgpio_disable(struct halgpio_dev *dev)
{
	if (!dev)
		return 0;
	if (!dev->disable)
		return -ENODEV;
	return dev->disable(dev);
}

static inline void halgpio_close(struct halgpio_dev *dev)
{
	if (dev && dev->close)
		dev->close(dev);
}

/*
 * Create a new halgpio_dev device based the configuration string.
 * Returns the new object on success, or NULL otherwise.
 */
struct halgpio_dev* halgpio_open(char* config);

#endif /* HALGPIO_H_ */
