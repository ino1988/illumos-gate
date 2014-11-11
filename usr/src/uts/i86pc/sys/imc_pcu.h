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
#ifndef	_IMC_PCU_H
#define	_IMC_PCU_H

typedef enum { ENABLE_CLTT, DISABLE_CLTT } cltt_state_t;

extern int	imc_pcu_set_cltt(dev_info_t *, cltt_state_t, cltt_state_t *);

#endif
