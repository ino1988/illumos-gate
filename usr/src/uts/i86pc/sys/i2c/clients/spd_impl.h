/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
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
