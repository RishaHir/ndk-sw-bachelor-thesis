/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * libnfb - data transmission module
 *
 * Copyright (C) 2017-2022 CESNET
 * Author(s):
 *   Martin Spinler <spinler@cesnet.cz>
 */

#include <netcope/ndp_core_queue.h>
#include <netcope/ndp.h>
#include <netcope/rxqueue.h>
#include <netcope/txqueue.h>

void *nfb_nalloc(int numa_node, size_t size)
{
#ifndef __KERNEL__
	if (numa_node == -1)
		return (malloc(size));

	return (numa_alloc_onnode(size, numa_node));
#else
	return kmalloc_node(size, GFP_KERNEL, numa_node);
#endif
}

void nfb_nfree(int numa_node, void *ptr, size_t size)
{
#ifndef __KERNEL__
	if (numa_node == -1)
		free(ptr);
	else
		numa_free(ptr, size);
#else
	kfree(ptr);
#endif
}

void _ndp_queue_init(struct ndp_queue *q, struct nfb_device *dev, int numa, int dir, int index)
{
	memset(q, 0, sizeof(struct ndp_queue));

	q->numa = numa;
	q->dir = dir;
	q->dev = dev;

	q->index = index;

	q->status = NDP_QUEUE_STOPPED;
#ifdef __KERNEL__
	q->alloc = 0;
#endif
}

struct ndp_queue * ndp_queue_create(struct nfb_device *dev, int numa, int dir, int index)
{
	struct ndp_queue *q;

	/* Create queue structure */
	q = nfb_nalloc(numa, sizeof(*q));
	if (q == NULL) {
		goto err_nalloc;
	}
	_ndp_queue_init(q, dev, numa, dir, index);

#ifdef __KERNEL__
	q->alloc = 1;
#endif

	return q;

err_nalloc:
	return NULL;
}

void ndp_queue_destroy(struct ndp_queue* q)
{
#ifdef __KERNEL__
	if (!q->alloc)
		return;
#endif
	nfb_nfree(q->numa, q, sizeof(*q));
}

void* ndp_queue_get_priv(struct ndp_queue *q)
{
	return q->priv;
}

void ndp_queue_set_priv(struct ndp_queue *q, void *priv)
{
	q->priv = priv;
}

struct ndp_queue_ops* ndp_queue_get_ops(struct ndp_queue *q)
{
	return &q->ops;
}

static int nfb_queue_add(struct ndp_queue *q)
{
#ifdef __KERNEL__
//	(void*) q;
#else
	struct ndp_queue **queues;
	struct nfb_device *dev;

	dev = q->dev;
	queues = realloc(dev->queues, sizeof(*dev->queues) * (dev->queue_count + 1));
	if (queues == NULL)
		return ENOMEM;

	dev->queues = queues;
	dev->queues[dev->queue_count] = q;
	dev->queue_count++;
#endif
	return 0;
}

static void nfb_queue_remove(struct ndp_queue *q)
{
#ifndef __KERNEL__
	struct ndp_queue **queues;

	queues = q->dev->queues;
	while (*queues != q)
		queues++;
	*queues = NULL;
#endif
}

struct ndp_queue *ndp_open_queue(struct nfb_device *dev, unsigned index, int dir, int in_flags)
{
	int ret;
	int flags = 0;
	struct ndp_queue *q = NULL;

	if (in_flags & NDP_OPEN_FLAG_USERSPACE) {
		flags |= NDP_CHANNEL_FLAG_EXCLUSIVE | NDP_CHANNEL_FLAG_USERSPACE;
	}

#ifndef __KERNEL__
	if (!dev->ops.ndp_queue_open || !dev->ops.ndp_queue_close) {
		ret = -ENXIO;
		goto err_dev_ops_invalid;
	}
#endif

#ifdef __KERNEL__
	if ((ret = ndp_base_queue_open(dev, NULL, index, dir, flags, &q))) {
#else
	if ((ret = dev->ops.ndp_queue_open(dev, dev->priv, index, dir, flags, &q))) {
#endif
		goto err_ndp_queue_open_init;
	}

	if (q->ops.control.start == NULL || q->ops.control.stop == NULL ||
			q->ops.burst.tx.get == NULL || q->ops.burst.tx.put == NULL ||
			(q->ops.burst.tx.flush == NULL && q->dir != NDP_CHANNEL_TYPE_RX)) {
		ret = -EINVAL;
		goto err_ops_invalid;
	}

	if ((ret = nfb_queue_add(q))) {
		goto err_nfb_queue_add;
	}

	return q;

err_nfb_queue_add:
err_ops_invalid:
#ifdef __KERNEL__
	ndp_base_queue_close(q->priv);
#else
	dev->ops.ndp_queue_close(q->priv);
#endif
err_ndp_queue_open_init:
#ifndef __KERNEL__
err_dev_ops_invalid:
	errno = ret;
#endif
	return NULL;
}

struct ndp_queue *ndp_open_rx_queue_ext(struct nfb_device *dev, unsigned index, ndp_open_flags_t flags)
{
	return ndp_open_queue(dev, index, NDP_CHANNEL_TYPE_RX, flags);
}

struct ndp_queue *ndp_open_rx_queue(struct nfb_device *dev, unsigned index)
{
	return ndp_open_rx_queue_ext(dev, index, 0);
}

struct ndp_queue *ndp_open_tx_queue_ext(struct nfb_device *dev, unsigned index, ndp_open_flags_t flags)
{
	return ndp_open_queue(dev, index, NDP_CHANNEL_TYPE_TX, flags);
}

struct ndp_queue *ndp_open_tx_queue(struct nfb_device *dev, unsigned index)
{
	return ndp_open_tx_queue_ext(dev, index, 0);
}

void ndp_close_queue(struct ndp_queue *q)
{
	ndp_queue_stop(q);

	nfb_queue_remove(q);

#ifdef __KERNEL__
	ndp_base_queue_close(q->priv);
#else
	q->dev->ops.ndp_queue_close(q->priv);
#endif
}

void ndp_close_rx_queue(struct ndp_queue *q)
{
	ndp_close_queue(q);
}

void ndp_close_tx_queue(struct ndp_queue *q)
{
	ndp_close_queue(q);
}

int ndp_queue_get_numa_node(const struct ndp_queue *q)
{
	return q->numa;
}

static inline int fdt_get_subnode_count(const void *fdt, const char *path)
{
	int count = 0;
	int node;
	int fdt_offset;

	fdt_offset = fdt_path_offset(fdt, path);
	fdt_for_each_subnode(node, fdt, fdt_offset) {
		count++;
	}
	return count;
}

#ifndef __KERNEL__
int ndp_get_rx_queue_count(const struct nfb_device *dev)
{
	if (fdt_path_offset(dev->fdt, "/driver/ndp/") >= 0) {
		return fdt_get_subnode_count(dev->fdt, "/drivers/ndp/rx_queues");
	} else {
		return
				nfb_comp_count(dev, COMP_NETCOPE_RXQUEUE_SZE) +
				nfb_comp_count(dev, COMP_NETCOPE_RXQUEUE_NDP) +
				nfb_comp_count(dev, COMP_NETCOPE_RXQUEUE_CALYPTE);
	}
}

int ndp_get_tx_queue_count(const struct nfb_device *dev)
{
	if (fdt_path_offset(dev->fdt, "/driver/ndp/") >= 0) {
		return fdt_get_subnode_count(dev->fdt, "/drivers/ndp/tx_queues");
	} else {
		return
				nfb_comp_count(dev, COMP_NETCOPE_TXQUEUE_SZE) +
				nfb_comp_count(dev, COMP_NETCOPE_TXQUEUE_NDP) +
				nfb_comp_count(dev, COMP_NETCOPE_TXQUEUE_CALYPTE);
	}
}

int ndp_queue_is_available(const struct nfb_device *dev, unsigned index, int dir)
{
	int fdt_offset;
	size_t prop;
	char path[64];

	const char *dir_str = dir ? "tx" : "rx";
	const void *fdt = nfb_get_fdt(dev);

	snprintf(path, 64, "/drivers/ndp/%s_queues/%s%u", dir_str, dir_str, index);

	/* Parse base values from FDT */
	fdt_offset = fdt_path_offset(fdt, path);
	if (fdt_offset < 0)
		return 0;

	/* Get mmap size */
	if (fdt_getprop64(fdt, fdt_offset, "mmap_size", &prop))
		return 0;
	return prop == 0 ? 0 : 1;
}

int ndp_rx_queue_is_available(const struct nfb_device *dev, unsigned index)
{
	return ndp_queue_is_available(dev, index, NDP_CHANNEL_TYPE_RX);
}

int ndp_tx_queue_is_available(const struct nfb_device *dev, unsigned index)
{
	return ndp_queue_is_available(dev, index, NDP_CHANNEL_TYPE_TX);
}

int ndp_get_rx_queue_available_count(const struct nfb_device *dev)
{
	int i;
	int count = 0;
	int total = ndp_get_rx_queue_count(dev);

	for (i = 0; i < total; i++) {
		if (ndp_rx_queue_is_available(dev, i))
			count++;
	}
	return count;
}

int ndp_get_tx_queue_available_count(const struct nfb_device *dev)
{
	int i;
	int count = 0;
	int total = ndp_get_tx_queue_count(dev);

	for (i = 0; i < total; i++) {
		if (ndp_tx_queue_is_available(dev, i))
			count++;
	}
	return count;
}
#endif

int ndp_queue_start(struct ndp_queue *q)
{
	int ret;

	if (q->status == NDP_QUEUE_RUNNING)
		return EALREADY;

	ret = q->ops.control.start(q->priv);
	if (ret)
		return ret;

	q->status = NDP_QUEUE_RUNNING;
	return 0;
}

int ndp_queue_stop(struct ndp_queue *q)
{
	int ret;

	if (q->status == NDP_QUEUE_STOPPED)
		return EALREADY;

	if (q->dir == NDP_CHANNEL_TYPE_TX)
		q->ops.burst.tx.flush(q->priv);

	ret = q->ops.control.stop(q->priv);
	if (ret)
		return ret;

	q->status = NDP_QUEUE_STOPPED;
	return 0;
}

unsigned ndp_rx_burst_get(struct ndp_queue *q, struct ndp_packet *packets, unsigned count)
{
	return q->ops.burst.rx.get(q->priv, packets, count);
}

void ndp_rx_burst_put(struct ndp_queue *q)
{
	q->ops.burst.rx.put(q->priv);
}

unsigned ndp_tx_burst_get(struct ndp_queue *q, struct ndp_packet *packets, unsigned count)
{
	return q->ops.burst.tx.get(q->priv, packets, count);
}

void ndp_tx_burst_put(struct ndp_queue *q)
{
	q->ops.burst.tx.put(q->priv);
}

void ndp_tx_burst_flush(struct ndp_queue *q)
{
	q->ops.burst.tx.flush(q->priv);
}

#ifndef __KERNEL__

#define NDP_TX_BURST_COPY_ATTEMPTS 1000

unsigned ndp_tx_burst_copy(struct ndp_queue *q, struct ndp_packet *packets, unsigned count)
{
	struct ndp_packet *our_packets;
	unsigned our_burst_count;
	unsigned packets_sent = 0;
	unsigned attempts = 0;

	our_packets = malloc(sizeof(our_packets[0]) * count);
	if (!our_packets) {
		return 0; /* -ENOMEM */
	}

	for (unsigned i = 0; i < count; i++) {
		our_packets[i].header_length = 0;
		our_packets[i].data_length = packets[i].data_length;
	}

	while (packets_sent < count && attempts < NDP_TX_BURST_COPY_ATTEMPTS) {
		our_burst_count = ndp_tx_burst_get(q, our_packets + packets_sent, count - packets_sent);

		for (unsigned i = 0; i < our_burst_count; i++) {
			memcpy(our_packets[packets_sent + i].data, packets[packets_sent + i].data,
					our_packets[packets_sent + i].data_length);
		}

		ndp_tx_burst_put(q);

		packets_sent += our_burst_count;
		attempts++;
	}

	free(our_packets);

	return packets_sent;
}

int ndp_rx_poll(struct nfb_device *dev, int timeout, struct ndp_queue **q)
{
	int ret = -ENXIO;
	(void) dev;
	(void) timeout;
	(void) q;

#if 0
	int i;
	struct pollfd pfd;
	size_t size, max_size = 0;

	for (i = 0; i < dev->queue_count; i++) {
		if (dev->queues[i] == NULL)
			continue;
		if (dev->queues[i]->version == 2) {
			/* TODO */
		} else if (dev->queues[i]->version == 1) {
			nc_ndp_v1_rx_unlock(dev->queues[i]);
		}
	}

	pfd.fd = dev->fd;
	pfd.events = POLLIN;

	ret = poll(&pfd, 1, timeout);
	if (ret <= 0) {
		if (ret < 0) {
			ret = -errno;
		}
		goto err;
	}
	if (pfd.revents & POLLERR) {
		ret = -1;
		goto err;
	}

	ret = 0;
	if (q) {
		/* Find queue with biggest amount of data */
		for (i = 0; i < dev->queue_count; i++) {
			if (dev->queues[i] == NULL)
				continue;
			size = 0;
			if (dev->queues[i]->version == 2) {
				/* TODO */
			} else if (dev->queues[i]->version == 1) {
				nc_ndp_v1_rx_lock(dev->queues[i]);
				size = dev->queues[i]->u.v1.bytes;
			}
			if (size)
				ret++;
			if (size > max_size) {
				*q = dev->queues[i];
				max_size = size;
			}
		}
	}

	return ret;
err:
#endif
	return ret;
}
#endif
