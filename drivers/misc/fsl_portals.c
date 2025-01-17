// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2008-2011 Freescale Semiconductor, Inc.
 * Copyright 2017 NXP
 */

#include <common.h>
#include <log.h>
#include <linux/libfdt.h>
#include <fdt_support.h>

#include <asm/processor.h>
#include <asm/io.h>
#ifdef CONFIG_PPC
#include <asm/fsl_portals.h>
#include <asm/fsl_liodn.h>
#else
#include <asm/arch-fsl-layerscape/fsl_portals.h>
#include <asm/arch-fsl-layerscape/fsl_icid.h>
#endif
#include <fsl_qbman.h>

#ifdef CONFIG_RESV_RAM
#include <asm/global_data.h>
DECLARE_GLOBAL_DATA_PTR;
#endif

#define MAX_BPORTALS (CFG_SYS_BMAN_CINH_SIZE / CFG_SYS_BMAN_SP_CINH_SIZE)
#define MAX_QPORTALS (CFG_SYS_QMAN_CINH_SIZE / CFG_SYS_QMAN_SP_CINH_SIZE)
void setup_qbman_portals(void)
{
	void __iomem *bpaddr = (void *)CFG_SYS_BMAN_CINH_BASE +
				CFG_SYS_BMAN_SWP_ISDR_REG;
	void __iomem *qpaddr = (void *)CFG_SYS_QMAN_CINH_BASE +
				CFG_SYS_QMAN_SWP_ISDR_REG;
	struct ccsr_qman *qman = (void *)CFG_SYS_FSL_QMAN_ADDR;

	/* Set the Qman initiator BAR to match the LAW (for DQRR stashing) */
#ifdef CONFIG_PHYS_64BIT
	out_be32(&qman->qcsp_bare, (u32)(CFG_SYS_QMAN_MEM_PHYS >> 32));
#endif
	out_be32(&qman->qcsp_bar, (u32)CFG_SYS_QMAN_MEM_PHYS);
#ifdef CONFIG_FSL_CORENET
	int i;

	for (i = 0; i < CFG_SYS_QMAN_NUM_PORTALS; i++) {
		u8 sdest = qp_info[i].sdest;
		u16 fliodn = qp_info[i].fliodn;
		u16 dliodn = qp_info[i].dliodn;
		u16 liodn_off = qp_info[i].liodn_offset;

		out_be32(&qman->qcsp[i].qcsp_lio_cfg, (liodn_off << 16) |
					dliodn);
		/* set frame liodn */
		out_be32(&qman->qcsp[i].qcsp_io_cfg, (sdest << 16) | fliodn);
	}
#else
#if defined(CONFIG_ARCH_LS1043A) || defined(CONFIG_ARCH_LS1046A)
	int i;

	for (i = 0; i < CFG_SYS_QMAN_NUM_PORTALS; i++) {
		u8 sdest = qp_info[i].sdest;
		u16 ficid = qp_info[i].ficid;
		u16 dicid = qp_info[i].dicid;
		u16 icid = qp_info[i].icid;

		out_be32(&qman->qcsp[i].qcsp_lio_cfg, (icid << 16) |
					dicid);
		/* set frame icid */
		out_be32(&qman->qcsp[i].qcsp_io_cfg, (sdest << 16) | ficid);
	}
#endif
#endif

	/* Change default state of BMan ISDR portals to all 1s */
	inhibit_portals(bpaddr, CFG_SYS_BMAN_NUM_PORTALS, MAX_BPORTALS,
			CFG_SYS_BMAN_SP_CINH_SIZE);
	inhibit_portals(qpaddr, CFG_SYS_QMAN_NUM_PORTALS, MAX_QPORTALS,
			CFG_SYS_QMAN_SP_CINH_SIZE);
}

void inhibit_portals(void __iomem *addr, int max_portals,
		     int arch_max_portals, int portal_cinh_size)
{
	u32 val;
	int i;

	/* arch_max_portals is the maximum based on memory size. This includes
	 * the reserved memory in the SoC.  max_portals the number of physical
	 * portals in the SoC
	 */
	if (max_portals > arch_max_portals) {
		printf("ERROR: portal config error\n");
		max_portals = arch_max_portals;
	}

	for (i = 0; i < max_portals; i++) {
		out_be32(addr, -1);
		val = in_be32(addr);
		if (!val) {
			printf("ERROR: Stopped after %d portals\n", i);
			return;
		}
		addr += portal_cinh_size;
	}
	debug("Cleared %d portals\n", i);
}

#ifdef CONFIG_PPC
static int fdt_qportal(void *blob, int off, int id, char *name,
		       enum fsl_dpaa_dev dev, int create)
{
	int childoff, dev_off, ret = 0;
	unsigned int dev_handle;
#ifdef CONFIG_FSL_CORENET
	int num;
	u32 liodns[2];
#endif

	childoff = fdt_subnode_offset(blob, off, name);
	if (create) {
		char handle[64], *p;

		strncpy(handle, name, sizeof(handle));
		p = strchr(handle, '@');
		if (!strncmp(name, "fman", 4)) {
			*p = *(p + 1);
			p++;
		}
		*p = '\0';

		dev_off = fdt_path_offset(blob, handle);
		/* skip this node if alias is not found */
		if (dev_off == -FDT_ERR_BADPATH)
			return 0;
		if (dev_off < 0)
			return dev_off;

		if (childoff <= 0)
			childoff = fdt_add_subnode(blob, off, name);

		/* need to update the dev_off after adding a subnode */
		dev_off = fdt_path_offset(blob, handle);
		if (dev_off < 0)
			return dev_off;

		if (childoff > 0) {
			dev_handle = fdt_get_phandle(blob, dev_off);
			if (dev_handle <= 0) {
				dev_handle = fdt_create_phandle(blob, dev_off);
				if (!dev_handle)
					return -FDT_ERR_NOPHANDLES;
			}

			ret = fdt_setprop(blob, childoff, "dev-handle",
					  &dev_handle, sizeof(dev_handle));
			if (ret < 0)
				return ret;

#ifdef CONFIG_FSL_CORENET
			num = get_dpaa_liodn(dev, &liodns[0], id);
			ret = fdt_setprop(blob, childoff, "fsl,liodn",
					  &liodns[0], sizeof(u32) * num);
			if (!strncmp(name, "pme", 3)) {
				u32 pme_rev1, pme_rev2;
				ccsr_pme_t *pme_regs =
					(void *)CFG_SYS_FSL_CORENET_PME_ADDR;

				pme_rev1 = in_be32(&pme_regs->pm_ip_rev_1);
				pme_rev2 = in_be32(&pme_regs->pm_ip_rev_2);
				ret = fdt_setprop(blob, childoff,
						  "fsl,pme-rev1", &pme_rev1,
						  sizeof(u32));
				if (ret < 0)
					return ret;
				ret = fdt_setprop(blob, childoff,
						  "fsl,pme-rev2", &pme_rev2,
						  sizeof(u32));
			}
#endif
		} else {
			return childoff;
		}
	} else {
		if (childoff > 0)
			ret = fdt_del_node(blob, childoff);
	}

	return ret;
}
#endif /* CONFIG_PPC */

#ifdef CONFIG_RESV_RAM
/**
 * fdt_fixup_dynamic_reserved_mem(): Convert a dynamic reserved-memory node to static
 *
 * @fdt: Pointer to flat device tree blob
 * @offset: Property offset within flat device tree blob
 * @path: String holding the property path, used for printing
 * @na: #address-cells of /reserved-memory node
 * @ns: #size-cells of /reserved-memory node
 *
 * Takes add a reserved memory region under a reserved-memory node. Uses
 * gd->arch.resv_ram to carve out memory and convert a dynamic allocation
 * (using "size" and "alignment") to a static one (using "reg").
 *
 * Implementation inspired by fdtdec_add_reserved_memory(). While the existing
 * API inserts a new node, we only need to insert the 'reg' property, if
 * absent, in existing nodes.
 *
 * The gd->arch.resv_ram address is updated after alignment, so that successive
 * calls do not result in overlapping memory regions.
 */
static void fdt_fixup_dynamic_reserved_mem(void *fdt, int offset, const char *path,
					   int na, int ns)
{
	fdt32_t cells[4] = {}, *ptr = cells;
	u64 base = gd->arch.resv_ram;
	u64 size, alignment;
	const fdt32_t *prop;
	u32 upper, lower;
	int err;

	prop = fdt_getprop(fdt, offset, "reg", NULL);
	if (prop) {
		log_debug("node \"%s\" already contains \"reg\" property, skipping fixup\n",
			  path);
		return;
	}

	/* Read the size and alignment from the device tree */
	prop = fdt_getprop(fdt, offset, "size", NULL);
	if (!prop) {
		log_warning("No \"size\" property for node \"%s\", aborting fixup\n",
			    path);
		return;
	}

	size = fdt_read_number(prop, na);

	prop = fdt_getprop(fdt, offset, "alignment", NULL);
	if (!prop) {
		log_warning("No \"alignment\" property for node \"%s\", aborting fixup\n",
			    path);
		return;
	}
	alignment = fdt_read_number(prop, na);

	/* Align the base address */
	base = ALIGN_DOWN(base - size, alignment);
	upper = upper_32_bits(base);
	lower = lower_32_bits(base);

	if (na > 1)
		*ptr++ = cpu_to_fdt32(upper);

	*ptr++ = cpu_to_fdt32(lower);

	upper = upper_32_bits(size);
	lower = lower_32_bits(size);

	if (ns > 1)
		*ptr++ = cpu_to_fdt32(upper);

	*ptr++ = cpu_to_fdt32(lower);

	err = fdt_setprop(fdt, offset, "reg", cells, (na + ns) * sizeof(*cells));
	if (err < 0) {
		log_warning("Failed to add reserved memory for node \"%s\": %d\n",
			    path, err);
		return;
	}

	/* We don't need these anymore */
	fdt_delprop(fdt, offset, "size");
	fdt_delprop(fdt, offset, "alignment");
	fdt_delprop(fdt, offset, "alloc-ranges");

	log_debug("Added reservation of size 0x%llx at address 0x%llx for node \"%s\"\n",
		  size, base, path);

	gd->arch.resv_ram = base;
}

static void fdt_fixup_qbman_reserved_mem_one(void *fdt, const char *alias,
					     int na, int ns)
{
	const char *path = fdt_get_alias(fdt, alias);
	int offset;

	if (!path) {
		log_debug("No \"%s\" alias found, skipping base address fixup\n",
			  alias);
		return;
	}

	offset = fdt_path_offset(fdt, path);
	if (offset < 0) {
		log_debug("Failed to find offset for path \"%s\"\n", path);
		return;
	}

	fdt_fixup_dynamic_reserved_mem(fdt, offset, path, na, ns);
}

/* Reserve memory in RAM for the QBMan FBPR, FQD and PFDR private memory
 * regions. Update the device tree with the corresponding addresses.
 *
 * The regions are accessed in Linux and can be configured in BAR registers
 * only once per SoC reset. In order to guarantee the same memory regions
 * are used by successive kernels without a reset (e.g. kexec), reserve the
 * regions from the bootloader so they persist between kernels.
 */
void fdt_fixup_qbman_reserved_mem(void *fdt)
{
	int offset = fdt_path_offset(fdt, "/reserved-memory");
	int na, ns;

	if (offset < 0) {
		log_warning("No \"/reserved-memory\" node, aborting QBMan fixup\n");
		return;
	}

	na = fdt_address_cells(fdt, offset);
	ns = fdt_size_cells(fdt, offset);

	/* Reserve memory regions for each area. The calling order here
	 * should be kept the same as their relative ordering within the
	 * /reserved-memory node, if U-Boot should reserve the same addresses
	 * as Linux. The gd->arch.resv_ram address is updated in each call.
	 */
	fdt_fixup_qbman_reserved_mem_one(fdt, "bman_fbpr", na, ns);
	fdt_fixup_qbman_reserved_mem_one(fdt, "qman_fqd", na, ns);
	fdt_fixup_qbman_reserved_mem_one(fdt, "qman_pfdr", na, ns);
}
#endif

void fdt_fixup_qportals(void *blob)
{
	int off, err;
	unsigned int maj, min;
	unsigned int ip_cfg;
	struct ccsr_qman *qman = (void *)CFG_SYS_FSL_QMAN_ADDR;
	u32 rev_1 = in_be32(&qman->ip_rev_1);
	u32 rev_2 = in_be32(&qman->ip_rev_2);
	char compat[64];
	int compat_len;

#if defined(CONFIG_ARCH_LS1043A) || defined(CONFIG_ARCH_LS1046A)
	int smmu_ph = fdt_get_smmu_phandle(blob);
#endif

	maj = (rev_1 >> 8) & 0xff;
	min = rev_1 & 0xff;
	ip_cfg = rev_2 & 0xff;

	compat_len = sprintf(compat, "fsl,qman-portal-%u.%u.%u",
			     maj, min, ip_cfg) + 1;
	compat_len += sprintf(compat + compat_len, "fsl,qman-portal") + 1;

	fdt_for_each_node_by_compatible(off, blob, -1, "fsl,qman-portal") {
#if defined(CONFIG_PPC) || defined(CONFIG_ARCH_LS1043A) || \
defined(CONFIG_ARCH_LS1046A)
#ifdef CONFIG_FSL_CORENET
		u32 liodns[2];
#endif
		const int *ci = fdt_getprop(blob, off, "cell-index", &err);
		int i;

		if (!ci)
			goto err;

		i = fdt32_to_cpu(*ci);
#if defined(CONFIG_SYS_DPAA_FMAN) && defined(CONFIG_PPC)
		int j;
#endif

#endif /* CONFIG_PPC || CONFIG_ARCH_LS1043A || CONFIG_ARCH_LS1046A */
		err = fdt_setprop(blob, off, "compatible", compat, compat_len);
		if (err < 0)
			goto err;
#ifdef CONFIG_PPC
#ifdef CONFIG_FSL_CORENET
		liodns[0] = qp_info[i].dliodn;
		liodns[1] = qp_info[i].fliodn;
		err = fdt_setprop(blob, off, "fsl,liodn",
				  &liodns, sizeof(u32) * 2);
		if (err < 0)
			goto err;
#endif

		i++;

		err = fdt_qportal(blob, off, i, "crypto@0", FSL_HW_PORTAL_SEC,
				  IS_E_PROCESSOR(get_svr()));
		if (err < 0)
			goto err;

#ifdef CONFIG_FSL_CORENET
#ifdef CONFIG_SYS_DPAA_PME
		err = fdt_qportal(blob, off, i, "pme@0", FSL_HW_PORTAL_PME, 1);
		if (err < 0)
			goto err;
#else
		fdt_qportal(blob, off, i, "pme@0", FSL_HW_PORTAL_PME, 0);
#endif
#endif

#ifdef CONFIG_SYS_DPAA_FMAN
		for (j = 0; j < CFG_SYS_NUM_FMAN; j++) {
			char name[] = "fman@0";

			name[sizeof(name) - 2] = '0' + j;
			err = fdt_qportal(blob, off, i, name,
					  FSL_HW_PORTAL_FMAN1 + j, 1);
			if (err < 0)
				goto err;
		}
#endif
#ifdef CONFIG_SYS_DPAA_RMAN
		err = fdt_qportal(blob, off, i, "rman@0",
				  FSL_HW_PORTAL_RMAN, 1);
		if (err < 0)
			goto err;
#endif
#else
#if defined(CONFIG_ARCH_LS1043A) || defined(CONFIG_ARCH_LS1046A)
		if (smmu_ph >= 0) {
			u32 icids[3];

			icids[0] = qp_info[i].icid;
			icids[1] = qp_info[i].dicid;
			icids[2] = qp_info[i].ficid;

			fdt_set_iommu_prop(blob, off, smmu_ph, icids, 3);
		}
#endif
#endif /* CONFIG_PPC */

err:
		if (err < 0) {
			printf("ERROR: unable to create props for %s: %s\n",
			       fdt_get_name(blob, off, NULL),
			       fdt_strerror(err));
			return;
		}
	}
}

void fdt_fixup_bportals(void *blob)
{
	int off, err;
	unsigned int maj, min;
	unsigned int ip_cfg;
	struct ccsr_bman *bman = (void *)CFG_SYS_FSL_BMAN_ADDR;
	u32 rev_1 = in_be32(&bman->ip_rev_1);
	u32 rev_2 = in_be32(&bman->ip_rev_2);
	char compat[64];
	int compat_len;

	maj = (rev_1 >> 8) & 0xff;
	min = rev_1 & 0xff;

	ip_cfg = rev_2 & 0xff;

	compat_len = sprintf(compat, "fsl,bman-portal-%u.%u.%u",
			     maj, min, ip_cfg) + 1;
	compat_len += sprintf(compat + compat_len, "fsl,bman-portal") + 1;

	off = fdt_node_offset_by_compatible(blob, -1, "fsl,bman-portal");
	while (off != -FDT_ERR_NOTFOUND) {
		err = fdt_setprop(blob, off, "compatible", compat, compat_len);
		if (err < 0) {
			printf("ERROR: unable to create props for %s: %s\n",
			       fdt_get_name(blob, off, NULL),
			       fdt_strerror(err));
			return;
		}

		off = fdt_node_offset_by_compatible(blob, off,
						    "fsl,bman-portal");
	}
}
