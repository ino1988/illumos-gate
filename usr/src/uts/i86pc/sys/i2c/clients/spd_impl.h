/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 *
 * Copyright (c) 2014, Tegile Systems, Inc. All rights reserved.
 */
#ifndef	_SPD_IMPL_H
#define	_SPD_IMPL_H

#include <sys/list.h>
#include <sys/i2c/clients/spd.h>

typedef struct sdram sdram_t;

#define	SPD_EEPROM_SIZE		256

struct sdram {
	dev_info_t	*dip;
	i2c_client_hdl_t i2c_hdl;
	int		open_flags;
	int		open_cnt;
	kmutex_t	spd_lock;
	kcondvar_t	spd_cv;
	boolean_t	spd_busy;
	void		*pvt;
	list_node_t	link;
	spd_dimm_t	spd_info;	/* extracted from spd_data */
	int		spd_size;
	uint8_t		spd_data[SPD_EEPROM_SIZE];
};

#define	SPD_SIZE		0x00
#define	SPD_REVISION		0x01
#define	SPD_MEMORY_TYPE		0x02
#define	SPD_DDR3_SDRAM		11
#define	SPD_MODULE_TYPE		0x03
#define	SPD_BANKS		0x04
#define	SPD_ADDRS		0x05
#define	SPD_MOD_ORG		0x07
#define	SPD_BUS_WIDTH		0x08
#define	SPD_FTB			0x09
#define	SPD_MTB_DIVIDEND	0x0a
#define	SPD_MTB_DIVISOR		0x0b
#define	SPD_MIN_CTIME_MTB	0x0c
#define	SPD_MIN_CTIME_FTB	0x22

#define	SPD_MMID_LSB		0x75
#define	SPD_MMID_MSB		0x76
#define	SPD_YEAR		0x78
#define	SPD_WEEKS		0x79
#define	SPD_SERIAL_NUM		0x7a
#define	SPD_PART_NO		0x80
#define	SPD_MOD_REV		0x92

#endif
