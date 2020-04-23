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

#ifndef HALSE_H_
#define HALSE_H_

#include <stddef.h>
#include <stdbool.h>
#include <wintypes.h>

#define MAX_SE_DEVICES 16

struct halse_dev {
	void (*close)(struct halse_dev* dev);
	int (*get_atr)(struct halse_dev* dev, unsigned char *buf, size_t *len);
	int (*power_up)(struct halse_dev *device);
	int (*power_down)(struct halse_dev *device);
	int (*warm_reset)(struct halse_dev *device);
	int (*xfer)(struct halse_dev *device, unsigned char *tx_buf, size_t tx_len, unsigned char *rx_buf, size_t *rx_len);
};

/* Check if SE with given lun exists */
bool halse_exists(DWORD lun);

/* Creates a new SE with given lun and config */
struct halse_dev* halse_open(DWORD lun, char* config);

/* Gets (existing) SE with the given lun. */
struct halse_dev* halse_get(DWORD lun);

/* Free the lun */
void halse_free(DWORD lun);

#endif /* HALSE_H_ */
