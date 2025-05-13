/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */
/*
 * XDP driver of the NFB platform - sysfs
 *
 * Copyright (C) 2025 CESNET
 * Author(s):
 *   Richard Hyros <hyros@cesnet.cz>
 */

#include "sysfs.h"
#include "driver.h"

#include <linux/pci.h>
#include <linux/netdevice.h>

// -------------------- SYSFS files for the MODULE - top level information ---------------

static ssize_t channel_total_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nfb_xdp *module = dev_get_drvdata(dev);
	if (module->rxqc != module->txqc)
		return -EINVAL;

	return sysfs_emit(buf, "%d\n", module->rxqc);
}
static DEVICE_ATTR_RO(channel_total);

static ssize_t ethdev_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nfb_xdp *module = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", module->ethc);
}
static DEVICE_ATTR_RO(ethdev_count);

struct attribute *nfb_module_attrs[] = {
	&dev_attr_channel_total.attr,
	&dev_attr_ethdev_count.attr,
	NULL,
};
ATTRIBUTE_GROUPS(nfb_module);

void nfb_xdp_sysfs_init_module_attributes(struct nfb_xdp *module)
{
	struct device *dev = &module->dev;
	dev->groups = nfb_module_groups;
}

void nfb_xdp_sysfs_deinit_module(struct nfb_ethdev *ethdev)
{
	return;
}

// --------------------------- SYSFS files for each ethdev -------------------------------- 

static ssize_t channel_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nfb_ethdev *ethdev = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", ethdev->channel_count);
}
static DEVICE_ATTR_RO(channel_count);

static ssize_t channel_offset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nfb_ethdev *ethdev = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", ethdev->channel_offset);
}
static DEVICE_ATTR_RO(channel_offset);

static ssize_t ifname_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nfb_ethdev *ethdev = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%s\n", netdev_name(ethdev->netdev));
}
static DEVICE_ATTR_RO(ifname);

struct attribute *nfb_ethdev_attrs[] = {
	&dev_attr_channel_count.attr,
	&dev_attr_channel_offset.attr,
	&dev_attr_ifname.attr,
	NULL,
};
ATTRIBUTE_GROUPS(nfb_ethdev);

int nfb_xdp_sysfs_init_ethdev(struct nfb_ethdev *ethdev)
{
	struct device *dev = &ethdev->sysfsdev;
	device_initialize(dev); 
	dev->parent = &ethdev->module->dev;
	dev->groups = nfb_ethdev_groups;
	dev_set_name(dev, "ethdev%d", ethdev->index);
	dev_set_drvdata(dev, ethdev);
	return device_add(dev);
}

void nfb_xdp_sysfs_deinit_ethdev(struct nfb_ethdev *ethdev)
{
	device_del(&ethdev->sysfsdev);
}
