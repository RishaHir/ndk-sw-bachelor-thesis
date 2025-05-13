/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * XDP common functions
 * Mapping NDK DMA queues to AF_XDP channels
 * via sysfs interface
 *  
 * Copyright (C) 2025 CESNET
 * Author(s):
 *   Richard Hyroš <hyros@cesnet.cz>
 */

#include <stdlib.h>
#include <stdio.h>

#include <nfb/nfb.h>

#include "xdp_common.h"

#define PATH_BUFF 128
#define MAX_ETHDEV 16

struct ethdev {
	char ifname[IF_NAMESIZE];
	int channel_count;
	int channel_offset;
};

// Read int from sysfs
static void xdp_mode_common_sysfs_int(const char *sysfs_path, const char *postfix, int *ret) {
	FILE *file;
	char path[PATH_BUFF];
	int err;
	err = snprintf(path, sizeof(path), "%s%s", sysfs_path, postfix);
	if (err < 0 || err >= PATH_BUFF) {
		fprintf(stderr,"Failed to parse sysfs path\n");
		exit(-1);
	}
	if (!(file = fopen(path , "r"))) {
		fprintf(stderr,"Failed to open %s; Is the XDP driver loaded?\n", path);
		exit(-1);
	}
	err = fscanf(file, "%d", ret);
	if(err == EOF) {
		fprintf(stderr,"Failed to read %s\n", path);
		fclose(file);
		exit(-1);
	}
	fclose(file);
}

// Read IFNAME sized string from sysfs
static void xdp_mode_common_sysfs_ifname(const char *sysfs_path, const char *postfix, char *ret) {
	FILE *file;
	char path[PATH_BUFF];
	int err;
	err = snprintf(path, sizeof(path), "%s%s", sysfs_path, postfix);
	if (err < 0 || err >= PATH_BUFF) {
		fprintf(stderr,"Failed to parse sysfs path\n");
		exit(-1);
	}
	if (!(file = fopen(path , "r"))) {
		fprintf(stderr,"Failed to open %s; Is the XDP driver loaded?\n", path);
		exit(-1);
	}
	err = fscanf(file, "%s", ret);
	if(err == EOF) {
		fprintf(stderr,"Failed to read %s\n", path);
		fclose(file);
		exit(-1);
	}
	fclose(file);
}

// Map NDP queues to AF_XDP channels
int xdp_mode_common_parse_queues(struct ndp_tool_params *p) {
	struct nfb_device *nfb;
	struct ndp_mode_xdp_params *params = &p->mode.xdp;
	struct list_range *queue_range = &params->queue_range;
	int queue_count;
	int ret, nfb_system_id, ethdev_count, channel_total;
	struct ethdev eths[MAX_ETHDEV];
	char sysfs_path[PATH_BUFF];
	
	// get nfb system id
	if(!(nfb = nfb_open(p->nfb_path))) {
		fprintf(stderr,"Failed to open nfb\n");
		exit(-1);
	}
	nfb_system_id = nfb_get_system_id(nfb);
	nfb_close(nfb);

	// read xdp module sysfs
	ret = snprintf(sysfs_path, sizeof(sysfs_path), "/sys/class/nfb/nfb%d/nfb_xdp", nfb_system_id);
	if (ret < 0 || ret >= PATH_BUFF) {
		fprintf(stderr,"Failed to get sysfs path\n");
		exit(-1);
	}
	xdp_mode_common_sysfs_int(sysfs_path, "/ethdev_count", &ethdev_count);
	xdp_mode_common_sysfs_int(sysfs_path, "/channel_total", &channel_total);
	// read ethdev sysfs
	for (int eth_idx = 0; eth_idx < ethdev_count; eth_idx++) {
		// read sysfs
		ret = snprintf(sysfs_path, sizeof(sysfs_path), "/sys/class/nfb/nfb%d/nfb_xdp/ethdev%d",
			 nfb_system_id, eth_idx);
		if (ret < 0 || ret >= PATH_BUFF) {
			fprintf(stderr,"Failed to parse sysfs path for ethdev %d\n", eth_idx);
			exit(-1);
		}
		xdp_mode_common_sysfs_int(sysfs_path, "/channel_count", &eths[eth_idx].channel_count);
		xdp_mode_common_sysfs_int(sysfs_path, "/channel_offset", &eths[eth_idx].channel_offset);
		xdp_mode_common_sysfs_ifname(sysfs_path, "/ifname", eths[eth_idx].ifname);
	}

	// map the queues
	if(!(queue_count = list_range_count(queue_range))) { // Open all queues, no mapping needed
		// allocate for all the channels
		if(!(params->queue_data_arr = calloc(channel_total, sizeof(struct ndp_mode_xdp_xsk_data)))) {
			fprintf(stderr,"Failed to alloc xdp xsk_data\n");
			exit(-1);
		}
		for (int eth_idx = 0; eth_idx < ethdev_count; eth_idx++) {
			// fill the queue data
			for (int eth_qid = 0; eth_qid < eths[eth_idx].channel_count; eth_qid++) {
				int channel_offset = eths[eth_idx].channel_offset;
				char *ifname = eths[eth_idx].ifname;
				params->queue_data_arr[channel_offset + eth_qid].eth_qid = eth_qid;
				params->queue_data_arr[channel_offset + eth_qid].nfb_qid = channel_offset + eth_qid;
				strcpy(params->queue_data_arr[channel_offset + eth_qid].ifname, ifname);
			}
		}
		params->socket_cnt = channel_total;
	} else { // map the queue range to XDP queues
		// allocate queue_count
		if(!(params->queue_data_arr = calloc(queue_count, sizeof(struct ndp_mode_xdp_xsk_data)))) {
			fprintf(stderr,"Failed to alloc xdp xsk_data\n");
			exit(-1);
		}
		// iterate over nfb queues and check if they are in the queue_range
		int data_idx = 0;
		for (int nfb_qid = 0; nfb_qid < channel_total && data_idx < queue_count; nfb_qid++) {
			if (!list_range_contains(queue_range, nfb_qid))
				continue;

			// iterate over ethdevices until the corrent queue is found
			for (int eth_idx = 0; eth_idx < ethdev_count; eth_idx++) {
				for (int eth_qid = 0; eth_qid < eths[eth_idx].channel_count; eth_qid++) {
					if(nfb_qid == eth_qid + eths[eth_idx].channel_offset) {
						// fill the queue data
						params->queue_data_arr[data_idx].eth_qid = eth_qid;
						params->queue_data_arr[data_idx].nfb_qid = nfb_qid;
						strcpy(params->queue_data_arr[data_idx].ifname, eths[eth_idx].ifname);
						data_idx++;
					}
				}	
			}
		}
		params->socket_cnt = data_idx;
	}
	
	if(!params->socket_cnt) {
		fprintf(stderr,"No queues found\n");
		exit(-1);
	}

	return 0;
}