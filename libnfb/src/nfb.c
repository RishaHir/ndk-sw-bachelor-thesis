/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * libnfb - base module
 *
 * Copyright (C) 2017-2022 CESNET
 * Author(s):
 *   Martin Spinler <spinler@cesnet.cz>
 */

#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include <dlfcn.h>

#include <libfdt.h>
#include <nfb/nfb.h>
#include <linux/nfb/nfb.h>

#include "nfb.h"

//#include "mi.c"

#define PATH_LEN (32)

int nfb_base_open(const char *devname, int oflag, void **priv, void **fdt);
void nfb_base_close(void *priv);
int nfb_bus_open_mi(void *dev_priv, int bus_node, int comp_node, void ** bus_priv, struct libnfb_bus_ext_ops* ops);
void nfb_bus_close_mi(void *bus_priv);
int nfb_base_comp_lock(const struct nfb_comp *comp, uint32_t features);
void nfb_base_comp_unlock(const struct nfb_comp *comp, uint32_t features);

int ndp_base_queue_open(struct nfb_device *dev, void *dev_priv, unsigned index, int dir, int flags, struct ndp_queue ** pq);
int ndp_base_queue_close(struct ndp_queue * q);

struct libnfb_ext_ops nfb_base_ops = {
	.open = nfb_base_open,
	.close = nfb_base_close,
	.bus_open_mi = nfb_bus_open_mi,
	.bus_close_mi = nfb_bus_close_mi,
	.comp_lock = nfb_base_comp_lock,
	.comp_unlock = nfb_base_comp_unlock,

	.ndp_queue_open = ndp_base_queue_open,
	.ndp_queue_close = ndp_base_queue_close,
};

const void *nfb_get_fdt(const struct nfb_device *dev)
{
	return dev->fdt;
}

struct nfb_comp *nfb_user_to_comp(void *ptr)
{
	return ((struct nfb_comp *) ptr) - 1;
}

void *nfb_comp_to_user(struct nfb_comp *ptr)
{
	return (void *)(ptr + 1);
}

const char * nfb_default_dev_path()
{
	const char *devname = getenv("LIBNFB_DEFAULT_DEV");
	if (devname == NULL) {
		devname = "/dev/nfb0";
	}
	return devname;
}

int load_lib_extension(const char * lib_name, const char * dev_name, struct nfb_device *dev)
{
	int ret = 0;

	struct libnfb_ext_abi_version *ext_abi_version;
	struct libnfb_ext_abi_version current_abi_version = libnfb_ext_abi_version_current;

	libnfb_ext_get_ops_t* get_ops;

	dev->ext_lib = dlopen(lib_name, RTLD_NOW);
	if (dev->ext_lib == NULL) {
		fprintf(stderr, "libnfb fatal: can't open extension library '%s': %s\n", lib_name, dlerror());
		ret = -ENOENT;
		goto err_dlopen;
	}

	*(void**)(&get_ops) = dlsym(dev->ext_lib, "libnfb_ext_get_ops");
	*(void**)(&ext_abi_version) = dlsym(dev->ext_lib, "libnfb_ext_abi_version");
	if (ext_abi_version == NULL) {
		fprintf(stderr, "libnfb fatal: extension doesn't have libnfb_ext_abi_version symbol.\n");
		ret = -EBADF;
		goto err_no_abi;
	}

	if (ext_abi_version->major != current_abi_version.major) {
		fprintf(stderr, "libnfb fatal: extension ABI major version doesn't match.\n");
		ret = -EBADF;
		goto err_abi_mismatch;
	}

	if (ext_abi_version->minor != current_abi_version.minor) {
		fprintf(stderr, "libnfb warning: extension ABI minor version doesn't match.\n");
	}

	if (get_ops == NULL) {
		ret = -EBADF;
		goto err_no_ops;
	}

	ret = get_ops(dev_name, &dev->ops);
	if (ret <= 0) {
		dlclose(dev->ext_lib);
		dev->ext_lib = NULL;
	}
	return ret;

err_no_ops:
err_abi_mismatch:
err_no_abi:
	dlclose(dev->ext_lib);
	dev->ext_lib = NULL;
err_dlopen:
	return ret;
}

int load_extension(const char **devname, struct nfb_device *dev)
{
	int ret;
	char *lib_name = NULL;
	const char *dev_name = *devname;
	const char *ext_name = NULL;
	const char *ext_name_end = NULL;

	const char *LIBNFB_EXT_PREFIX = "libnfb-ext:";
	const char *LIBNFB_EXT_PATTERN = "libnfb-ext-";

	/* Check for prefix */
	if (strstr(dev_name, LIBNFB_EXT_PREFIX) == dev_name) {
		ext_name = dev_name + strlen(LIBNFB_EXT_PREFIX);
		ext_name_end = strchr(ext_name, ':');
	/* Check for pattern: deprecated */
	} else if (strstr(dev_name, LIBNFB_EXT_PATTERN)) {
		ext_name = dev_name;
		ext_name_end = strchr(dev_name, ':');
	}

	if (ext_name == NULL)
		return 0;

	/* No colon means whole string for library name and empty device name */
	if (ext_name_end == NULL) {
		ext_name_end = ext_name + strlen(ext_name);
		dev_name = ext_name_end;
	} else {
		dev_name = ext_name_end + 1;
	}

	lib_name = strndup(ext_name, (ext_name_end - ext_name));
	if (!lib_name)
		return -ENOMEM;
	ret = load_lib_extension(lib_name, dev_name, dev);
	free(lib_name);

	*devname = dev_name;

	return ret;
}

struct nfb_device *nfb_open_ext(const char *devname, int oflag)
{
	int ret;
	struct nfb_device *dev;
	char path[PATH_LEN];
	unsigned index;

	if (devname == NULL)
		devname = nfb_default_dev_path();

	if (sscanf(devname, "%u%n", &index, &ret) >= 1 && (size_t) ret == strlen(devname)) {
		ret = snprintf(path, PATH_LEN, "/dev/nfb%u", index);
		if (ret >= PATH_LEN || ret < 0) {
			errno = ENODEV;
			return NULL;
		}
		devname = (const char *)path;
	}

	/* Allocate structure */
	dev = (struct nfb_device*) malloc(sizeof(struct nfb_device));
	if (dev == 0) {
		goto err_malloc_dev;
	}

	memset(dev, 0, sizeof(struct nfb_device));
	dev->fd = -1;

	ret = load_extension(&devname, dev);
	if (ret < 0) {
		goto err_ext_load;
	} else if (ret == 0) {
		dev->ops = nfb_base_ops;
	}

	if (!dev->ops.open || !dev->ops.close ||
			!dev->ops.bus_open_mi || !dev->ops.bus_close_mi ||
			!dev->ops.comp_lock || !dev->ops.comp_unlock) {
		goto err_ops;
	}

	ret = dev->ops.open(devname, oflag, &dev->priv, &dev->fdt);
	if (ret) {
		//errno = ret;
		goto err_open;
	}

	/* TODO */
	if (dev->ops.open == nfb_base_open) {
		dev->fd = ((struct nfb_base_priv*) dev->priv)->fd;
	}

	/* Check for valid FDT */
	ret = fdt_check_header(dev->fdt);
	if (ret != 0) {
		errno = EBADF;
		goto err_fdt_check_header;
	}

	return dev;

err_fdt_check_header:
	dev->ops.close(dev->priv);
	free(dev->fdt);
err_ops:
err_open:
err_ext_load:
	if (dev->ext_lib)
		dlclose(dev->ext_lib);
	free(dev);
err_malloc_dev:
	return NULL;
}

int nfb_base_open(const char *devname, int oflag, void **priv, void **fdt)
{
	int fd;
	int ret;
	off_t size;
	struct nfb_base_priv *dev;

	dev = malloc(sizeof(struct nfb_base_priv));
	if (dev == NULL)
		return -ENOMEM;

	/* Open device */
	fd = open(devname, O_RDWR | oflag, 0);
	if (fd == -1) {
		goto err_open;
	}

	/* Read FDT from driver */
	size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	if (size == 0) {
		errno = ENODEV;
		fprintf(stderr, "FDT size is zero\n");
		goto err_fdt_size;
	}
	dev->fdt = malloc(size);
	if (dev->fdt == NULL) {
		errno = ENOMEM;
		goto err_malloc_fdt;
	}
	ret = read(fd, dev->fdt, size);
	if (ret != size) {
		errno = ENODEV;
		goto err_read_fdt;
	}

	dev->fd = fd;

	*fdt = dev->fdt;
	*priv = dev;
	return 0;

err_read_fdt:
	free(dev->fdt);
err_malloc_fdt:
err_fdt_size:
	close(fd);
err_open:
	free(dev);
	return errno;
}

void nfb_base_close(void *priv)
{
	struct nfb_base_priv *dev = priv;
	close(dev->fd);
	free(dev);
}

struct nfb_device *nfb_open(const char *devname)
{
	return nfb_open_ext(devname, 0);
}

void nfb_close(struct nfb_device *dev)
{
	dev->ops.close(dev->priv);
	if (dev->queues)
		free(dev->queues);

	if (dev->ext_lib)
		dlclose(dev->ext_lib);

	free(dev->fdt);
	free(dev);
	dev = NULL;
}

int nfb_get_system_id(const struct nfb_device *dev)
{
	int len;
	int fdt_offset;
	const uint32_t *prop32;

	fdt_offset = fdt_path_offset(dev->fdt, "/system/device");
	prop32 = fdt_getprop(dev->fdt, fdt_offset, "card-id", &len);
	if (fdt_offset < 0 || len != sizeof(*prop32)) {
		return -1;
	}
	return fdt32_to_cpu(*prop32);
}

int nfb_comp_count(const struct nfb_device *dev, const char *compatible)
{
	if (!dev || !compatible)
		return -1;

	const void *fdt = nfb_get_fdt(dev);
	int node_offset;
	int count = 0;

	fdt_for_each_compatible_node(fdt, node_offset, compatible) {
		count++;
	}

	return count;
}

int nfb_comp_find(const struct nfb_device *dev, const char *compatible, unsigned index)
{
	if (!dev || !compatible)
		return -1;

	const void *fdt = nfb_get_fdt(dev);
	int node_offset;
	unsigned count = 0;

	fdt_for_each_compatible_node(fdt, node_offset, compatible) {
		if (count == index)
			return node_offset;
		count++;
	}

	return node_offset;
}

/**
 * find_in_subtree() - get offset of n-th compatible component in subtree
 *
 * @fdt:  FDT blob
 * @subtree_offset: Offset of the parent node (subtree root)
 * @compatible:     Searched component compatible property
 * @index_searched: Index of the searched component
 * @index_current:  [internal] Pointer to zero-initialized variable
 */
static int find_in_subtree(const void* fdt,
		int subtree_offset,
		const char* compatible,
		unsigned index_searched,
		unsigned* index_current)
{
	int node;
	int ret;

	fdt_for_each_subnode(node, fdt, subtree_offset) {
		if (fdt_node_check_compatible(fdt, node, compatible) == 0) {
			(*index_current)++;
			if (*index_current == (index_searched + 1) ) {
				return node;
			}
		}

		if (fdt_first_subnode(fdt, node) > 0) {
			ret = find_in_subtree(fdt, node, compatible, index_searched, index_current);
			if (ret > 0)
				return ret;
		}
	}

	return -FDT_ERR_NOTFOUND;
}


int nfb_comp_find_in_parent(const struct nfb_device *dev, const char *compatible, unsigned index, int parent_offset)
{
	if (!dev || !compatible)
		return -1;

	const void *fdt = nfb_get_fdt(dev);
	unsigned subtree_index = 0;

	return find_in_subtree(fdt, parent_offset, compatible, index, &subtree_index);
}

int nfb_bus_open_for_comp(struct nfb_comp *comp, int nodeoffset)
{
	int compatible_offset;
	int comp_offset = nodeoffset;

	do {
		compatible_offset = -1;
		while ((compatible_offset = fdt_node_offset_by_compatible(comp->dev->fdt, compatible_offset, "netcope,bus,mi")) >= 0) {
			if (compatible_offset == nodeoffset) {
				return nfb_bus_open(comp, nodeoffset, comp_offset);
			}
		}
	} while ((nodeoffset = fdt_parent_offset(comp->dev->fdt, nodeoffset)) >= 0);

	return ENODEV;
}

ssize_t nfb_bus_mi_read(void *p, void *buf, size_t nbyte, off_t offset);
ssize_t nfb_bus_mi_write(void *p, const void *buf, size_t nbyte, off_t offset);

int nfb_bus_open(struct nfb_comp *comp, int fdt_offset, int comp_offset)
{
	int ret;
	comp->bus.dev = comp->dev;
	comp->bus.type = 0;

	ret = comp->dev->ops.bus_open_mi(comp->dev->priv, fdt_offset, comp_offset, &comp->bus.priv, &comp->bus.ops);

	if (getenv("LIBNFB_BUS_DEBUG"))
		comp->bus_debug = 1;

	/* INFO: Shortcut only for direct access MI bus (into PCI BAR): speed optimization
	 * uses direct call to nfb_bus_mi_read/write in nfb_comp_read/write */
	if (comp->bus.ops.read == nfb_bus_mi_read && comp->bus_debug == 0)
		comp->bus.type = NFB_BUS_TYPE_MI;

	return ret;
}

void nfb_bus_close(struct nfb_comp * comp)
{
	comp->dev->ops.bus_close_mi(comp->bus.priv);
}

struct nfb_comp *nfb_comp_open(const struct nfb_device *dev, int fdt_offset)
{
	return nfb_comp_open_ext(dev, fdt_offset, 0);
}

#define MAX_PATH_LEN  512

struct nfb_comp *nfb_comp_open_ext(const struct nfb_device *dev, int fdt_offset, int user_size)
{
	int ret;
	int proplen;
	const fdt32_t *prop;
	struct nfb_comp *comp;

	char path[MAX_PATH_LEN];

	prop = fdt_getprop(dev->fdt, fdt_offset, "reg", &proplen);
	if (proplen != sizeof(*prop) * 2) {
		errno = EBADFD;
		goto err_fdt_getprop;
	}

	if (fdt_get_path(dev->fdt, fdt_offset, path, MAX_PATH_LEN) != 0) {
		errno = EBADFD;
		goto err_fdt_get_path;
	}
	proplen = strlen(path) + 1;

	comp = malloc(sizeof(struct nfb_comp) + user_size + proplen);
	if (!comp) {
		goto err_malloc;
	}

	comp->dev = dev;
	comp->base = fdt32_to_cpu(prop[0]);
	comp->size = fdt32_to_cpu(prop[1]);
	comp->bus_debug = 0;

	comp->path = user_size + (char*)(comp + 1);
	strcpy(comp->path, path);

	ret = nfb_bus_open_for_comp(comp, fdt_offset);
	if (ret) {
		errno = ret;
		goto err_bus_open;
	}

	return comp;

err_bus_open:
	free(comp);
err_malloc:
err_fdt_get_path:
err_fdt_getprop:
	return NULL;
}

void nfb_comp_close(struct nfb_comp *comp)
{
	nfb_bus_close(comp);
	free(comp);
}

int nfb_comp_trylock(const struct nfb_comp *comp, uint32_t features, int timeout)
{
	struct timespec start, end;
	int64_t diff;
	int ret = 0;

	if (!comp)
		return -EINVAL;

	if (timeout > 0)
		clock_gettime(CLOCK_MONOTONIC, &start);

	do {
		ret = comp->dev->ops.comp_lock(comp, features);
		if (ret == 1) {
			/* Successful lock: return OK */
			return 0;
		} else if (ret != 0 && ret != -EBUSY) {
			/* Unsuccessful lock with specific error value: terminate function with error code */
			return ret;
		} else if (timeout == 0) {
			/* No timeout: return immediately with BUSY error value */
			return -EBUSY;
		} else {
			/* Generic unsuccessful lock: try lock again until timeout */
			usleep(50);
			if (timeout > 0) {
				clock_gettime(CLOCK_MONOTONIC, &end);
				diff = 1000000000L * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
			}
		}
	} while (timeout == -1 || (timeout > 0 && diff < timeout * 1000000L));

	return -ETIMEDOUT;
}

int nfb_comp_lock(const struct nfb_comp *comp, uint32_t features)
{
	int ret;

	ret = nfb_comp_trylock(comp, features, -1);
	if (ret == 0)
		return 1;
	return 0;
}

int nfb_base_comp_lock(const struct nfb_comp *comp, uint32_t features)
{
	struct nfb_lock lock;
	int ret;

	lock.path = comp->path;
	lock.features = features;

	while ((ret = ioctl(comp->dev->fd, NFB_LOCK_IOC_TRY_LOCK, &lock)) != 0) {
		if (ret == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				return -errno;
			}
		}
	}

	return 1;
}

void nfb_comp_unlock(const struct nfb_comp *comp, uint32_t features)
{
	if (!comp)
		return;
	comp->dev->ops.comp_unlock(comp, features);
}

void nfb_base_comp_unlock(const struct nfb_comp *comp, uint32_t features)
{
	struct nfb_lock lock;

	lock.path = comp->path;
	lock.features = features;

	ioctl(comp->dev->fd, NFB_LOCK_IOC_UNLOCK, &lock);
}

int nfb_comp_get_version(const struct nfb_comp *comp)
{
	int fdt_offset;
	int proplen;
	const fdt32_t *prop;

	if (!comp)
		return -1;

	fdt_offset = fdt_path_offset(comp->dev->fdt, comp->path);

	prop = fdt_getprop(comp->dev->fdt, fdt_offset, "version", &proplen);
	if (proplen != sizeof(*prop))
		return -1;

	return fdt32_to_cpu(*prop);
}

const char *nfb_comp_path(struct nfb_comp *comp)
{
	return comp->path;
}

const struct nfb_device *nfb_comp_get_device(struct nfb_comp *comp)
{
	return comp->dev;
}

static void nfb_bus_mi_dump(const char *type, const void *buf, size_t nbyte, off_t offset)
{
	size_t i;
	for (i = 0; i < nbyte; i++) {
		if (i == 0)
			fprintf(stderr, "libnfb: MI %s: % 4zdB -> [0x%08jx] |", type, nbyte, offset);

		fprintf(stderr, " %02x", ((unsigned char*)buf)[i]);
		if (i % 4 == 3)
			fprintf(stderr, " |");

		if (i == nbyte - 1)
			fprintf(stderr, "\n");
		else if (i % 16 == 15)
			fprintf(stderr, "\nlibnfb:                                  ");
	}

}

ssize_t nfb_comp_read(const struct nfb_comp *comp, void *buf, size_t nbyte, off_t offset)
{
	ssize_t ret;
	if (offset + nbyte > comp->size)
		return -1;

	if (comp->bus.type == NFB_BUS_TYPE_MI) {
		ret = nfb_bus_mi_read(comp->bus.priv, buf, nbyte, offset + comp->base);
	} else {
		ret = comp->bus.ops.read(comp->bus.priv, buf, nbyte, offset + comp->base);

		if (comp->bus_debug)
			nfb_bus_mi_dump("Read ", buf, nbyte, offset + comp->base);
	}

	return ret;
}

ssize_t nfb_comp_write(const struct nfb_comp *comp, const void *buf, size_t nbyte, off_t offset)
{
	ssize_t ret;
	if (offset + nbyte > comp->size)
		return -1;

	if (comp->bus.type == NFB_BUS_TYPE_MI) {
		ret = nfb_bus_mi_write(comp->bus.priv, buf, nbyte, offset + comp->base);
	} else {
		ret = comp->bus.ops.write(comp->bus.priv, buf, nbyte, offset + comp->base);
		if (comp->bus_debug)
			nfb_bus_mi_dump("Write", buf, nbyte, offset + comp->base);
	}

	return ret;
}
