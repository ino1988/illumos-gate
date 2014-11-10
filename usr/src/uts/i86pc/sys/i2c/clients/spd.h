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
#ifndef	_SPD_H
#define	_SPD_H

#ifdef  __cplusplus
extern "C" {
#endif

#define	SPD_IOCTL			(((uint32_t)'s') << 24)
#define	SPD_IOCTL_GET_SPD		(SPD_IOCTL | 1)

typedef enum {
	RDIMM = 1,
	UDIMM,
	SODIMM,
	MICRO_DIMM,
	MINI_RDIMM,
	MINI_UDIM,
	MINI_CDIM,
	ECC_SO_RDIMM,
	ECC_SO_UDIM,
	ECC_SO_CDIM,
	LRDIM,
	DIMM_TYPE_COUNT
} dimm_type_t;

#define	PARTNO_SIZE	0x12

typedef struct spd_dimm {
	uint16_t	socket;		/* which socket the dimm is on */
	uint16_t	slot;		/* ... and slot number */
	uint16_t	manu_id;	/* manufacturer Id */
	uint8_t		dram_type;	/* DDR3 etc */
	uint8_t		non_volatile;	/* 1 == NVDIMM */
	dimm_type_t	dimm_type;	/* dimm types */
	uint32_t	size;		/* MByte */
	uint8_t		version;	/* SPD version */
	uint8_t		ranks;
	uint8_t		banks;
	uint8_t		rows;
	uint8_t		columns;
	uint8_t		bits;
	uint16_t	year;		/* year of manufacture */
	uint16_t	week;		/* week of manufacture */
	uint32_t	serial_no;	/* module serial number */
	uint16_t	revision;	/* module revision code */
	uint8_t		ftb_dividend;	/* fine-time-base */
	uint8_t		ftb_divisor;
	uint8_t		mtb_dividend;	/* medium-time-base */
	uint8_t		mtb_divisor;
	uint8_t		min_ctime_mtb;	/* min clock time in mtb */
	int8_t		min_ctime_ftb;	/* ftb correction to min clock time */
	char		part_no[PARTNO_SIZE + 1];
} spd_dimm_t;

#ifdef  __cplusplus
}
#endif

#endif
