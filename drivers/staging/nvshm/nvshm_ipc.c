/*
 * Copyright (C) 2012-2013 NVIDIA Corporation.
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "nvshm_types.h"
#include "nvshm_if.h"
#include "nvshm_priv.h"
#include "nvshm_iobuf.h"
#include "nvshm_ipc.h"
#include "nvshm_queue.h"

#include <asm/mach/map.h>
#include <mach/tegra_bb.h>
#include <asm/cacheflush.h>

#define NVSHM_QUEUE_TIMEOUT_US (1000)

static int ipc_readconfig(struct nvshm_handle *handle)
{
	struct nvshm_config *conf;
	int chan;

	pr_debug("%s\n", __func__);

	conf = (struct nvshm_config *)(handle->mb_base_virt
				       + NVSHM_CONFIG_OFFSET);
	if (conf->version != NVSHM_CONFIG_VERSION) {
		pr_err("%s: wrong config version 0x%x\n",
		       __func__, (unsigned int)conf->version);
		return -1;
	}
	if (handle->ipc_size != conf->shmem_size) {
		pr_warn("%s shmem mapped/reported not matching: 0x%x/0x%x\n",
		       __func__, handle->ipc_size, conf->shmem_size);
	}
	handle->desc_base_virt = handle->ipc_base_virt
		+ conf->region_ap_desc_offset;
	pr_debug("%s desc_base_virt=0x%x\n",
		__func__, (unsigned int)handle->desc_base_virt);

	handle->desc_size = conf->region_ap_desc_size;
	pr_debug("%s desc_size=%d\n",
		__func__, (int)handle->desc_size);

	/* Data is cached */
	handle->data_base_virt = handle->ipc_base_virt
		+ conf->region_ap_data_offset;
	pr_debug("%s data_base_virt=0x%x\n",
		__func__, (unsigned int)handle->data_base_virt);

	handle->data_size = conf->region_ap_data_size;
	pr_debug("%s data_size=%d\n", __func__, (int)handle->data_size);

#ifndef CONFIG_TEGRA_BASEBAND_SIMU
	handle->shared_queue_head =
		(struct nvshm_iobuf *)(handle->ipc_base_virt
				     + conf->queue_bb_offset);
	pr_debug("%s shared_queue_head offset=0x%x\n",
		__func__,
		(unsigned int)handle->shared_queue_head-
		(unsigned int)handle->ipc_base_virt);
#else
	handle->shared_queue_head =
		(struct nvshm_iobuf *)(handle->ipc_base_virt
				      + conf->queue_ap_offset);
	pr_debug("%s shared_queue_head offset=0x%x\n",
		__func__,
		(unsigned int)handle->shared_queue_head -
		(unsigned int)handle->ipc_base_virt);
#endif
	handle->shared_queue_tail =
		(struct nvshm_iobuf *)(handle->ipc_base_virt
				     + conf->queue_ap_offset);
	pr_debug("%s shared_queue_tail offset=0x%x\n",
		__func__, (unsigned int)handle->shared_queue_tail -
		(unsigned int)handle->ipc_base_virt);

	for (chan = 0; chan < NVSHM_MAX_CHANNELS; chan++) {
		handle->chan[chan].index = chan;
		handle->chan[chan].map = conf->chan_map[chan];
		if (handle->chan[chan].map.type != NVSHM_CHAN_UNMAP) {
			pr_debug("%s chan[%d]=%s\n",
				__func__, chan, handle->chan[chan].map.name);
		}
	}
	handle->conf = conf;
	handle->configured = 1;
	return 0;
}

static int init_interfaces(struct nvshm_handle *handle)
{
	int nlog = 0, ntty = 0, nnet = 0;
	int chan;

	for (chan = 0; chan < NVSHM_MAX_CHANNELS; chan++) {
		switch (handle->chan[chan].map.type) {
		case NVSHM_CHAN_TTY:
			ntty++;
			break;
		case NVSHM_CHAN_LOG:
			nlog++;
			break;
		case NVSHM_CHAN_NET:
			nnet++;
			break;
		default:
			break;
		}
	}

	if (ntty) {
		pr_debug("%s init %d tty channels\n", __func__, ntty);
		nvshm_tty_init(handle);
	}

	if (nlog)
		pr_debug("%s init %d log channels\n", __func__, nlog);

	if (nnet) {
		pr_debug("%s init %d net channels\n", __func__, nnet);
		nvshm_net_init(handle);
	}

	return 0;
}

static int cleanup_interfaces(struct nvshm_handle *handle)
{
	int nlog = 0, ntty = 0, nnet = 0;
	int chan;

	/* No need to protect this as configuration will arrive after cleanup
	 * is propagated to userland
	 */
	handle->configured = 0;

	for (chan = 0; chan < NVSHM_MAX_CHANNELS; chan++) {
		switch (handle->chan[chan].map.type) {
		case NVSHM_CHAN_TTY:
			ntty++;
			break;
		case NVSHM_CHAN_LOG:
			nlog++;
			break;
		case NVSHM_CHAN_NET:
			nnet++;
			break;
		default:
			break;
		}
	}

	if (ntty) {
		pr_debug("%s cleanup %d tty channels\n", __func__, ntty);
		nvshm_tty_cleanup();
	}

	if (nlog)
		pr_debug("%s cleanup %d log channels\n", __func__, nlog);

	if (nnet) {
		pr_debug("%s cleanup %d net channels\n", __func__, nnet);
		nvshm_net_cleanup();
	}
	return 0;
}

static void ipc_work(struct work_struct *work)
{
	struct nvshm_handle *handle = container_of(work,
						   struct nvshm_handle,
						   nvshm_work);
	int new_state;
	int cmd;

	new_state = *((int *)handle->mb_base_virt);
	cmd = new_state & 0xFFFF;
	if (((~new_state >> 16) ^ (cmd)) & 0xFFFF) {
		pr_err("%s IPC check failure msg=0x%x\n",
		       __func__, new_state);
		if (handle->configured) {
			nvshm_abort_queue(handle);
			cleanup_interfaces(handle);
		}
		return;
	}
	switch (cmd) {
	case NVSHM_IPC_READY:
		/* most encountered message - process queue */
		if (cmd == handle->old_status) {
			/* Process IPC queue but do not notify sysfs */
			nvshm_process_queue(handle);
		} else {
			ipc_readconfig(handle);
			nvshm_iobuf_init(handle);
			nvshm_init_queue(handle);
			init_interfaces(handle);
		}
		handle->old_status = cmd;
		return;
	case NVSHM_IPC_BOOT_FW_REQ:
	case NVSHM_IPC_BOOT_RESTART_FW_REQ:
		if (handle->configured) {
			nvshm_abort_queue(handle);
			cleanup_interfaces(handle);
		}
		break;
	case NVSHM_IPC_BOOT_ERROR_BT2_HDR:
	case NVSHM_IPC_BOOT_ERROR_BT2_SIGN:
	case NVSHM_IPC_BOOT_ERROR_HWID:
	case NVSHM_IPC_BOOT_ERROR_APP_HDR:
	case NVSHM_IPC_BOOT_ERROR_APP_SIGN:
	case NVSHM_IPC_BOOT_ERROR_UNLOCK_HEADER:
	case NVSHM_IPC_BOOT_ERROR_UNLOCK_SIGN:
	case NVSHM_IPC_BOOT_ERROR_UNLOCK_PCID:
		pr_err("%s BB startup failure: msg=0x%x\n",
		       __func__, new_state);
		break;
	case NVSHM_IPC_BOOT_COLD_BOOT_IND:
	case NVSHM_IPC_BOOT_FW_CONF:
		/* Should not have these - something went wrong... */
		pr_err("%s IPC IT error: msg=0x%x\n",
		       __func__, new_state);
		break;
	default:
		pr_err("%s unknown IPC message found: msg=0x%x\n",
		       __func__, new_state);
		break;
	}
	handle->old_status = cmd;
}

static void nvshm_ipc_handler(void *data)
{
	struct nvshm_handle *handle = (struct nvshm_handle *)data;
	pr_debug("%s\n", __func__);
	queue_work(handle->nvshm_wq, &handle->nvshm_work);
}

int nvshm_register_ipc(struct nvshm_handle *handle)
{
	pr_debug("%s\n", __func__);
	snprintf(handle->wq_name, 15, "nvshm_queue%d", handle->instance);
	handle->nvshm_wq = create_singlethread_workqueue(handle->wq_name);
	INIT_WORK(&handle->nvshm_work, ipc_work);
	tegra_bb_register_ipc(handle->tegra_bb, nvshm_ipc_handler, handle);
	return 0;
}

int nvshm_unregister_ipc(struct nvshm_handle *handle)
{
	pr_debug("%s flush workqueue\n", __func__);
	flush_workqueue(handle->nvshm_wq);
	pr_debug("%s destroy workqueue\n", __func__);
	destroy_workqueue(handle->nvshm_wq);
	pr_debug("%s unregister tegra_bb\n", __func__);
	tegra_bb_register_ipc(handle->tegra_bb, NULL, NULL);
	return 0;
}

int nvshm_generate_ipc(struct nvshm_handle *handle)
{
	mb();
	/* generate ipc */
	tegra_bb_generate_ipc(handle->tegra_bb);
	return 0;
}
