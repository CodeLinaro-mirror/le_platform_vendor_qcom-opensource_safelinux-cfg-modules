// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018 Synopsys, Inc. and/or its affiliates.
 * stmmac XGMAC support.
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/slab.h>

#define BASE_ADDR 	0x23000000
#define MEM_SIZE  	0x1000
#define DMA_CH 		4
#define VLAN_N_VID	4095

/*MAC_PACKET_FILTER RA & VTFE*/
#define MAC_PACKET_FILTER		0x00000008
#define MAC_PACKET_FILTER_RA		BIT(31)
#define MAC_PACKET_FILTER_VTFE		BIT(16)

/*VLAN MACRO*/
#define MAC_VLAN_CTRL_TAG		0x00000050
#define MAC_VLAN_DATA_TAG		0x00000054

/* MAC VLAN DATA Bit */
#define MAC_VLAN_TAG_DATA_VID		GENMASK(15, 0)
#define MAC_VLAN_TAG_DATA_VEN		BIT(16)
#define MAC_VLAN_TAG_DATA_ETV		BIT(17)
#define MAC_VLAN_TAG_DATA_DOVLTC	BIT(18)
#define MAC_VLAN_TAG_DATA_DMACHEN	BIT(24)
#define MAC_VLAN_TAG_DATA_DMACHN	25

/* MAC VLAN CTRL Bit */
#define MAC_VLAN_TAG_CTRL_OB		BIT(0)
#define MAC_VLAN_TAG_CTRL_CT		BIT(1)
#define MAC_VLAN_TAG_CTRL_OFS_MASK	GENMASK(6, 2)
#define MAC_VLAN_TAG_CTRL_OFS_SHIFT	2

/*HW VLAN tags supported*/
#define MAC_HW_FEATURE3			0x00000128
#define MAC_HW_FEAT_NRVF 		GENMASK(2, 0)

/*enable dynamic routing on queue0*/
#define MAC_MTL_RXQ_DMA_MAP0		0x00000C30
#define MAC_MTL_RXQ_DMA_MAP1		0x00000C34
#define MTL_QUEUE_TO_DMA		0

#define MODIFY_DYNAMIC_DMA(queue, value, set) \
	do { \
		switch (queue) { \
		case 0: \
		case 4: \
			if (set) \
				value |= (1 << 4); \
			else \
				value &= ~(1 << 4); \
			break; \
		case 1: \
		case 5: \
			if (set) \
				value |= (1 << 12); \
			else \
				value &= ~(1 << 12); \
			break; \
		case 2: \
		case 6: \
			if (set) \
				value |= (1 << 20); \
			else \
				value &= ~(1 << 20); \
			break; \
		case 3: \
		case 7: \
			if (set) \
				value |= (1 << 28); \
			else \
				value &= ~(1 << 28); \
			break; \
		default: \
			break; \
		} \
	} while (0)

/* Command Line params */
static unsigned long mac_base_addr = BASE_ADDR;
module_param(mac_base_addr, ulong, 0444);
MODULE_PARM_DESC(mac_base_addr, "Physical address to map");

static int dma_ch = DMA_CH;
module_param(dma_ch, int, 0444);
MODULE_PARM_DESC(dma_ch, "DMA channel to which packets are routed");

static int vlan_num;
module_param(vlan_num, int, 0444);
MODULE_PARM_DESC(vlan_num, "Number of VLAN IDs");

static int vlan_ids[32] = {0};
module_param_array(vlan_ids, int, NULL, 0444);
MODULE_PARM_DESC(vlan_ids, "Array of VLAN IDs");

struct mac_device_info {
	int num_vlan;
	int vlan_filter[32];
	int pvm_vlan_filter[32];
};

struct mac_device_info *hw;
void __iomem *mac_base;

static void enable_mac_packet_filter_config(void)
{
	uint32_t reg_value;

	reg_value = readl(mac_base + MAC_PACKET_FILTER);
	reg_value |= MAC_PACKET_FILTER_VTFE | MAC_PACKET_FILTER_RA;
	writel(reg_value, mac_base + MAC_PACKET_FILTER);
}

static void disable_mac_packet_filter_config(void)
{
	uint32_t reg_value;

	reg_value = readl(mac_base + MAC_PACKET_FILTER);
	reg_value &= ~(MAC_PACKET_FILTER_VTFE | MAC_PACKET_FILTER_RA);
	writel(reg_value, mac_base + MAC_PACKET_FILTER);
}

 static void enable_dynamic_dma_ch_selection(int queue)
{
	uint32_t val;
	void __iomem *ioaddr = mac_base;
	uint32_t reg;

	reg = (queue < 4) ? MAC_MTL_RXQ_DMA_MAP0 : MAC_MTL_RXQ_DMA_MAP1;

	val = readl(ioaddr + reg);
	MODIFY_DYNAMIC_DMA(queue, val, 1);
	writel(val, ioaddr + reg);
}

static void disable_dynamic_dma_ch_selection(int queue)
{
	uint32_t val;
	void __iomem *ioaddr = mac_base;
	uint32_t reg;

	reg = (queue < 4) ? MAC_MTL_RXQ_DMA_MAP0 : MAC_MTL_RXQ_DMA_MAP1;

	val = readl(ioaddr + reg);
	MODIFY_DYNAMIC_DMA(queue, val, 0);
	writel(val, ioaddr + reg);
}

static int write_vlan_filter(int index, uint32_t data)
{
	void __iomem *ioaddr = mac_base;
	int ret;
	uint32_t val;

	if (index >= hw->num_vlan)
		return -EINVAL;

	writel(data, ioaddr + MAC_VLAN_DATA_TAG);

	val = readl(ioaddr + MAC_VLAN_CTRL_TAG);
	val &= ~(MAC_VLAN_TAG_CTRL_OFS_MASK |
		 MAC_VLAN_TAG_CTRL_CT |
		 MAC_VLAN_TAG_CTRL_OB);
	val |= (index << MAC_VLAN_TAG_CTRL_OFS_SHIFT) | MAC_VLAN_TAG_CTRL_OB;
	writel(val, ioaddr + MAC_VLAN_CTRL_TAG);

	/* Wait for done */
	ret = readl_poll_timeout(ioaddr + MAC_VLAN_CTRL_TAG,
		 val, !(val & MAC_VLAN_TAG_CTRL_OB), 1, 10);

	if (!ret)
		return ret;
	pr_err("Timeout accessing MAC_VLAN_Tag_Filter\n");
	return -EBUSY;
}

static int add_hw_vlan_rx_fltr_with_route(int vid, int dma_ch)
{
	uint32_t val = 0;
	int ret, i;
	int index = -1;

	if (vid < 1 || vid > VLAN_N_VID) {
		pr_err("Invalid vlan id : %d\n",vid);
		return -EINVAL;
	}

	/* Extended Rx VLAN Filter Enable */
	val |= MAC_VLAN_TAG_DATA_ETV | MAC_VLAN_TAG_DATA_VEN | vid;
	val |= MAC_VLAN_TAG_DATA_DOVLTC;
	val |= MAC_VLAN_TAG_DATA_DMACHEN;
	val |= (dma_ch << MAC_VLAN_TAG_DATA_DMACHN);

	for (i = 0; i < hw->num_vlan; i++) {
		if (hw->vlan_filter[i] == val)
			return 0;
		else if (!(hw->vlan_filter[i] & MAC_VLAN_TAG_DATA_VEN))
			index = i;
	}

	if (index == -1) {
		pr_err("MAC_VLAN_Tag_Filter full\n");
		return -EPERM;
	}

	ret = write_vlan_filter(index, val);
	if (!ret)
		hw->vlan_filter[index] = val;

	return ret;
}

static int del_hw_vlan_rx_fltr(int vid)
{
	int i, ret = 0;

	/* Extended Rx VLAN Filter disable */
	for (i = 0; i < hw->num_vlan; i++) {
		if ((hw->vlan_filter[i] & MAC_VLAN_TAG_DATA_VID) == vid
		    &&  hw->vlan_filter[i] != hw->pvm_vlan_filter[i]) {
			ret = write_vlan_filter(i, 0);
			if (!ret) {
				hw->vlan_filter[i] = 0;
				hw->pvm_vlan_filter[i] = 0;
			} else
				return ret;
		}
	}
	return ret;
}

void read_available_vlan_tags(void)
{
	void __iomem *ioaddr = mac_base;
	uint32_t read_tag_reg, read_data_reg;
	int i;

	for (i = 0; i < hw->num_vlan; i++) {
		read_tag_reg = readl(ioaddr + MAC_VLAN_CTRL_TAG);
		read_tag_reg &= ~(MAC_VLAN_TAG_CTRL_OFS_MASK |
				  MAC_VLAN_TAG_CTRL_CT |
				  MAC_VLAN_TAG_CTRL_OB);
		read_tag_reg |= (((i << MAC_VLAN_TAG_CTRL_OFS_SHIFT) &
				 MAC_VLAN_TAG_CTRL_OFS_MASK) |
				 MAC_VLAN_TAG_CTRL_CT |
				 MAC_VLAN_TAG_CTRL_OB);
		writel(read_tag_reg, ioaddr + MAC_VLAN_CTRL_TAG);

		/*read data and store in local array*/
		read_data_reg = readl(ioaddr + MAC_VLAN_DATA_TAG);
		hw->vlan_filter[i] = read_data_reg;
		hw->pvm_vlan_filter[i] = read_data_reg;
		pr_debug("vlan added at index %d is %d\n",
			 i, (hw->vlan_filter[i] & MAC_VLAN_TAG_DATA_VID));
		/* Wait for done */
		readl_poll_timeout(ioaddr + MAC_VLAN_CTRL_TAG,
				   read_tag_reg,
				   !(read_tag_reg & MAC_VLAN_TAG_CTRL_OB),
				   1, 10);
	}
	pr_debug("vlan array updated with HW\n");
}

static int get_hw_num_vlan(void)
{
	int val, num_vlan;

	val = readl(mac_base + MAC_HW_FEATURE3);
	switch (val & MAC_HW_FEAT_NRVF) {
	case 0:
		num_vlan = 1;
		break;
	case 1:
		num_vlan = 4;
		break;
	case 2:
		num_vlan = 8;
		break;
	case 3:
		num_vlan = 16;
		break;
	case 4:
		num_vlan = 24;
		break;
	case 5:
		num_vlan = 32;
		break;
	default:
		num_vlan = 1;
	}
	return num_vlan;
}

static int __init filter_init(void)
{
	int i;

	pr_debug("qcom_ethqos_filter probe start\n");

	mac_base = ioremap(mac_base_addr, MEM_SIZE);
	if (!mac_base) {
		pr_err("Failed to map memory region\n");
		return -ENOMEM;
	}

	/*VLAN filtering */
	if (vlan_num > 0) {
		hw = kmalloc(sizeof(struct mac_device_info), GFP_KERNEL);
		if (!hw) {
			pr_err("Failed to allocate memory to mac_device_info");
			return -ENOMEM;
		}
		memset(hw, 0, sizeof(struct mac_device_info));
		hw->num_vlan = get_hw_num_vlan();

		enable_mac_packet_filter_config();
		enable_dynamic_dma_ch_selection(MTL_QUEUE_TO_DMA);
		read_available_vlan_tags();

		for (i = 0; i < vlan_num; i++)
			add_hw_vlan_rx_fltr_with_route(vlan_ids[i], dma_ch);

		pr_info("qcom_ethqos_filter vlan added successfully\n");
	}
	pr_debug("qcom_ethqos_filter probe end\n");
	return 0;
}

static void __exit filter_exit(void)
{
	int i;

	if (vlan_num > 0) {
		for (i = 0; i < vlan_num; i++)
			del_hw_vlan_rx_fltr(vlan_ids[i]);

		disable_dynamic_dma_ch_selection(MTL_QUEUE_TO_DMA);
		disable_mac_packet_filter_config();
		kfree(hw);
		pr_debug("VLAN config removed successfully\n");
	}

	if (mac_base) {
		iounmap(mac_base);
		pr_debug("Memory region unmapped successfully\n");
	}
	pr_debug("qcom_ethqos_filter closed!\n");
}

module_init(filter_init);
module_exit(filter_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm ETHQOS RX filter driver");
