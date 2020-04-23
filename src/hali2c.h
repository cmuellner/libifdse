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

struct hali2c_dev {
	int (*read)(struct hali2c_dev* device, unsigned char* buf, size_t len);
	int (*write)(struct hali2c_dev* device, const unsigned char* buf, size_t len);
	void (*close)(struct hali2c_dev* device);
};

struct hali2c_dev* hali2c_open(char* config);

#endif /* HALI2C_H_ */
