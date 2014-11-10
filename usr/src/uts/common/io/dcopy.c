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
 */

/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2014, Tegile Systems, Inc. All rights reserved.
 */

/*
 * dcopy.c
 *    dcopy misc module
 */

#include <sys/conf.h>
#include <sys/kmem.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/modctl.h>
#include <sys/sysmacros.h>
#include <sys/atomic.h>


#include <sys/dcopy.h>
#include <sys/dcopy_device.h>


/* Number of entries per channel to allocate */
uint_t dcopy_channel_size = 1024;


typedef struct dcopy_list_s {
	list_t			dl_list;
	kmutex_t		dl_mutex;
	uint_t			dl_cnt; /* num entries on list */
} dcopy_list_t;

/* device state for register/unregister */
struct dcopy_device_s {
	/* DMA device drivers private pointer */
	void			*dc_device_private;

	/* to track list of channels from this DMA device */
	dcopy_list_t		dc_devchan_list;
	list_node_t		dc_device_list_node;

	/*
	 * dc_removing_cnt track how many channels still have to be freed up
	 * before it's safe to allow the DMA device driver to detach.
	 */
	uint_t			dc_removing_cnt;
	dcopy_device_cb_t	*dc_cb;

	dcopy_device_info_t	dc_info;

};

typedef struct dcopy_stats_s {
	kstat_named_t	cs_bytes_xfer;
	kstat_named_t	cs_cmd_alloc;
	kstat_named_t	cs_cmd_post;
	kstat_named_t	cs_cmd_poll;
	kstat_named_t	cs_notify_poll;
	kstat_named_t	cs_notify_pending;
	kstat_named_t	cs_id;
	kstat_named_t	cs_capabilities;
} dcopy_stats_t;

/* DMA channel state */
struct dcopy_channel_s {
	/* DMA driver channel private pointer */
	void			*ch_channel_private;

	/* shortcut to device callbacks */
	dcopy_device_cb_t	*ch_cb;

	/*
	 * number of outstanding allocs for this channel. used to track when
	 * it's safe to free up this channel so the DMA device driver can
	 * detach.
	 */
	uint64_t		ch_ref_cnt;

	/* state for if channel needs to be removed when ch_ref_cnt gets to 0 */
	boolean_t		ch_removing;

	/* B_TRUE if the channel is reserved for exclusive use */
	boolean_t		ch_exclusive;

	list_node_t		ch_devchan_list_node;
	list_node_t		ch_globalchan_list_node;

	/*
	 * per channel list of commands actively blocking waiting for
	 * completion.
	 */
	dcopy_list_t		ch_poll_list;

	/* pointer back to our device */
	struct dcopy_device_s	*ch_device;

	dcopy_query_channel_t	ch_info;

	kstat_t			*ch_kstat;
	dcopy_stats_t		ch_stat;
};

/*
 * If grabbing both device_list mutex & globalchan_list mutex,
 * Always grab globalchan_list mutex before device_list mutex
 */
typedef struct dcopy_state_s {
	dcopy_list_t		d_device_list;
	dcopy_list_t		d_globalchan_list;
} dcopy_state_t;
dcopy_state_t *dcopy_statep;

static dcopy_handle_t next_channel;

/* Module Driver Info */
static struct modlmisc dcopy_modlmisc = {
	&mod_miscops,
	"dcopy kernel module"
};

/* Module Linkage */
static struct modlinkage dcopy_modlinkage = {
	MODREV_1,
	&dcopy_modlmisc,
	NULL
};

static int dcopy_init();
static void dcopy_fini();

static int dcopy_list_init(dcopy_list_t *list, size_t node_size,
    offset_t link_offset);
static void dcopy_list_fini(dcopy_list_t *list);
static void dcopy_list_push(dcopy_list_t *list, void *list_node);
static void dcopy_list_remove(dcopy_list_t *list, void *list_node);
static void *dcopy_list_pop(dcopy_list_t *list);
static void *dcopy_list_next(list_t *list, void *item);

static void dcopy_device_cleanup(dcopy_device_handle_t device,
    boolean_t do_callback);

static int dcopy_stats_init(dcopy_handle_t channel);
static void dcopy_stats_fini(dcopy_handle_t channel);


/*
 * _init()
 */
int
_init()
{
	int e;

	e = dcopy_init();
	if (e != 0) {
		return (e);
	}

	return (mod_install(&dcopy_modlinkage));
}


/*
 * _info()
 */
int
_info(struct modinfo *modinfop)
{
	return (mod_info(&dcopy_modlinkage, modinfop));
}


/*
 * _fini()
 */
int
_fini()
{
	int e;

	e = mod_remove(&dcopy_modlinkage);
	if (e != 0) {
		return (e);
	}

	dcopy_fini();

	return (e);
}

/*
 * dcopy_init()
 */
static int
dcopy_init()
{
	int e;


	dcopy_statep = kmem_zalloc(sizeof (*dcopy_statep), KM_SLEEP);

	/* Initialize the list we use to track device register/unregister */
	e = dcopy_list_init(&dcopy_statep->d_device_list,
	    sizeof (struct dcopy_device_s),
	    offsetof(struct dcopy_device_s, dc_device_list_node));
	if (e != DCOPY_SUCCESS) {
		goto dcopyinitfail_device;
	}

	/* Initialize the list we use to track all DMA channels */
	e = dcopy_list_init(&dcopy_statep->d_globalchan_list,
	    sizeof (struct dcopy_channel_s),
	    offsetof(struct dcopy_channel_s, ch_globalchan_list_node));
	if (e != DCOPY_SUCCESS) {
		goto dcopyinitfail_global;
	}

	return (0);

dcopyinitfail_global:
	dcopy_list_fini(&dcopy_statep->d_device_list);
dcopyinitfail_device:
	kmem_free(dcopy_statep, sizeof (*dcopy_statep));

	return (-1);
}


/*
 * dcopy_fini()
 */
static void
dcopy_fini()
{
	/*
	 * if mod_remove was successfull, we shouldn't have any
	 * devices/channels to worry about.
	 */
	ASSERT(list_head(&dcopy_statep->d_globalchan_list.dl_list) == NULL);
	ASSERT(list_head(&dcopy_statep->d_device_list.dl_list) == NULL);

	dcopy_list_fini(&dcopy_statep->d_globalchan_list);
	dcopy_list_fini(&dcopy_statep->d_device_list);
	kmem_free(dcopy_statep, sizeof (*dcopy_statep));
}


/* *** EXTERNAL INTERFACE *** */
/*
 * dcopy_query()
 */
void
dcopy_query(dcopy_query_t *query)
{
	query->dq_version = DCOPY_QUERY_V0;
	query->dq_num_channels = dcopy_statep->d_globalchan_list.dl_cnt;
}


/*
 * dcopy_alloc()
 */
/*ARGSUSED*/
int
dcopy_alloc(int flags, dev_info_t *root, dcopy_handle_t *handle)
{
	dcopy_handle_t channel;
	dcopy_list_t *list;
	boolean_t found;


	/*
	 * we don't use the dcopy_list_* code here because we need to due
	 * some non-standard stuff.
	 */

	list = &dcopy_statep->d_globalchan_list;

	/*
	 * if nothing is on the channel list, or not enough channels to
	 * allocate an exclusive channel then return DCOPY_NORESOURCES
	 */
	mutex_enter(&list->dl_mutex);
	if (list_is_empty(&list->dl_list) || (list->dl_cnt <= 1 &&
	    (flags & DCOPY_EXCLUSIVE))) {
		/* Always leave one channel for shared use */
		mutex_exit(&list->dl_mutex);
		return (DCOPY_NORESOURCES);
	}

	ASSERT(next_channel != NULL);

	found = B_FALSE;
	channel = next_channel;
	do {
		if ((root == DCOPY_ANY_ROOT ||
		    channel->ch_info.qc_root_dip == root) &&
		    ((flags & DCOPY_EXCLUSIVE) == 0 ||
		    channel->ch_ref_cnt == 0)) {
			found = B_TRUE;
			break;
		}

		channel = dcopy_list_next(&list->dl_list, channel);
	} while (channel != next_channel);

	if (!found) {
		mutex_exit(&list->dl_mutex);
		return (DCOPY_NORESOURCES);
	}

	/*
	 * move the next_channel along and increment the reference count
	 * of the one selected
	 */
	next_channel = dcopy_list_next(&list->dl_list, channel);
	channel->ch_ref_cnt++;

	if ((flags & DCOPY_EXCLUSIVE)) {
		/*
		 * We are requesting a channel for exclusive use, take it off
		 * the globalchan list
		 */
		list_remove(&list->dl_list, channel);
		list->dl_cnt--;
		channel->ch_exclusive = B_TRUE;
	}

	mutex_exit(&list->dl_mutex);

	*handle = (dcopy_handle_t)channel;
	return (DCOPY_SUCCESS);
}


/*
 * dcopy_free()
 */
void
dcopy_free(dcopy_handle_t *channel)
{
	dcopy_device_handle_t device;
	dcopy_list_t *list;
	boolean_t cleanup = B_FALSE;


	ASSERT(*channel != NULL);

	/*
	 * we don't need to add the channel back to the list since we never
	 * removed it. decrement the reference count.
	 */
	list = &dcopy_statep->d_globalchan_list;
	mutex_enter(&list->dl_mutex);
	(*channel)->ch_ref_cnt--;

	if ((*channel)->ch_exclusive) {
		/*
		 * Put the exclusive channel back on the globalchan_list
		 * if it is not pending removal
		 */
		ASSERT((*channel)->ch_ref_cnt == 0);
		(*channel)->ch_exclusive = B_FALSE;
		if (!(*channel)->ch_removing) {
			list_insert_tail(&list->dl_list, *channel);
			list->dl_cnt++;
		}
	}

	/*
	 * if we need to remove this channel, and the reference count is down
	 * to 0, decrement the number of channels which still need to be
	 * removed on the device.
	 */
	if ((*channel)->ch_removing && ((*channel)->ch_ref_cnt == 0)) {
		device = (*channel)->ch_device;
		mutex_enter(&device->dc_devchan_list.dl_mutex);
		device->dc_removing_cnt--;
		if (device->dc_removing_cnt == 0) {
			cleanup = B_TRUE;
		}
		mutex_exit(&device->dc_devchan_list.dl_mutex);
	}
	mutex_exit(&list->dl_mutex);

	/*
	 * if there are no channels which still need to be removed, cleanup the
	 * device state and call back into the DMA device driver to tell them
	 * the device is free.
	 */
	if (cleanup) {
		dcopy_device_cleanup(device, B_TRUE);
	}

	*channel = NULL;
}


/*
 * dcopy_query_channel()
 */
void
dcopy_query_channel(dcopy_handle_t channel, dcopy_query_channel_t *query)
{
	*query = channel->ch_info;
}


/*
 * called from the cmd constructor of ioat to initialise private state
 */
void
dcopy_cmd_init(dcopy_cmd_t cmd)
{
	dcopy_cmd_priv_t priv = cmd->dp_private;

	list_link_init(&priv->pr_poll_list_node);
	mutex_init(&priv->pr_mutex, NULL, MUTEX_DRIVER, NULL);
	cv_init(&priv->pr_cv, NULL, CV_DRIVER, NULL);
	priv->pr_cmd = cmd;
	priv->pr_channel = NULL;
}

/*
 * called from the cmd destructor of ioat to destroy private state
 */
void
dcopy_cmd_fini(dcopy_cmd_t cmd)
{
	dcopy_cmd_priv_t priv = cmd->dp_private;

	cv_destroy(&priv->pr_cv);
	mutex_destroy(&priv->pr_mutex);
}

/*
 * dcopy_cmd_alloc()
 */
int
dcopy_cmd_alloc(dcopy_handle_t handle, int flags, dcopy_cmd_t *cmd)
{
	dcopy_handle_t channel;
	dcopy_cmd_priv_t priv;
	int e;


	channel = handle;

	atomic_inc_64(&channel->ch_stat.cs_cmd_alloc.value.ui64);
	e = channel->ch_cb->cb_cmd_alloc(channel->ch_channel_private, flags,
	    cmd);
	if (e == DCOPY_SUCCESS) {
		priv = (*cmd)->dp_private;
		priv->pr_channel = channel;
		priv->pr_poll_err = DCOPY_FAILURE;
	}

	return (e);
}


/*
 * dcopy_cmd_free()
 */
void
dcopy_cmd_free(dcopy_cmd_t *cmd)
{
	dcopy_handle_t channel;
	dcopy_cmd_priv_t priv;


	ASSERT(*cmd != NULL);

	priv = (*cmd)->dp_private;
	channel = priv->pr_channel;

	channel->ch_cb->cb_cmd_free(channel->ch_channel_private, cmd);
}

/*
 * Called just before the cmd is returned to the memory cache, allows
 * us to clean up
 */
void
dcopy_cmd_freed(dcopy_cmd_t cmd)
{
	dcopy_cmd_priv_t priv = cmd->dp_private;

	if (priv->pr_channel)
		dcopy_list_remove(&priv->pr_channel->ch_poll_list, priv);
}

/*
 * dcopy_cmd_post()
 */
int
dcopy_cmd_post(dcopy_cmd_t cmd)
{
	dcopy_handle_t channel;
	int e;


	channel = cmd->dp_private->pr_channel;

	atomic_inc_64(&channel->ch_stat.cs_cmd_post.value.ui64);
	if (cmd->dp_cmd == DCOPY_CMD_COPY) {
		atomic_add_64(&channel->ch_stat.cs_bytes_xfer.value.ui64,
		    cmd->dp.copy.cc_size);
	}

	e = channel->ch_cb->cb_cmd_post(channel->ch_channel_private, cmd);
	if (e != DCOPY_SUCCESS) {
		return (e);
	}

	return (DCOPY_SUCCESS);
}

/*
 * Called when the cmd has been posted in the ioat ring, but before the
 * ring is given a kick to make it process the cmd
 */
void
dcopy_cmd_posted(dcopy_cmd_t cmd)
{
	dcopy_handle_t channel;

	channel = cmd->dp_private->pr_channel;

	/*
	 * If the cmd has the intr flag set, put it on the poll list
	 * so it can be later waited for.
	 */
	if ((cmd->dp_flags & DCOPY_CMD_INTR))
		dcopy_list_push(&channel->ch_poll_list, cmd->dp_private);
}

/*
 * dcopy_cmd_poll()
 */
int
dcopy_cmd_poll(dcopy_cmd_t cmd, int flags)
{
	dcopy_handle_t channel;
	dcopy_cmd_priv_t priv;
	dcopy_device_cb_t *ch_cb;
	int e;


	priv = cmd->dp_private;
	channel = priv->pr_channel;
	ch_cb = channel->ch_cb;

	/*
	 * if the caller is trying to block, they needed to post the
	 * command with DCOPY_CMD_INTR set.
	 */
	if ((flags & DCOPY_POLL_BLOCK) && !(cmd->dp_flags & DCOPY_CMD_INTR)) {
		return (DCOPY_FAILURE);
	}

	atomic_inc_64(&channel->ch_stat.cs_cmd_poll.value.ui64);

	mutex_enter(&priv->pr_mutex);
	e = ch_cb->cb_cmd_poll(channel->ch_channel_private, cmd);
	if (e == DCOPY_PENDING && (flags & DCOPY_POLL_BLOCK)) {
		/*
		 * if the command is still active, and the blocking flag
		 * is set.
		 */
		priv->pr_wait = B_TRUE;
		while (priv->pr_wait)
			cv_wait(&priv->pr_cv, &priv->pr_mutex);
		e = priv->pr_poll_err;
	}
	mutex_exit(&priv->pr_mutex);

	return (e);
}

/*
 * Given a dev_info_t pointer, work up the device tree to get the dev_info_t
 * of the root complex it is under
 */
dev_info_t *
dcopy_get_root_complex(dev_info_t *dip)
{
	dev_info_t *pdip;
	char **prop_val;
	uint_t prop_len;
	int i;

	for (pdip = ddi_get_parent(dip); pdip; pdip = ddi_get_parent(pdip)) {
		if (ddi_prop_lookup_string_array(DDI_DEV_T_ANY, pdip, 0,
		    "compatible", &prop_val, &prop_len) != DDI_SUCCESS)
			continue;

		for (i = 0; i < prop_len; i++) {
			if (strcmp(prop_val[i], "pciex_root_complex") == 0)
				return (pdip);
		}
	}

	return (NULL);
}

/* *** END OF EXTERNAL INTERFACE *** */

/*
 * dcopy_list_init()
 */
static int
dcopy_list_init(dcopy_list_t *list, size_t node_size, offset_t link_offset)
{
	mutex_init(&list->dl_mutex, NULL, MUTEX_DRIVER, NULL);
	list_create(&list->dl_list, node_size, link_offset);
	list->dl_cnt = 0;

	return (DCOPY_SUCCESS);
}


/*
 * dcopy_list_fini()
 */
static void
dcopy_list_fini(dcopy_list_t *list)
{
	list_destroy(&list->dl_list);
	mutex_destroy(&list->dl_mutex);
}


/*
 * dcopy_list_push()
 */
static void
dcopy_list_push(dcopy_list_t *list, void *list_node)
{
	mutex_enter(&list->dl_mutex);
	list_insert_tail(&list->dl_list, list_node);
	list->dl_cnt++;
	mutex_exit(&list->dl_mutex);
}

/*
 * dcopy_list_pop()
 */
static void *
dcopy_list_pop(dcopy_list_t *list)
{
	list_node_t *list_node;

	mutex_enter(&list->dl_mutex);
	list_node = list_head(&list->dl_list);
	if (list_node == NULL) {
		mutex_exit(&list->dl_mutex);
		return (list_node);
	}
	list->dl_cnt--;
	list_remove(&list->dl_list, list_node);
	mutex_exit(&list->dl_mutex);

	return (list_node);
}

static void
dcopy_list_remove(dcopy_list_t *list, void *list_node)
{
	mutex_enter(&list->dl_mutex);
	if (list_link_active(list_node)) {
		list_remove(&list->dl_list, list_node);
		list->dl_cnt--;
	}
	mutex_exit(&list->dl_mutex);
}

/*
 * get the next item in the list, but treat it as circular
 */
static void *
dcopy_list_next(list_t *list, void *item)
{
	item = list_next(list, item);
	if (item == NULL)
		item = list_head(list);

	return (item);
}

/* *** DEVICE INTERFACE *** */
/*
 * dcopy_device_register()
 */
int
dcopy_device_register(void *device_private, dcopy_device_info_t *info,
    dcopy_device_handle_t *handle)
{
	struct dcopy_channel_s *channel;
	struct dcopy_device_s *device;
	int e;
	int i;


	/* initialize the per device state */
	device = kmem_zalloc(sizeof (*device), KM_SLEEP);
	device->dc_device_private = device_private;
	device->dc_info = *info;
	device->dc_removing_cnt = 0;
	device->dc_cb = info->di_cb;

	/*
	 * we have a per device channel list so we can remove a device in the
	 * future.
	 */
	e = dcopy_list_init(&device->dc_devchan_list,
	    sizeof (struct dcopy_channel_s),
	    offsetof(struct dcopy_channel_s, ch_devchan_list_node));
	if (e != DCOPY_SUCCESS) {
		goto registerfail_devchan;
	}

	/*
	 * allocate state for each channel, allocate the channel,  and then add
	 * the devices dma channels to the devices channel list.
	 */
	for (i = 0; i < info->di_num_dma; i++) {
		channel = kmem_zalloc(sizeof (*channel), KM_SLEEP);
		channel->ch_device = device;
		channel->ch_removing = B_FALSE;
		channel->ch_ref_cnt = 0;
		channel->ch_cb = info->di_cb;

		e = info->di_cb->cb_channel_alloc(device_private, channel,
		    DCOPY_SLEEP, dcopy_channel_size, &channel->ch_info,
		    &channel->ch_channel_private);
		if (e != DCOPY_SUCCESS) {
			kmem_free(channel, sizeof (*channel));
			goto registerfail_alloc;
		}

		e = dcopy_stats_init(channel);
		if (e != DCOPY_SUCCESS) {
			info->di_cb->cb_channel_free(
			    &channel->ch_channel_private);
			kmem_free(channel, sizeof (*channel));
			goto registerfail_alloc;
		}

		e = dcopy_list_init(&channel->ch_poll_list,
		    sizeof (struct dcopy_cmd_priv_s),
		    offsetof(struct dcopy_cmd_priv_s, pr_poll_list_node));
		if (e != DCOPY_SUCCESS) {
			dcopy_stats_fini(channel);
			info->di_cb->cb_channel_free(
			    &channel->ch_channel_private);
			kmem_free(channel, sizeof (*channel));
			goto registerfail_alloc;
		}

		dcopy_list_push(&device->dc_devchan_list, channel);
	}

	/* add the device to device list */
	dcopy_list_push(&dcopy_statep->d_device_list, device);

	/*
	 * add the device's dma channels to the global channel list (where
	 * dcopy_alloc's come from)
	 */
	mutex_enter(&dcopy_statep->d_globalchan_list.dl_mutex);
	mutex_enter(&dcopy_statep->d_device_list.dl_mutex);
	channel = list_head(&device->dc_devchan_list.dl_list);
	while (channel != NULL) {
		list_insert_tail(&dcopy_statep->d_globalchan_list.dl_list,
		    channel);
		dcopy_statep->d_globalchan_list.dl_cnt++;
		channel = list_next(&device->dc_devchan_list.dl_list, channel);
	}
	mutex_exit(&dcopy_statep->d_device_list.dl_mutex);

	if (next_channel == NULL)
		next_channel = list_head(
		    &dcopy_statep->d_globalchan_list.dl_list);

	mutex_exit(&dcopy_statep->d_globalchan_list.dl_mutex);

	*handle = device;

	/* last call-back into kernel for dcopy KAPI enabled */
	uioa_dcopy_enable();

	return (DCOPY_SUCCESS);

registerfail_alloc:
	channel = list_head(&device->dc_devchan_list.dl_list);
	while (channel != NULL) {
		/* remove from the list */
		channel = dcopy_list_pop(&device->dc_devchan_list);
		ASSERT(channel != NULL);

		dcopy_list_fini(&channel->ch_poll_list);
		dcopy_stats_fini(channel);
		info->di_cb->cb_channel_free(&channel->ch_channel_private);
		kmem_free(channel, sizeof (*channel));
	}

	dcopy_list_fini(&device->dc_devchan_list);
registerfail_devchan:
	kmem_free(device, sizeof (*device));

	return (DCOPY_FAILURE);
}


/*
 * dcopy_device_unregister()
 */
/*ARGSUSED*/
int
dcopy_device_unregister(dcopy_device_handle_t *handle)
{
	struct dcopy_channel_s *channel;
	dcopy_device_handle_t device;
	boolean_t device_busy, list_empty;

	device = *handle;
	device_busy = B_FALSE;

	/*
	 * remove the devices dma channels from the global channel list (where
	 * dcopy_alloc's come from)
	 */
	mutex_enter(&dcopy_statep->d_globalchan_list.dl_mutex);
	mutex_enter(&device->dc_devchan_list.dl_mutex);
	channel = list_head(&device->dc_devchan_list.dl_list);
	while (channel != NULL) {
		/*
		 * If the channel has already been marked for removing
		 */
		if (channel->ch_removing) {
			channel = list_next(&device->dc_devchan_list.dl_list,
			    channel);
			device_busy = B_TRUE;
			continue;
		}

		/*
		 * if the channel has outstanding allocs, mark it as having
		 * to be removed and increment the number of channels which
		 * need to be removed in the device state too.
		 */
		if (channel->ch_ref_cnt != 0) {
			channel->ch_removing = B_TRUE;
			device_busy = B_TRUE;
			device->dc_removing_cnt++;
		}

		if (list_link_active(&channel->ch_globalchan_list_node) &&
		    !channel->ch_exclusive) {
			if (channel == next_channel) {
				/*
				 * the channel being removed is also the next
				 * one to be allocated, bump next_channel along
				 */
				next_channel = dcopy_list_next(
				    &dcopy_statep->d_globalchan_list.dl_list,
				    next_channel);
			}

			dcopy_statep->d_globalchan_list.dl_cnt--;
			list_remove(&dcopy_statep->d_globalchan_list.dl_list,
			    channel);
		}
		channel = list_next(&device->dc_devchan_list.dl_list, channel);
	}
	mutex_exit(&device->dc_devchan_list.dl_mutex);
	list_empty = !!list_is_empty(&dcopy_statep->d_globalchan_list.dl_list);
	if (list_empty) {
		next_channel = NULL;
		mutex_exit(&dcopy_statep->d_globalchan_list.dl_mutex);

		uioa_dcopy_disable();
	} else {
		mutex_exit(&dcopy_statep->d_globalchan_list.dl_mutex);
	}

	/*
	 * if there are channels which still need to be removed, we will clean
	 * up the device state after they are freed up.
	 */
	if (device_busy) {
		return (DCOPY_PENDING);
	}

	dcopy_device_cleanup(device, B_FALSE);

	*handle = NULL;
	return (DCOPY_SUCCESS);
}


/*
 * dcopy_device_cleanup()
 */
static void
dcopy_device_cleanup(dcopy_device_handle_t device, boolean_t do_callback)
{
	struct dcopy_channel_s *channel;
	boolean_t list_empty;

	/*
	 * remove all the channels in the device list, free them, and clean up
	 * the state.
	 */
	mutex_enter(&dcopy_statep->d_device_list.dl_mutex);
	channel = list_head(&device->dc_devchan_list.dl_list);
	while (channel != NULL) {
		device->dc_devchan_list.dl_cnt--;
		list_remove(&device->dc_devchan_list.dl_list, channel);
		dcopy_list_fini(&channel->ch_poll_list);
		dcopy_stats_fini(channel);
		channel->ch_cb->cb_channel_free(&channel->ch_channel_private);
		kmem_free(channel, sizeof (*channel));
		channel = list_head(&device->dc_devchan_list.dl_list);
	}

	/* remove it from the list of devices */
	list_remove(&dcopy_statep->d_device_list.dl_list, device);

	mutex_exit(&dcopy_statep->d_device_list.dl_mutex);

	/*
	 * notify the DMA device driver that the device is free to be
	 * detached.
	 */
	if (do_callback) {
		device->dc_cb->cb_unregister_complete(
		    device->dc_device_private, DCOPY_SUCCESS);
	}

	dcopy_list_fini(&device->dc_devchan_list);
	kmem_free(device, sizeof (*device));

	mutex_enter(&dcopy_statep->d_globalchan_list.dl_mutex);
	list_empty = !!list_is_empty(&dcopy_statep->d_globalchan_list.dl_list);
	mutex_exit(&dcopy_statep->d_globalchan_list.dl_mutex);
	/*
	 * no more channels so disanle uiao KAPI
	 */
	if (list_empty)
		uioa_dcopy_disable();
}


/*
 * dcopy_device_channel_notify()
 */
/*ARGSUSED*/
void
dcopy_device_channel_notify(dcopy_handle_t handle, int status)
{
	struct dcopy_channel_s *channel;
	dcopy_list_t *poll_list;
	dcopy_cmd_priv_t priv, next;
	int e;


	ASSERT(status == DCOPY_COMPLETION);
	channel = handle;

	poll_list = &channel->ch_poll_list;

	/*
	 * when we get a completion notification from the device, go through
	 * all of the commands blocking on this channel and see if they have
	 * completed. Remove the command and wake up the block thread if they
	 * have. Once we hit a command which is still pending, we are done
	 * polling since commands in a channel complete in order.
	 */
	mutex_enter(&poll_list->dl_mutex);
	for (priv = list_head(&poll_list->dl_list); priv; priv = next) {
		next = list_next(&poll_list->dl_list, priv);

		atomic_inc_64(&channel->ch_stat.cs_notify_poll.value.ui64);

		mutex_enter(&priv->pr_mutex);
		e = channel->ch_cb->cb_cmd_poll(channel->ch_channel_private,
		    priv->pr_cmd);

		if (e == DCOPY_PENDING) {
			mutex_exit(&priv->pr_mutex);
			atomic_inc_64(&channel->
			    ch_stat.cs_notify_pending.value.ui64);
			break;
		}

		poll_list->dl_cnt--;
		list_remove(&poll_list->dl_list, priv);

		if (priv->pr_wait) {
			priv->pr_wait = B_FALSE;
			priv->pr_poll_err = e;
			cv_signal(&priv->pr_cv);
		}
		mutex_exit(&priv->pr_mutex);
	}

	mutex_exit(&poll_list->dl_mutex);
}


/*
 * dcopy_stats_init()
 */
static int
dcopy_stats_init(dcopy_handle_t channel)
{
#define	CHANSTRSIZE	20
	char chanstr[CHANSTRSIZE];
	dcopy_stats_t *stats;
	int instance;
	char *name;


	stats = &channel->ch_stat;
	name = (char *)ddi_driver_name(channel->ch_device->dc_info.di_dip);
	instance = ddi_get_instance(channel->ch_device->dc_info.di_dip);

	(void) snprintf(chanstr, CHANSTRSIZE, "channel%d",
	    (uint32_t)channel->ch_info.qc_chan_num);

	channel->ch_kstat = kstat_create(name, instance, chanstr, "misc",
	    KSTAT_TYPE_NAMED, sizeof (dcopy_stats_t) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL);
	if (channel->ch_kstat == NULL) {
		return (DCOPY_FAILURE);
	}
	channel->ch_kstat->ks_data = stats;

	kstat_named_init(&stats->cs_bytes_xfer, "bytes_xfer",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&stats->cs_cmd_alloc, "cmd_alloc",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&stats->cs_cmd_post, "cmd_post",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&stats->cs_cmd_poll, "cmd_poll",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&stats->cs_notify_poll, "notify_poll",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&stats->cs_notify_pending, "notify_pending",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&stats->cs_id, "id",
	    KSTAT_DATA_UINT64);
	kstat_named_init(&stats->cs_capabilities, "capabilities",
	    KSTAT_DATA_UINT64);

	kstat_install(channel->ch_kstat);

	channel->ch_stat.cs_id.value.ui64 = channel->ch_info.qc_id;
	channel->ch_stat.cs_capabilities.value.ui64 =
	    channel->ch_info.qc_capabilities;

	return (DCOPY_SUCCESS);
}


/*
 * dcopy_stats_fini()
 */
static void
dcopy_stats_fini(dcopy_handle_t channel)
{
	kstat_delete(channel->ch_kstat);
}
/* *** END OF DEVICE INTERFACE *** */
