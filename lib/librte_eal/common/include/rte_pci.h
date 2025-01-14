/*-
 *   BSD LICENSE
 * 
 *   Copyright(c) 2010-2012 Intel Corporation. All rights reserved.
 *   Copyright(c) 2013 6WIND.
 *   All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without 
 *   modification, are permitted provided that the following conditions 
 *   are met:
 * 
 *     * Redistributions of source code must retain the above copyright 
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright 
 *       notice, this list of conditions and the following disclaimer in 
 *       the documentation and/or other materials provided with the 
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its 
 *       contributors may be used to endorse or promote products derived 
 *       from this software without specific prior written permission.
 * 
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR 
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 *  version: DPDK.L.1.2.3-3
 */

#ifndef _RTE_PCI_H_
#define _RTE_PCI_H_

/**
 * @file
 *
 * RTE PCI Interface
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/queue.h>
#include <stdint.h>
#include <inttypes.h>
#include <rte_interrupts.h>

TAILQ_HEAD(pci_device_list, rte_pci_device); /**< PCI devices in D-linked Q. */
TAILQ_HEAD(pci_driver_list, rte_pci_driver); /**< PCI drivers in D-linked Q. */

extern struct pci_driver_list driver_list; /**< Global list of PCI drivers. */
extern struct pci_device_list device_list; /**< Global list of PCI devices. */

/** Pathname of PCI devices directory. */
#define SYSFS_PCI_DEVICES "/sys/bus/pci/devices"

/** Formatting string for PCI device identifier: Ex: 0000:00:01.0 */
#define PCI_PRI_FMT "%.4"PRIx16":%.2"PRIx8":%.2"PRIx8".%"PRIx8

/** Nb. of values in PCI device identifier format string. */
#define PCI_FMT_NVAL 4

/** Nb. of values in PCI resource format. */
#define PCI_RESOURCE_FMT_NVAL 3

/**
 * A structure describing a PCI resource.
 */
struct rte_pci_resource {
	uint64_t phys_addr;   /**< Physical address, 0 if no resource. */
	uint64_t len;         /**< Length of the resource. */
	void *addr;           /**< Virtual address, NULL when not mapped. */
};

/** Maximum number of PCI resources. */
#define PCI_MAX_RESOURCE 7

/**
 * A structure describing an ID for a PCI driver. Each driver provides a
 * table of these IDs for each device that it supports.
 */
struct rte_pci_id {
	uint16_t vendor_id;           /**< Vendor ID or PCI_ANY_ID. */
	uint16_t device_id;           /**< Device ID or PCI_ANY_ID. */
	uint16_t subsystem_vendor_id; /**< Subsystem vendor ID or PCI_ANY_ID. */
	uint16_t subsystem_device_id; /**< Subsystem device ID or PCI_ANY_ID. */
};

/**
 * A structure describing the location of a PCI device.
 */
struct rte_pci_addr {
	uint16_t domain;                /**< Device domain */
	uint8_t bus;                    /**< Device bus */
	uint8_t devid;                  /**< Device ID */
	uint8_t function;               /**< Device function. */
};

/**
 * A structure describing a PCI device.
 */
struct rte_pci_device {
	TAILQ_ENTRY(rte_pci_device) next;       /**< Next probed PCI device. */
	struct rte_pci_addr addr;               /**< PCI location. */
	struct rte_pci_id id;                   /**< PCI ID. */
	struct rte_pci_resource mem_resource;   /**< PCI Memory Resource */
	struct rte_intr_handle intr_handle;     /**< Interrupt handle */
	const struct rte_pci_driver *driver;    /**< Associated driver */
	unsigned int blacklisted:1;             /**< Device is blacklisted */
};

/** Any PCI device identifier (vendor, device, ...) */
#define PCI_ANY_ID (0xffff)

#ifdef __cplusplus
/** C++ macro used to help building up tables of device IDs */
#define RTE_PCI_DEVICE(vend, dev) \
	(vend),                   \
	(dev),                    \
	PCI_ANY_ID,               \
	PCI_ANY_ID
#else
/** Macro used to help building up tables of device IDs */
#define RTE_PCI_DEVICE(vend, dev)          \
	.vendor_id = (vend),               \
	.device_id = (dev),                \
	.subsystem_vendor_id = PCI_ANY_ID, \
	.subsystem_device_id = PCI_ANY_ID
#endif

struct rte_pci_driver;

/**
 * Initialisation function for the driver called during PCI probing.
 */
typedef int (pci_devinit_t)(struct rte_pci_driver *, struct rte_pci_device *);

/**
 * A structure describing a PCI driver.
 */
struct rte_pci_driver {
	TAILQ_ENTRY(rte_pci_driver) next;       /**< Next in list. */
	const char *name;                       /**< Driver name. */
	pci_devinit_t *devinit;                 /**< Device init. function. */
	struct rte_pci_id *id_table;            /**< ID table, NULL terminated. */
	uint32_t drv_flags;                     /**< Flags contolling handling of device. */
};

/** Device needs igb_uio kernel module */
#define RTE_PCI_DRV_NEED_IGB_UIO 0x0001
/** Device driver must be registered several times until failure */
#define RTE_PCI_DRV_MULTIPLE 0x0002
/** Device needs to be unbound even if no module is provided */
#define RTE_PCI_DRV_FORCE_UNBIND 0x0004

/**
 * Probe the PCI bus for registered drivers.
 *
 * Scan the content of the PCI bus, and call the probe() function for
 * all registered drivers that have a matching entry in its id_table
 * for discovered devices.
 *
 * @return
 *   - 0 on success.
 *   - Negative on error.
 */
int rte_eal_pci_probe(void);

/**
 * Dump the content of the PCI bus.
 */
void rte_eal_pci_dump(void);

/**
 * Register a PCI driver.
 *
 * @param driver
 *   A pointer to a rte_pci_driver structure describing the driver
 *   to be registered.
 */
void rte_eal_pci_register(struct rte_pci_driver *driver);

/**
 * Register a list of PCI locations that will be blacklisted (not used by DPDK).
 *
 * @param blacklist
 *   List of PCI device addresses that will not be used by DPDK.
 * @param size
 *   Number of items in the list.
 */
void rte_eal_pci_set_blacklist(struct rte_pci_addr *blacklist, unsigned size);

#ifdef __cplusplus
}
#endif

#endif /* _RTE_PCI_H_ */
