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
#ifndef	_IMC_PCU_IMPL_H
#define	_IMC_PCU_IMPL_H

typedef struct pcu {
	dev_info_t	*pcu_dip;
	ddi_acc_handle_t pcu_rhdl;
	uint8_t		*pcu_regs;
	int		hold;
	list_node_t	link;
} pcu_t;

#define	MEM_TRML_ESTIMATION_CFG		0x4c
#define	MEM_TRML_DISABLE_IMC		(1ul << 2)

#endif
