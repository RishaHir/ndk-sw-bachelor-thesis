/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */
/*
 * XDP driver of the NFB platform - ctrl module for default XDP operation
 *	uses page pool API to allocate memory
 *
 * Copyright (C) 2025 CESNET
 * Author(s):
 *   Richard Hyros <hyros@cesnet.cz>
 */

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/pci.h>
#include <net/xdp_sock_drv.h>

#include "ctrl_xdp_common.h"

/**
 * @brief Tries to rexmit pp page. Frees page on fail via xdp_return_buff (gets the page_pool from mem info)
 * 
 * @param ctrl 
 * @param xdp 
 * @return int 
 */
static inline int nfb_xctrl_rexmit_pp(struct xctrl *ctrl, struct xdp_buff *xdp)
{
	dma_addr_t dma;
	u32 free_desc;
	u32 len;
	u32 min_len = ETH_ZLEN;
	int ret = 0;

	// ctrl vars
	struct nc_ndp_desc *descs = ctrl->desc_buffer_virt;
	u32 sdp = ctrl->c.sdp;
	u32 mdp = ctrl->c.mdp;
	u64 last_upper_addr = ctrl->c.last_upper_addr;

	// handle small packets
	len = max((u32)(xdp->data_end - xdp->data), min_len);
	// it is assumed here that we have whole page and that the headroom is XDP_PACKET_HEADROOM
	if (xdp->data_end - xdp->data < min_len) {
		memset(xdp->data_end, 0, ETH_ZLEN);
	}

	spin_lock(&ctrl->tx.tx_lock);
	{
		// Reclaim tx buffers
		nc_ndp_ctrl_hdp_update(&ctrl->c);
		nfb_xctrl_tx_free_buffers(ctrl);

		free_desc = (ctrl->tx.fdp - sdp - 1) & mdp;

		dma = page_pool_get_dma_addr(virt_to_page(xdp->data_hard_start)) + XDP_PACKET_HEADROOM;

		if (unlikely(NDP_CTRL_DESC_UPPER_ADDR(dma) != last_upper_addr)) {
			if (unlikely(free_desc < 2)) {
				printk(KERN_ERR "nfb: %s busy warning\n", __func__);
				ret = -EBUSY;
				goto exit_free;
			}

			last_upper_addr = NDP_CTRL_DESC_UPPER_ADDR(dma);
			ctrl->c.last_upper_addr = last_upper_addr;
			descs[sdp] = nc_ndp_tx_desc0(dma);
			ctrl->tx.buffers[sdp].type = NFB_XCTRL_BUFF_DESC_TYPE0;
			free_desc--;
			sdp = (sdp + 1) & mdp;
		}

		if (unlikely(free_desc == 0)) {
			printk(KERN_ERR "nfb: %s busy warning\n", __func__);
			ret = -EBUSY;
			goto exit_free;
		}

		ctrl->tx.buffers[sdp].type = NFB_XCTRL_BUFF_FRAME_PP;
		// Here the xdp buffer is located on the ctrl.rx.xdp_ring
		// converting buff to frame writes the metadata into XDP_PACKET_HEADROOM
		// this frees the ctrl.rx.xdp_ring buffer to be reused
		ctrl->tx.buffers[sdp].frame = xdp_convert_buff_to_frame(xdp);
		descs[sdp] = nc_ndp_tx_desc2(dma, len, 0, 0);
		dma_sync_single_for_device(ctrl->dma_dev, dma, len, DMA_BIDIRECTIONAL);
		sdp = (sdp + 1) & mdp;

		// update ctrl
		ctrl->c.sdp = sdp;

exit_free:
		if (unlikely(ret)) {
			// xdp_return_buff definition is missing in 4.18.0-477.10.1.el8_8.x86_64
			// xdp_return_buff(xdp);
			xdp_return_frame(xdp_convert_buff_to_frame(xdp));
		}

		// flush counters when done
		nc_ndp_ctrl_sdp_flush(&ctrl->c);
	}
	spin_unlock(&ctrl->tx.tx_lock);
	return ret;
}

/**
 * @brief Page pool operation to fill card with rx descriptors.
 * 
 * @param ctrl 
 * @param pool struct page_pool
 * @return int number of descriptors fileld
 */
static inline int nfb_xctrl_rx_fill_pp(struct xctrl *ctrl)
{
	const u32 batch_size = NFB_XDP_CTRL_PACKET_BURST;

	// ctrl vars
	u64 last_upper_addr = ctrl->c.last_upper_addr;
	u32 mdp = ctrl->c.mdp;
	u32 sdp = ctrl->c.sdp;
	u32 mhp = ctrl->c.mhp;
	u32 php = ctrl->rx.php;

	// NOTE: XDP should have reserved tailroom for passing the page to the network stack
	const u32 frame_len = PAGE_SIZE - SKB_DATA_ALIGN(XDP_PACKET_HEADROOM + sizeof(struct skb_shared_info));
	struct page_pool *pool = ctrl->rx.pp.pool;
	struct page *page;
	dma_addr_t dma;
	struct nc_ndp_desc *descs = ctrl->desc_buffer_virt;
	u32 free_desc, free_hdrs;
	u32 i;

	// Check if refill needed
	nc_ndp_ctrl_hdp_update(&ctrl->c);
	free_hdrs = (ctrl->c.shp - php - 1) & mhp;
	free_desc = (ctrl->c.hdp - sdp - 1) & mdp;
	if (free_hdrs < batch_size || free_desc < batch_size)
		return 0;

	// Alloc buffers and send them to card
	for (i = 0; i < batch_size; i++) {
		if (!(page = page_pool_dev_alloc_pages(pool))) {
			printk(KERN_WARNING "nfb: failed to allocate page from page pool\n");
			break;
		}
		dma = page_pool_get_dma_addr(page) + XDP_PACKET_HEADROOM;
		if (unlikely(NDP_CTRL_DESC_UPPER_ADDR(dma) != last_upper_addr)) {
			if (unlikely(free_desc == 0)) {
				page_pool_put_full_page(pool, page, false);
				break;
			}
			last_upper_addr = NDP_CTRL_DESC_UPPER_ADDR(dma);
			ctrl->c.last_upper_addr = last_upper_addr;
			descs[sdp] = nc_ndp_rx_desc0(dma);
			sdp = (sdp + 1) & mdp;
			free_desc--;
		}
		if (unlikely(free_desc == 0)) {
			page_pool_put_full_page(pool, page, false);
			break;
		}
		xdp_init_buff(ctrl->rx.pp.xdp_ring[php], PAGE_SIZE, &ctrl->rx.rxq_info);
		xdp_prepare_buff(ctrl->rx.pp.xdp_ring[php], page_to_virt(page), XDP_PACKET_HEADROOM, 0, false);
		descs[sdp] = nc_ndp_rx_desc2(dma, frame_len, 0);
		sdp = (sdp + 1) & mdp;
		php = (php + 1) & mhp;
		free_desc--;
	}

	// update ctrl state
	ctrl->rx.php = php;
	ctrl->c.sdp = sdp;

	return i;
}

/**
 * @brief XDP handler
 * 
 * @param prog 
 * @param xdp 
 * @param rxq 
 * @return result
 */
static inline void nfb_xctrl_handle_pp(struct bpf_prog *prog, struct xdp_buff *xdp, struct nfb_xdp_queue *rxq)
{
	unsigned act;
	int ret;
	struct bpf_prog *xdp_prog;
	struct nfb_xdp_channel *channel = container_of(rxq, struct nfb_xdp_channel, rxq);
	struct nfb_ethdev *ethdev = channel->ethdev;
	struct sk_buff *skb;

	rcu_read_lock();
	xdp_prog = rcu_dereference(prog);
	if (xdp_prog) {
		act = bpf_prog_run_xdp(xdp_prog, xdp);
	} else {
		act = XDP_PASS;
	}

	switch (act) {
	case XDP_PASS:
		// NOTE: This function does a lot of things internally, check it's implementation
		// if you ever want to add support for fragmented packets
		skb = xdp_build_skb_from_frame(xdp_convert_buff_to_frame(xdp), ethdev->netdev);
		if (IS_ERR_OR_NULL(skb)) {
			printk(KERN_DEBUG "SKB build failed\n");
			goto aborted;
		}
		// receive packet onto queue it arriverd on
		skb_record_rx_queue(skb, channel->index);
		// TODO: gro_receive is supposed to be a free
		// 		performance boost but it is quite bad for
		//		debugging because you cannot check if all packet
		//		received are the same as sent
		// napi_gro_receive(napi, skb);
		// Send packet to the kernel network stack, it will be freed via xdp_return_frame
		netif_receive_skb(skb);
		// TODO: future extension
		// if (netif_receive_skb(skb) != NET_RX_DROP) {
		// 	ctrl.stats = ...
		// } else {
		// 	ctrl.stats = ...
		// }
		break;
	case XDP_TX:
		// returned via xdp_return_frame on tx reclaim;
		nfb_xctrl_rexmit_pp(channel->txq.ctrl, xdp);
		break;
	case XDP_REDIRECT:
		// redirected packet is internally returned via xdp_return_frame
		ret = xdp_do_redirect(ethdev->netdev, xdp, xdp_prog);
		if (unlikely(ret)) {
			printk(KERN_INFO "nfb: xdp_do_redirect error ret: %d, maybe RX cannot keep up.\n", ret);
			goto aborted;
		}
		break;
	default:
		fallthrough;
	case XDP_ABORTED:
aborted:
		printk(KERN_ERR "nfb: %s packet aborted\n", __func__);
		fallthrough;
	case XDP_DROP:
		// TODO: add this into autoconf
		// xdp_return_buff definition is missing in 4.18.0-477.10.1.el8_8.x86_64
		// xdp_return_buff(xdp);
		xdp_return_frame(xdp_convert_buff_to_frame(xdp));
		break;
	}
	rcu_read_unlock();
}

static inline u16 nfb_xctrl_rx_pp(struct xctrl *ctrl, struct xdp_buff **buffs, u16 nb_pkts)
{
	struct nc_ndp_hdr *hdrs = ctrl->rx.hdr_buffer_cpu;
	struct nc_ndp_hdr *hdr;
	u32 i;
	u16 nb_rx;
	u32 shp = ctrl->c.shp;
	u32 mhp = ctrl->c.mhp;

	// fill the card with empty buffers
	while (nfb_xctrl_rx_fill_pp(ctrl))
		;
	nc_ndp_ctrl_sdp_flush(&ctrl->c);

	// Get the amount of packets ready to be processed
	nc_ndp_ctrl_hhp_update(&ctrl->c);
	nb_rx = (ctrl->c.hhp - shp) & mhp;
	if (nb_pkts < nb_rx)
		nb_rx = nb_pkts;

	// ready packets for receive
	for (i = 0; i < nb_rx; ++i) {
		hdr = &hdrs[shp];
		buffs[i] = ctrl->rx.pp.xdp_ring[shp];
		dma_sync_single_for_cpu(ctrl->dma_dev, page_pool_get_dma_addr(virt_to_page(buffs[i]->data_hard_start)), PAGE_SIZE, DMA_BIDIRECTIONAL);
		buffs[i]->data_end = buffs[i]->data + hdr->frame_len;
		// NOTE: Right now fragmented buffers are not supported
		// Setting this is important if SKB is to be built from pagepool page without copying
		// xdp_get_shared_info_from_buff(buffs[i])->nr_frags = 0;
		shp = (shp + 1) & mhp;
	}

	// update ctrl state
	ctrl->c.shp = shp;

	return nb_rx;
}

int nfb_xctrl_napi_poll_pp(struct napi_struct *napi, int budget)
{
	struct nfb_xdp_queue *rxq = container_of(napi, struct nfb_xdp_queue, napi_pp);
	struct xctrl *ctrl = rxq->ctrl;
	struct net_device *netdev = napi->dev;
	struct nfb_ethdev *ethdev = netdev_priv(netdev);
	unsigned received, i = 0;
	struct xdp_buff *xdp[NAPI_POLL_WEIGHT];

	if (unlikely(budget > NAPI_POLL_WEIGHT)) {
		printk(KERN_ERR "nfb: NAPI budget is bigger than weight. This is a driver bug.\n");
		BUG();
	}

	received = nfb_xctrl_rx_pp(ctrl, xdp, budget);
	for (i = 0; i < received; i++) {
		nfb_xctrl_handle_pp(ethdev->prog, xdp[i], rxq);
	}
	// flush sdp and shp after software processing is done
	nc_ndp_ctrl_sp_flush(&ctrl->c);

	// Flushes redirect maps
	xdp_do_flush();

	// Work not done -> reschedule if budget remains
	if (received == budget)
		return budget;

	// Work done -> finish
	napi_complete_done(napi, received);
	return received;
}

// TODO: asynchronous stopping (waiting synchronously for timeout is terrible)
static void nfb_xctrl_stop_pp(struct xctrl *ctrl)
{
	int cnt, i = 0;
	int err;
	u32 count;
	u32 shp = ctrl->c.shp;
	u32 mhp = ctrl->c.mhp;
	u32 status;

	do {
		status = nfb_comp_read32(ctrl->c.comp, NDP_CTRL_REG_STATUS);
		if (status & NDP_CTRL_REG_STATUS_RUNNING) {
			err = nc_ndp_ctrl_stop(&ctrl->c);
			if (err != -EAGAIN && err != -EINPROGRESS)
				break;

			if (ctrl->type == NFB_XCTRL_RX) {
				// receive packets from card and try again
				nc_ndp_ctrl_hhp_update(&ctrl->c);
				count = (ctrl->c.hhp - shp) & mhp;
				for (i = 0; i < count; ++i) {
					// Returns pages to page_pool
					// xdp_return_buff definition is missing in 4.18.0-477.10.1.el8_8.x86_64
					// xdp_return_buff(ctrl->rx.pp.xdp_ring[shp]);
					xdp_return_frame(xdp_convert_buff_to_frame(ctrl->rx.pp.xdp_ring[shp]));
					shp = (shp + 1) & mhp;
				}
				ctrl->c.shp = shp;
				nc_ndp_ctrl_sp_flush(&ctrl->c);
				err = nc_ndp_ctrl_stop(&ctrl->c);
				if (err != -EAGAIN && err != -EINPROGRESS)
					break;
			}
			mdelay(1);
		}
	} while (cnt++ < 100);

	if (err) {
		err = nc_ndp_ctrl_stop_force(&ctrl->c);
		printk("nfb: queue id %u didn't stop in 100 msecs; Force stopping dma ctrl; This might damage firmware.\n", ctrl->nfb_queue_id);
	}
}

struct xctrl *nfb_xctrl_alloc_pp(struct net_device *netdev, u32 queue_id, u32 desc_cnt, enum xdp_ctrl_type type)
{
	struct nfb_ethdev *ethdev = netdev_priv(netdev);
	struct nfb_device *nfb = ethdev->nfb;
	struct nfb_xdp_channel *channel = &ethdev->channels[queue_id];
	struct nfb_xdp_queue *queue;
	struct xctrl *ctrl;
	int fdt_offset;
	u32 i;
	struct xdp_buff *buffs;

	struct page_pool_params ppp = {
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.dev = &nfb->pci->dev,
		.dma_dir = DMA_BIDIRECTIONAL,
		.max_len = PAGE_SIZE,
		.offset = 0,
		.order = 0,
		.pool_size = desc_cnt,
	};
	int err;

	// Finding the fdt offset
	switch (type) {
	case NFB_XCTRL_RX:
		queue = &channel->rxq;
		fdt_offset = nfb_comp_find(nfb, "netcope,dma_ctrl_ndp_rx", channel->nfb_index);
		break;
	case NFB_XCTRL_TX:
		queue = &channel->txq;
		fdt_offset = nfb_comp_find(nfb, "netcope,dma_ctrl_ndp_tx", channel->nfb_index);
		break;
	default:
		err = -EINVAL;
		goto fdt_offset_fail;
		break;
	}
	if (fdt_offset < 0) {
		err = -ENODEV;
		goto fdt_offset_fail;
	}

	ppp.nid = channel->numa;

	// Allocating struct
	if (!(ctrl = kzalloc_node(sizeof(struct xctrl), GFP_KERNEL, channel->numa))) {
		err = -ENOMEM;
		goto ctrl_alloc_fail;
	}

	ctrl->type = type;
	ctrl->nfb_queue_id = channel->nfb_index;
	ctrl->netdev_queue_id = channel->index;
	ctrl->dma_dev = &nfb->pci->dev;
	ctrl->nb_desc = desc_cnt;

	// Allocating control buffers
	switch (type) {
	case NFB_XCTRL_RX:
		if (!(ctrl->rx.pp.xdp_ring = kzalloc_node(sizeof(struct xdp_buff *) * desc_cnt, GFP_KERNEL, channel->numa))) {
			err = -ENOMEM;
			goto buff_alloc_fail;
		}
		if (!(buffs = kzalloc_node(sizeof(struct xdp_buff) * desc_cnt, GFP_KERNEL, channel->numa))) {
			err = -ENOMEM;
			goto buffs_alloc_fail;
		}
		break;
	case NFB_XCTRL_TX:
		spin_lock_init(&ctrl->tx.tx_lock);
		if (!(ctrl->tx.buffers = kzalloc_node(sizeof(struct xctrl_tx_buffer) * desc_cnt, GFP_KERNEL, channel->numa))) {
			err = -ENOMEM;
			goto buff_alloc_fail;
		}
		break;
	default:
		err = -EINVAL;
		goto fdt_offset_fail;
		break;
	}

	// Allocating DMA buffers
	if (!(ctrl->desc_buffer_virt = dma_alloc_coherent(ctrl->dma_dev, ctrl->nb_desc * sizeof(struct nc_ndp_desc), &ctrl->desc_buffer_dma, GFP_KERNEL))) {
		err = -ENOMEM;
		goto dma_data_fail;
	}
	if (!(ctrl->update_buffer_virt = dma_alloc_coherent(ctrl->dma_dev, sizeof(u32) * 2, &ctrl->update_buffer_dma, GFP_KERNEL))) {
		err = -ENOMEM;
		goto dma_update_fail;
	}

	if (type == NFB_XCTRL_RX) {
		ctrl->rx.nb_hdr = desc_cnt;
		if (!(ctrl->rx.hdr_buffer_cpu = dma_alloc_coherent(ctrl->dma_dev, ctrl->rx.nb_hdr * sizeof(struct nc_ndp_hdr), &ctrl->rx.hdr_buffer_dma, GFP_KERNEL))) {
			err = -ENOMEM;
			goto dma_hdr_fail;
		}
	}

	// creating page pool and info
	if (type == NFB_XCTRL_RX) {
		if (!(ctrl->rx.pp.pool = page_pool_create(&ppp))) {
			printk(KERN_ERR "nfb: Failed to create pagepool\n");
			err = -ENOMEM;
			goto pp_alloc_fail;
		}
		if ((err = xdp_rxq_info_reg(&ctrl->rx.rxq_info, netdev, channel->index, 0))) {
			printk(KERN_ERR "nfb: rx_info register fail with: %d\n", err);
			goto meminfo_reg_fail;
		}
		if ((err = xdp_rxq_info_reg_mem_model(&ctrl->rx.rxq_info, MEM_TYPE_PAGE_POOL, ctrl->rx.pp.pool))) {
			printk(KERN_ERR "nfb: mem_model register fail with: %d\n", err);
			goto meminfo_model_fail;
		}

		for (i = 0; i < desc_cnt; i++) {
			ctrl->rx.pp.xdp_ring[i] = &buffs[i];
		}
	}

	// Opening controller
	if ((err = nc_ndp_ctrl_open(nfb, fdt_offset, &ctrl->c))) {
		goto ndp_ctrl_open_fail;
	}

	return ctrl;

ndp_ctrl_open_fail:
	if (type == NFB_XCTRL_RX) {
		xdp_rxq_info_unreg_mem_model(&ctrl->rx.rxq_info);
	}
meminfo_model_fail:
	if (type == NFB_XCTRL_RX) {
		xdp_rxq_info_unreg(&ctrl->rx.rxq_info);
	}
meminfo_reg_fail:
	if (type == NFB_XCTRL_RX) {
		page_pool_destroy(ctrl->rx.pp.pool);
	}
pp_alloc_fail:
	if (type == NFB_XCTRL_RX) {
		dma_free_coherent(ctrl->dma_dev, ctrl->rx.nb_hdr * sizeof(struct nc_ndp_hdr), ctrl->rx.hdr_buffer_cpu, ctrl->rx.hdr_buffer_dma);
	}
dma_hdr_fail:
	dma_free_coherent(ctrl->dma_dev, sizeof(u32) * 2, ctrl->update_buffer_virt, ctrl->update_buffer_dma);
dma_update_fail:
	dma_free_coherent(ctrl->dma_dev, ctrl->nb_desc * sizeof(struct nc_ndp_desc), ctrl->desc_buffer_virt, ctrl->desc_buffer_dma);
dma_data_fail:
	kfree(buffs);
buffs_alloc_fail:
	switch (type) {
	case NFB_XCTRL_RX:
		kfree(ctrl->rx.pp.xdp_ring);
		break;
	case NFB_XCTRL_TX:
		kfree(ctrl->tx.buffers);
		break;
	default:
		break;
	}
buff_alloc_fail:
	kfree(ctrl);
ctrl_alloc_fail:
fdt_offset_fail:
	printk(KERN_ERR "nfb: Error opening dma ctrl on queue %d; %d", channel->nfb_index, err);
	return NULL;
}

void nfb_xctrl_destroy_pp(struct xctrl *ctrl)
{
	if (test_bit(XCTRL_STATUS_IS_RUNNING, &ctrl->status)) {
		nfb_xctrl_stop_pp(ctrl);
	}
	nc_ndp_ctrl_close(&ctrl->c);
	dma_free_coherent(ctrl->dma_dev, ctrl->nb_desc * sizeof(struct nc_ndp_desc), ctrl->desc_buffer_virt, ctrl->desc_buffer_dma);
	dma_free_coherent(ctrl->dma_dev, sizeof(u32) * 2, ctrl->update_buffer_virt, ctrl->update_buffer_dma);
	switch (ctrl->type) {
	case NFB_XCTRL_RX:
		dma_free_coherent(ctrl->dma_dev, ctrl->rx.nb_hdr * sizeof(struct nc_ndp_hdr), ctrl->rx.hdr_buffer_cpu, ctrl->rx.hdr_buffer_dma);
		// Unreg_mem_model calls page_pool_destroy internally
		// page_pool_destroy(ctrl->rx.pp.pool);
		xdp_rxq_info_unreg_mem_model(&ctrl->rx.rxq_info);
		xdp_rxq_info_unreg(&ctrl->rx.rxq_info);
		kfree(ctrl->rx.pp.xdp_ring[0]);
		kfree(ctrl->rx.pp.xdp_ring);
		break;
	case NFB_XCTRL_TX:
		// free all enqueued tx buffers
		ctrl->c.hdp = ctrl->c.sdp;
		nfb_xctrl_tx_free_buffers(ctrl);
		kfree(ctrl->tx.buffers);
		break;
	default:
		break;
	}
	kfree(ctrl);
	ctrl = NULL;
}
