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
#include <sys/sunndi.h>
#include <sys/modctl.h>
#include <sys/kmem.h>
#include <sys/pci.h>
#include <sys/sdt.h>
#include <sys/x86_archext.h>

#include <sys/i2c/misc/i2c_svc.h>
#include <sys/i2c/misc/i2c_svc_impl.h>
#include <sys/i2c/nexus/imc.h>

static void	*imc_state;

static void	imc_resume(dev_info_t *);
static void	imc_suspend(dev_info_t *);
static imc_chan_t *chn_claim(imc_t *, dev_info_t *, int, i2c_transfer_t *);
static void	chn_release(imc_chan_t *);
static void	imc_free_regs(imc_t *);
static int	imc_setup_regs(dev_info_t *, imc_t *);
static void	imc_reportdev(dev_info_t *, dev_info_t *);
static void	imc_uninitchild(dev_info_t *);
static int	imc_initchild(dev_info_t *);
static int	imc_transfer(dev_info_t *, i2c_transfer_t *);
static int	chn_wr(imc_chan_t *, boolean_t);
static int	chn_wr_rd(imc_chan_t *, boolean_t);

static i2c_nexus_reg_t imc_regvec = {
	I2C_NEXUS_REV,
	imc_transfer,
};

static int
imc_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	imc_t	*imc;
	int	inst = ddi_get_instance(dip);
	int	i;

	switch (cmd) {
	case DDI_ATTACH:
		break;

	case DDI_RESUME:
		imc_resume(dip);
		return (DDI_SUCCESS);

	default:
		return (DDI_FAILURE);
	}

	/*
	 * Allocate soft state structure.
	 */
	if (ddi_soft_state_zalloc(imc_state, inst) != DDI_SUCCESS)
		return (DDI_FAILURE);

	imc = ddi_get_soft_state(imc_state, inst);
	imc->imc_dip = dip;

	if (imc_setup_regs(dip, imc) != DDI_SUCCESS) {
		ddi_soft_state_free(imc_state, inst);
		return (DDI_FAILURE);
	}

	/*
	 * There are two independent i2c channels/segments on each device
	 */
	for (i = 0; i < 2; i++) {
		mutex_init(&imc->imc_channel[i].chn_mutex, NULL, MUTEX_DRIVER,
		    NULL);
		cv_init(&imc->imc_channel[i].chn_cv, NULL, CV_DRIVER, NULL);
		imc->imc_channel[i].chn_imc = imc;
		imc->imc_channel[i].chn_seg = i;
	}

	/*
	 * Register with the i2c framework
	 */
	i2c_nexus_register(dip, &imc_regvec);

	return (DDI_SUCCESS);
}

static int
imc_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	imc_t	*imc;
	int	inst = ddi_get_instance(dip);
	int	i, seg, addr;

	switch (cmd) {
	case DDI_DETACH:
		break;

	case DDI_SUSPEND:
		imc_suspend(dip);
		return (DDI_SUCCESS);

	default:
		return (DDI_FAILURE);
	}

	if ((imc = ddi_get_soft_state(imc_state, inst)) == NULL)
		return (DDI_SUCCESS);

	for (seg = 0; seg < SEGMENTS; seg++) {
		for (addr = 0; addr < ADDR_PER_SEG; addr++) {
			if (imc->imc_children[seg][addr] == NULL)
				continue;

			/*
			 * Cannot detach if we have any children attached
			 */
			if (i_ddi_node_state(imc->imc_children[seg][addr]) >=
			    DS_INITIALIZED) {
				return (DDI_FAILURE);
			}
		}
	}

	/*
	 * Now we can clean up all the children devices
	 */
	for (seg = 0; seg < SEGMENTS; seg++) {
		for (addr = 0; addr < ADDR_PER_SEG; addr++) {
			if (imc->imc_children[seg][addr] == NULL)
				continue;

			ddi_prop_remove_all(imc->imc_children[seg][addr]);
			(void) ndi_devi_free(imc->imc_children[seg][addr]);
		}
	}

	for (i = 0; i < 2; i++) {
		cv_destroy(&imc->imc_channel[i].chn_cv);
		mutex_destroy(&imc->imc_channel[i].chn_mutex);
	}

	imc_free_regs(imc);

	i2c_nexus_unregister(dip);

	ddi_soft_state_free(imc_state, inst);

	return (DDI_SUCCESS);
}

static int
imc_bus_ctl(dev_info_t *dip, dev_info_t *rdip, ddi_ctl_enum_t op,
    void *arg, void *result)
{
	switch (op) {
	case DDI_CTLOPS_INITCHILD:
		return (imc_initchild((dev_info_t *)arg));

	case DDI_CTLOPS_UNINITCHILD:
		imc_uninitchild((dev_info_t *)arg);
		return (DDI_SUCCESS);

	case DDI_CTLOPS_REPORTDEV:
		imc_reportdev(dip, rdip);
		return (DDI_SUCCESS);

	case DDI_CTLOPS_DMAPMAPC:
	case DDI_CTLOPS_POKE:
	case DDI_CTLOPS_PEEK:
	case DDI_CTLOPS_IOMIN:
	case DDI_CTLOPS_REPORTINT:
	case DDI_CTLOPS_SIDDEV:
	case DDI_CTLOPS_SLAVEONLY:
	case DDI_CTLOPS_AFFINITY:
	case DDI_CTLOPS_PTOB:
	case DDI_CTLOPS_BTOP:
	case DDI_CTLOPS_BTOPR:
	case DDI_CTLOPS_DVMAPAGESIZE:
		return (DDI_FAILURE);

	default:
		return (ddi_ctlops(dip, rdip, op, arg, result));
	}
}

/*
 * Update the segment and address of the slave propperties. The
 * slv_info_t is saved as parent data on the dev_info of the i2c
 * slave
 */
static int
imc_update_slave_info(dev_info_t *dip, slv_info_t *slv)
{
	slv->slv_segment = (uint32_t)ddi_prop_get_int(DDI_DEV_T_ANY, dip,
	    DDI_PROP_DONTPASS, "segment", -1);
	if (slv->slv_segment == -1U) {
		cmn_err(CE_WARN, "required property \"segment\" not "
		    "specified");
		return (DDI_FAILURE);
	}

	slv->slv_address = (uint32_t)ddi_prop_get_int(DDI_DEV_T_ANY, dip,
	    DDI_PROP_DONTPASS, "address", -1);
	if (slv->slv_address == -1U) {
		cmn_err(CE_WARN, "required property \"address\" not "
		    "specified");
		return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);
}

/*
 * Saves the i2c segment/address the slave is attached to as parent
 * data on the slaves dev_info
 */
static int
imc_initchild(dev_info_t *cdip)
{
	slv_info_t	*slv;
	char		name[30];

	slv = kmem_zalloc(sizeof (slv_info_t), KM_SLEEP);
	if (imc_update_slave_info(cdip, slv) != DDI_SUCCESS) {
		kmem_free(slv, sizeof (slv_info_t));
		return (DDI_FAILURE);
	}

	(void) snprintf(name, sizeof (name), "%x,%x", slv->slv_segment,
	    slv->slv_address);

	ddi_set_name_addr(cdip, name);
	ddi_set_parent_data(cdip, slv);

	return (DDI_SUCCESS);
}

static void
imc_uninitchild(dev_info_t *cdip)
{
	slv_info_t	*slv;

	slv = ddi_get_parent_data(cdip);
	ddi_set_parent_data(cdip, NULL);

	ddi_set_name_addr(cdip, NULL);

	kmem_free(slv, sizeof (slv_info_t));
}

/*
 * Allocates a dev_info for the slave which can be bound to with
 * the alias "spd,dimm".
 * It also adds properties which maybe of interest to the slave's
 * driver.
 */
static void
imc_alloc_slave_device(imc_t *imc, int seg, int addr)
{
	dev_info_t	*dip = imc->imc_dip;
	dev_info_t	*cdip;
	int		slot;
	char		prop[32];

	if (imc->imc_children[seg][addr] != NULL)
		return;

	ndi_devi_alloc_sleep(dip, "spd", (pnode_t)DEVI_SID_NODEID, &cdip);

	(void) ndi_prop_update_int(DDI_DEV_T_NONE, cdip, "segment", seg);
	(void) ndi_prop_update_int(DDI_DEV_T_NONE, cdip, "address", addr);
	(void) ndi_prop_update_int(DDI_DEV_T_NONE, cdip,
	    "device-type-identifier", SMB_DTI_EEPROM);

	(void) snprintf(prop, sizeof (prop), "slot-mapping-%d-%d", seg, addr);
	slot = ddi_prop_get_int(DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS, prop,
	    -1);
	if (slot != -1)
		(void) ndi_prop_update_int(DDI_DEV_T_NONE, cdip, "slot", slot);

	(void) ndi_prop_update_string(DDI_DEV_T_NONE, cdip, "compatible",
	    "spd,dimm");

	if (ndi_devi_bind_driver(cdip, 0) != DDI_SUCCESS) {
		ddi_prop_remove_all(cdip);
		(void) ndi_devi_free(cdip);
	} else {
		imc->imc_children[seg][addr] = cdip;
	}
}

/*
 * Creates a dev_info for each possible slave address.
 */
/* ARGSUSED */
static int
imc_bus_config(dev_info_t *dip, uint_t flags, ddi_bus_config_op_t op,
    void *arg, dev_info_t **cdip)
{
	int	circ, rv;
	int	seg, addr;
	int	inst = ddi_get_instance(dip);
	imc_t	*imc;

	imc = ddi_get_soft_state(imc_state, inst);

	ndi_devi_enter(dip, &circ);

	if (op == BUS_CONFIG_DRIVER || op == BUS_CONFIG_ALL) {
		for (seg = 0; seg < SEGMENTS; seg++) {
			for (addr = 0; addr < ADDR_PER_SEG; addr++)
				imc_alloc_slave_device(imc, seg, addr);
		}
	}

	rv = ndi_busop_bus_config(dip, flags, op, arg, cdip, 0);

	ndi_devi_exit(dip, circ);

	return (rv);
}

static int
imc_bus_unconfig(dev_info_t *dip, uint_t flags, ddi_bus_config_op_t op,
    void *arg)
{
	int	circ, rv;

	ndi_devi_enter(dip, &circ);
	flags &= ~NDI_DEVI_REMOVE;
	rv = ndi_busop_bus_unconfig(dip, flags | NDI_UNCONFIG, op, arg);
	ndi_devi_exit(dip, circ);

	return (rv);
}

static void
imc_reportdev(dev_info_t *dip, dev_info_t *rdip)
{
	slv_info_t	*slv;

	slv = ddi_get_parent_data(rdip);

	cmn_err(CE_CONT, "?%s%d at %s%d: bus %d addr 0x%x",
	    ddi_driver_name(rdip), ddi_get_instance(rdip),
	    ddi_driver_name(dip), ddi_get_instance(dip),
	    slv->slv_segment, slv->slv_address);
}

/*
 * Gain access the IMC's registers
 */
static int
imc_setup_regs(dev_info_t *dip, imc_t *imc)
{
	ddi_device_acc_attr_t	attr;
	pci_regspec_t		*regp;
	uint64_t		msr;
	uint32_t		ctl;
	int			ret, length;

	(void) snprintf(imc->imc_name, sizeof (imc->imc_name),
	    "%s:%d", ddi_node_name(dip), ddi_get_instance(dip));

	if (ddi_prop_lookup_int_array(DDI_DEV_T_ANY, dip,
	    DDI_PROP_DONTPASS, "reg", (int **)&regp,
	    (uint_t *)&length) != DDI_PROP_SUCCESS) {
		cmn_err(CE_WARN, "%s unable to get \"reg\" property",
		    imc->imc_name);
		return (DDI_FAILURE);
	}

	imc->imc_dev = PCI_REG_DEV_G(regp[0].pci_phys_hi);
	imc->imc_fn = PCI_REG_FUNC_G(regp[0].pci_phys_hi);

	attr.devacc_attr_version = DDI_DEVICE_ATTR_V0;
	attr.devacc_attr_endian_flags = DDI_STRUCTURE_LE_ACC;
	attr.devacc_attr_dataorder = DDI_STRICTORDER_ACC;

	ret = ddi_regs_map_setup(dip, 0, (caddr_t *)&imc->imc_regs,
	    0, 0, &attr, &imc->imc_rhdl);

	if (ret != DDI_SUCCESS) {
		cmn_err(CE_WARN, "%s unable to map config regs",
		    imc->imc_name);
	}

	/*
	 * Look at MSR 0x2e7 (LT_MEMORY_LOCK) and the TSOD Poll enable bit.
	 * LT_MEMORY_LOCK will prevent us from flipping the TSOD poll bit,
	 * which means accessing the SPD will fail unless TSOD polling (CLTT)
	 * is explicitly disabled in the BIOS.
	 */
	msr = rdmsr(0x2e7);
	ctl = ddi_get32(imc->imc_rhdl, (uint32_t *)&imc->imc_regs[SMB_CTL(0)]);
	if ((msr & 1) && (ctl & SMB_TSOD_POLL_EN)) {
		cmn_err(CE_WARN, "!The iMC memory is locked and CLTT "
		    "(TSOD polling) is enabled in the BIOS. Access to SPD "
		    "data may not be possible.");
	}

	return (ret);
}

static void
imc_free_regs(imc_t *imc)
{
	if (imc->imc_regs != NULL)
		ddi_regs_map_free(&imc->imc_rhdl);
}

/*
 * take a slave dip and return its i2c address.
 */
static int
imc_dip_to_addr(dev_info_t *cdip)
{
	slv_info_t	*slv = ddi_get_parent_data(cdip);

	return (slv->slv_address);
}

/*
 * take a slave dip and return which segment of the IMC it is on
 */
static int
imc_dip_to_seg(dev_info_t *cdip)
{
	slv_info_t	*slv = ddi_get_parent_data(cdip);

	return (slv->slv_segment);
}

/*
 * By claiming the channels, all transfers will be blocked.
 */
static void
imc_suspend(dev_info_t *dip)
{
	imc_t	*imc;
	int	inst;

	inst = ddi_get_instance(dip);
	imc = ddi_get_soft_state(imc_state, inst);

	(void) chn_claim(imc, NULL, 0, NULL);
	(void) chn_claim(imc, NULL, 1, NULL);
}

/*
 * On resume, channels are released and any blocked transfers will complete
 */
static void
imc_resume(dev_info_t *dip)
{
	imc_t	*imc;
	int	inst;

	inst = ddi_get_instance(dip);
	imc = ddi_get_soft_state(imc_state, inst);

	chn_release(&imc->imc_channel[0]);
	chn_release(&imc->imc_channel[1]);
}

/*
 * Ultimately called by a thread wanting to do an i2c transfer.
 * It serialises access to each channel.
 */
static imc_chan_t *
chn_claim(imc_t *imc, dev_info_t *dip, int chan, i2c_transfer_t *tp)
{
	imc_chan_t	*chn = &imc->imc_channel[chan];

	mutex_enter(&chn->chn_mutex);
	while (chn->chn_busy) {
		cv_wait(&chn->chn_cv, &chn->chn_mutex);
	}
	chn->chn_busy = 1;
	mutex_exit(&chn->chn_mutex);

	chn->chn_cur_tran = tp;
	chn->chn_cur_dip = dip;

	return (chn);
}

static void
chn_release(imc_chan_t *chn)
{
	chn->chn_cur_tran = NULL;
	chn->chn_cur_dip = NULL;

	mutex_enter(&chn->chn_mutex);
	chn->chn_busy = 0;
	cv_signal(&chn->chn_cv);
	mutex_exit(&chn->chn_mutex);
}

/*
 * busy wait for the last request on the channel to complete, or
 * generally become "idle"
 */
static int
chn_wait_idle(imc_chan_t *chn)
{
	imc_t	*imc = chn->chn_imc;
	int	i;
	uint32_t data;

	for (i = 0; i < SMB_ITERATIONS; i++) {
		data = ddi_get32(imc->imc_rhdl,
		    (uint32_t *)&imc->imc_regs[SMB_STS(chn->chn_seg)]);

		if ((data & SMB_STS_BSY) == 0)
			return (DDI_SUCCESS);

		drv_usecwait(SMB_ITER_TIMEOUT);
	}

	return (DDI_FAILURE);
}

/*
 * Before accessing the i2c through the IMC, other possible means of
 * access should be blocked. Do this by calling into the IMC_PCU
 * to disable CLTT and then explicitly switch off TSOD polling.
 */
static int
chn_disable_tsod_polling(dev_info_t *cdip, imc_chan_t *chn)
{
	imc_t	*imc = chn->chn_imc;
	uint32_t dti;
	uint32_t data, ctl;

	/*
	 * The device type identifier targets a specific device
	 * at the i2c address, specifically these are SPD data or
	 * the NVDIMM eeprom
	 */
	dti = ddi_prop_get_int(DDI_DEV_T_ANY, cdip,
	    DDI_PROP_DONTPASS, "device-type-identifier", -1);

	if (dti == -1u)
		return (DDI_FAILURE);

	data = ddi_get32(imc->imc_rhdl,
	    (uint32_t *)&imc->imc_regs[SMB_CTL(chn->chn_seg)]);

	DTRACE_PROBE1(ctl_reg, uint32_t, data);

	/*
	 * Save old value of Thermal Sensor on DIMM polling for later
	 * restore
	 */
	chn->chn_poll_en = !!(data & SMB_TSOD_POLL_EN);

	/*
	 * Disable CLTT (Closed Loop Thermal Throttling, this will
	 * stop TSOD polling by IMC
	 */
	if (imc_pcu_set_cltt(imc->imc_dip, DISABLE_CLTT, &chn->chn_cltt_save)
	    != DDI_SUCCESS)
		return (DDI_FAILURE);

	if (chn_wait_idle(chn) != DDI_SUCCESS) {
		(void) imc_pcu_set_cltt(imc->imc_dip, chn->chn_cltt_save, NULL);
		return (DDI_FAILURE);
	}

	ctl = data;
	/*
	 * Save the current dti, and use the one from the dev_info of
	 * the child
	 */
	chn->chn_dti_save = data & SMB_DTI_MASK;
	data &= ~SMB_TSOD_POLL_EN;
	data &= ~SMB_DTI_MASK;
	data |= SMB_DTI(dti);

	ddi_put32(imc->imc_rhdl,
	    (uint32_t *)&imc->imc_regs[SMB_CTL(chn->chn_seg)], data);

	if (chn_wait_idle(chn) != DDI_SUCCESS) {
		/* restore previous settings */
		ddi_put32(imc->imc_rhdl,
		    (uint32_t *)&imc->imc_regs[SMB_CTL(chn->chn_seg)], ctl);
		(void) imc_pcu_set_cltt(imc->imc_dip, chn->chn_cltt_save, NULL);
		return (DDI_FAILURE);
	}

	return (DDI_SUCCESS);
}

/*
 * Restore CLTT and TSOD polling back to their previous state
 */
static int
chn_enable_tsod_polling(imc_chan_t *chn)
{
	imc_t	*imc = chn->chn_imc;
	uint32_t data;

	if (chn_wait_idle(chn) != DDI_SUCCESS)
		return (DDI_FAILURE);

	data = ddi_get32(imc->imc_rhdl,
	    (uint32_t *)&imc->imc_regs[SMB_CTL(chn->chn_seg)]);

	DTRACE_PROBE1(ctl_reg, uint32_t, data);

	if (chn->chn_poll_en) {
		/*
		 * TSOD polling was previously enabled, restore now
		 */
		data |= SMB_TSOD_POLL_EN;
	}

	data &= ~SMB_DTI_MASK;
	data |= chn->chn_dti_save;

	ddi_put32(imc->imc_rhdl,
	    (uint32_t *)&imc->imc_regs[SMB_CTL(chn->chn_seg)], data);

	/*
	 * Restore CLTT state
	 */
	if (imc_pcu_set_cltt(imc->imc_dip, chn->chn_cltt_save, NULL) !=
	    DDI_SUCCESS)
		return (DDI_FAILURE);

	return (DDI_SUCCESS);
}

/*
 * This is ultimately called by the slave device driver.
 * - serialises access
 * - disables CLTT/TSOD
 * - actions i2c request
 * - restores CLTT/TSOD state.
 */
static int
imc_transfer(dev_info_t *dip, i2c_transfer_t *tp)
{
	imc_t		*imc;
	imc_chan_t	*chn;
	int		seg = imc_dip_to_seg(dip);

	imc = ddi_get_soft_state(imc_state,
	    ddi_get_instance(ddi_get_parent(dip)));

	tp->i2c_r_resid = tp->i2c_rlen;
	tp->i2c_w_resid = tp->i2c_wlen;

	if ((chn = chn_claim(imc, dip, seg, tp)) == NULL) {
		tp->i2c_result = I2C_FAILURE;
		return (I2C_FAILURE);
	}

	if (chn_disable_tsod_polling(dip, chn) != DDI_SUCCESS) {
		tp->i2c_result = I2C_FAILURE;
		chn_release(chn);
		return (tp->i2c_result);
	}

	switch (tp->i2c_flags) {
	case I2C_WR:
		tp->i2c_result = chn_wr(chn, B_TRUE);
		break;

	case I2C_WR_RD:
		tp->i2c_result = chn_wr_rd(chn, B_TRUE);
		break;

	case I2C_RD:
	default:
		tp->i2c_result = I2C_FAILURE;
		break;
	}

	(void) chn_enable_tsod_polling(chn);
	chn_release(chn);

	return (tp->i2c_result);
}

/*
 * An SBE can be asserted if a previous command failed. It is cleared by
 * disabling chkovrd and enabling soft-reset, waiting 35ms and them
 * res-enabling chkovrd with soft-reset cleared.
 */
static void
chn_soft_reset(imc_chan_t *chn)
{
	imc_t		*imc = chn->chn_imc;
	uint32_t	ctl;

	ctl = ddi_get32(imc->imc_rhdl,
	    (uint32_t *)&imc->imc_regs[SMB_CTL(chn->chn_seg)]);

	ctl &= ~SMB_CKOVRD;
	ctl |= SMB_SOFT_RESET;
	ddi_put32(imc->imc_rhdl,
	    (uint32_t *)&imc->imc_regs[SMB_CTL(chn->chn_seg)], ctl);

	delay(drv_usectohz(35000));	/* 35 ms */

	ctl |= SMB_CKOVRD;
	ctl &= ~SMB_SOFT_RESET;
	ddi_put32(imc->imc_rhdl,
	    (uint32_t *)&imc->imc_regs[SMB_CTL(chn->chn_seg)], ctl);
}

/*
 * This will write a single byte to the slave.
 * wbuf[0] is the target address, and wbuf[1] is the data.
 */
static int
chn_wr(imc_chan_t *chn, boolean_t retry)
{
	imc_t		*imc = chn->chn_imc;
	i2c_transfer_t	*tp = chn->chn_cur_tran;
	uint8_t		addr = imc_dip_to_addr(chn->chn_cur_dip);
	uint32_t	cmd, sts;
	uint16_t	w_resid;

	if (tp->i2c_w_resid == 0)
		return (I2C_SUCCESS);

	w_resid = tp->i2c_w_resid;

	cmd = SMB_CMD_SLAVE(addr);
	cmd |= SMB_CMD_BA(tp->i2c_wbuf[tp->i2c_wlen - tp->i2c_w_resid--]);
	cmd |= SMB_CMD_WRITE;

	if (tp->i2c_w_resid > 0)
		cmd |= SMB_WDATA(tp->i2c_wbuf[tp->i2c_wlen -
		    tp->i2c_w_resid--]);

	cmd |= SMB_CMD_TRIGGER;

	ddi_put32(imc->imc_rhdl,
	    (uint32_t *)&imc->imc_regs[SMB_CMD(chn->chn_seg)], cmd);

	if (chn_wait_idle(chn) != DDI_SUCCESS)
		return (I2C_FAILURE);

	sts = ddi_get32(imc->imc_rhdl,
	    (uint32_t *)&imc->imc_regs[SMB_STS(chn->chn_seg)]);

	DTRACE_PROBE2(rd, uint32_t, cmd, uint32_t, sts);

	if ((sts & SMB_STS_SBE) && retry) {
		/* Issue a soft reset and try again */
		chn_soft_reset(chn);

		tp->i2c_w_resid = w_resid;
		return (chn_wr(chn, B_FALSE));
	}

	if ((sts & SMB_STS_WOD) == 0)
		return (I2C_FAILURE);

	return (tp->i2c_w_resid == 0 ? I2C_SUCCESS : I2C_INCOMPLETE);
}

/*
 * This reads a byte from the slave device from a give register.
 * The wr_rd version is required as for each read a register/location
 * within the slave needs to be provided. The rd version of the interface
 * infers sequential reading from the last known register. The IMC doesn't
 * have that mechanism.
 * wbuf[0] is written to pci address space as the register to read,
 * rbuf[0] is the byte returned.
 */
static int
chn_wr_rd(imc_chan_t *chn, boolean_t retry)
{
	imc_t		*imc = chn->chn_imc;
	i2c_transfer_t	*tp = chn->chn_cur_tran;
	uint8_t		addr = imc_dip_to_addr(chn->chn_cur_dip);
	uint32_t	cmd, sts;
	uint16_t	w_resid;

	if (tp->i2c_w_resid != 1)
		return (I2C_FAILURE);

	if (tp->i2c_r_resid == 0)
		return (I2C_SUCCESS);

	w_resid = tp->i2c_w_resid;

	cmd = SMB_CMD_SLAVE(addr);
	/* insert the register which will be returned */
	cmd |= SMB_CMD_BA(tp->i2c_wbuf[tp->i2c_wlen - tp->i2c_w_resid--]);
	cmd |= SMB_CMD_TRIGGER;

	ddi_put32(imc->imc_rhdl,
	    (uint32_t *)&imc->imc_regs[SMB_CMD(chn->chn_seg)], cmd);

	if (chn_wait_idle(chn) != DDI_SUCCESS)
		return (I2C_FAILURE);

	sts = ddi_get32(imc->imc_rhdl,
	    (uint32_t *)&imc->imc_regs[SMB_STS(chn->chn_seg)]);

	DTRACE_PROBE2(wr_rd, uint32_t, cmd, uint32_t, sts);

	if ((sts & SMB_STS_SBE) && retry) {
		/* Issue a soft reset and try again */
		chn_soft_reset(chn);

		tp->i2c_w_resid = w_resid;
		return (chn_wr_rd(chn, B_FALSE));
	}

	if ((sts & SMB_STS_RDO) == 0)
		return (I2C_FAILURE);

	tp->i2c_rbuf[tp->i2c_rlen - tp->i2c_r_resid--] =
	    (uint8_t)SMB_RDATA(sts);

	return (tp->i2c_r_resid == 0 ? I2C_SUCCESS : I2C_INCOMPLETE);
}

static struct bus_ops imc_busops = {
	BUSO_REV,
	nullbusmap,			/* bus_map */
	NULL,				/* bus_get_intrspec */
	NULL,				/* bus_add_intrspec */
	NULL,				/* bus_remove_intrspec */
	NULL,				/* bus_map_fault */
	ddi_no_dma_map,			/* bus_dma_map */
	ddi_no_dma_allochdl,		/* bus_dma_allochdl */
	ddi_no_dma_freehdl,		/* bus_dma_freehdl */
	ddi_no_dma_bindhdl,		/* bus_dma_bindhdl */
	ddi_no_dma_unbindhdl,		/* bus_unbindhdl */
	ddi_no_dma_flush,		/* bus_dma_flush */
	ddi_no_dma_win,			/* bus_dma_win */
	ddi_no_dma_mctl,		/* bus_dma_ctl */
	imc_bus_ctl,			/* bus_ctl */
	ddi_bus_prop_op,		/* bus_prop_op */
	NULL,				/* bus_get_eventcookie */
	NULL,				/* bus_add_eventcall */
	NULL,				/* bus_remove_eventcall */
	NULL,				/* bus_post_event */
	0,				/* bus_intr_ctl 	*/
	imc_bus_config,			/* bus_config		*/
	imc_bus_unconfig,		/* bus_unconfig		*/
	0,				/* bus_fm_init		*/
	0,				/* bus_fm_fini		*/
	0,				/* bus_fm_access_enter	*/
	0,				/* bus_fm_access_exit	*/
	0,				/* bus_power		*/
	i_ddi_intr_ops			/* bus_intr_op		*/
};

static struct cb_ops imc_cb_ops = {
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
	0,			/* streamtab  */
	D_MP | D_NEW		/* Driver compatibility flag */
};

static struct dev_ops imc_ops = {
	DEVO_REV,
	0,
	ddi_no_info,
	nulldev,
	nulldev,
	imc_attach,
	imc_detach,
	nodev,
	&imc_cb_ops,
	&imc_busops,
	NULL,
	ddi_quiesce_not_needed	/* devo_quiesce */
};

static struct modldrv modldrv = {
	&mod_driverops, /* Type of module. This one is a driver */
	"IMC nexus Driver",	/* Name of the module. */
	&imc_ops,		/* driver ops */
};

static struct modlinkage modlinkage = {
	MODREV_1,
	&modldrv,
	NULL
};

int
_init(void)
{
	int	status;

	status = ddi_soft_state_init(&imc_state, sizeof (imc_t), 1);
	if (status != 0)
		return (status);

	if ((status = mod_install(&modlinkage)) != 0) {
		ddi_soft_state_fini(&imc_state);
	}
	return (status);
}

int
_fini(void)
{
	int	status;

	if ((status = mod_remove(&modlinkage)) == 0) {
		ddi_soft_state_fini(&imc_state);
	}

	return (status);
}

/*
 * The loadable-module _info(9E) entry point
 */
int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}
