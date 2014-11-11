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
