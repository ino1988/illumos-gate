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
