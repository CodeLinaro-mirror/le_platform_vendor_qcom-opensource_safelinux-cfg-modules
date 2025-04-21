// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018 Synopsys, Inc. and/or its affiliates.
 * stmmac XGMAC support.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/cdev.h>

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

#define DEVICE_NAME "qcom_ethqos_filter_dev"
#define CLASS_NAME "qcom_ethqos_filter_class"
#define BUF_LEN 120
#define EMAC_LINK_DOWN 2

/* Command Line params */
static unsigned long mac_base_addr = BASE_ADDR;
module_param(mac_base_addr, ulong, 0444);
MODULE_PARM_DESC(mac_base_addr, "Physical address to map");

static int dma_ch = DMA_CH;
module_param(dma_ch, int, 0444);
MODULE_PARM_DESC(dma_ch, "DMA channel to which packets are routed");

static int dma_dynamic_ch = MTL_QUEUE_TO_DMA;
module_param(dma_dynamic_ch, int, 0444);
MODULE_PARM_DESC(dma_dynamic_ch, "DMA channel to which dynamic channel enable");

static int vlan_num;
module_param(vlan_num, int, 0444);
MODULE_PARM_DESC(vlan_num, "Number of VLAN IDs");

static int vlan_ids[32] = {0};
module_param_array(vlan_ids, int, NULL, 0444);
MODULE_PARM_DESC(vlan_ids, "Array of VLAN IDs");

enum {
	DEL_GVM_THIN_VLAN = 2,
	ADD_GVM_THIN_VLAN = 3,
	ADD_ALL_VLAN = 4,
	GVM_REMOVE = 5
};

struct mac_device_info {
	int num_vlan;
	int vlan_filter[32];
	int pvm_vlan_filter[32];
};

struct char_device_info {
	struct class *char_class;
	struct cdev *my_cdev;
	dev_t dev;
	char message[BUF_LEN];
};

struct mac_device_info *hw;
static struct char_device_info char_dev_info;
void __iomem *mac_base;
static int vlan_added;

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

static void remove_char_device(void)
{
	if (char_dev_info.char_class && char_dev_info.dev)
		device_destroy(char_dev_info.char_class, char_dev_info.dev);
	if (char_dev_info.char_class) {
		class_destroy(char_dev_info.char_class);
		char_dev_info.char_class = NULL;
	}
	if (char_dev_info.my_cdev) {
		cdev_del(char_dev_info.my_cdev);
		char_dev_info.my_cdev = NULL;
	}
	unregister_chrdev_region(char_dev_info.dev, 1);
	pr_debug("chrdev: Device removed successfully\n");
}

static void add_all_vlan_gvm(void)
{
	int i, ret;

	if (vlan_num > 0 && vlan_added == 0) {
		vlan_added = 1;
		hw = kzalloc(sizeof(struct mac_device_info), GFP_KERNEL);
		if (!hw) {
			pr_err("Failed to allocate memory to mac_device_info\n");
			iounmap(mac_base);
			remove_char_device();
			return;
		}
		hw->num_vlan = get_hw_num_vlan();
		enable_mac_packet_filter_config();
		enable_dynamic_dma_ch_selection(dma_dynamic_ch);
		read_available_vlan_tags();

		for (i = 0; i < vlan_num; i++) {
			ret = add_hw_vlan_rx_fltr_with_route(vlan_ids[i], dma_ch);
			if (ret)
				pr_err("Failed to add VLAN filter for ID %d: %d\n",
				       vlan_ids[i], ret);
		}
	}
}

static void add_last_vlan_gvm(int vid)
{
	if (vlan_num >= 32) {
		pr_err("Maximum number of VLANs (32) already configured\n");
		return;
	}

	vlan_num++;
	vlan_ids[vlan_num-1] = vid;
	if (vlan_num > 0) {
		if (vlan_added == 0) {
			hw = kzalloc(sizeof(struct mac_device_info), GFP_KERNEL);
			if (!hw) {
				iounmap(mac_base);
				remove_char_device();
				vlan_num--;
				return;
			}
			hw->num_vlan = get_hw_num_vlan();
			enable_mac_packet_filter_config();
			enable_dynamic_dma_ch_selection(dma_dynamic_ch);
			read_available_vlan_tags();
		}
		vlan_added = 1;
		add_hw_vlan_rx_fltr_with_route(vlan_ids[vlan_num-1], dma_ch);
	}
}

static void remove_all_vlan_gvm(void)
{
	int i;

	if (vlan_num > 0 && vlan_added == 1) {
		for (i = 0; i < vlan_num; i++)
			del_hw_vlan_rx_fltr(vlan_ids[i]);

		disable_dynamic_dma_ch_selection(dma_dynamic_ch);
		disable_mac_packet_filter_config();
		kfree(hw);
		vlan_added = 0;
	}
}

static void remove_one_vlan_gvm(int vid)
{
	int i, index = -1;

	if (vlan_num > 0 && vlan_added == 1) {
		for (i = 0; i < vlan_num; i++) {
			if (vlan_ids[i] == vid) {
				del_hw_vlan_rx_fltr(vlan_ids[i]);
				index = i;
				break;
			}
		}
		if (index != -1) {
			for (i = index; i < vlan_num - 1; i++)
				vlan_ids[i] = vlan_ids[i + 1];
			vlan_ids[vlan_num - 1] = 0;
			vlan_num--;

			if (vlan_num == 0) {
				vlan_added = 0;
				disable_dynamic_dma_ch_selection(dma_dynamic_ch);
				disable_mac_packet_filter_config();
				kfree(hw);
			}
		}
	}
}


static ssize_t device_write(struct file *device_file, const char *buffer,
			    size_t len, loff_t *file_position)
{
	int vlan_status = -1;
	int vid = -1;
	int pvm_link_state = -1;
	int gvm_link_state = -1;

	if (len >= BUF_LEN) {
		pr_err("chrdev: Input too large, maximum allowed is %d bytes\n", BUF_LEN - 1);
		return -EINVAL;
	}

	if (copy_from_user(char_dev_info.message, buffer, len)) {
		pr_err("chrdev: Failed to copy data from user\n");
		return -EFAULT;
	}

	char_dev_info.message[len] = '\0';
	pr_debug("chrdev: Received %zu characters from netlink: %s\n", len, char_dev_info.message);

	if (sscanf(char_dev_info.message, "VLAN Status: %d, VID: %d, PVM: %d, GVM: %d",
		 &vlan_status, &vid, &pvm_link_state, &gvm_link_state) == 4) {
		pr_debug("chrdev: Parsed VLAN status = %d, VID = %d, PVM = %d, GVM = %d\n",
			 vlan_status, vid, pvm_link_state, gvm_link_state);
		if (vlan_status == GVM_REMOVE) {
			remove_all_vlan_gvm();
			vlan_num = 0;
			pr_info("chrdev: VLAN config deleted successfully\n");
		} else if (gvm_link_state == EMAC_LINK_DOWN) {
			remove_all_vlan_gvm();
			pr_info("chrdev: all VLAN filters removed successfully\n");
		} else if (pvm_link_state == EMAC_LINK_DOWN) {
			remove_all_vlan_gvm();
			pr_info("chrdev: all VLAN filters removed successfully\n");
		} else if (vlan_status == ADD_GVM_THIN_VLAN) {
			add_last_vlan_gvm(vid);
			pr_info("chrdev: qcom_ethqos_filter vlan %d added successfully\n", vid);
		} else if (vlan_status == DEL_GVM_THIN_VLAN) {
			remove_one_vlan_gvm(vid);
			pr_info("chrdev: qcom_ethqos_filter vlan %d deleted successfully\n", vid);
		} else if (vlan_status == ADD_ALL_VLAN) {
			add_all_vlan_gvm();
			pr_info("chrdev: qcom_ethqos_filter all vlan filters present added successfully\n");
		}
	} else {
		pr_err("chrdev: Failed to parse VLAN info from input\n");
	}
	return len;
}


static const struct file_operations fops = {
	.write = device_write,
};

static int create_char_device(void)
{
	int ret;

	ret = alloc_chrdev_region(&char_dev_info.dev, 0, 1, DEVICE_NAME);
	if (ret) {
		pr_err("chrdev: Failed to allocate a major number\n");
		goto err_alloc;
	}

	char_dev_info.my_cdev = cdev_alloc();
	if (!char_dev_info.my_cdev) {
		pr_err("chrdev: Failed to allocate cdev\n");
		ret = -ENOMEM;
		goto err_cdev_alloc;
	}

	cdev_init(char_dev_info.my_cdev, &fops);
	ret = cdev_add(char_dev_info.my_cdev, char_dev_info.dev, 1);
	if (ret < 0) {
		pr_err("chrdev: Failed to add the cdev\n");
		kfree(char_dev_info.my_cdev);
		char_dev_info.my_cdev = NULL;
		goto err_cdev_alloc;
	}

	char_dev_info.char_class = class_create(CLASS_NAME);
	if (IS_ERR(char_dev_info.char_class)) {
		pr_err("chrdev: Failed to register device class\n");
		ret = PTR_ERR(char_dev_info.char_class);
		goto err_class_create;
	}

	if (IS_ERR(device_create(char_dev_info.char_class, NULL,
		   char_dev_info.dev, NULL, DEVICE_NAME))) {
		pr_err("chrdev: Failed to create the device\n");
		ret = -EFAULT;
		goto err_device_create;
	}
	pr_debug("chrdev: Device created successfully\n");
	return 0;

err_device_create:
	class_destroy(char_dev_info.char_class);
err_class_create:
	cdev_del(char_dev_info.my_cdev);
	char_dev_info.my_cdev = NULL;
err_cdev_alloc:
	unregister_chrdev_region(char_dev_info.dev, 1);
err_alloc:
	return ret;
}

static int __init filter_init(void)
{
	pr_debug("qcom_ethqos_filter probe start\n");
	if (create_char_device()) {
		pr_err("Failed to create char device\n");
		return -ENOMEM;
	}
	mac_base = ioremap(mac_base_addr, MEM_SIZE);
	if (!mac_base) {
		pr_err("Failed to map memory region\n");
		remove_char_device();
		return -ENOMEM;
	}
	add_all_vlan_gvm();
	if (vlan_num > 0 && !hw) {
		pr_err("Failed to initialize VLAN filtering\n");
		iounmap(mac_base);
		remove_char_device();
		return -ENOMEM;
	}
	pr_debug("qcom_ethqos_filter probe end\n");
	return 0;
}

static void __exit filter_exit(void)
{
	remove_all_vlan_gvm();
	pr_debug("VLAN config removed successfully\n");
	remove_char_device();
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
