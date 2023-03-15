/*
 * Copyright (C) 2017 Spreadtrum Communications Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/bug.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <misc/marlin_platform.h>
#include <misc/wcn_bus.h>

#include "../wcn_gnss.h"
#include "../../include/wcn_glb_reg.h"
#include "gnss_common_ctl.h"
#include "wcn_glb.h"

#define GNSSCOMM_INFO(format, arg...) pr_info("gnss_ctl: " format, ## arg)
#define GNSSCOMM_ERR(format, arg...) pr_err("gnss_ctl: " format, ## arg)

#define GNSS_DATA_BASE_TYPE_H  16
#define GNSS_MAX_STRING_LEN	10
#define GNSS_DUMP_DATA_START_UP 1
#define FIRMWARE_FILEPATHNAME_LENGTH_MAX 256

struct gnss_common_ctl {
	struct device *dev;
	unsigned long chip_ver;
	unsigned int gnss_status;
	unsigned int gnss_subsys;
	char firmware_path[FIRMWARE_FILEPATHNAME_LENGTH_MAX];
};

static struct gnss_common_ctl gnss_common_ctl_dev;

enum gnss_status_e {
	GNSS_STATUS_POWEROFF = 0,
	GNSS_STATUS_POWERON,
	GNSS_STATUS_ASSERT,
	GNSS_STATUS_POWEROFF_GOING,
	GNSS_STATUS_POWERON_GOING,
	GNSS_STATUS_MAX,
};
#ifdef CONFIG_WCN_INTEG
enum gnss_cp_status_subtype {
	GNSS_CP_STATUS_CALI = 1,
	GNSS_CP_STATUS_INIT = 2,
	GNSS_CP_STATUS_INIT_DONE = 3,
	GNSS_CP_STATUS_IDLEOFF = 4,
	GNSS_CP_STATUS_IDLEON = 5,
	GNSS_CP_STATUS_SLEEP = 6,
	GNSS_CP_STATUS__MAX,
};

#endif
static unsigned int gnssver = 0x22;
static const struct of_device_id gnss_common_ctl_of_match[] = {
	{.compatible = "sprd,gnss_common_ctl", .data = (void *)&gnssver},
	{},
};


#if defined(CONFIG_UMW2652_S) || defined(CONFIG_UMW2631_I)
static int gnss_tsen_enable(int type)
{
	struct platform_device *pdev_regmap;
	struct device_node *regmap_np;
	static struct regmap *regmap;
	static unsigned int base;
	int ret;
	unsigned int value, temp;

	if (base == 0) {
		regmap_np = of_find_compatible_node(NULL, NULL,
						    "sprd,sc27xx-syscon");
		if (!regmap_np) {
			GNSSCOMM_ERR("%s, error np\n", __func__);
			return -EINVAL;
		}
		pdev_regmap = of_find_device_by_node(regmap_np);
		if (!pdev_regmap) {
			GNSSCOMM_ERR("%s, error regmap\n", __func__);
			return -EINVAL;
		}
		regmap = dev_get_regmap(pdev_regmap->dev.parent, NULL);
		ret = of_property_read_u32_index(regmap_np, "reg", 0, &base);
		if (ret) {
			GNSSCOMM_ERR("%s, error base\n", __func__);
			return -EINVAL;
		}
	}
	GNSSCOMM_ERR("%s, base 0x%x\n", __func__, base);
	regmap_read(regmap, (REGS_ANA_APB_BASE + XTL_WAIT_CTRL0), &value);
	GNSSCOMM_ERR("%s, XTL_WAIT_CTRL0 value read 0x%x\n", __func__, value);
	temp = value | BIT_XTL_EN;
	regmap_write(regmap, (REGS_ANA_APB_BASE + XTL_WAIT_CTRL0), temp);
	regmap_read(regmap, (REGS_ANA_APB_BASE + XTL_WAIT_CTRL0), &value);
	GNSSCOMM_ERR("%s, 2nd read 0x%x\n", __func__, value);

	regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL0), &value);
	GNSSCOMM_ERR("%s, TSEN_CTRL0 value read 0x%x\n", __func__, value);
	temp = value | BIT_TSEN_CLK_SRC_SEL | BIT_TSEN_ADCLDO_EN;
	regmap_write(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL0), temp);
	regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL0), &value);
	GNSSCOMM_ERR("%s, 2nd read 0x%x\n", __func__, value);

	regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL1), &value);
	GNSSCOMM_ERR("%s, TSEN_CTRL1 value read 0x%x\n", __func__, value);
	temp = value | BIT_TSEN_SDADC_EN | BIT_TSEN_CLK_EN | BIT_TSEN_UGBUF_EN;
	regmap_write(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL1), temp);
	regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL1), &value);
	GNSSCOMM_ERR("%s, 2nd read 0x%x\n", __func__, value);

	regmap_read(regmap, (SC2730_PIN_REG_BASE + PTEST0), &value);
	GNSSCOMM_ERR("%s, PTEST0 value read 0x%x\n", __func__, value);
	temp = (value & (~PTEST0_MASK)) | PTEST0_sel(2);
	regmap_write(regmap, (SC2730_PIN_REG_BASE + PTEST0), temp);
	regmap_read(regmap, (SC2730_PIN_REG_BASE + PTEST0), &value);
	GNSSCOMM_ERR("%s, 2nd read 0x%x\n", __func__, value);

	regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL3), &value);
	GNSSCOMM_ERR("%s, TSEN_CTRL3 value read 0x%x\n", __func__, value);
	temp = value | BIT_TSEN_EN;
	temp = (temp & (~BIT_TSEN_TIME_SEL_MASK)) | BIT_TSEN_TIME_sel(2);
	regmap_write(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL3), temp);
	regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL3), &value);
	GNSSCOMM_ERR("%s, 2nd read 0x%x\n", __func__, value);

	if (type == TSEN_EXT)
		regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL4), &value);
	else
		regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL5), &value);
	GNSSCOMM_ERR("%s, 0x%x read 0x%x\n", __func__, TSEN_CTRL4, value);

	return 0;
}

static int gnss_tsen_disable(int type)
{
	struct platform_device *pdev_regmap;
	struct device_node *regmap_np;
	static struct regmap *regmap;
	static unsigned int base;
	int ret;
	unsigned int value, temp;

	if (base == 0) {
		regmap_np = of_find_compatible_node(NULL, NULL,
						    "sprd,sc27xx-syscon");
		if (!regmap_np) {
			GNSSCOMM_ERR("%s, error np\n", __func__);
			return -EINVAL;
		}
		pdev_regmap = of_find_device_by_node(regmap_np);
		if (!pdev_regmap) {
			GNSSCOMM_ERR("%s, error regmap\n", __func__);
			return -EINVAL;
		}
		regmap = dev_get_regmap(pdev_regmap->dev.parent, NULL);
		ret = of_property_read_u32_index(regmap_np, "reg", 0, &base);
		if (ret) {
			GNSSCOMM_ERR("%s, error base\n", __func__);
			return -EINVAL;
		}
	}
	GNSSCOMM_ERR("%s, base 0x%x\n", __func__, base);

	regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL0), &value);
	GNSSCOMM_ERR("%s, TSEN_CTRL0 value read 0x%x\n", __func__, value);
	temp = BIT_TSEN_CLK_SRC_SEL | BIT_TSEN_ADCLDO_EN;
	temp = value & (~temp);
	regmap_write(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL0), temp);
	regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL0), &value);
	GNSSCOMM_ERR("%s, 2nd read 0x%x\n", __func__, value);

	regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL1), &value);
	GNSSCOMM_ERR("%s, TSEN_CTRL1 value read 0x%x\n", __func__, value);
	temp = BIT_TSEN_SDADC_EN | BIT_TSEN_CLK_EN | BIT_TSEN_UGBUF_EN;
	temp = value & (~temp);
	regmap_write(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL1), temp);
	regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL1), &value);
	GNSSCOMM_ERR("%s, 2nd read 0x%x\n", __func__, value);

	regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL3), &value);
	GNSSCOMM_ERR("%s, TSEN_CTRL3 value read 0x%x\n", __func__, value);
	temp = value & ~BIT_TSEN_EN;
	regmap_write(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL3), temp);
	regmap_read(regmap, (REGS_ANA_APB_BASE + TSEN_CTRL3), &value);
	GNSSCOMM_ERR("%s, 2nd read 0x%x\n", __func__, value);

	return 0;
}
#endif

#ifdef CONFIG_UMW2631_I
static void gnss_tcxo_enable(void)
{
	u32 val_buf;
	u32 val_ctl;

	wcn_read_data_from_phy_addr(XTLBUF3_REL_CFG_ADDR, (void *)&val_buf, 4);
	wcn_read_data_from_phy_addr(WCN_XTL_CTRL_ADDR, (void *)&val_ctl, 4);
	GNSSCOMM_INFO("tcxo_en 1read buf=%x ctl=%x\n", val_buf, val_ctl);

	/* XTLBUF3_REL_CF bit[1:0]=01 */
	val_buf &= ~(0x1 << 1);
	val_buf |=  (0x1 << 0);
	/* WCN_XTL_CTRL bit[2:1]=01 */
	val_ctl &= ~(0x1 << 2);
	val_ctl |=  (0x1 << 1);
	GNSSCOMM_INFO("tcxo_en write buf=%x ctl=%x\n", val_buf, val_ctl);

	wcn_write_data_to_phy_addr(XTLBUF3_REL_CFG_ADDR, (void *)&val_buf, 4);
	wcn_write_data_to_phy_addr(WCN_XTL_CTRL_ADDR, (void *)&val_ctl, 4);

	wcn_read_data_from_phy_addr(XTLBUF3_REL_CFG_ADDR, (void *)&val_buf, 4);
	wcn_read_data_from_phy_addr(WCN_XTL_CTRL_ADDR, (void *)&val_ctl, 4);
	GNSSCOMM_INFO("tcxo_en 2read buf=%x ctl=%x\n", val_buf, val_ctl);
}
static void gnss_tcxo_disable(void)
{
	u32 val_buf;
	u32 val_ctl;

	wcn_read_data_from_phy_addr(XTLBUF3_REL_CFG_ADDR, (void *)&val_buf, 4);
	wcn_read_data_from_phy_addr(WCN_XTL_CTRL_ADDR, (void *)&val_ctl, 4);
	GNSSCOMM_INFO("tcxo_dis 1read buf=%x ctl=%x\n", val_buf, val_ctl);

	/* bit[1:0] */
	val_buf &= ~(0x1 << 1);
	val_buf &= ~(0x1 << 0);
	/* bit[2:1] */
	val_ctl &= ~(0x1 << 2);
	val_ctl &= ~(0x1 << 1);
	GNSSCOMM_INFO("tcxo_dis write buf=%x ctl=%x\n", val_buf, val_ctl);

	wcn_write_data_to_phy_addr(XTLBUF3_REL_CFG_ADDR, (void *)&val_buf, 4);
	wcn_write_data_to_phy_addr(WCN_XTL_CTRL_ADDR, (void *)&val_ctl, 4);

	wcn_read_data_from_phy_addr(XTLBUF3_REL_CFG_ADDR, (void *)&val_buf, 4);
	wcn_read_data_from_phy_addr(WCN_XTL_CTRL_ADDR, (void *)&val_ctl, 4);
	GNSSCOMM_INFO("tcxo_dis 2read buf=%x ctl=%x\n", val_buf, val_ctl);
}
#endif

static void gnss_power_on(bool enable)
{
	int ret;
	enum wcn_clock_type clk_type = wcn_get_xtal_26m_clk_type();

	GNSSCOMM_INFO("%s:enable=%d status=%d,clktp=%d\n", __func__,
		      enable, gnss_common_ctl_dev.gnss_status, clk_type);
	if (enable && gnss_common_ctl_dev.gnss_status == GNSS_STATUS_POWEROFF) {
		gnss_common_ctl_dev.gnss_status = GNSS_STATUS_POWERON_GOING;
#ifdef CONFIG_UMW2652_S
		if (clk_type == WCN_CLOCK_TYPE_TSX)
			gnss_tsen_enable(TSEN_EXT);
#endif
#ifdef CONFIG_UMW2631_I
		if (clk_type == WCN_CLOCK_TYPE_TSX)
			gnss_tsen_enable(TSEN_EXT);
		else if (clk_type == WCN_CLOCK_TYPE_TCXO)
			gnss_tcxo_enable();
		else
			GNSSCOMM_ERR("%s: unknown clk_type\n", __func__);
#endif
		ret = start_marlin(gnss_common_ctl_dev.gnss_subsys);
		if (ret != 0)
			GNSSCOMM_INFO("%s: start marlin failed ret=%d\n",
					__func__, ret);
		else
			gnss_common_ctl_dev.gnss_status = GNSS_STATUS_POWERON;
	} else if (!enable && gnss_common_ctl_dev.gnss_status
			== GNSS_STATUS_POWERON) {
		gnss_common_ctl_dev.gnss_status = GNSS_STATUS_POWEROFF_GOING;
#ifdef CONFIG_UMW2652_S
		if (clk_type == WCN_CLOCK_TYPE_TSX)
			gnss_tsen_disable(TSEN_EXT);
#endif
#ifdef CONFIG_UMW2631_I
		if (clk_type == WCN_CLOCK_TYPE_TSX)
			gnss_tsen_disable(TSEN_EXT);
		else if (clk_type == WCN_CLOCK_TYPE_TCXO)
			gnss_tcxo_disable();
		else
			GNSSCOMM_ERR("%s: unknown clk_type\n", __func__);
#endif

		ret = stop_marlin(gnss_common_ctl_dev.gnss_subsys);
		if (ret != 0)
			GNSSCOMM_INFO("%s: stop marlin failed ret=%d\n",
				 __func__, ret);
		else
			gnss_common_ctl_dev.gnss_status = GNSS_STATUS_POWEROFF;
	} else {
		GNSSCOMM_INFO("%s: status is not match\n", __func__);
	}
}

static ssize_t gnss_power_enable_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	unsigned long set_value;

	if (kstrtoul(buf, GNSS_MAX_STRING_LEN, &set_value)) {
		GNSSCOMM_ERR("%s, Maybe store string is too long\n", __func__);
		return -EINVAL;
	}
	GNSSCOMM_INFO("%s,%lu\n", __func__, set_value);
	if (set_value == 1)
		gnss_power_on(1);
	else if (set_value == 0)
		gnss_power_on(0);
	else {
		count = -EINVAL;
		GNSSCOMM_INFO("%s,unknown control\n", __func__);
	}

	return count;
}

static DEVICE_ATTR_WO(gnss_power_enable);

static ssize_t gnss_subsys_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	unsigned long set_value;

	if (kstrtoul(buf, GNSS_MAX_STRING_LEN, &set_value))
		return -EINVAL;

	GNSSCOMM_INFO("%s,%lu\n", __func__, set_value);
#ifndef CONFIG_WCN_INTEG
	gnss_common_ctl_dev.gnss_subsys = MARLIN_GNSS;
#else
	if (set_value == WCN_GNSS)
		gnss_common_ctl_dev.gnss_subsys = WCN_GNSS;
	else if (set_value == WCN_GNSS_BD)
		gnss_common_ctl_dev.gnss_subsys  = WCN_GNSS_BD;
	else
		count = -EINVAL;
#endif
	return count;
}

static ssize_t gnss_subsys_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int i = 0;

	GNSSCOMM_INFO("%s\n", __func__);
	if (gnss_common_ctl_dev.gnss_status == GNSS_STATUS_POWERON) {
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d:%s\n",
				gnss_common_ctl_dev.gnss_subsys,
				&gnss_common_ctl_dev.firmware_path[0]);
	} else {
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n",
				gnss_common_ctl_dev.gnss_subsys);
	}

	return i;
}

static DEVICE_ATTR_RW(gnss_subsys);

#ifdef CONFIG_WCN_INTEG
static int gnss_status_get(void)
{
	phys_addr_t phy_addr;
	u32 magic_value = 0;

	phy_addr = wcn_get_gnss_base_addr() + GNSS_STATUS_OFFSET;
#ifdef CONFIG_UMW2631_I
	phy_addr = wcn_get_gnss_base_addr() + GNSS_QOGIRL6_STATUS_OFFSET;
#endif
	wcn_read_data_from_phy_addr(phy_addr, &magic_value, sizeof(u32));
	GNSSCOMM_INFO("[%s] magic_value=%d\n", __func__, magic_value);

	return magic_value;
}

static void gnss_dump_mem_ctrl_co(char *trigStr)
{
	unsigned int temp_status = 0;
	static char dump_flag;
	char triggerStr[64];

	GNSSCOMM_INFO("[%s], flag is %d\n", __func__, dump_flag);
	if (dump_flag == 1)
		return;
	memset(triggerStr, 0, 64);
	strcpy(triggerStr, trigStr);
	GNSSCOMM_INFO("%s trigStr=%s\n", __func__, triggerStr);

	dump_flag = 1;
	temp_status = gnss_common_ctl_dev.gnss_status;
	GNSSCOMM_INFO("%s: status=%u\n", __func__, temp_status);
	if ((temp_status == GNSS_STATUS_POWERON_GOING) ||
		((temp_status == GNSS_STATUS_POWERON) &&
		(gnss_status_get() != GNSS_CP_STATUS_SLEEP))) {
		wcn_assert_interface(WCN_SOURCE_GNSS, triggerStr);
		gnss_common_ctl_dev.gnss_status = GNSS_STATUS_ASSERT;
	}
}
#else
static void gnss_dump_mem_ctrl(char *trigStr)
{
	static char dump_flag;
	char triggerStr[64];

	GNSSCOMM_INFO("[%s], flag is %d\n", __func__, dump_flag);
	if (dump_flag == 1)
		return;
	memset(triggerStr, 0, 64);
	strcpy(triggerStr, trigStr);
	GNSSCOMM_INFO("%s trigStr=%s\n", __func__, triggerStr);

	dump_flag = 1;
	if (gnss_common_ctl_dev.gnss_status == GNSS_STATUS_POWERON) {
		wcn_assert_interface(WCN_SOURCE_GNSS, triggerStr);
		gnss_common_ctl_dev.gnss_status = GNSS_STATUS_ASSERT;
	}
}
#endif
static ssize_t gnss_dump_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	unsigned long set_value;
	int ret = -1;
	int strlen = 0;
	char trigger_str[64];

	set_value = buf[0] - '0';
	GNSSCOMM_INFO("%s, set_value: %lu\n", __func__, set_value);

	memset(trigger_str, 0, 64);
	strlen = ((count - 2) > 63) ? 63 : (count - 2);
	memcpy(trigger_str, &buf[2], strlen);
	GNSSCOMM_INFO("%s, trigger_str: %s\n", __func__, trigger_str);

	if (set_value == 1) {
#ifdef CONFIG_WCN_INTEG
		gnss_dump_mem_ctrl_co(trigger_str);
#else
		gnss_dump_mem_ctrl(trigger_str);
#endif
	} else
		count = -EINVAL;

	return ret;
}

static DEVICE_ATTR_WO(gnss_dump);

static ssize_t gnss_status_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int i = 0;
	unsigned int status = gnss_common_ctl_dev.gnss_status;

	GNSSCOMM_INFO("%s: %d\n", __func__, status);
	i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n", status);

	return i;
}
static DEVICE_ATTR_RO(gnss_status);

#if defined(CONFIG_UMW2652_S) || defined(CONFIG_UMW2631_I)
static ssize_t gnss_clktype_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int i = 0;
	enum wcn_clock_type clktype = WCN_CLOCK_TYPE_UNKNOWN;

	clktype = wcn_get_xtal_26m_clk_type();
	GNSSCOMM_INFO("%s: %d\n", __func__, clktype);
	i = scnprintf(buf, PAGE_SIZE, "%d\n", clktype);

	return i;
}
static DEVICE_ATTR_RO(gnss_clktype);
#endif

#ifndef CONFIG_WCN_INTEG
static uint gnss_op_reg;
static uint gnss_indirect_reg_offset;
static ssize_t gnss_regr_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	int i = 0;
	unsigned int op_reg = gnss_op_reg;
	unsigned int buffer;

	GNSSCOMM_INFO("%s, register is 0x%x\n", __func__, gnss_op_reg);
	if (op_reg == GNSS_INDIRECT_OP_REG) {
		int set_value;

		set_value = gnss_indirect_reg_offset + 0x80000000;
		sprdwcn_bus_reg_write(op_reg, &set_value, 4);
	}
	sprdwcn_bus_reg_read(op_reg, &buffer, 4);
	GNSSCOMM_INFO("%s,temp value is 0x%x\n", __func__, buffer);

	i += scnprintf((char *)buf + i, PAGE_SIZE - i, "show: 0x%x\n", buffer);

	return i;
}
static DEVICE_ATTR_RO(gnss_regr);

static ssize_t gnss_regaddr_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	unsigned long set_addr;

	if (kstrtoul(buf, GNSS_DATA_BASE_TYPE_H, &set_addr)) {
		GNSSCOMM_ERR("%s, input error\n", __func__);
		return -EINVAL;
	}
	GNSSCOMM_INFO("%s,0x%lx\n", __func__, set_addr);
	gnss_op_reg = (uint)set_addr;

	return count;
}
static DEVICE_ATTR_WO(gnss_regaddr);

static ssize_t gnss_regspaddr_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned long set_addr;

	if (kstrtoul(buf, GNSS_DATA_BASE_TYPE_H, &set_addr)) {
		GNSSCOMM_ERR("%s, input error\n", __func__);
		return -EINVAL;
	}
	GNSSCOMM_INFO("%s,0x%lx\n", __func__, set_addr);
	gnss_op_reg = GNSS_INDIRECT_OP_REG;
	gnss_indirect_reg_offset = (uint)set_addr;
	return count;
}
static DEVICE_ATTR_WO(gnss_regspaddr);

static ssize_t gnss_regw_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long set_value;
	unsigned int op_reg = gnss_op_reg;

	if (kstrtoul(buf, GNSS_DATA_BASE_TYPE_H, &set_value)) {
		GNSSCOMM_ERR("%s, input error\n", __func__);
		return -EINVAL;
	}
	if (op_reg == GNSS_INDIRECT_OP_REG)
		set_value = gnss_indirect_reg_offset + set_value;
	GNSSCOMM_INFO("%s,0x%lx\n", __func__, set_value);
	sprdwcn_bus_reg_write(op_reg, &set_value, 4);

	return count;
}
static DEVICE_ATTR_WO(gnss_regw);
#endif

bool gnss_delay_ctl(void)
{
	return (gnss_common_ctl_dev.gnss_status == GNSS_STATUS_POWERON);
}

static struct attribute *gnss_common_ctl_attrs[] = {
	&dev_attr_gnss_power_enable.attr,
	&dev_attr_gnss_dump.attr,
	&dev_attr_gnss_status.attr,
	&dev_attr_gnss_subsys.attr,
#if defined(CONFIG_UMW2652_S) || defined(CONFIG_UMW2631_I)
	&dev_attr_gnss_clktype.attr,
#endif
#ifndef CONFIG_WCN_INTEG
	&dev_attr_gnss_regr.attr,
	&dev_attr_gnss_regaddr.attr,
	&dev_attr_gnss_regspaddr.attr,
	&dev_attr_gnss_regw.attr,
#endif
	NULL,
};

static struct attribute_group gnss_common_ctl_group = {
	.name = NULL,
	.attrs = gnss_common_ctl_attrs,
};

static struct miscdevice gnss_common_ctl_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gnss_common_ctl",
	.fops = NULL,
};

static int gnss_common_ctl_probe(struct platform_device *pdev)
{
	int ret;
	const struct of_device_id *of_id;

	GNSSCOMM_INFO("%s enter", __func__);
	GNSSCOMM_INFO("%s adj_dump_ops enter", __func__);
	gnss_common_ctl_dev.dev = &pdev->dev;

	gnss_common_ctl_dev.gnss_status = GNSS_STATUS_POWEROFF;
#ifndef CONFIG_WCN_INTEG
	gnss_common_ctl_dev.gnss_subsys = MARLIN_GNSS;
#else
	gnss_common_ctl_dev.gnss_subsys = WCN_GNSS;
#endif
	/* considering backward compatibility, it's not use now  start */
	of_id = of_match_node(gnss_common_ctl_of_match,
		pdev->dev.of_node);
	if (!of_id) {
		dev_err(&pdev->dev,
			"get gnss_common_ctl of device id failed!\n");
		return -ENODEV;
	}
	gnss_common_ctl_dev.chip_ver = (unsigned long)(of_id->data);
	/* considering backward compatibility, it's not use now  end */

	platform_set_drvdata(pdev, &gnss_common_ctl_dev);
	ret = misc_register(&gnss_common_ctl_miscdev);
	if (ret) {
		GNSSCOMM_ERR("%s failed to register gnss_common_ctl.\n",
			__func__);
		return ret;
	}

	ret = sysfs_create_group(&gnss_common_ctl_miscdev.this_device->kobj,
			&gnss_common_ctl_group);
	if (ret) {
		GNSSCOMM_ERR("%s failed to create device attributes.\n",
			__func__);
		goto err_attr_failed;
	}

	return 0;

err_attr_failed:
	misc_deregister(&gnss_common_ctl_miscdev);
	return ret;
}

static int gnss_common_ctl_remove(struct platform_device *pdev)
{
	sysfs_remove_group(&gnss_common_ctl_miscdev.this_device->kobj,
				&gnss_common_ctl_group);

	misc_deregister(&gnss_common_ctl_miscdev);
	return 0;
}
static struct platform_driver gnss_common_ctl_drv = {
	.driver = {
		   .name = "gnss_common_ctl",
		   .of_match_table = of_match_ptr(gnss_common_ctl_of_match),
		   },
	.probe = gnss_common_ctl_probe,
	.remove = gnss_common_ctl_remove
};

//static int __init gnss_common_ctl_init(void)
int gnss_common_ctl_init(void)
{
	GNSSCOMM_INFO("%s enter", __func__);
	return platform_driver_register(&gnss_common_ctl_drv);
}

void gnss_common_ctl_exit(void)
{
	platform_driver_unregister(&gnss_common_ctl_drv);
}

//module_init(gnss_common_ctl_init);
//module_exit(gnss_common_ctl_exit);
//MODULE_LICENSE("GPL v2");
//MODULE_DESCRIPTION("Spreadtrum Gnss Driver");
//MODULE_AUTHOR("Jun.an<jun.an@spreadtrum.com>");
