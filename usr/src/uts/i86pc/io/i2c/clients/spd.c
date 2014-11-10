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
#include <sys/types.h>
#include <sys/conf.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/modctl.h>
#include <sys/sysmacros.h>
#include <sys/i2c/misc/i2c_svc.h>
#include <sys/i2c/clients/spd_impl.h>

static void	*dimm_state;

static krwlock_t	dimm_lock;
static list_t		dimm_list;


/*
 * Detect the presence of a DIMM by probing the i2C device for the SPD.
 * For now we only support DDR3 SPD.
 * Convert the eeprom format SPD into a more friendly format.
 */
static boolean_t
detect_dimm(sdram_t *slv)
{
	dev_info_t	*dip = slv->dip;
	dev_info_t	*pdip = ddi_get_parent(dip);
	i2c_transfer_t	*i2c_tp;
	boolean_t	rv = B_FALSE;
	int		capacity;
	int		i, size;

	(void) i2c_transfer_alloc(slv->i2c_hdl, &i2c_tp, 1, 1, I2C_SLEEP);
	i2c_tp->i2c_version = I2C_XFER_REV;
	i2c_tp->i2c_flags = I2C_WR_RD;

	/*
	 * Check for DDR3 SPD
	 */
	i2c_tp->i2c_wbuf[0] = SPD_MEMORY_TYPE;
	if (i2c_transfer(slv->i2c_hdl, i2c_tp) != I2C_SUCCESS)
		goto out;

	if (i2c_tp->i2c_rbuf[0] != SPD_DDR3_SDRAM)
		goto out;

	/*
	 * Get the size of SPD - should be 176 for DDR3
	 */
	i2c_tp->i2c_wbuf[0] = SPD_SIZE;
	if (i2c_transfer(slv->i2c_hdl, i2c_tp) != I2C_SUCCESS)
		goto out;

	size = (i2c_tp->i2c_rbuf[0] >> 4) & 7;
	if (size == 0)
		slv->spd_size = 128;
	else if (size == 1)
		slv->spd_size = 176;
	else if (size == 2)
		slv->spd_size = 256;
	else
		slv->spd_size = 64;

	/*
	 * Read the complete SPD
	 */
	for (i = 0; i < slv->spd_size; i++) {
		i2c_tp->i2c_wbuf[0] = (uint8_t)i;
		if (i2c_transfer(slv->i2c_hdl, i2c_tp) != I2C_SUCCESS)
			goto out;

		slv->spd_data[i] = i2c_tp->i2c_rbuf[0];
	}

	/*
	 * Now convert the raw spd data into something more understandable
	 */
	slv->spd_info.dram_type = slv->spd_data[SPD_MEMORY_TYPE];
	slv->spd_info.dimm_type = slv->spd_data[SPD_MODULE_TYPE];
	slv->spd_info.version = slv->spd_data[SPD_REVISION];
	slv->spd_info.manu_id = slv->spd_data[SPD_MMID_LSB] & 0x7f;
	slv->spd_info.manu_id |= slv->spd_data[SPD_MMID_MSB] << 8;
	slv->spd_info.ranks = ((slv->spd_data[SPD_MOD_ORG] >> 3) & 0xf) + 1;
	slv->spd_info.banks = 1 << (((slv->spd_data[SPD_BANKS] >> 4) & 7) + 3);
	slv->spd_info.rows = ((slv->spd_data[SPD_ADDRS] >> 3) & 0xf) + 12;
	slv->spd_info.columns = (slv->spd_data[SPD_ADDRS] & 7) + 9;
	slv->spd_info.bits = 1 << ((slv->spd_data[SPD_BUS_WIDTH] & 7) + 3);
	/*
	 * This is log(2) of the actually size
	 */
	capacity = (slv->spd_data[SPD_BANKS] & 7) + 28;
	capacity += (slv->spd_data[SPD_BUS_WIDTH] & 7) + 3;
	capacity -= (slv->spd_data[SPD_MOD_ORG] & 7) + 2;
	capacity -= 23;		/* from bits to MBytes */
	slv->spd_info.size = (1u << capacity) * slv->spd_info.ranks;

	/*
	 * This set of characteristics should provide enough for a
	 * user program to determine the clock speed of the RAM. We
	 * can't do it he due to lack of floating point.
	 */
	slv->spd_info.ftb_dividend = slv->spd_data[SPD_FTB] >> 4;
	slv->spd_info.ftb_divisor = slv->spd_data[SPD_FTB] & 0xf;
	slv->spd_info.mtb_dividend = slv->spd_data[SPD_MTB_DIVIDEND];
	slv->spd_info.mtb_divisor = slv->spd_data[SPD_MTB_DIVISOR];
	slv->spd_info.min_ctime_mtb = slv->spd_data[SPD_MIN_CTIME_MTB];
	slv->spd_info.min_ctime_ftb = (int8_t)slv->spd_data[SPD_MIN_CTIME_FTB];

	slv->spd_info.serial_no = 0;
	for (i = 0; i < 4; i++)
		slv->spd_info.serial_no |= slv->spd_data[SPD_SERIAL_NUM + i] <<
		    (8 * (3 - i));
	slv->spd_info.revision = slv->spd_data[SPD_MOD_REV] << 8;
	slv->spd_info.revision |= slv->spd_data[SPD_MOD_REV + 1];

	for (i = 0; i < PARTNO_SIZE; i++)
		slv->spd_info.part_no[i] = slv->spd_data[SPD_PART_NO + i];
	slv->spd_info.part_no[i] = 0;

	slv->spd_info.year = 10 * (slv->spd_data[SPD_YEAR] >> 4);
	slv->spd_info.year += slv->spd_data[SPD_YEAR] & 0xf;
	slv->spd_info.year += slv->spd_info.year >= 80 ? 1900 : 2000;
	slv->spd_info.week = 10 * (slv->spd_data[SPD_WEEKS] >> 4);
	slv->spd_info.week += slv->spd_data[SPD_WEEKS] & 0xf;

	slv->spd_info.socket = ddi_get_instance(pdip);
	slv->spd_info.slot = (uint16_t)ddi_prop_get_int(DDI_DEV_T_ANY, dip,
	    DDI_PROP_DONTPASS, "slot", -1);

	rv = B_TRUE;

out:
	i2c_transfer_free(slv->i2c_hdl, i2c_tp);

	return (rv);
}

static int
spd_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	sdram_t		*slv;
	int		inst = ddi_get_instance(dip);

	switch (cmd) {
	case DDI_ATTACH:
		break;

	case DDI_RESUME:
		return (DDI_SUCCESS);

	default:
		return (DDI_FAILURE);
	}

	if (ddi_soft_state_zalloc(dimm_state, inst) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "!Failed to allocate soft_state");
		return (DDI_FAILURE);
	}

	slv = ddi_get_soft_state(dimm_state, inst);
	slv->dip = dip;
	if (i2c_client_register(dip, &slv->i2c_hdl) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "!Failed to register with i2c_svc");
		ddi_soft_state_free(dimm_state, inst);
		return (DDI_FAILURE);
	}

	/*
	 * See if this device is indeed a supported DIMM
	 */
	if (!detect_dimm(slv)) {
		i2c_client_unregister(slv->i2c_hdl);
		ddi_soft_state_free(dimm_state, inst);

		return (DDI_FAILURE);
	}

	if (ddi_create_minor_node(dip, "dimm", S_IFCHR, inst, DDI_PSEUDO, 0) !=
	    DDI_SUCCESS) {
		i2c_client_unregister(slv->i2c_hdl);
		ddi_soft_state_free(dimm_state, inst);

		return (DDI_FAILURE);
	}

	mutex_init(&slv->spd_lock, NULL, MUTEX_DRIVER, NULL);
	cv_init(&slv->spd_cv, NULL, CV_DRIVER, NULL);

	rw_enter(&dimm_lock, RW_WRITER);
	list_insert_tail(&dimm_list, slv);
	rw_exit(&dimm_lock);

	ddi_report_dev(dip);

	return (DDI_SUCCESS);
}

/* ARGSUSED */
static int
spd_open(dev_t *devp, int flags, int otyp, cred_t *credp)
{
	sdram_t	*slv;
	int	inst = getminor(*devp);
	int	rv = 0;

	if ((slv = ddi_get_soft_state(dimm_state, inst)) == NULL)
		return (ENODEV);

	mutex_enter(&slv->spd_lock);

	if ((slv->open_flags & FEXCL)) {
		rv = EBUSY;
	} else if ((flags & FEXCL)) {
		if (slv->open_cnt > 0)
			rv = EBUSY;
		else
			slv->open_flags |= FEXCL;
	}

	if (rv == 0)
		slv->open_cnt++;

	mutex_exit(&slv->spd_lock);

	return (rv);
}

/* ARGSUSED */
static int
spd_close(dev_t dev, int flags, int otyp, cred_t *credp)
{
	sdram_t	*slv;
	int	inst = getminor(dev);

	if ((slv = ddi_get_soft_state(dimm_state, inst)) == NULL)
		return (ENODEV);

	mutex_enter(&slv->spd_lock);
	slv->open_flags &= ~FEXCL;
	slv->open_cnt--;
	mutex_exit(&slv->spd_lock);

	return (0);
}

/* ARGSUSED */
static int
spd_ioctl(dev_t dev, int cmd, intptr_t data, int mode, cred_t *credp, int *rval)
{
	sdram_t	*slv;
	int	inst = getminor(dev);

	if ((slv = ddi_get_soft_state(dimm_state, inst)) == NULL)
		return (ENODEV);

	switch (cmd) {
	case SPD_IOCTL_GET_SPD:
		*rval = 0;
		if (ddi_copyout(&slv->spd_info, (void *)data,
		    sizeof (spd_dimm_t), mode) < 0)
			return (EFAULT);
		break;

	default:
		return (ENOTTY);
	}

	return (0);
}

static int
spd_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	sdram_t		*slv;
	int		inst = ddi_get_instance(dip);

	switch (cmd) {
	case DDI_DETACH:
		break;

	case DDI_SUSPEND:
		return (DDI_SUCCESS);

	default:
		return (DDI_FAILURE);
	}

	slv = ddi_get_soft_state(dimm_state, inst);

	ddi_remove_minor_node(dip, NULL);
	i2c_client_unregister(slv->i2c_hdl);
	rw_enter(&dimm_lock, RW_WRITER);
	list_remove(&dimm_list, slv);
	rw_exit(&dimm_lock);
	ddi_soft_state_free(dimm_state, inst);

	return (DDI_SUCCESS);
}

static struct cb_ops spd_cb_ops = {
	spd_open,		/* open */
	spd_close,		/* close */
	nodev,			/* strategy */
	nodev,			/* print */
	nodev,			/* dump */
	nodev,			/* read */
	nodev,			/* write */
	spd_ioctl,		/* ioctl */
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

static struct dev_ops spd_ops = {
	DEVO_REV,
	0,
	ddi_no_info,
	nulldev,
	nulldev,
	spd_attach,
	spd_detach,
	nodev,
	&spd_cb_ops,
	NULL,
	nodev,
	ddi_quiesce_not_needed
};

static struct modldrv modldrv = {
	&mod_driverops,		/* Type of module. This one is a driver */
	"SPD I2C Slave",	/* Name of the module. */
	&spd_ops,		/* driver ops */
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

	status = ddi_soft_state_init(&dimm_state, sizeof (sdram_t), 1);
	if (status != 0)
		return (status);

	if ((status = mod_install(&modlinkage)) != 0)
		ddi_soft_state_fini(&dimm_state);

	rw_init(&dimm_lock, NULL, RW_DRIVER, NULL);
	list_create(&dimm_list, sizeof (sdram_t), offsetof(sdram_t, link));
	return (status);
}

int
_fini(void)
{
	int status;

	if ((status = mod_remove(&modlinkage)) == 0) {
		rw_destroy(&dimm_lock);
		list_destroy(&dimm_list);
		ddi_soft_state_fini(&dimm_state);
	}

	return (status);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}
