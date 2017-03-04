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

#ifndef KERKEY_H_
#define KERKEY_H_

#include <stdint.h>

#include "utils.h"

int kerkey_open(struct reader *r, char *device);

int kerkey_close(struct reader *r);

int kerkey_get_atr(struct reader *r, unsigned char *buf, size_t *len);

int kerkey_power_up(struct reader *r);

int kerkey_power_down(struct reader *r);

int kerkey_warm_reset(struct reader *r);

int kerkey_xfer(struct reader *r, unsigned char *tx_buf, size_t tx_len, unsigned char *rx_buf, size_t *rx_len);

#endif /* KERKEY_I2C_H_ */
