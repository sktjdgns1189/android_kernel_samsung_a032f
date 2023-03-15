/*
 * Copyright (C) 2018 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/iio/consumer.h>
#include <linux/of.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/usb/phy.h>
#include <linux/usb/otg.h>
#include <linux/power/sc2721-usb-charger.h>
#include <dt-bindings/soc/sprd,sharkl3-mask.h>
#include <dt-bindings/soc/sprd,sharkl3-regs.h>

// /sys/module/phy_sprd_sharkl3/parameters
static unsigned int usb20_tfregres_device = 0x09;
module_param(usb20_tfregres_device, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(usb20_tfregres_device, "These e-fuse signals adjust the internal REG resitor.(efuse)");
static unsigned int usb20_tfhsres_device = 0x16;
module_param(usb20_tfhsres_device, int, S_IRUGO | S_IWUSR);
static unsigned int usb20_tunedsc_device = 0x1;
module_param(usb20_tunedsc_device, int, S_IRUGO | S_IWUSR);
static unsigned int usb20_tunehsamp_device = 0x3;
module_param(usb20_tunehsamp_device, int, S_IRUGO | S_IWUSR);
static unsigned int usb20_tuneotg_device = 0x1;
module_param(usb20_tuneotg_device, int, S_IRUGO | S_IWUSR);



static unsigned int usb20_tfregres_host = 0x22;
module_param(usb20_tfregres_host, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(usb20_tfregres_host, "These e-fuse signals adjust the internal REG resitor.(efuse)");
static unsigned int usb20_tfhsres_host = 0x0F;
module_param(usb20_tfhsres_host, int, S_IRUGO | S_IWUSR);
static unsigned int usb20_tunedsc_host = 0x3;
module_param(usb20_tunedsc_host, int, S_IRUGO | S_IWUSR);
static unsigned int usb20_tunehsamp_host = 0x3;
module_param(usb20_tunehsamp_host, int, S_IRUGO | S_IWUSR);
static unsigned int usb20_tuneotg_host = 0x1;
module_param(usb20_tuneotg_host, int, S_IRUGO | S_IWUSR);



struct sprd_hsphy {
	struct device		*dev;
	struct usb_phy		phy;
	void __iomem		*base;
	struct regulator	*vdd;
	struct regmap           *hsphy_glb;
	struct regmap           *apahb;
	struct regmap           *ana_g2;
	struct regmap           *ana_g4;
	struct regmap           *pmic;
	u32			vdd_vol;
	atomic_t		reset;
	atomic_t		inited;
	bool			is_host;
	struct iio_channel	*dp;
	struct iio_channel	*dm;
};

#define SC2721_CHARGE_DET_FGU_CTRL      0xecc
#define BIT_DP_DM_AUX_EN                BIT(1)
#define BIT_DP_DM_BC_ENB                BIT(0)
#define VOLT_LO_LIMIT                   1200
#define VOLT_HI_LIMIT                   600


static inline void sprd_hsphy_reset_core(struct sprd_hsphy *phy)
{
	u32 msk1, msk2;

	/* Reset PHY */
	msk1 = MASK_AP_AHB_OTG_UTMI_SOFT_RST | MASK_AP_AHB_OTG_SOFT_RST;
	msk2 = MASK_AON_APB_OTG_PHY_SOFT_RST;
	regmap_update_bits(phy->apahb, REG_AP_AHB_AHB_RST,
		msk1, msk1);
	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_APB_RST2,
		msk2, msk2);
	usleep_range(20000, 30000);
	regmap_update_bits(phy->apahb, REG_AP_AHB_AHB_RST,
		msk1, 0);
	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_APB_RST2,
		msk2, 0);
}

static int sprd_hsphy_reset(struct usb_phy *x)
{
	struct sprd_hsphy *phy = container_of(x, struct sprd_hsphy, phy);

	sprd_hsphy_reset_core(phy);
	return 0;
}

static int sprd_hostphy_set(struct usb_phy *x, int on)
{
	struct sprd_hsphy *phy = container_of(x, struct sprd_hsphy, phy);
	u32 reg, msk;

	if (on) {
		msk = MASK_ANLG_PHY_G4_ANALOG_USB20_UTMIOTG_IDDG;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_IDDG, msk, 0);

		msk = MASK_ANLG_PHY_G4_DBG_SEL_ANALOG_USB20_USB20_DMPULLDOWN |
			MASK_ANLG_PHY_G4_DBG_SEL_ANALOG_USB20_USB20_DPPULLDOWN;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_REG_SEL_CFG_0,
			msk, msk);

		msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_DMPULLDOWN |
			MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_DPPULLDOWN;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_USB20_UTMI_CTL2,
			msk, msk);

		msk = 0x200;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_USB20_UTMI_CTL1,
			msk, msk);
		phy->is_host = true;
	} else {
		reg = msk = MASK_ANLG_PHY_G4_ANALOG_USB20_UTMIOTG_IDDG;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_IDDG, msk, reg);

		msk = MASK_ANLG_PHY_G4_DBG_SEL_ANALOG_USB20_USB20_DMPULLDOWN |
			MASK_ANLG_PHY_G4_DBG_SEL_ANALOG_USB20_USB20_DPPULLDOWN;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_REG_SEL_CFG_0,
			msk, msk);

		msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_DMPULLDOWN |
			MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_DPPULLDOWN;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_USB20_UTMI_CTL2,
			msk, 0);

		msk = 0x200;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_USB20_UTMI_CTL1,
			msk, 0);
		phy->is_host = false;
	}
	return 0;
}

static void sprd_hsphy_emphasis_set(struct usb_phy *x, bool enabled)
{
	struct sprd_hsphy *phy = container_of(x, struct sprd_hsphy, phy);
	u32 msk;

	msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEHSAMP;
	if (enabled)
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_USB20_TRIMMING,
			msk, msk);
	else
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_USB20_TRIMMING,
			msk, 0);
}

static int sprd_hsphy_init(struct usb_phy *x)
{
	struct sprd_hsphy *phy = container_of(x, struct sprd_hsphy, phy);
	u32 value, reg, msk;
	static unsigned int usb20_tfregres_default = 0x00;
	int ret;

	if (atomic_read(&phy->inited)) {
		dev_dbg(x->dev, "%s is already inited!\n", __func__);
		return 0;
	}
	dev_info(phy->dev, "sprd_hsphy_init: flags = %d\n", phy->phy.flags);
	/* Turn On VDD */
	regulator_set_voltage(phy->vdd, phy->vdd_vol, phy->vdd_vol);
	if (!regulator_is_enabled(phy->vdd)) {
		ret = regulator_enable(phy->vdd);
		if (ret)
			return ret;
	}
	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_APB_EB2,
		MASK_AON_APB_OTG_REF_EB, MASK_AON_APB_OTG_REF_EB);

	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_PWR_CTRL,
		MASK_AON_APB_USB_PHY_PD_L, 0);
	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_PWR_CTRL,
		MASK_AON_APB_USB_PHY_PD_S, 0);
	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_PWR_CTRL,
		MASK_AON_APB_USB_ISO_SW_EN, 0);

	/* usb vbus valid */
	value = readl_relaxed(phy->base + REG_AP_AHB_OTG_PHY_TEST);
	value |= MASK_AP_AHB_OTG_VBUS_VALID_PHYREG;
	writel_relaxed(value, phy->base + REG_AP_AHB_OTG_PHY_TEST);

	reg = msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_VBUSVLDEXT;
	regmap_update_bits(phy->ana_g4,
		REG_ANLG_PHY_G4_ANALOG_USB20_USB20_UTMI_CTL1, msk, reg);

	reg = msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_DATABUS16_8;
	regmap_update_bits(phy->ana_g4,
		REG_ANLG_PHY_G4_ANALOG_USB20_USB20_UTMI_CTL1, msk, reg);

	/* for SPRD phy utmi_width sel */
	value = readl_relaxed(phy->base + REG_AP_AHB_OTG_PHY_CTRL);
	value |= MASK_AP_AHB_UTMI_WIDTH_SEL;
	writel_relaxed(value, phy->base + REG_AP_AHB_OTG_PHY_CTRL);

	reg = msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEHSAMP;
	regmap_update_bits(phy->ana_g4,
		REG_ANLG_PHY_G4_ANALOG_USB20_USB20_TRIMMING, msk, reg);

	if (usb20_tfregres_default == 0) {
		regmap_read_poll_timeout(phy->ana_g4, REG_ANLG_PHY_G4_ANALOG_USB20_USB20_TRIMMING, usb20_tfregres_default, 1, 100, 1000);
		usb20_tfregres_default &= MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TFREGRES;
		usb20_tfregres_default = usb20_tfregres_default >> 21;
		dev_info(phy->dev, "sprd_hsphy_init: usb20_tfregres_default = 0x%x\n", usb20_tfregres_default);
	}

	if (phy->phy.flags & PHY_HOST_MODE) {
		if (usb20_tfregres_host == 0)
			usb20_tfregres_host = usb20_tfregres_default; //default
		dev_info(phy->dev, "sprd_hsphy_init: usb20_tfregres_host = 0x%x\n", usb20_tfregres_host);
		msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TFREGRES|MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TFHSRES|\
		      MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEDSC|MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEHSAMP|MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEOTG;
		reg =   ((usb20_tunehsamp_host<< 27)& MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEHSAMP)|\
			   ((usb20_tfregres_host << 21)  & MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TFREGRES)|\
			   ((usb20_tfhsres_host << 16)    & MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TFHSRES) |\
			   ((usb20_tuneotg_host << 11)   & MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEOTG)|\
			   ((usb20_tunedsc_host << 9)     & MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEDSC);

		//msk = 0xffffffff;
		//reg = 0x1b934f01;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_USB20_TRIMMING, msk, reg);
	} else {
		if (usb20_tfregres_device == 0)
			usb20_tfregres_device = usb20_tfregres_default; //default
		dev_info(phy->dev, "sprd_hsphy_init: usb20_tfregres_device = 0x%x\n", usb20_tfregres_device);
		msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TFREGRES|MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TFHSRES|\
		      MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEDSC|MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEHSAMP|MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEOTG;
		reg = ((usb20_tunehsamp_device<< 27)& MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEHSAMP)|\
			  ((usb20_tfregres_device<< 21) & MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TFREGRES)|\
			  ((usb20_tfhsres_device << 16)   & MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TFHSRES)|\
			  ((usb20_tuneotg_device << 11)   & MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEOTG)|\
			  ((usb20_tunedsc_device << 9)    & MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TUNEDSC);

		//msk = 0xffffffff;
		//reg = 0x19164b01;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_USB20_TRIMMING, msk, reg);
	}

	if (!atomic_read(&phy->reset)) {
		sprd_hsphy_reset_core(phy);
		atomic_set(&phy->reset, 1);
	}

	atomic_set(&phy->inited, 1);

	return 0;
}

static void sprd_hsphy_shutdown(struct usb_phy *x)
{
	struct sprd_hsphy *phy = container_of(x, struct sprd_hsphy, phy);
	u32 value, reg, msk;

	if (!atomic_read(&phy->inited)) {
		dev_dbg(x->dev, "%s is already shut down\n", __func__);
		return;
	}

	/* usb vbus */
	value = readl_relaxed(phy->base + REG_AP_AHB_OTG_PHY_TEST);
	value &= ~MASK_AP_AHB_OTG_VBUS_VALID_PHYREG;
	writel_relaxed(value, phy->base + REG_AP_AHB_OTG_PHY_TEST);

	reg = msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_VBUSVLDEXT;
	regmap_update_bits(phy->ana_g4,
		REG_ANLG_PHY_G4_ANALOG_USB20_USB20_UTMI_CTL1, msk, 0);

	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_PWR_CTRL,
		MASK_AON_APB_USB_ISO_SW_EN, MASK_AON_APB_USB_ISO_SW_EN);
	usleep_range(10000, 15000);
	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_PWR_CTRL,
		MASK_AON_APB_USB_PHY_PD_L, MASK_AON_APB_USB_PHY_PD_L);
	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_PWR_CTRL,
		MASK_AON_APB_USB_PHY_PD_S, MASK_AON_APB_USB_PHY_PD_S);

	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_APB_EB2,
		MASK_AON_APB_OTG_REF_EB, 0);

	if (regulator_is_enabled(phy->vdd))
		regulator_disable(phy->vdd);

	atomic_set(&phy->inited, 0);
	atomic_set(&phy->reset, 0);
}

static int sprd_hsphy_post_init(struct usb_phy *x)
{
	struct sprd_hsphy *phy = container_of(x, struct sprd_hsphy, phy);

	if (regulator_is_enabled(phy->vdd))
		regulator_disable(phy->vdd);

	return 0;
}

// /sys/devices/platform/soc/soc:ap-ahb/20e00000.hsphy
static ssize_t usb20_tfregres_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	u32 value = 0;
	struct sprd_hsphy *phy = dev_get_drvdata(dev);

	regmap_read_poll_timeout(phy->ana_g4, REG_ANLG_PHY_G4_ANALOG_USB20_USB20_TRIMMING, value, 1, 100, 1000);
	//dev_err(dev, " LJW voltage reg1 %x\n", value);
	value &= MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TFREGRES;
	value = value >> 21;
	//dev_err(dev, " LJW voltage reg2 %x\n", value);
	return sprintf(buf, "0x%x\n", value);
}

static ssize_t usb20_tfregres_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	struct sprd_hsphy *phy = dev_get_drvdata(dev);
	u32 reg , msk;

	if (!phy)
		return -EINVAL;

	if (kstrtouint(buf, 16, &reg) < 0)
		return -EINVAL;

	if (reg == 0) {
		dev_err(dev, "Invalid voltage reg %x\n", reg);
		return -EINVAL;
	}
	//usb20_tfregres = reg;
	msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_TFREGRES;
	reg = reg << 21;
	regmap_update_bits(phy->ana_g4,
		REG_ANLG_PHY_G4_ANALOG_USB20_USB20_TRIMMING, msk, reg);

	return size;
}
static DEVICE_ATTR_RW(usb20_tfregres);

static ssize_t vdd_voltage_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct sprd_hsphy *x = dev_get_drvdata(dev);

	if (!x)
		return -EINVAL;

	return sprintf(buf, "%d\n", x->vdd_vol);
}

static ssize_t vdd_voltage_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	struct sprd_hsphy *x = dev_get_drvdata(dev);
	u32 vol;

	if (!x)
		return -EINVAL;

	if (kstrtouint(buf, 16, &vol) < 0)
		return -EINVAL;

	if (vol < 1200000 || vol > 3750000) {
		dev_err(dev, "Invalid voltage value %d\n", vol);
		return -EINVAL;
	}
	x->vdd_vol = vol;

	return size;
}
static DEVICE_ATTR_RW(vdd_voltage);

static struct attribute *usb_hsphy_attrs[] = {
	&dev_attr_vdd_voltage.attr,
	&dev_attr_usb20_tfregres.attr,
	NULL
};
ATTRIBUTE_GROUPS(usb_hsphy);

static int sprd_hsphy_vbus_notify(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	struct usb_phy *usb_phy = container_of(nb, struct usb_phy, vbus_nb);
	struct sprd_hsphy *phy = container_of(usb_phy, struct sprd_hsphy, phy);
	u32 value, reg, msk;

	if (phy->is_host) {
		dev_info(phy->dev, "USB PHY is host mode\n");
		return 0;
	}

	if (event) {
		/* usb vbus valid */
		value = readl_relaxed(phy->base + REG_AP_AHB_OTG_PHY_TEST);
		value |= MASK_AP_AHB_OTG_VBUS_VALID_PHYREG;
		writel_relaxed(value, phy->base + REG_AP_AHB_OTG_PHY_TEST);

		reg = msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_VBUSVLDEXT;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_USB20_UTMI_CTL1, msk, reg);

		reg = msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_DATABUS16_8;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_USB20_UTMI_CTL1, msk, reg);
		usb_phy_set_charger_state(usb_phy, USB_CHARGER_PRESENT);
	} else {
		/* usb vbus invalid */
		value = readl_relaxed(phy->base + REG_AP_AHB_OTG_PHY_TEST);
		value &= ~MASK_AP_AHB_OTG_VBUS_VALID_PHYREG;
		writel_relaxed(value, phy->base + REG_AP_AHB_OTG_PHY_TEST);

		reg = msk = MASK_ANLG_PHY_G4_ANALOG_USB20_USB20_VBUSVLDEXT;
		regmap_update_bits(phy->ana_g4,
			REG_ANLG_PHY_G4_ANALOG_USB20_USB20_UTMI_CTL1, msk, 0);
		usb_phy_set_charger_state(usb_phy, USB_CHARGER_ABSENT);
	}

	return 0;
}

static enum usb_charger_type sprd_hsphy_charger_detect(struct usb_phy *x)
{
	struct sprd_hsphy *phy = container_of(x, struct sprd_hsphy, phy);

	return sc27xx_charger_detect(phy->pmic);
}

static enum usb_charger_type sprd_hsphy_retry_charger_detect(struct usb_phy *x)
{
	struct sprd_hsphy *phy = container_of(x, struct sprd_hsphy, phy);
	enum usb_charger_type type = UNKNOWN_TYPE;
	int dm_voltage, dp_voltage;
	int cnt = 20;

	if (!phy->dm || !phy->dp) {
		dev_err(x->dev, " phy->dp:%p, phy->dm:%p\n",
			phy->dp, phy->dm);
		return UNKNOWN_TYPE;
	}

	regmap_update_bits(phy->pmic, SC2721_CHARGE_DET_FGU_CTRL,
			   BIT_DP_DM_AUX_EN | BIT_DP_DM_BC_ENB,
			   BIT_DP_DM_AUX_EN);
	msleep(300);
	iio_read_channel_processed(phy->dp, &dp_voltage);
	if (dp_voltage > VOLT_LO_LIMIT) {
		do {
			iio_read_channel_processed(phy->dm, &dm_voltage);
			if (dm_voltage > VOLT_LO_LIMIT) {
				type = DCP_TYPE;
				break;
			}
			msleep(100);
			cnt--;
			iio_read_channel_processed(phy->dp, &dp_voltage);
			if (dp_voltage  < VOLT_HI_LIMIT) {
				type = SDP_TYPE;
				break;
			}
		} while ((x->chg_state == USB_CHARGER_PRESENT) && cnt > 0);
	}
	regmap_update_bits(phy->pmic, SC2721_CHARGE_DET_FGU_CTRL,
			   BIT_DP_DM_AUX_EN | BIT_DP_DM_BC_ENB, 0);
	dev_dbg(x->dev, "correct type is %x\n", type);
	if (type != UNKNOWN_TYPE) {
		x->chg_type = type;
		schedule_work(&x->chg_work);
	}
	return type;
}

static int sprd_hsphy_probe(struct platform_device *pdev)
{
	struct device_node *regmap_np;
	struct platform_device *regmap_pdev;
	struct sprd_hsphy *phy;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret;
	struct usb_otg *otg;
	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "phy_glb_regs");
	if (!res) {
		dev_err(dev, "missing USB PHY registers resource\n");
		return -ENODEV;
	}

	phy->base = devm_ioremap_nocache(dev, res->start, resource_size(res));
	if (IS_ERR(phy->base))
		return PTR_ERR(phy->base);

	regmap_np = of_find_compatible_node(NULL, NULL, "sprd,sc27xx-syscon");
	if (!regmap_np) {
		dev_err(dev, "unable to get syscon node\n");
		return -ENODEV;
	}

	regmap_pdev = of_find_device_by_node(regmap_np);
	if (!regmap_pdev) {
		of_node_put(regmap_np);
		dev_err(dev, "unable to get syscon platform device\n");
		return -ENODEV;
	}

	phy->pmic = dev_get_regmap(regmap_pdev->dev.parent, NULL);
	if (!phy->pmic) {
		dev_err(dev, "unable to get pmic regmap device\n");
		return -ENODEV;
	}

	ret = of_property_read_u32(dev->of_node, "sprd,vdd-voltage",
				   &phy->vdd_vol);
	if (ret < 0) {
		dev_err(dev, "unable to read ssphy vdd voltage\n");
		return ret;
	}

	phy->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(phy->vdd)) {
		dev_err(dev, "unable to get ssphy vdd supply\n");
		return PTR_ERR(phy->vdd);
	}

	ret = regulator_set_voltage(phy->vdd, phy->vdd_vol, phy->vdd_vol);
	if (ret < 0) {
		dev_err(dev, "fail to set ssphy vdd voltage at %dmV\n",
			phy->vdd_vol);
		return ret;
	}

	otg = devm_kzalloc(&pdev->dev, sizeof(*otg), GFP_KERNEL);
	if (!otg)
		return -ENOMEM;

	phy->apahb = syscon_regmap_lookup_by_phandle(dev->of_node,
				 "sprd,syscon-apahb");

	if (IS_ERR(phy->apahb)) {
		dev_err(&pdev->dev, "ap USB apahb syscon failed!\n");
		return PTR_ERR(phy->apahb);
	}

	phy->ana_g4 = syscon_regmap_lookup_by_phandle(dev->of_node,
				 "sprd,syscon-anag4");
	if (IS_ERR(phy->ana_g4)) {
		dev_err(&pdev->dev, "ap USB anag4 syscon failed!\n");
		return PTR_ERR(phy->ana_g4);
	}

	phy->hsphy_glb = syscon_regmap_lookup_by_phandle(dev->of_node,
				 "sprd,syscon-enable");
	if (IS_ERR(phy->hsphy_glb)) {
		dev_err(&pdev->dev, "ap USB aon apb syscon failed!\n");
		return PTR_ERR(phy->hsphy_glb);
	}

	phy->dp = devm_iio_channel_get(dev, "dp");
	phy->dm = devm_iio_channel_get(dev, "dm");
	if (IS_ERR(phy->dp)) {
		phy->dp = NULL;
		dev_warn(dev, "failed to get dp or dm channel\n");
	}
	if (IS_ERR(phy->dm)) {
		phy->dm = NULL;
		dev_warn(dev, "failed to get dp or dm channel\n");
	}

	/* enable usb module */
	regmap_update_bits(phy->apahb, REG_AP_AHB_AHB_EB,
		MASK_AP_AHB_OTG_EB, MASK_AP_AHB_OTG_EB);

	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_APB_EB2,
		MASK_AON_APB_ANLG_APB_EB | MASK_AON_APB_ANLG_EB |
		MASK_AON_APB_OTG_REF_EB,
		MASK_AON_APB_ANLG_APB_EB | MASK_AON_APB_ANLG_EB |
		MASK_AON_APB_OTG_REF_EB);

	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_PWR_CTRL,
		MASK_AON_APB_USB_PHY_PD_L, MASK_AON_APB_USB_PHY_PD_L);

	regmap_update_bits(phy->hsphy_glb, REG_AON_APB_PWR_CTRL,
		MASK_AON_APB_USB_PHY_PD_S, MASK_AON_APB_USB_PHY_PD_S);

	phy->phy.dev = dev;
	phy->phy.label = "sprd-hsphy";
	phy->phy.otg = otg;
	phy->phy.init = sprd_hsphy_init;
	phy->phy.shutdown = sprd_hsphy_shutdown;
	phy->phy.post_init = sprd_hsphy_post_init;
	phy->phy.reset_phy = sprd_hsphy_reset;
	phy->phy.set_vbus = sprd_hostphy_set;
	phy->phy.set_emphasis = sprd_hsphy_emphasis_set;
	phy->phy.type = USB_PHY_TYPE_USB2;
	phy->phy.vbus_nb.notifier_call = sprd_hsphy_vbus_notify;
	phy->phy.charger_detect = sprd_hsphy_charger_detect;
	phy->phy.retry_charger_detect = sprd_hsphy_retry_charger_detect;
	otg->usb_phy = &phy->phy;

	platform_set_drvdata(pdev, phy);

	ret = usb_add_phy_dev(&phy->phy);
	if (ret) {
		dev_err(dev, "fail to add phy\n");
		return ret;
	}

	ret = sysfs_create_groups(&dev->kobj, usb_hsphy_groups);
	if (ret)
		dev_err(dev, "failed to create usb hsphy attributes\n");

	if (extcon_get_state(phy->phy.edev, EXTCON_USB) > 0)
		usb_phy_set_charger_state(&phy->phy, USB_CHARGER_PRESENT);

	dev_info(dev, "sprd usb phy probe ok\n");

	return 0;
}

static int sprd_hsphy_remove(struct platform_device *pdev)
{
	struct sprd_hsphy *phy = platform_get_drvdata(pdev);

	sysfs_remove_groups(&pdev->dev.kobj, usb_hsphy_groups);
	usb_remove_phy(&phy->phy);
	regulator_disable(phy->vdd);

	return 0;
}

static const struct of_device_id sprd_hsphy_match[] = {
	{ .compatible = "sprd,sharkl3-phy" },
	{},
};

MODULE_DEVICE_TABLE(of, sprd_ssphy_match);

static struct platform_driver sprd_hsphy_driver = {
	.probe = sprd_hsphy_probe,
	.remove = sprd_hsphy_remove,
	.driver = {
		.name = "sprd-hsphy",
		.of_match_table = sprd_hsphy_match,
	},
};

static int __init sprd_hsphy_driver_init(void)
{
	return platform_driver_register(&sprd_hsphy_driver);
}

static void __exit sprd_hsphy_driver_exit(void)
{
	platform_driver_unregister(&sprd_hsphy_driver);
}

late_initcall(sprd_hsphy_driver_init);
module_exit(sprd_hsphy_driver_exit);
