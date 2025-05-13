/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */
/*
 * XDP driver of the NFB platform - header file for sysfs
 *
 * Copyright (C) 2025 CESNET
 * Author(s):
 *   Richard Hyros <hyros@cesnet.cz>
 */

#ifndef NFB_XDP_SYSFS_H
#define NFB_XDP_SYSFS_H
#include "ethdev.h"

void nfb_xdp_sysfs_init_module_attributes(struct nfb_xdp *module);
int nfb_xdp_sysfs_init_ethdev(struct nfb_ethdev *ethdev);
void nfb_xdp_sysfs_deinit_ethdev(struct nfb_ethdev *ethdev);

#endif // NFB_XDP_SYSFS_H