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
#ifndef _IMC_H
#define	_IMC_H

#include <sys/imc_pcu.h>

#define	SEGMENTS		2
#define	ADDR_PER_SEG		8

#define	SMB_TOTAL_TIMEOUT	50000	/* 50 ms */
#define	SMB_ITER_TIMEOUT	250	/* 0.25 ms */
#define	SMB_ITERATIONS		(SMB_TOTAL_TIMEOUT / SMB_ITER_TIMEOUT)

/*
 * Register offsets
 */
#define	SMB_BASE	0x180
#define	SMB_STS(i)	(SMB_BASE + (i) * 0x10)
#define	SMB_CMD(i)	(SMB_BASE + 0x4 + (i) * 0x10)
#define	SMB_CTL(i)	(SMB_BASE + 0x8 + (i) * 0x10)

/*
 * Bit values for SMB_STS (status) register
 */
#define	SMB_STS_RDO	(1ul << 31)
#define	SMB_STS_WOD	(1ul << 30)
#define	SMB_STS_SBE	(1ul << 29)
#define	SMB_STS_BSY	(1ul << 28)
#define	SMB_RDATA(x)	((x) & 0xffff)

/*
 * Bit values for SMB_CTL register
 */
#define	SMB_DTI_MASK		(0xful << 28)
#define	SMB_DTI(x)		(((x) & 0xf) << 28)
#define	SMB_DTI_TSOD		0x3
#define	SMB_DTI_EEPROM		0xa
#define	SMB_DTI_EEPROM_RO	0x6		/* read only */
#define	SMB_TSOD_POLL_EN	(1ul << 8)
#define	SMB_SOFT_RESET		(1ul << 10)
#define	SMB_CKOVRD		(1ul << 27)

/*
 * Bit values for the SMB_CMD register
 */
#define	SMB_CMD_TRIGGER		(1ul << 31)
#define	SMB_CMD_WORD		(1ul << 29)
#define	SMB_CMD_WRITE		(1ul << 27)
#define	SMB_CMD_SLAVE(x)	(((x) & 7) << 24)
#define	SMB_CMD_BA(x)		(((x) & 0xff) << 16)
#define	SMB_WDATA(x)		((x) & 0xffff)

/*
 * slv_info_t contains info that is client device specific
 * and is stored on the child's devinfo parent private data.
 */
typedef struct slv_info {
	uint32_t	slv_segment;	/* bus 0 or 1 */
	uint32_t	slv_address;	/* address of I2C device */
} slv_info_t;

typedef struct imc imc_t;

typedef struct imc_chan {
	imc_t		*chn_imc;	/* back ptr to containing imc struct */
	kmutex_t	chn_mutex;
	kcondvar_t	chn_cv;
	int		chn_busy;	/* channel busy indicator */
	int		chn_seg;	/* segment this channel is for */
	i2c_transfer_t	*chn_cur_tran;
	dev_info_t	*chn_cur_dip;	/* active dip */
	boolean_t	chn_poll_en;
	cltt_state_t	chn_cltt_save;
	uint32_t	chn_dti_save;
} imc_chan_t;

struct imc {
	dev_info_t	*imc_dip;
	ddi_acc_handle_t imc_rhdl;
	char		imc_name[24];
	uint8_t		*imc_regs;
	uint_t		imc_dev;
	uint_t		imc_fn;
	imc_chan_t	imc_channel[2];	/* one per segment */
	dev_info_t	*imc_children[SEGMENTS][ADDR_PER_SEG];
};

#endif /* _IMC_H */
