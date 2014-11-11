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
#include <sys/types.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/modctl.h>
#include <sys/sysmacros.h>
#include <sys/list.h>
#include <sys/sdt.h>
#include <sys/imc_pcu.h>

#include "imc_pcu_impl.h"

static void	*pcu_state;

static kmutex_t	pcu_lock;
static list_t	pcu_list;

static ddi_device_acc_attr_t pcu_attr = {
	DDI_DEVICE_ATTR_V1,
	DDI_STRUCTURE_LE_ACC,
	DDI_STRICTORDER_ACC,
	DDI_DEFAULT_ACC
};

/*
 * Find and return the pcu which has the same parent
 */
static pcu_t *
find_pcu(dev_info_t *dip)
{
	dev_info_t	*pdip = ddi_get_parent(dip);
	pcu_t		*pcu;

	mutex_enter(&pcu_lock);
	for (pcu = list_head(&pcu_list); pcu; pcu = list_next(&pcu_list, pcu)) {
		if (ddi_get_parent(pcu->pcu_dip) == pdip) {
			pcu->hold++;
			break;
		}
	}
	mutex_exit(&pcu_lock);

	return (pcu);
}

static void
release_pcu(pcu_t *pcu)
{
	mutex_enter(&pcu_lock);
	pcu->hold--;
	mutex_exit(&pcu_lock);
}

/*
 * Enable or disable closed-loop thermal throttling.
 * CLTT uses the same i2c device as that used by the IMC to get DIMM
 * SPD data.
 * It is disabled before each i2c transaction by the IMC and re-enabled
 * when complete.
 */
int
imc_pcu_set_cltt(dev_info_t *dip, cltt_state_t cltt, cltt_state_t *old)
{
	pcu_t	*pcu;
	uint32_t data;
	cltt_state_t prev;

	if ((pcu = find_pcu(dip)) == NULL)
		return (DDI_FAILURE);

	data = ddi_get32(pcu->pcu_rhdl,
	    (uint32_t *)&pcu->pcu_regs[MEM_TRML_ESTIMATION_CFG]);

	DTRACE_PROBE1(trml_cfg, uint32_t, data);

	prev = (data & MEM_TRML_DISABLE_IMC) ? DISABLE_CLTT : ENABLE_CLTT;
	if (old)
		*old = prev;

	if (prev == cltt)
		return (DDI_SUCCESS);

	if (cltt == ENABLE_CLTT)
		data &= ~MEM_TRML_DISABLE_IMC;
	else
		data |= MEM_TRML_DISABLE_IMC;

	ddi_put32(pcu->pcu_rhdl,
	    (uint32_t *)&pcu->pcu_regs[MEM_TRML_ESTIMATION_CFG], data);

	release_pcu(pcu);

	return (DDI_SUCCESS);
}

static int
pcu_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	pcu_t	*pcu;
	int	inst = ddi_get_instance(dip);

	switch (cmd) {
	case DDI_ATTACH:
		break;

	case DDI_RESUME:
		return (DDI_SUCCESS);

	default:
		return (DDI_FAILURE);
	}

	if (ddi_soft_state_zalloc(pcu_state, inst) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "!Failed to allocate soft_state");
		return (DDI_FAILURE);
	}

	pcu = ddi_get_soft_state(pcu_state, inst);
	pcu->pcu_dip = dip;

	if (ddi_regs_map_setup(dip, 0, (caddr_t *)&pcu->pcu_regs, 0, 0,
	    &pcu_attr, &pcu->pcu_rhdl) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "!Failed to map config registers");
		ddi_soft_state_free(pcu_state, inst);
		return (DDI_FAILURE);
	}

	mutex_enter(&pcu_lock);
	list_insert_tail(&pcu_list, pcu);
	mutex_exit(&pcu_lock);

	return (DDI_SUCCESS);
}

static int
pcu_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	pcu_t	*pcu;
	int	inst = ddi_get_instance(dip);

	switch (cmd) {
	case DDI_DETACH:
		break;

	case DDI_SUSPEND:
		return (DDI_SUCCESS);

	default:
		return (DDI_FAILURE);
	}

	pcu = ddi_get_soft_state(pcu_state, inst);
	mutex_enter(&pcu_lock);
	if (pcu->hold == 0) {
		list_remove(&pcu_list, pcu);
		mutex_exit(&pcu_lock);

		ddi_regs_map_free(&pcu->pcu_rhdl);
		ddi_soft_state_free(pcu_state, inst);

		return (DDI_SUCCESS);
	}
	mutex_exit(&pcu_lock);
	return (DDI_FAILURE);
}

static struct cb_ops pcu_cb_ops = {
	nodev,			/* open */
	nodev,			/* close */
	nodev,			/* strategy */
	nodev,			/* print */
	nodev,			/* dump */
	nodev,			/* read */
	nodev,			/* write */
	nodev,			/* ioctl */
	nodev,			/* devmap */
	nodev,			/* mmap */
	nodev,			/* segmap */
	nochpoll,		/* poll */
	ddi_prop_op,		/* cb_prop_op */
	NULL,			/* streamtab  */
	D_MP,			/* Driver compatibility flag */
	CB_REV,			/* cb_rev */
	nodev,			/* cb_aread */
	nodev			/* cb_awrite */
};

static struct dev_ops pcu_ops = {
	DEVO_REV,
	0,
	ddi_no_info,
	nulldev,
	nulldev,
	pcu_attach,
	pcu_detach,
	nodev,
	&pcu_cb_ops,
	NULL,
	nodev,
	ddi_quiesce_not_needed,
};

static struct modldrv modldrv = {
	&mod_driverops,		/* Type of module. This one is a driver */
	"IMC PCU Driver",	/* Name of the module. */
	&pcu_ops,		/* driver ops */
};

static struct modlinkage modlinkage = {
	MODREV_1,
	&modldrv,
	NULL
};

int
_init(void)
{
	int status;

	status = ddi_soft_state_init(&pcu_state, sizeof (pcu_t), 1);
	if (status != 0)
		return (status);

	if ((status = mod_install(&modlinkage)) != 0)
		ddi_soft_state_fini(&pcu_state);

	list_create(&pcu_list, sizeof (pcu_t), offsetof(pcu_t, link));
	mutex_init(&pcu_lock, NULL, MUTEX_DEFAULT, NULL);

	return (status);
}

int
_fini(void)
{
	int status;

	if ((status = mod_remove(&modlinkage)) == 0) {
		mutex_destroy(&pcu_lock);
		list_destroy(&pcu_list);
		ddi_soft_state_fini(&pcu_state);
	}

	return (status);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}
