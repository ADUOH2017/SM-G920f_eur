/*
 * Copyright (C) 2014 Samsung Electronics.
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

#include "modem_utils.h"
#include "link_device_memory.h"
#include "link_ctrlmsg_iosm.h"

#ifdef GROUP_MEM_LINK_IOSM_MESSAGE

#ifdef CONFIG_SEC_MODEM_DEBUG
#define pr_circ_idx(hdr) \
	mif_info("circ: in=%d, out=%d\n", hdr->w_idx, hdr->r_idx);
#else
#define pr_circ_idx(hdr) {}
#endif

static struct workqueue_struct *iosm_wq;
static struct mutex iosm_mtx;
static atomic_t mdm_ready;

static const char const *tx_iosm_str[] = {
	[IOSM_A2C_AP_READY] = "AP_READY",
	[IOSM_A2C_CONF_CH_REQ] = "CONF_CH_REQ",
	[IOSM_A2C_OPEN_CH] = "OPEN_CH",
	[IOSM_A2C_CLOSE_CH] = "CLOSE_CH",
	[IOSM_A2C_STOP_TX_CH] = "STOP_TX_CH",
	[IOSM_A2C_START_TX_CH] = "START_TX_CH",
	[IOSM_A2C_ACK] = "ACK",
	[IOSM_A2C_NACK] = "NACK",
	[IOSM_A2C_PIN_INIT_DONE] = "PIN_INIT_DONE",
	[IOSM_A2C_INIT_END] = "INIT_END",
};

/* 8-bit transaction ID: holds unique id value (1 ~ 255)
 * different number than the last message and incremented by one
 * also to be echoed back in the response of received message
 */
static atomic_t tid = ATOMIC_INIT(-1);

static inline int get_transaction_id(void)
{
	return atomic_inc_return(&tid) % IOSM_TRANS_ID_MAX + 1;
}

static inline int check_ul_space(u32 qlen, u32 in, u32 out)
{
	u32 usage, space;

	if (!circ_valid(qlen, in, out)) {
		mif_err("ERR! TXQ DIRTY (qlen:%d in:%d out:%d)\n",
				qlen, in, out);
		return -EIO;
	}

	usage = circ_get_usage(qlen, in, out);
	if (unlikely(usage > SBD_UL_LIMIT)) {
		mif_err("TXQ BUSY (qlen:%d in:%d out:%d usage:%d)\n",
			qlen, in, out, usage);
		return -EBUSY;
	}

	space = circ_get_space(qlen, in, out);
	if (unlikely(space < 1)) {
		mif_err("TXQ NOSPC (qlen:%d in:%d out:%d)\n", qlen, in, out);
		return -ENOSPC;
	}

	return space;
}

inline void create_iosm_message(struct iosm_msg *txmsg, u8 mid, u32 *args)
{
	struct iosm_msg *msg;

	txmsg->msg_id = mid;

	switch (mid) {
	case IOSM_A2C_AP_READY:
		/* set global descriptor address */
		txmsg->ap_ready.addr = IOSM_MSG_DESC_OFFSET;
		break;
	case IOSM_A2C_CONF_CH_REQ:
		txmsg->conf_ch_req.ch_id = *((u32 *)args);
		txmsg->conf_ch_req.cfg = 0x1;
		break;
	case IOSM_A2C_OPEN_CH:
	case IOSM_A2C_CLOSE_CH:
		txmsg->close_ch.ch_id = *((u32 *)args);
		txmsg->close_ch.cfg = 0x7;
		break;
	case IOSM_A2C_PIN_INIT_DONE:
	case IOSM_A2C_INIT_END:
		break;
	case IOSM_A2C_ACK:
	case IOSM_A2C_NACK:
		msg = (struct iosm_msg *) args;
		txmsg->ack.msg_id = msg->msg_id;
		if (msg->msg_id == IOSM_C2A_CONF_CH_RSP)
			txmsg->ack.ch_id = msg->conf_ch_rsp.ch_id;
		if (msg->msg_id == IOSM_C2A_STOP_TX_CH)
			txmsg->ack.ch_id = msg->stop_tx_ch.ch_id;
		if (msg->msg_id == IOSM_C2A_START_TX_CH)
			txmsg->ack.ch_id = msg->start_tx_ch.ch_id;
		/* trans_id shouldn't be modified from end of this function */
		txmsg->trs_id = msg->trs_id;
		return;
	default:
		mif_err("0x%x message is not supported.\n", mid);
	}

	txmsg->trs_id = get_transaction_id();
}

void __tx_iosm_message(struct mem_link_device *mld, u8 id)
{
	tx_iosm_message(mld, id, 0);
}
void tx_iosm_message(struct mem_link_device *mld, u8 id, u32 *args)
{
	struct iosm_msg_area *base;
	struct iosm_msg_area_head *hdr;
	struct iosm_msg *msg;
	int space;
	struct link_device *ld = &mld->link_dev;
	struct modem_ctl *mc = ld->mc;

#ifndef CONFIG_SEC_MODEM_XMM7260_CAT6
	if (atomic_read(&mld->cp_boot_done)) {
#endif
		if (!cp_online(mc))
			return;
#ifndef CONFIG_SEC_MODEM_XMM7260_CAT6
	}
#endif

	mutex_lock(&iosm_mtx);

	base = (struct iosm_msg_area *) (mld->base + IOSM_MSG_TX_OFFSET);
	hdr = &base->hdr;

	/* A message sender reads the read and write index and
	 * determines whether there are free elements.
	 */
	space = check_ul_space(IOSM_NUM_ELEMENTS, hdr->w_idx, hdr->r_idx);
	if (space <= 0) {
		mutex_unlock(&iosm_mtx);
		return;
	}

	msg = &base->elements[hdr->w_idx];
	create_iosm_message(msg, id, args);

	/* The write index is incremented and interrupt is triggered
	 * to the message receiver.
	 */
	hdr->w_idx = circ_new_ptr(IOSM_NUM_ELEMENTS, hdr->w_idx, 1);
	pr_circ_idx(hdr);

	mutex_unlock(&iosm_mtx);

	if (cp_online(mc) && mld->forbid_cp_sleep_wait) {
		if (mld->forbid_cp_sleep_wait(mld, REFCNT_IOSM)) {
			send_ipc_irq(mld, mask2int(MASK_CMD_VALID));
			mif_info("sent msg %s\n", tx_iosm_str[msg->msg_id]);
		} else {
			mif_err("failed to send iosm msg(%s)\n",
				tx_iosm_str[msg->msg_id]);
		}
	}

	if (cp_online(mc) && mld->permit_cp_sleep)
		mld->permit_cp_sleep(mld, REFCNT_IOSM);
}

void mdm_ready_handler(struct mem_link_device *mld, struct iosm_msg *msg)
{
	int err;
	struct link_device *ld = &mld->link_dev;
	struct modem_ctl *mc = ld->mc;

	mif_err("%s: %s.state:%s cp_boot_done:%d\n", ld->name,
		mc->name, mc_state(mc), atomic_read(&mld->cp_boot_done));

	if (!ld->sbd_ipc) {
		mif_err("%s: LINK_ATTR_SBD_IPC is NOT set\n", ld->name);
		return;
	}

	ld->netif_stop_mask = 0;
	atomic_set(&ld->netif_stopped, 0);
	atomic_set(&mc->forced_cp_crash, 0);
	atomic_set(&mld->cp_boot_done, 1);

	mc->iod->modem_state_changed(mc->iod, STATE_ONLINE);

#ifdef CONFIG_LINK_POWER_MANAGEMENT
	if (mld->start_pm) {
		mld->start_pm(mld);
		gpio_set_value(mld->gpio_cp_wakeup, 0);
		gpio_set_value(mld->gpio_ap_status, 0);
	}

	if (cp_online(mc) && mld->forbid_cp_sleep)
		mld->forbid_cp_sleep(mld, REFCNT_IOSM);
#endif

	tx_iosm_message(mld, IOSM_A2C_ACK, (u32 *)msg);

	err = init_sbd_link(&mld->sbd_link_dev);
	if (err < 0) {
		mif_err("%s: init_sbd_link fail(%d)\n", ld->name, err);
		return;
	}

	if (mld->attrs & LINK_ATTR(LINK_ATTR_IPC_ALIGNED))
		ld->aligned = true;
	else
		ld->aligned = false;

	sbd_activate(&mld->sbd_link_dev);

	tx_iosm_message(mld, IOSM_A2C_AP_READY, 0);

	mif_info("%s: %s mdm_ready done\n", ld->name, mc->name);
}

void conf_ch_rsp_handler(struct mem_link_device *mld, struct iosm_msg *msg)
{
	struct sbd_link_device *sl = &mld->sbd_link_dev;
	int dev_id = sbd_ch2id(sl, msg->ack.ch_id);
	struct sbd_ipc_device *sid = sbd_id2dev(sl, dev_id);

	atomic_set(&sid->config_done, 1);
	tx_iosm_message(mld, IOSM_A2C_ACK, (u32 *)msg);

	mif_info("ch_id : %d, dev_id : %d\n", sid->ch, dev_id);
}

void stop_tx_ch_handler(struct mem_link_device *mld, struct iosm_msg *msg)
{
	struct link_device *ld = &mld->link_dev;

#ifdef CONFIG_SEC_MODEM_XMM7260_CAT6
	stop_net_iface(ld, msg->stop_tx_ch.ch_id);
#else
	stop_net_ifaces(ld);
#endif
	tx_iosm_message(mld, IOSM_A2C_ACK, (u32 *)msg);
}

void start_tx_ch_handler(struct mem_link_device *mld, struct iosm_msg *msg)
{
	struct link_device *ld = &mld->link_dev;

#ifdef CONFIG_SEC_MODEM_XMM7260_CAT6
	resume_net_iface(ld, msg->start_tx_ch.ch_id);
#else
	resume_net_ifaces(ld);
#endif
	tx_iosm_message(mld, IOSM_A2C_ACK, (u32 *)msg);
}

static void action(struct io_device *iod, void *args)
{
	struct mem_link_device *mld = (struct mem_link_device *) args;

	tx_iosm_message(mld, IOSM_A2C_CONF_CH_REQ, (u32 *)&iod->id);
}

void ack_handler(struct mem_link_device *mld, struct iosm_msg *msg)
{
	struct link_device *ld = &mld->link_dev;
#if defined(CONFIG_LINK_POWER_MANAGEMENT) && defined(CONFIG_SEC_MODEM_XMM7260_CAT6)
	struct modem_ctl *mc = ld->mc;
#endif

	mif_err("got ack for msg id = 0x%x\n", msg->ack.msg_id);

	switch (msg->ack.msg_id) {
	case IOSM_A2C_AP_READY:
		atomic_set(&mdm_ready, 1);
		iodevs_for_each(ld->msd, action, mld);
		break;
	case IOSM_A2C_OPEN_CH:
#if defined(CONFIG_LINK_POWER_MANAGEMENT) && defined(CONFIG_SEC_MODEM_XMM7260_CAT6)
	if (msg->ack.ch_id == SIPC5_CH_ID_FMT_0) {
		if (cp_online(mc) && mld->permit_cp_sleep)
			mld->permit_cp_sleep(mld, REFCNT_IOSM);
	}
#endif
		break;
	case IOSM_A2C_CLOSE_CH:
	default:
		break;
	}
}

void nack_handler(struct mem_link_device *mld, struct iosm_msg *msg)
{
	mif_err("got nack for msg id = 0x%x\n", msg->nack.msg_id);
}

static bool rild_ready(struct link_device *ld)
{
	struct io_device *fmt_iod;
	struct io_device *rfs_iod;
	int fmt_opened;
	int rfs_opened;

	fmt_iod = link_get_iod_with_channel(ld, SIPC5_CH_ID_FMT_0);
	if (!fmt_iod) {
		mif_err("%s: No FMT io_device\n", ld->name);
		return false;
	}

	rfs_iod = link_get_iod_with_channel(ld, SIPC5_CH_ID_RFS_0);
	if (!rfs_iod) {
		mif_err("%s: No RFS io_device\n", ld->name);
		return false;
	}

	fmt_opened = atomic_read(&fmt_iod->opened);
	rfs_opened = atomic_read(&rfs_iod->opened);
	mif_err("%s: %s.opened=%d, %s.opened=%d\n", ld->name,
		fmt_iod->name, fmt_opened, rfs_iod->name, rfs_opened);
	if (fmt_opened > 0 && rfs_opened > 0)
		return true;

	return false;
}

static void init_start_handler(struct mem_link_device *mld, struct iosm_msg *msg)
{
	struct link_device *ld = &mld->link_dev;
	struct modem_ctl *mc = ld->mc;
	int err;

	mif_err("%s: INIT_START <- %s (%s.state:%s cp_boot_done:%d)\n",
		ld->name, mc->name, mc->name, mc_state(mc),
		atomic_read(&mld->cp_boot_done));

	if (!ld->sbd_ipc) {
		mif_err("%s: LINK_ATTR_SBD_IPC is NOT set\n", ld->name);
		return;
	}

	err = init_sbd_link(&mld->sbd_link_dev);
	if (err < 0) {
		mif_err("%s: init_sbd_link fail(%d)\n", ld->name, err);
		return;
	}

	if (mld->attrs & LINK_ATTR(LINK_ATTR_IPC_ALIGNED))
		ld->aligned = true;
	else
		ld->aligned = false;

	sbd_activate(&mld->sbd_link_dev);

	mif_err("%s: PIF_INIT_DONE -> %s\n", ld->name, mc->name);
	tx_iosm_message(mld, IOSM_A2C_PIN_INIT_DONE, 0);
}

static void phone_start_handler(struct mem_link_device *mld, struct iosm_msg *msg)
{
	struct link_device *ld = &mld->link_dev;
	struct modem_ctl *mc = ld->mc;
	unsigned long flags;
	int err;

	mif_err("%s: CP_START <- %s (%s.state:%s cp_boot_done:%d)\n",
		ld->name, mc->name, mc->name, mc_state(mc),
		atomic_read(&mld->cp_boot_done));

#ifdef CONFIG_LINK_POWER_MANAGEMENT
	if (mld->start_pm)
		mld->start_pm(mld);
#endif

	spin_lock_irqsave(&ld->lock, flags);

	if (ld->state == LINK_STATE_IPC) {
		/*
		If there is no INIT_END command from AP, CP sends a CP_START
		command to AP periodically until it receives INIT_END from AP
		even though it has already been in ONLINE state.
		*/
		if (rild_ready(ld)) {
			mif_err("%s: INIT_END -> %s\n", ld->name, mc->name);
			tx_iosm_message(mld, IOSM_A2C_INIT_END, 0);
		}
		goto exit;
	}

	err = mem_reset_ipc_link(mld);
	if (err) {
		mif_err("%s: mem_reset_ipc_link fail(%d)\n", ld->name, err);
		goto exit;
	}

	if (rild_ready(ld)) {
		mif_err("%s: INIT_END -> %s\n", ld->name, mc->name);
		tx_iosm_message(mld, IOSM_A2C_INIT_END, 0);
		atomic_set(&mld->cp_boot_done, 1);
	}

	ld->state = LINK_STATE_IPC;

	complete_all(&mc->init_cmpl);

exit:
	spin_unlock_irqrestore(&ld->lock, flags);
}

static struct {
	u16	cmd;
	char	*name;
	void (*handler)(struct mem_link_device *mld, struct iosm_msg *msg);
} iosm_handler[] = {
	{ IOSM_C2A_MDM_READY, "MDM_READY", mdm_ready_handler },
	{ IOSM_C2A_CONF_CH_RSP, "CONFG_CH_RSP", conf_ch_rsp_handler },
	{ IOSM_C2A_STOP_TX_CH, "STOP_TX_CH", stop_tx_ch_handler },
	{ IOSM_C2A_START_TX_CH, "START_TX_CH", start_tx_ch_handler },
	{ IOSM_C2A_ACK, "ACK", ack_handler },
	{ IOSM_C2A_NACK, "NACK", nack_handler },
	{ IOSM_C2A_INIT_START, "INIT_START", init_start_handler },
	{ IOSM_C2A_PHONE_START, "PHONE_START", phone_start_handler },
};

void iosm_event_work(struct work_struct *work)
{
	struct iosm_msg_area *base;
	struct iosm_msg_area_head *hdr;
	struct iosm_msg *msg;
	u32 i, size;
	struct mem_link_device *mld =
		container_of(work, struct mem_link_device, iosm_w);

	base = (struct iosm_msg_area *) (mld->base + IOSM_MSG_RX_OFFSET);
	hdr = &base->hdr;

	if (unlikely(circ_empty(hdr->w_idx, hdr->r_idx))) {
		mif_info("iosm message area is full\n");
		return;
	}

	/* The message receiver determines the number of available messages
	 * based on the read and write index.
	 */
	size = circ_get_usage(IOSM_NUM_ELEMENTS, hdr->w_idx, hdr->r_idx);
	mif_debug("number of available messages = %d\n", size);

	while (size--) {
		msg = &base->elements[hdr->r_idx];
		for (i = 0; i < ARRAY_SIZE(iosm_handler); i++)
			if (iosm_handler[i].cmd == msg->msg_id) {
				mif_info("got msg %s\n", iosm_handler[i].name);
				(*iosm_handler[i].handler)(mld, msg);
				break;
			}
		if (i >= ARRAY_SIZE(iosm_handler))
			mif_err("0x%x message is not supported\n", msg->msg_id);
		/* read index is increamented by the number of read messages */
		hdr->r_idx = circ_new_ptr(IOSM_NUM_ELEMENTS, hdr->r_idx, 1);
	}
	pr_circ_idx(hdr);
}

void iosm_event_bh(struct mem_link_device *mld, u16 cmd)
{
	queue_work(iosm_wq, &mld->iosm_w);
}

static int __init iosm_init(void)
{
	iosm_wq = create_singlethread_workqueue("iosm_wq");
	if (!iosm_wq) {
		mif_err("ERR! fail to create tx_wq\n");
		return -ENOMEM;
	}
	mutex_init(&iosm_mtx);
#ifdef CONFIG_SEC_MODEM_XMM7260_CAT6
	atomic_set(&mdm_ready, 0);
#endif
	mif_info("iosm_msg size = %ld, num of iosm elements = %ld\n",
			(long)sizeof(struct iosm_msg), (long)IOSM_NUM_ELEMENTS);
	return 0;
}
module_init(iosm_init);

static void __exit iosm_exit(void)
{
	destroy_workqueue(iosm_wq);
}
module_exit(iosm_exit);

#endif
