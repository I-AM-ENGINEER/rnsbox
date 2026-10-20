/**
 * aicwf_sdmmc.c
 *
 * SDIO function declarations
 *
 * Copyright (C) AICSemi 2018-2020
 */
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/semaphore.h>
#include <linux/debugfs.h>
#include <linux/kthread.h>
#include <linux/gpio.h>
#include "aicsdio_txrxif.h"
#include "aicsdio.h"
#include "aic_bsp_driver.h"
#include <linux/version.h>
#include <linux/delay.h>

/* RNSBox bring-up knobs: the D80 (v3) init path never programs the
 * host clock, so allow overriding clock and bus width from insmod. */
static unsigned int sdio_clk_mhz = 25;
module_param(sdio_clk_mhz, uint, 0644);
MODULE_PARM_DESC(sdio_clk_mhz, "RNSBox: force SDIO clock in MHz (0=leave)");

static void cvi_wlregon(int on);

/* RNSBox: WL_REG_ON is GPIOA15 on the Duo Module 01 / Duo S, but
 * GPIOA26 on the LicheeRV Nano (vendor u-boot cvi_board_init). */
static int wlregon_pin = 15;
module_param(wlregon_pin, int, 0644);
MODULE_PARM_DESC(wlregon_pin, "RNSBox: WL_REG_ON GPIOA bit (-1 disables)");
static bool sd1_pads_config = true;
module_param(sd1_pads_config, bool, 0644);
MODULE_PARM_DESC(sd1_pads_config, "RNSBox: program the SG2000 SD1 pads");

static int chip_reset;
static int rnsbox_chip_reset_set(const char *val, const struct kernel_param *kp)
{
	int rc = param_set_int(val, kp);

	if (rc)
		return rc;
	if (chip_reset) {
		/* power-cycle the AIC chip via WL_REG_ON so a wedged bootrom or
		 * app recovers without a board power cycle */
		cvi_wlregon(0);
		msleep(300);
		cvi_wlregon(1);
		msleep(500);
		printk(KERN_ERR "RNSBOX chip power-cycled via WL_REG_ON\n");
		chip_reset = 0;
	}
	return 0;
}

static const struct kernel_param_ops rnsbox_chip_reset_ops = {
	.set = rnsbox_chip_reset_set,
	.get = param_get_int,
};
module_param_cb(chip_reset, &rnsbox_chip_reset_ops, &chip_reset, 0644);
MODULE_PARM_DESC(chip_reset, "RNSBox: write 1 to power-cycle the chip via WL_REG_ON");
static bool sdio_1bit;
int fw_variant = -1;
module_param(fw_variant, int, 0644);
MODULE_PARM_DESC(fw_variant, "RNSBox: D80 fw list 0=u02 1=u01 2=h_u02");
module_param(sdio_1bit, bool, 0644);
MODULE_PARM_DESC(sdio_1bit, "RNSBox: force 1-bit SDIO bus");
#ifdef CONFIG_PLATFORM_ROCKCHIP
#include <linux/rfkill-wlan.h>
#endif /* CONFIG_PLATFORM_ROCKCHIP */
#ifdef CONFIG_PLATFORM_ROCKCHIP2
#include <linux/rfkill-wlan.h>
#endif /* CONFIG_PLATFORM_ROCKCHIP */


#ifdef CONFIG_PLATFORM_ALLWINNER
extern void sunxi_mmc_rescan_card(unsigned ids);
extern void sunxi_wlan_set_power(int on);
extern int sunxi_wlan_get_bus_index(void);
static int aicbsp_bus_index = -1;
#endif

#ifdef CONFIG_PLATFORM_AMLOGIC//for AML
#include <linux/amlogic/aml_gpio_consumer.h>
extern void sdio_reinit(void);
extern void extern_wifi_set_enable(int is_on);
extern void set_power_control_lock(int lock);
#endif//for AML

// RNSBox Duo Module 01-W: ported from the milkv-duo SDK aicsemi driver —
// without this glue aicbsp_platform_power_on() never asserts WL_REG_ON
// (wifi_pin poweron-gpio, porta 15) and never rescans the wifi-sd host, so
// the chip-up semaphore times out after 2 s ("fail to set AIC_WIFI power
// state to 1") and no SDIO device ever appears.
#ifdef CONFIG_PLATFORM_CVITEK
/* RNSBox mainline 7.3 port: the vendor kernel's wifi_pin driver does not
 * exist in mainline. Drive WL_REG_ON (porta 15 of the gpio0 bank at
 * 0x03020000, a dw-apb-gpio) directly through the register block, and
 * "rescan" the wifi-sd host by re-probing the platform device. */
#include <linux/io.h>
#include <linux/device.h>
#include <linux/platform_device.h>
static void __iomem *cvi_porta_base;

/* SD1 pad config registers inside the pinmux block (0x03001000):
 * CLK 0xA00, CMD 0xA04, D0..D3 0xA08..0xA14. Bits: DS[7:5], PD 3, PU 2. */
static void cvi_sd1_pads_config(void)
{
	static void __iomem *pads;
	int i;
	u8 v;

	if (!pads) {
		pads = ioremap(0x03001000, 0x1000);
		if (!pads)
			return;
	}
	/* Vendor pad values: CLK pull-down, CMD/DAT pull-up, no drive
	 * strength override (PU=bit2, PD=bit3, DS=bits 7:5). */
	v = readb(pads + 0xA00);
	v &= (u8)~0xEC;
	v |= 0x08;
	writeb(v, pads + 0xA00);
	for (i = 0; i < 5; i++) {
		v = readb(pads + 0xA04 + i * 4);
		v &= (u8)~0xE8;
		v |= 0x04;
		writeb(v, pads + 0xA04 + i * 4);
	}
	printk(KERN_ERR "RNSBOX sd1 pads configured\n");
}

static void cvi_sd1_regdump(void)
{
	void __iomem *hc = ioremap(0x04320000, 0x1000);
	u16 clk, ctrl2;
	u8 ctrl0;

	if (!hc)
		return;
	/* Vendor sdhci_cv181x_sdio_reset sets MSHC_CTRL bit16 ("for sd1");
	 * mainline never does. Set it before the host is (re)probed so the
	 * SD1 PHY instance is selected. */
	writel(readl(hc + 0x200) | BIT(16), hc + 0x200);
	clk = readw(hc + 0x2C);
	ctrl0 = readb(hc + 0x28);
	ctrl2 = readw(hc + 0x3E);
	printk(KERN_ERR "RNSBOX sd1 regs: 0x200=%08x 0x240=%08x 0x244=%08x 0x24c=%08x clk=%04x ctrl0=%02x ctrl2=%04x\n",
	       readl(hc + 0x200), readl(hc + 0x240), readl(hc + 0x244),
	       readl(hc + 0x24C), clk, ctrl0, ctrl2);
	iounmap(hc);
}

static void cvi_lpo_fix(void)
{
	static void __iomem *pad, *rtc1, *rtc2;

	if (!pad) {
		pad = ioremap(0x030010B0, 4);   /* PINMUX: CLK32K -> PWR_GPIO_10 */
		rtc1 = ioremap(0x050250AC, 4);  /* RTC domain power (wevb ref) */
		rtc2 = ioremap(0x050260D0, 4);
		if (!pad || !rtc1 || !rtc2) {
			printk(KERN_ERR "RNSBOX lpo: ioremap fail\n");
			return;
		}
	}
	printk(KERN_ERR "RNSBOX lpo before: pad=%08x r1=%08x r2=%08x\n",
	       readl(pad), readl(rtc1), readl(rtc2));
	writel(0x3, pad);
	writel(0x2, rtc1);
	writel(0x3, rtc2);
	printk(KERN_ERR "RNSBOX lpo after: pad=%08x r1=%08x r2=%08x\n",
	       readl(pad), readl(rtc1), readl(rtc2));
}

static void cvi_wifi_wake(int on)
{
	u32 dr, ddr;
	static void __iomem *porte;

	if (!porte) {
		porte = ioremap(0x05021000, 0x100);
		if (!porte)
			return;
	}
	/* dw-apb-ggpio porte (AON bank): E7 = WIFI_WAKE, E9 = BT_REG_ON */
	ddr = readl(porte + 0x04);
	dr = readl(porte + 0x00);
	printk(KERN_ERR "RNSBOX porte before: dr=%08x ddr=%08x\n", dr, ddr);
	ddr |= BIT(7) | BIT(9);
	if (on)
		dr |= BIT(7) | BIT(9);
	else
		dr &= ~(BIT(7) | BIT(9));
	writel(ddr, porte + 0x04);
	writel(dr, porte + 0x00);
	printk(KERN_ERR "RNSBOX porte after: dr=%08x ddr=%08x (on=%d)\n",
	       readl(porte + 0x00), readl(porte + 0x04), on);
}

static void cvi_wlregon(int on)
{
	u32 val;

	if (wlregon_pin < 0 || wlregon_pin > 31)
		return;
	if (!cvi_porta_base) {
		cvi_porta_base = ioremap(0x03020000, 0x100);
		if (!cvi_porta_base)
			return;
	}
	/* dw-apb-gpio bank A: SWPORTA_DR @0x00, SWPORTA_DDR @0x04 */
	val = readl(cvi_porta_base + 0x04) | BIT(wlregon_pin);
	writel(val, cvi_porta_base + 0x04);
	val = readl(cvi_porta_base + 0x00);
	if (on)
		val |= BIT(wlregon_pin);
	else
		val &= ~BIT(wlregon_pin);
	writel(val, cvi_porta_base + 0x00);
}
static int cvi_sdio_rescan(void)
{
	/* mainline: no public API to force a bound platform device to
	 * re-probe from a module. The enumeration is triggered from
	 * userspace instead: after this module asserts WL_REG_ON,
	 * `echo 4320000.mmc > /sys/bus/platform/drivers/sdhci-dwcmshc/unbind`
	 * then `.../bind` rescans the host and the AIC8800 SDIO function
	 * appears (the chip-up semaphore below is posted by the probe).
	 * With the v16+ DT regulator the chip is already powered at boot
	 * and enumerates on its own - this call is then a no-op. */
	return 0;
}
#endif //CONFIG_PLATFORM_CVITEK


static int aicbsp_platform_power_on(void);
static void aicbsp_platform_power_off(void);

struct aic_sdio_dev *aicbsp_sdiodev = NULL;
static struct semaphore *aicbsp_notify_semaphore;
extern struct semaphore aicbsp_probe_semaphore;

static const struct sdio_device_id aicbsp_sdmmc_ids[];
static bool aicbsp_load_fw_in_fdrv = false;

#define FW_PATH_MAX 200

//#ifdef CONFIG_PLATFORM_UBUNTU
//static const char* aic_default_fw_path = "/lib/firmware/aic8800_sdio";
//#else
static const char* aic_default_fw_path = CONFIG_AIC_FW_PATH;
//#endif
char aic_fw_path[FW_PATH_MAX];
module_param_string(aic_fw_path, aic_fw_path, FW_PATH_MAX, 0660);
#ifdef CONFIG_M2D_OTA_AUTO_SUPPORT
char saved_sdk_ver[64];
module_param_string(saved_sdk_ver, saved_sdk_ver,64, 0660);
#endif

extern int testmode;


#define SDIO_DEVICE_ID_AIC8801_FUNC2	0x0146
#define SDIO_DEVICE_ID_AIC8800D80_FUNC2	0x0182

/* SDIO Device ID */
#define SDIO_VENDOR_ID_AIC8801              0x5449
#define SDIO_VENDOR_ID_AIC8800DC            0xc8a1
#define SDIO_VENDOR_ID_AIC8800D80           0xc8a1
#define SDIO_VENDOR_ID_AIC8800D80X2         0xc8a1

#define SDIO_DEVICE_ID_AIC8801				0x0145
#define SDIO_DEVICE_ID_AIC8800DC			0xc08d
#define SDIO_DEVICE_ID_AIC8800D80           0x0082
#define SDIO_DEVICE_ID_AIC8800D80X2         0x2082


static int aicbsp_dummy_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
	if (func && (func->num != 2))
		return 0;

	if(func->vendor != SDIO_VENDOR_ID_AIC8801 &&
		func->device != SDIO_DEVICE_ID_AIC8801 &&
		func->device != SDIO_DEVICE_ID_AIC8801_FUNC2 &&
		func->vendor != SDIO_VENDOR_ID_AIC8800DC &&
		func->device != SDIO_DEVICE_ID_AIC8800DC &&
		func->vendor != SDIO_VENDOR_ID_AIC8800D80 &&
		func->device != SDIO_DEVICE_ID_AIC8800D80 &&
		func->device != SDIO_DEVICE_ID_AIC8800D80_FUNC2){
			printk("VID:%x DID:%X \r\n", func->vendor, func->device);
			aicbsp_load_fw_in_fdrv = true;
    }

	if (aicbsp_notify_semaphore)
		up(aicbsp_notify_semaphore);
	return 0;
}

static void aicbsp_dummy_remove(struct sdio_func *func)
{
}

static struct sdio_driver aicbsp_dummy_sdmmc_driver = {
	.probe		= aicbsp_dummy_probe,
	.remove		= aicbsp_dummy_remove,
	.name		= "aicbsp_dummy_sdmmc",
	.id_table	= aicbsp_sdmmc_ids,
};

static int aicbsp_reg_sdio_notify(void *semaphore)
{
	aicbsp_notify_semaphore = semaphore;
	return sdio_register_driver(&aicbsp_dummy_sdmmc_driver);
}

static void aicbsp_unreg_sdio_notify(void)
{
	mdelay(15);
	sdio_unregister_driver(&aicbsp_dummy_sdmmc_driver);
}

static const char *aicbsp_subsys_name(int subsys)
{
	switch (subsys) {
	case AIC_BLUETOOTH:
		return "AIC_BLUETOOTH";
	case AIC_WIFI:
		return "AIC_WIFI";
	default:
		return "unknown subsys";
	}
}

#ifdef CONFIG_PLATFORM_ROCKCHIP
#if 1//FOR RK SUSPEND
void rfkill_rk_sleep_bt(bool sleep);
#endif
#endif

#ifdef CONFIG_PLATFORM_ROCKCHIP2
#if 1//FOR RK SUSPEND
void rfkill_rk_sleep_bt(bool sleep);
#endif
#endif

int aicbsp_set_subsys(int subsys, int state)
{
	static int pre_power_map;
	int cur_power_map;
	int pre_power_state;
	int cur_power_state;

	mutex_lock(&aicbsp_power_lock);
	aicbsp_load_fw_in_fdrv = false;
	cur_power_map = pre_power_map;
	if (state)
		cur_power_map |= (1 << subsys);
	else
		cur_power_map &= ~(1 << subsys);

	pre_power_state = pre_power_map > 0;
	cur_power_state = cur_power_map > 0;

	sdio_dbg("%s, subsys: %s, state to: %d\n", __func__, aicbsp_subsys_name(subsys), state);

	if (cur_power_state != pre_power_state) {
		sdio_dbg("%s, power state change to %d dure to %s\n", __func__, cur_power_state, aicbsp_subsys_name(subsys));
		if (cur_power_state) {
			if (aicbsp_platform_power_on() < 0)
				goto err0;
			if (aicbsp_sdio_init())
				goto err1;
			if (aicbsp_driver_fw_init(aicbsp_sdiodev))
				goto err2;
#ifndef CONFIG_FDRV_NO_REG_SDIO
			aicbsp_sdio_release(aicbsp_sdiodev);
#endif

#if defined CONFIG_PLATFORM_ROCKCHIP || defined CONFIG_PLATFORM_ROCKCHIP2
#ifdef CONFIG_GPIO_WAKEUP
			//BT_SLEEP:true,BT_WAKEUP:false
			rfkill_rk_sleep_bt(true);
			printk("%s BT wake default to SLEEP\r\n", __func__);
#endif
#endif

//#ifndef CONFIG_PLATFORM_ROCKCHIP
//			aicbsp_sdio_exit();
//#endif
		} else {
		#ifndef CONFIG_PLATFORM_ROCKCHIP
			aicbsp_sdio_exit();
		#endif
			aicbsp_platform_power_off();
		}
	} else {
		sdio_dbg("%s, power state no need to change, current: %d\n", __func__, cur_power_state);
	}
	pre_power_map = cur_power_map;
	mutex_unlock(&aicbsp_power_lock);

	return cur_power_state;

err2:
	aicbsp_sdio_release(aicbsp_sdiodev);
	aicbsp_sdio_exit();

err1:
	aicbsp_platform_power_off();

err0:
	sdio_dbg("%s, fail to set %s power state to %d\n", __func__, aicbsp_subsys_name(subsys), state);
	mutex_unlock(&aicbsp_power_lock);
	return -1;
}
EXPORT_SYMBOL_GPL(aicbsp_set_subsys);

bool aicbsp_get_load_fw_in_fdrv(void){
	return aicbsp_load_fw_in_fdrv;
}

EXPORT_SYMBOL_GPL(aicbsp_get_load_fw_in_fdrv);

static int force_chipid = -1;
module_param(force_chipid, int, 0644);
MODULE_PARM_DESC(force_chipid, "RNSBox: force chipid (0=8801 1=8800DC 3=8800D80)");

static int aicwf_sdio_chipmatch(struct aic_sdio_dev *sdio_dev, uint16_t vid, uint16_t did){

	if (force_chipid >= 0) {
		sdio_dev->chipid = force_chipid;
		printk(KERN_ERR "RNSBOX chipmatch: forced chipid=%d (vid=%04x did=%04x)\n",
		       force_chipid, vid, did);
		return 0;
	}
	if(vid == SDIO_VENDOR_ID_AIC8801 && did == SDIO_DEVICE_ID_AIC8801){
		sdio_dev->chipid = PRODUCT_ID_AIC8801;
		AICWFDBG(LOGINFO, "%s USE AIC8801\r\n", __func__);
		return 0;
	}else if(vid == SDIO_VENDOR_ID_AIC8800DC && did == SDIO_DEVICE_ID_AIC8800DC){
		sdio_dev->chipid = PRODUCT_ID_AIC8800DC;
		AICWFDBG(LOGINFO, "%s USE AIC8800DC\r\n", __func__);
		return 0;
	}else if(vid == SDIO_VENDOR_ID_AIC8800D80 && did == SDIO_DEVICE_ID_AIC8800D80){
		sdio_dev->chipid = PRODUCT_ID_AIC8800D80;
		AICWFDBG(LOGINFO, "%s USE AIC8800D80\r\n", __func__);
		return 0;
	}else if(vid == SDIO_VENDOR_ID_AIC8800D80X2 && did == SDIO_DEVICE_ID_AIC8800D80X2){
		sdio_dev->chipid = PRODUCT_ID_AIC8800D80X2;
		AICWFDBG(LOGINFO, "%s USE AIC8800D80X2\r\n", __func__);
		return 0;
	}else{
		return -1;
	}
}

void *aicbsp_get_drvdata(void *args)
{
	(void)args;
	if (aicbsp_sdiodev)
		return aicbsp_sdiodev->bus_if;
	return dev_get_drvdata((const struct device *)args);
}


static int aicbsp_sdio_probe(struct sdio_func *func,
	const struct sdio_device_id *id)
{
	struct mmc_host *host;
	struct aic_sdio_dev *sdiodev;
	struct aicwf_bus *bus_if;
	int err = -ENODEV;

	if (func == NULL) {
		sdio_err("%s func is null\n", __func__);
		return err;
	}

	sdio_dbg("%s:%d vid:0x%04X  did:0x%04X\n", __func__, func->num,
		func->vendor, func->device);

	if(func->vendor != SDIO_VENDOR_ID_AIC8801 &&
		func->device != SDIO_DEVICE_ID_AIC8801 &&
		func->device != SDIO_DEVICE_ID_AIC8801_FUNC2 &&
		func->vendor != SDIO_VENDOR_ID_AIC8800DC &&
		func->device != SDIO_DEVICE_ID_AIC8800DC &&
		func->vendor != SDIO_VENDOR_ID_AIC8800D80 &&
		func->device != SDIO_DEVICE_ID_AIC8800D80 &&
		func->device != SDIO_DEVICE_ID_AIC8800D80_FUNC2){
		aicbsp_load_fw_in_fdrv = true;
		return err;
	}

	if (func->num != 2) {
		return err;
	}

	host = func->card->host;
	host->caps |= MMC_CAP_NONREMOVABLE;

	func = func->card->sdio_func[1 - 1]; //replace 2 with 1

	sdio_dbg("%s after replace:%d\n", __func__, func->num);

	bus_if = kzalloc(sizeof(struct aicwf_bus), GFP_KERNEL);
	if (!bus_if) {
		sdio_err("alloc bus fail\n");
		return -ENOMEM;
	}


	sdiodev = kzalloc(sizeof(struct aic_sdio_dev), GFP_KERNEL);
	if (!sdiodev) {
		sdio_err("alloc sdiodev fail\n");
		kfree(bus_if);
		return -ENOMEM;
	}
    aicbsp_sdiodev = sdiodev;

	err = aicwf_sdio_chipmatch(sdiodev, func->vendor, func->device);

	sdiodev->func = func;
	if(sdiodev->chipid == PRODUCT_ID_AIC8800DC || sdiodev->chipid == PRODUCT_ID_AIC8800DW){
		sdiodev->func_msg = func->card->sdio_func[1];
	}
	sdiodev->bus_if = bus_if;
	bus_if->bus_priv.sdio = sdiodev;
	if(sdiodev->chipid == PRODUCT_ID_AIC8800DC || sdiodev->chipid == PRODUCT_ID_AIC8800DW){
		dev_set_drvdata(&sdiodev->func_msg->dev, bus_if);
		printk("the device is PRODUCT_ID_AIC8800DC \n");
	}
	dev_set_drvdata(&func->dev, bus_if);
	sdiodev->dev = &func->dev;

    if (sdiodev->chipid != PRODUCT_ID_AIC8800D80 && sdiodev->chipid != PRODUCT_ID_AIC8800D80X2) {
	    err = aicwf_sdio_func_init(sdiodev);
    } else {
        err = aicwf_sdiov3_func_init(sdiodev);
    }
	if (err < 0) {
		sdio_err("sdio func init fail\n");
		goto fail;
	}

	if (aicwf_sdio_bus_init(sdiodev) == NULL) {
		sdio_err("sdio bus init err\r\n");
		goto fail;
	}

	aicbsp_platform_init(sdiodev);

	up(&aicbsp_probe_semaphore);

	return 0;
fail:
	aicwf_sdio_func_deinit(sdiodev);
	dev_set_drvdata(&func->dev, NULL);
	kfree(sdiodev);
	kfree(bus_if);
	return err;
}


static void aicbsp_sdio_remove(struct sdio_func *func)
{
	struct mmc_host *host;
	struct aicwf_bus *bus_if = NULL;
	struct aic_sdio_dev *sdiodev = NULL;

	AICWFDBG(LOGINFO, "%s\n", __func__);
	if (aicbsp_sdiodev == NULL) {
		AICWFDBG(LOGERROR, "%s: allready unregister\n", __func__);
		goto done;
	}
	if (func == NULL) {
		AICWFDBG(LOGERROR, "%s, sdio func is null\n", __func__);
		goto done;
	}

	host = func->card->host;
	host->caps &= ~MMC_CAP_NONREMOVABLE;

	bus_if = aicbsp_get_drvdata(&func->dev);

	if (!bus_if) {
		AICWFDBG(LOGERROR, "%s bus_if is NULL \r\n", __func__);
		goto done;
	}

	sdiodev = bus_if->bus_priv.sdio;
	if (!sdiodev) {
		AICWFDBG(LOGERROR, "%s sdiodev is NULL \r\n", __func__);
		goto done;
	}

	aicwf_sdio_release(sdiodev);
	aicwf_sdio_func_deinit(sdiodev);

	dev_set_drvdata(&sdiodev->func->dev, NULL);
	kfree(sdiodev);

done:
	if (bus_if)
		kfree(bus_if);
	aicbsp_sdiodev = NULL;
	sdio_dbg("%s done\n", __func__);
}


static int aicbsp_sdio_suspend(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	int err;
	mmc_pm_flag_t sdio_flags;

#if defined(CONFIG_PLATFORM_ROCKCHIP) || defined(CONFIG_PLATFORM_ROCKCHIP2)
#ifdef CONFIG_GPIO_WAKEUP
    //BT_SLEEP:true,BT_WAKEUP:false
    rfkill_rk_sleep_bt(false);
#endif
#endif

	sdio_dbg("%s, func->num = %d\n", __func__, func->num);
	if (func->num != 2)
		return 0;

	sdio_flags = sdio_get_host_pm_caps(func);
	if (!(sdio_flags & MMC_PM_KEEP_POWER)) {
		sdio_dbg("%s: can't keep power while host is suspended\n", __func__);
		return  -EINVAL;
	}

	/* keep power while host suspended */
	err = sdio_set_host_pm_flags(func, MMC_PM_KEEP_POWER);
	if (err) {
		sdio_dbg("%s: error while trying to keep power\n", __func__);
		return err;
	}

#if defined(CONFIG_PLATFORM_ROCKCHIP) || defined(CONFIG_PLATFORM_ROCKCHIP2)
#ifdef CONFIG_GPIO_WAKEUP
		//BT_SLEEP:true,BT_WAKEUP:false
		rfkill_rk_sleep_bt(true);
		printk("%s BT wake to SLEEP\r\n", __func__);
#endif
#endif

	return 0;
}

static int aicbsp_sdio_resume(struct device *dev)
{
	sdio_dbg("%s\n", __func__);

#if defined(CONFIG_PLATFORM_ROCKCHIP) || defined(CONFIG_PLATFORM_ROCKCHIP2)
#ifdef CONFIG_GPIO_WAKEUP
		//BT_SLEEP:true,BT_WAKEUP:false
		rfkill_rk_sleep_bt(false);
#endif
#endif

	return 0;
}

static const struct sdio_device_id aicbsp_sdmmc_ids[] = {
	{SDIO_DEVICE_CLASS(SDIO_CLASS_WLAN)},
	{ },
};

MODULE_DEVICE_TABLE(sdio, aicbsp_sdmmc_ids);

static const struct dev_pm_ops aicbsp_sdio_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(aicbsp_sdio_suspend, aicbsp_sdio_resume)
};

static struct sdio_driver aicbsp_sdio_driver = {
	.probe = aicbsp_sdio_probe,
	.remove = aicbsp_sdio_remove,
	.name = AICBSP_SDIO_NAME,
	.id_table = aicbsp_sdmmc_ids,
	.drv = {
		.pm = &aicbsp_sdio_pm_ops,
	},
};

static int aicbsp_platform_power_on(void)
{
	int ret = 0;
	struct semaphore aic_chipup_sem;
	sdio_dbg("%s\n", __func__);

#ifdef CONFIG_PLATFORM_ALLWINNER
	if (aicbsp_bus_index < 0)
		 aicbsp_bus_index = sunxi_wlan_get_bus_index();
	if (aicbsp_bus_index < 0)
		return aicbsp_bus_index;
#endif //CONFIG_PLATFORM_ALLWINNER

#ifdef CONFIG_PLATFORM_AMLOGIC
		extern_wifi_set_enable(0);
		mdelay(200);
		extern_wifi_set_enable(1);
		mdelay(200);
		sdio_reinit();
		set_power_control_lock(1);
#endif

#ifdef CONFIG_PLATFORM_ROCKCHIP2
            rockchip_wifi_power(0);
            mdelay(50);
            rockchip_wifi_power(1);
            mdelay(50);
            rockchip_wifi_set_carddetect(1);
#endif /*CONFIG_PLATFORM_ROCKCHIP2*/

#ifdef CONFIG_PLATFORM_CVITEK
	// Duo Module 01-W: assert WL_REG_ON before the bus scan; the AIC8800
	// comes up unpowered on this module.
	printk("======== CVITEK WLAN_POWER_ON ========\n");
	if (sd1_pads_config)
		cvi_sd1_pads_config();
	cvi_lpo_fix();
	cvi_wlregon(1);
	mdelay(50);
	cvi_sd1_regdump();
#endif //CONFIG_PLATFORM_CVITEK

	sema_init(&aic_chipup_sem, 0);
	ret = aicbsp_reg_sdio_notify(&aic_chipup_sem);
	if (ret) {
		sdio_dbg("%s aicbsp_reg_sdio_notify fail(%d)\n", __func__, ret);
			return ret;
	}

#ifdef CONFIG_PLATFORM_ALLWINNER
	sunxi_wlan_set_power(0);
	mdelay(50);
	sunxi_wlan_set_power(1);
	mdelay(50);
	sunxi_mmc_rescan_card(aicbsp_bus_index);
#endif //CONFIG_PLATFORM_ALLWINNER

#ifdef CONFIG_PLATFORM_CVITEK
	// The wifi-sd host has no card-detect pin; only an explicit
	// mmc_detect_change() makes it (re)enumerate the now-powered chip.
	printk("%s,%d: cvi_sdio_rescan\n", __func__, __LINE__);
	cvi_sdio_rescan();
	//udelay(1000);
#endif

	if (down_timeout(&aic_chipup_sem, msecs_to_jiffies(5000)) == 0) {
		aicbsp_unreg_sdio_notify();
		if(aicbsp_load_fw_in_fdrv){
			printk("%s load fw in fdrv\r\n", __func__);
			return -1;
		}
		return 0;
	}

	aicbsp_unreg_sdio_notify();
#ifdef CONFIG_PLATFORM_ALLWINNER
	sunxi_wlan_set_power(0);
#endif //CONFIG_PLATFORM_ALLWINNER

#ifdef CONFIG_PLATFORM_AMLOGIC
        extern_wifi_set_enable(0);
#endif


#ifdef CONFIG_PLATFORM_ROCKCHIP2
	rockchip_wifi_power(0);
#endif /*CONFIG_PLATFORM_ROCKCHIP2*/

#ifdef CONFIG_PLATFORM_CVITEK
	// Chip never enumerated: drop WL_REG_ON again so a later retry
	// starts from a clean power cycle.
	cvi_wlregon(0);
#endif //CONFIG_PLATFORM_CVITEK

	return -1;
}

static void aicbsp_platform_power_off(void)
{
//TODO wifi disable and sdio card detection
#ifdef CONFIG_PLATFORM_ALLWINNER
	if (aicbsp_bus_index < 0)
		 aicbsp_bus_index = sunxi_wlan_get_bus_index();
	if (aicbsp_bus_index < 0) {
		sdio_dbg("no aicbsp_bus_index\n");
		return;
	}
	sunxi_wlan_set_power(0);
	mdelay(100);
	sunxi_mmc_rescan_card(aicbsp_bus_index);
#endif //CONFIG_PLATFORM_ALLWINNER

#ifdef CONFIG_PLATFORM_ROCKCHIP2
	rockchip_wifi_set_carddetect(0);
	mdelay(200);
	rockchip_wifi_power(0);
	mdelay(200);
#endif /*CONFIG_PLATFORM_ROCKCHIP*/
#ifdef CONFIG_PLATFORM_AMLOGIC
	extern_wifi_set_enable(0);
#endif

#ifdef CONFIG_PLATFORM_CVITEK
	printk("======== CVITEK WLAN_POWER_OFF ========\n");
	cvi_wlregon(0);
#endif //CONFIG_PLATFORM_CVITEK

	sdio_dbg("%s\n", __func__);
}


int aicbsp_sdio_init(void)
{

	if (sdio_register_driver(&aicbsp_sdio_driver)) {
		return -1;
	} else {
		//may add mmc_rescan here
	}
	if (down_timeout(&aicbsp_probe_semaphore, msecs_to_jiffies(2000)) != 0){
		printk("%s aicbsp_sdio_probe fail\r\n", __func__);
		return -1;
	}

	
	return 0;
}

void aicbsp_sdio_exit(void)
{
	sdio_unregister_driver(&aicbsp_sdio_driver);
}

void aicbsp_sdio_release(struct aic_sdio_dev *sdiodev)
{
	sdiodev->bus_if->state = BUS_DOWN_ST;
	sdio_claim_host(sdiodev->func);
	sdio_release_irq(sdiodev->func);
	sdio_release_host(sdiodev->func);
	if(sdiodev->chipid == PRODUCT_ID_AIC8800DC  || sdiodev->chipid == PRODUCT_ID_AIC8800DW){
		sdio_claim_host(sdiodev->func_msg);
		sdio_release_irq(sdiodev->func_msg);
		sdio_release_host(sdiodev->func_msg);
	}
}

int aicwf_sdio_readb(struct aic_sdio_dev *sdiodev, uint regaddr, u8 *val)
{
	int ret;
	sdio_claim_host(sdiodev->func);
	*val = sdio_readb(sdiodev->func, regaddr, &ret);
	sdio_release_host(sdiodev->func);
	return ret;
}

int aicwf_sdio_readb_func2(struct aic_sdio_dev *sdiodev, uint regaddr, u8 *val)
{
    int ret;
    sdio_claim_host(sdiodev->func_msg);
    *val = sdio_readb(sdiodev->func_msg, regaddr, &ret);
    sdio_release_host(sdiodev->func_msg);
    return ret;
}

int aicwf_sdio_writeb(struct aic_sdio_dev *sdiodev, uint regaddr, u8 val)
{
	int ret;
	sdio_claim_host(sdiodev->func);
	sdio_writeb(sdiodev->func, val, regaddr, &ret);
	sdio_release_host(sdiodev->func);
	return ret;
}

int aicwf_sdio_writeb_func2(struct aic_sdio_dev *sdiodev, uint regaddr, u8 val)
{
    int ret;
    sdio_claim_host(sdiodev->func_msg);
    sdio_writeb(sdiodev->func_msg, val, regaddr, &ret);
    sdio_release_host(sdiodev->func_msg);
    return ret;
}

int aicwf_sdio_flow_ctrl(struct aic_sdio_dev *sdiodev)
{
	int ret = -1;
	u8 fc_reg = 0;
	u32 count = 0;

	while (true) {
		ret = aicwf_sdio_readb(sdiodev, sdiodev->sdio_reg.flow_ctrl_reg, &fc_reg);
		if (ret) {
			return -1;
		}

        if (sdiodev->chipid == PRODUCT_ID_AIC8801 || sdiodev->chipid == PRODUCT_ID_AIC8800DC ||
            sdiodev->chipid == PRODUCT_ID_AIC8800DW) {
            fc_reg = fc_reg & SDIOWIFI_FLOWCTRL_MASK_REG;
        }

		if (fc_reg != 0) {
            ret = fc_reg;
			return ret;
		} else {
			if (count >= FLOW_CTRL_RETRY_COUNT) {
				ret = -fc_reg;
				break;
			}
			count++;
			if (count < 30)
				udelay(200);
			else if (count < 40)
				mdelay(1);
			else
				mdelay(10);
		}
	}

	return ret;
}

int aicwf_sdio_send_msg(struct aic_sdio_dev *sdiodev, u8 *buf, uint count)
{
    int ret = 0;

    sdio_claim_host(sdiodev->func_msg);
    ret = sdio_writesb(sdiodev->func_msg, 7, buf, count);
    sdio_release_host(sdiodev->func_msg);

    return ret;
}

int aicwf_sdio_send_pkt(struct aic_sdio_dev *sdiodev, u8 *buf, uint count)
{
	int ret = 0;

	sdio_claim_host(sdiodev->func);
	ret = sdio_writesb(sdiodev->func, sdiodev->sdio_reg.wr_fifo_addr, buf, count);
	sdio_release_host(sdiodev->func);

	return ret;
}

int aicwf_sdio_recv_pkt(struct aic_sdio_dev *sdiodev, struct sk_buff *skbbuf,
	u32 size, u8 msg)
{
	int ret;

	if ((!skbbuf) || (!size)) {
		return -EINVAL;;
	}

    if(!msg) {
        sdio_claim_host(sdiodev->func);
        ret = sdio_readsb(sdiodev->func, skbbuf->data, sdiodev->sdio_reg.rd_fifo_addr, size);
        if (ret) {
            printk(KERN_ERR "RNSBOX readsb(fixed/block) ret=%d, trying incr\n", ret);
            ret = sdio_memcpy_fromio(sdiodev->func, skbbuf->data,
                                     sdiodev->sdio_reg.rd_fifo_addr, size);
            printk(KERN_ERR "RNSBOX memcpy_fromio(incr/block) ret=%d\n", ret);
        }
        sdio_release_host(sdiodev->func);
    } else {
        sdio_claim_host(sdiodev->func_msg);
        ret = sdio_readsb(sdiodev->func_msg, skbbuf->data, sdiodev->sdio_reg.rd_fifo_addr, size);
        sdio_release_host(sdiodev->func_msg);
    }


	if (ret < 0)
		printk(KERN_ERR "RNSBOX recv_pkt size=%u ret=%d\n", size, ret);
	if (ret < 0) {
		return ret;
	}
	skbbuf->len = size;

	return ret;
}


#if defined(CONFIG_SDIO_PWRCTRL)
int aicwf_sdio_wakeup(struct aic_sdio_dev *sdiodev)
{
	int ret = 0;
	int read_retry;
	int write_retry = 20;
    int wakeup_reg_val = 0;

    if (sdiodev->chipid == PRODUCT_ID_AIC8801 ||
        sdiodev->chipid == PRODUCT_ID_AIC8800DC ||
        sdiodev->chipid == PRODUCT_ID_AIC8800DW) {
        wakeup_reg_val = 1;
    } else if (sdiodev->chipid == PRODUCT_ID_AIC8800D80 || sdiodev->chipid == PRODUCT_ID_AIC8800D80X2) {
        wakeup_reg_val = 0x11;
    }

	if (sdiodev->state == SDIO_SLEEP_ST) {
		//if (sdiodev->rwnx_hw->vif_started) {
			down(&sdiodev->pwrctl_wakeup_sema);
			while (write_retry) {
				ret = aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.wakeup_reg, wakeup_reg_val);
				if (ret) {
					txrx_err("sdio wakeup fail\n");
					ret = -1;
				} else {
					read_retry = 10;
					while (read_retry) {
						u8 val;
						ret = aicwf_sdio_readb(sdiodev, sdiodev->sdio_reg.sleep_reg, &val);
						if ((ret == 0) && (val & 0x10)) {
							break;
						}
						read_retry--;
						udelay(200);
					}
					if (read_retry != 0)
						break;
				}
				sdio_dbg("write retry:  %d \n", write_retry);
				write_retry--;
				udelay(100);
			}
			up(&sdiodev->pwrctl_wakeup_sema);
	   // }
	}

	sdiodev->state = SDIO_ACTIVE_ST;
	aicwf_sdio_pwrctl_timer(sdiodev, sdiodev->active_duration);

	return ret;
}

extern u8 dhcped;
int aicwf_sdio_sleep_allow(struct aic_sdio_dev *sdiodev)
{
	int ret = 0;
	struct aicwf_bus *bus_if = sdiodev->bus_if;

	if (bus_if->state == BUS_DOWN_ST) {
		ret = aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.sleep_reg, 0x10);
		if (ret) {
			sdio_err("Write sleep fail!\n");
	}
		aicwf_sdio_pwrctl_timer(sdiodev, 0);
		return ret;
	}

	if (sdiodev->state == SDIO_ACTIVE_ST) {
		{
			sdio_dbg("s\n");
			ret = aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.sleep_reg, 0x10);
			if (ret)
				sdio_err("Write sleep fail!\n");
		}
		sdiodev->state = SDIO_SLEEP_ST;
		aicwf_sdio_pwrctl_timer(sdiodev, 0);
	} else {
		aicwf_sdio_pwrctl_timer(sdiodev, sdiodev->active_duration);
	}

	return ret;
}

int aicwf_sdio_pwr_stctl(struct aic_sdio_dev *sdiodev, uint target)
{
	int ret = 0;

	if (sdiodev->bus_if->state == BUS_DOWN_ST) {
		return -1;
	}

	if (sdiodev->state == target) {
		if (target == SDIO_ACTIVE_ST) {
			aicwf_sdio_pwrctl_timer(sdiodev, sdiodev->active_duration);
		}
		return ret;
	}

	switch (target) {
	case SDIO_ACTIVE_ST:
		aicwf_sdio_wakeup(sdiodev);
		break;
	case SDIO_SLEEP_ST:
		aicwf_sdio_sleep_allow(sdiodev);
		break;
	}

	return ret;
}
#endif

int aicwf_sdio_txpkt(struct aic_sdio_dev *sdiodev, struct sk_buff *pkt)
{
	int ret = 0;
	u8 *frame;
	u32 len = 0;
	struct aicwf_bus *bus_if = dev_get_drvdata(sdiodev->dev);

	if (bus_if->state == BUS_DOWN_ST) {
		sdio_dbg("tx bus is down!\n");
		return -EINVAL;
	}

	frame = (u8 *) (pkt->data);
	len = pkt->len;
	len = (len + SDIOWIFI_FUNC_BLOCKSIZE - 1) / SDIOWIFI_FUNC_BLOCKSIZE * SDIOWIFI_FUNC_BLOCKSIZE;
	ret = aicwf_sdio_send_pkt(sdiodev, pkt->data, len);
	if (ret)
		sdio_err("aicwf_sdio_send_pkt fail%d\n", ret);

	return ret;
}

static int aicwf_sdio_intr_get_len_bytemode(struct aic_sdio_dev *sdiodev, u8 *byte_len)
{
	int ret = 0;

	if (!byte_len)
		return -EBADE;

	if (sdiodev->bus_if->state == BUS_DOWN_ST) {
		*byte_len = 0;
	} else {
		ret = aicwf_sdio_readb(sdiodev, sdiodev->sdio_reg.bytemode_len_reg, byte_len);
		sdiodev->rx_priv->data_len = (*byte_len)*4;
	}

	return ret;
}

static void aicwf_sdio_bus_stop(struct device *dev)
{
	struct aicwf_bus *bus_if = aicbsp_get_drvdata(dev);
	struct aic_sdio_dev *sdiodev = bus_if->bus_priv.sdio;
	int ret;

#if defined(CONFIG_SDIO_PWRCTRL)
	aicwf_sdio_pwrctl_timer(sdiodev, 0);
	sdio_dbg("%s\n", __func__);
	if (sdiodev->pwrctl_tsk) {
		complete_all(&sdiodev->pwrctrl_trgg);
		kthread_stop(sdiodev->pwrctl_tsk);
		sdiodev->pwrctl_tsk = NULL;
	}
#endif
	bus_if->state = BUS_DOWN_ST;
	ret = down_interruptible(&sdiodev->tx_priv->txctl_sema);
	if (ret)
	   sdio_err("down txctl_sema fail\n");

#if defined(CONFIG_SDIO_PWRCTRL)
	aicwf_sdio_pwr_stctl(sdiodev, SDIO_SLEEP_ST);
#endif
	if (!ret)
		up(&sdiodev->tx_priv->txctl_sema);
	aicwf_frame_queue_flush(&sdiodev->tx_priv->txq);
}

struct sk_buff *aicwf_sdio_readframes(struct aic_sdio_dev *sdiodev, u8 msg)
{
	int ret = 0;
	u32 size = 0;
	struct sk_buff *skb = NULL;
	struct aicwf_bus *bus_if = dev_get_drvdata(sdiodev->dev);

	if (bus_if->state == BUS_DOWN_ST) {
		sdio_dbg("bus down\n");
		return NULL;
	}

	size = sdiodev->rx_priv->data_len;
	skb =  __dev_alloc_skb(size, GFP_KERNEL);
	if (!skb) {
		return NULL;
	}

	ret = aicwf_sdio_recv_pkt(sdiodev, skb, size, msg);
	if (ret) {
		dev_kfree_skb(skb);
		skb = NULL;
	}

	return skb;
}

static int aicwf_sdio_tx_msg(struct aic_sdio_dev *sdiodev)
{
	int err = 0;
	u16 len;
	u8 *payload = sdiodev->tx_priv->cmd_buf;
	u16 payload_len = sdiodev->tx_priv->cmd_len;
	u8 adjust_str[4] = {0, 0, 0, 0};
	int adjust_len = 0;
	int buffer_cnt = 0;
	u8 retry = 0;
	/* RNSBox: per-message print removed */

	len = payload_len;
	if ((len % TX_ALIGNMENT) != 0) {
		adjust_len = roundup(len, TX_ALIGNMENT);
		memcpy(payload+payload_len, adjust_str, (adjust_len - len));
		payload_len += (adjust_len - len);
	}
	len = payload_len;

	//link tail is necessary
	if ((len % SDIOWIFI_FUNC_BLOCKSIZE) != 0) {
		memset(payload+payload_len, 0, TAIL_LEN);
		payload_len += TAIL_LEN;
		len = (payload_len/SDIOWIFI_FUNC_BLOCKSIZE + 1) * SDIOWIFI_FUNC_BLOCKSIZE;
	} else
		len = payload_len;

	if(sdiodev->chipid == PRODUCT_ID_AIC8801 || sdiodev->chipid == PRODUCT_ID_AIC8800D80 || 
		sdiodev->chipid == PRODUCT_ID_AIC8800D80X2){
		buffer_cnt = aicwf_sdio_flow_ctrl(sdiodev);
		while ((buffer_cnt <= 0 || (buffer_cnt > 0 && len > (buffer_cnt * BUFFER_SIZE))) && retry < 10) {
			retry++;
			buffer_cnt = aicwf_sdio_flow_ctrl(sdiodev);
			printk("buffer_cnt = %d\n", buffer_cnt);
		}
	}
	down(&sdiodev->tx_priv->cmd_txsema);

	if(sdiodev->chipid == PRODUCT_ID_AIC8801 || sdiodev->chipid == PRODUCT_ID_AIC8800D80 ||
		sdiodev->chipid == PRODUCT_ID_AIC8800D80X2){
		if (buffer_cnt > 0 && len < (buffer_cnt * BUFFER_SIZE)) {
			err = aicwf_sdio_send_pkt(sdiodev, payload, len);
			if (err)
				printk(KERN_ERR "RNSBOX send_pkt len=%u err=%d buf_cnt=%d\n",
				       len, err, buffer_cnt);
			if (err) {
				sdio_err("aicwf_sdio_send_pkt fail%d\n", err);
			}
		} else {
			sdio_err("tx msg fc retry fail:%d, %d\n", buffer_cnt, len);
			up(&sdiodev->tx_priv->cmd_txsema);
			return -1;
		}
	}else if(sdiodev->chipid == PRODUCT_ID_AIC8800DC || sdiodev->chipid == PRODUCT_ID_AIC8800DW){
		err = aicwf_sdio_send_msg(sdiodev, payload, len);
		if (err) {
			sdio_err("aicwf_sdio_send_pkt fail%d\n", err);
		}
	} else {
		sdio_err("tx msg fc retry fail:%d, %d\n", buffer_cnt, len);
		up(&sdiodev->tx_priv->cmd_txsema);
		return -1;
	}

	sdiodev->tx_priv->cmd_txstate = false;
	if (!err)
		sdiodev->tx_priv->cmd_tx_succ = true;
	else
		sdiodev->tx_priv->cmd_tx_succ = false;

	up(&sdiodev->tx_priv->cmd_txsema);

	return err;
}

static void aicwf_sdio_tx_process(struct aic_sdio_dev *sdiodev)
{
	int err = 0;
	/* RNSBox: per-message print removed */

	if (sdiodev->bus_if->state == BUS_DOWN_ST) {
		sdio_err("Bus is down\n");
		return;
	}

#if defined(CONFIG_SDIO_PWRCTRL)
	aicwf_sdio_pwr_stctl(sdiodev, SDIO_ACTIVE_ST);
#endif

	//config
	sdio_info("send cmd\n");
	if (sdiodev->tx_priv->cmd_txstate) {
		if (down_interruptible(&sdiodev->tx_priv->txctl_sema)) {
			txrx_err("txctl down bus->txctl_sema fail\n");
			return;
		}
		if (sdiodev->state != SDIO_ACTIVE_ST) {
			txrx_err("state err\n");
			up(&sdiodev->tx_priv->txctl_sema);
			txrx_err("txctl up bus->txctl_sema fail\n");
			return;
		}

		err = aicwf_sdio_tx_msg(sdiodev);
		up(&sdiodev->tx_priv->txctl_sema);
		if (waitqueue_active(&sdiodev->tx_priv->cmd_txdone_wait))
			wake_up(&sdiodev->tx_priv->cmd_txdone_wait);
	}

	//data
	sdio_info("send data\n");
	if (down_interruptible(&sdiodev->tx_priv->txctl_sema)) {
		txrx_err("txdata down bus->txctl_sema\n");
		return;
	}

	if (sdiodev->state != SDIO_ACTIVE_ST) {
		txrx_err("sdio state err\n");
		up(&sdiodev->tx_priv->txctl_sema);
		return;
	}

    if(!aicwf_is_framequeue_empty(&sdiodev->tx_priv->txq))
	    sdiodev->tx_priv->fw_avail_bufcnt = aicwf_sdio_flow_ctrl(sdiodev);
	while (!aicwf_is_framequeue_empty(&sdiodev->tx_priv->txq)) {
		aicwf_sdio_send(sdiodev->tx_priv);
		if (sdiodev->tx_priv->cmd_txstate)
			break;
	}

	up(&sdiodev->tx_priv->txctl_sema);
}

static int aicwf_sdio_bus_txdata(struct device *dev, struct sk_buff *pkt)
{
	uint prio;
	int ret = -EBADE;
	struct aicwf_bus *bus_if = dev_get_drvdata(dev);
	struct aic_sdio_dev *sdiodev = bus_if->bus_priv.sdio;

	prio = (pkt->priority & 0x7);
	spin_lock_bh(&sdiodev->tx_priv->txqlock);
	if (!aicwf_frame_enq(sdiodev->dev, &sdiodev->tx_priv->txq, pkt, prio)) {
		spin_unlock_bh(&sdiodev->tx_priv->txqlock);
		return -ENOSR;
	} else {
		ret = 0;
	}

	if (bus_if->state != BUS_UP_ST) {
		sdio_err("bus_if stopped\n");
		spin_unlock_bh(&sdiodev->tx_priv->txqlock);
		return -1;
	}

	atomic_inc(&sdiodev->tx_priv->tx_pktcnt);
	spin_unlock_bh(&sdiodev->tx_priv->txqlock);
	complete(&bus_if->bustx_trgg);

	return ret;
}

static int aicwf_sdio_bus_txmsg(struct device *dev, u8 *msg, uint msglen)
{
	int ret = -1;
	struct aicwf_bus *bus_if = dev_get_drvdata(dev);
	struct aic_sdio_dev *sdiodev = bus_if->bus_priv.sdio;

	down(&sdiodev->tx_priv->cmd_txsema);
	sdiodev->tx_priv->cmd_txstate = true;
	sdiodev->tx_priv->cmd_tx_succ = false;
	sdiodev->tx_priv->cmd_buf = msg;
	sdiodev->tx_priv->cmd_len = msglen;
	up(&sdiodev->tx_priv->cmd_txsema);

	/* RNSBox: per-message print removed */
	if (bus_if->state != BUS_UP_ST) {
		printk(KERN_ERR "RNSBOX txmsg: bus not up, dropped\n");
		sdio_err("bus has stop\n");
		return -1;
	}

	complete(&bus_if->bustx_trgg);

	if (sdiodev->tx_priv->cmd_txstate) {
		int timeout = msecs_to_jiffies(CMD_TX_TIMEOUT);
		ret = wait_event_timeout(sdiodev->tx_priv->cmd_txdone_wait, \
											!(sdiodev->tx_priv->cmd_txstate), timeout);
	}

	if (!sdiodev->tx_priv->cmd_txstate && sdiodev->tx_priv->cmd_tx_succ) {
		ret = 0;
	} else {
		sdio_err("send faild:%d, %d,%x\n", sdiodev->tx_priv->cmd_txstate, sdiodev->tx_priv->cmd_tx_succ, ret);
		ret = -EIO;
	}

	return ret;
}

int aicwf_sdio_send(struct aicwf_tx_priv *tx_priv)
{
	struct sk_buff *pkt;
	struct aic_sdio_dev *sdiodev = tx_priv->sdiodev;
	u16 aggr_len = 0;
	int retry_times = 0;
	int max_retry_times = 5;

	aggr_len = (tx_priv->tail - tx_priv->head);
	if (((atomic_read(&tx_priv->aggr_count) == 0) && (aggr_len != 0))
		|| ((atomic_read(&tx_priv->aggr_count) != 0) && (aggr_len == 0))) {
		if (aggr_len > 0)
			aicwf_sdio_aggrbuf_reset(tx_priv);
		goto done;
	}

	if (tx_priv->fw_avail_bufcnt <= 0) { //flow control failed
		tx_priv->fw_avail_bufcnt = aicwf_sdio_flow_ctrl(sdiodev);
		while (tx_priv->fw_avail_bufcnt <= 0 && retry_times < max_retry_times) {
			retry_times++;
			tx_priv->fw_avail_bufcnt = aicwf_sdio_flow_ctrl(sdiodev);
		}
		if (tx_priv->fw_avail_bufcnt <= 0) {
			sdio_err("fc retry %d fail\n", tx_priv->fw_avail_bufcnt);
			goto done;
		}
	}

	if (atomic_read(&tx_priv->aggr_count) == tx_priv->fw_avail_bufcnt) {
		if (atomic_read(&tx_priv->aggr_count) > 0) {
			tx_priv->fw_avail_bufcnt -= atomic_read(&tx_priv->aggr_count);
			aicwf_sdio_aggr_send(tx_priv); //send and check the next pkt;
		}
	} else {
		spin_lock_bh(&sdiodev->tx_priv->txqlock);
		pkt = aicwf_frame_dequeue(&sdiodev->tx_priv->txq);
		if (pkt == NULL) {
			sdio_err("txq no pkt\n");
			spin_unlock_bh(&sdiodev->tx_priv->txqlock);
			goto done;
		}
		atomic_dec(&sdiodev->tx_priv->tx_pktcnt);
		spin_unlock_bh(&sdiodev->tx_priv->txqlock);

		if (tx_priv == NULL || tx_priv->tail == NULL || pkt == NULL)
			txrx_err("null error\n");
		if (aicwf_sdio_aggr(tx_priv, pkt)) {
			aicwf_sdio_aggrbuf_reset(tx_priv);
			sdio_err("add aggr pkts failed!\n");
			goto done;
		}

		//when aggr finish or there is cmd to send, just send this aggr pkt to fw
		if ((int)atomic_read(&sdiodev->tx_priv->tx_pktcnt) == 0 || sdiodev->tx_priv->cmd_txstate) { //no more pkt send it!
			tx_priv->fw_avail_bufcnt -= atomic_read(&tx_priv->aggr_count);
			aicwf_sdio_aggr_send(tx_priv);
		} else
			goto done;
	}

done:
	return 0;
}

int aicwf_sdio_aggr(struct aicwf_tx_priv *tx_priv, struct sk_buff *pkt)
{
	//struct rwnx_txhdr *txhdr = (struct rwnx_txhdr *)pkt->data;
	u8 *start_ptr = tx_priv->tail;
	u8 sdio_header[4];
	u8 adjust_str[4] = {0, 0, 0, 0};
	u16 curr_len = 0;
	int allign_len = 0;

	//sdio_header[0] =((pkt->len - sizeof(struct rwnx_txhdr) + sizeof(struct txdesc_api)) & 0xff);
	//sdio_header[1] =(((pkt->len - sizeof(struct rwnx_txhdr) + sizeof(struct txdesc_api)) >> 8)&0x0f);
	sdio_header[2] = 0x01; //data
	if (tx_priv->sdiodev->chipid == PRODUCT_ID_AIC8801 || tx_priv->sdiodev->chipid == PRODUCT_ID_AIC8800DC ||
        tx_priv->sdiodev->chipid == PRODUCT_ID_AIC8800DW)
        sdio_header[3] = 0; //reserved
    else if (tx_priv->sdiodev->chipid == PRODUCT_ID_AIC8800D80  || tx_priv->sdiodev->chipid == PRODUCT_ID_AIC8800D80X2)
	    sdio_header[3] = crc8_ponl_107(&sdio_header[0], 3); // crc8

	memcpy(tx_priv->tail, (u8 *)&sdio_header, sizeof(sdio_header));
	tx_priv->tail += sizeof(sdio_header);
	//payload
	//memcpy(tx_priv->tail, (u8 *)(long)&txhdr->sw_hdr->desc, sizeof(struct txdesc_api));
	//tx_priv->tail += sizeof(struct txdesc_api); //hostdesc
	//memcpy(tx_priv->tail, (u8 *)((u8 *)txhdr + txhdr->sw_hdr->headroom), pkt->len-txhdr->sw_hdr->headroom);
	//tx_priv->tail += (pkt->len - txhdr->sw_hdr->headroom);

	//word alignment
	curr_len = tx_priv->tail - tx_priv->head;
	if (curr_len & (TX_ALIGNMENT - 1)) {
		allign_len = roundup(curr_len, TX_ALIGNMENT)-curr_len;
		memcpy(tx_priv->tail, adjust_str, allign_len);
		tx_priv->tail += allign_len;
	}

    if (tx_priv->sdiodev->chipid == PRODUCT_ID_AIC8801 || tx_priv->sdiodev->chipid == PRODUCT_ID_AIC8800DC ||
        tx_priv->sdiodev->chipid == PRODUCT_ID_AIC8800DW) {
    	start_ptr[0] = ((tx_priv->tail - start_ptr - 4) & 0xff);
    	start_ptr[1] = (((tx_priv->tail - start_ptr - 4)>>8) & 0x0f);
    }

	tx_priv->aggr_buf->dev = pkt->dev;

	#if 0
	if (!txhdr->sw_hdr->need_cfm) {
		kmem_cache_free(txhdr->sw_hdr->rwnx_vif->rwnx_hw->sw_txhdr_cache, txhdr->sw_hdr);
		skb_pull(pkt, txhdr->sw_hdr->headroom);
		consume_skb(pkt);
	}
	#endif

	consume_skb(pkt);
	atomic_inc(&tx_priv->aggr_count);
	return 0;
}

void aicwf_sdio_aggr_send(struct aicwf_tx_priv *tx_priv)
{
	struct sk_buff *tx_buf = tx_priv->aggr_buf;
	int ret = 0;
	int curr_len = 0;

	//link tail is necessary
	curr_len = tx_priv->tail - tx_priv->head;
	if ((curr_len % TXPKT_BLOCKSIZE) != 0) {
		memset(tx_priv->tail, 0, TAIL_LEN);
		tx_priv->tail += TAIL_LEN;
	}

	tx_buf->len = tx_priv->tail - tx_priv->head;
	ret = aicwf_sdio_txpkt(tx_priv->sdiodev, tx_buf);
	if (ret < 0) {
		sdio_err("fail to send aggr pkt!\n");
	}

	aicwf_sdio_aggrbuf_reset(tx_priv);
}

void aicwf_sdio_aggrbuf_reset(struct aicwf_tx_priv *tx_priv)
{
	struct sk_buff *aggr_buf = tx_priv->aggr_buf;

	tx_priv->tail = tx_priv->head;
	aggr_buf->len = 0;
	atomic_set(&tx_priv->aggr_count, 0);
}

static int aicwf_sdio_bus_start(struct device *dev)
{
	struct aicwf_bus *bus_if = dev_get_drvdata(dev);
	struct aic_sdio_dev *sdiodev = bus_if->bus_priv.sdio;
	int ret = 0;

	if(sdiodev->chipid == PRODUCT_ID_AIC8801){
		sdio_claim_host(sdiodev->func);
		sdio_claim_irq(sdiodev->func, aicwf_sdio_hal_irqhandler);
		//enable sdio interrupt
		ret = aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.intr_config_reg, 0x07);
		if (ret != 0)
			sdio_err("intr register failed:%d\n", ret);
		sdio_release_host(sdiodev->func);
	}else if(sdiodev->chipid == PRODUCT_ID_AIC8800DC || sdiodev->chipid == PRODUCT_ID_AIC8800DW){
		sdio_claim_host(sdiodev->func);

		//since we have func2 we don't register irq handler
		sdio_claim_irq(sdiodev->func, NULL);
		sdio_claim_irq(sdiodev->func_msg, NULL);

		sdiodev->func->irq_handler = (sdio_irq_handler_t *)aicwf_sdio_hal_irqhandler;
		sdiodev->func_msg->irq_handler = (sdio_irq_handler_t *)aicwf_sdio_hal_irqhandler_func2;
		sdio_release_host(sdiodev->func);

		//enable sdio interrupt
		ret = aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.intr_config_reg, 0x07);

		if (ret != 0)
			sdio_err("intr register failed:%d\n", ret);

		//enable sdio interrupt
		ret = aicwf_sdio_writeb_func2(sdiodev, sdiodev->sdio_reg.intr_config_reg, 0x07);

		if (ret != 0)
			sdio_err("func2 intr register failed:%d\n", ret);
	}else if(sdiodev->chipid == PRODUCT_ID_AIC8800D80 || sdiodev->chipid == PRODUCT_ID_AIC8800D80X2){
		sdio_claim_host(sdiodev->func);
		sdio_claim_irq(sdiodev->func, aicwf_sdio_hal_irqhandler);

        sdio_f0_writeb(sdiodev->func, 0x07, 0x04, &ret);
        if (ret) {
            sdio_err("set func0 int en fail %d\n", ret);
        }
        sdio_release_host(sdiodev->func);
		//enable sdio interrupt
		ret = aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.intr_config_reg, 0x07);
		if (ret != 0)
			sdio_err("intr register failed:%d\n", ret);
	}

	bus_if->state = BUS_UP_ST;
	printk(KERN_ERR "RNSBOX bus_init: state=BUS_UP_ST\n");
	return ret;
}

int aicwf_sdio_bustx_thread(void *data)
{
	struct aicwf_bus *bus = (struct aicwf_bus *) data;
	struct aic_sdio_dev *sdiodev = bus->bus_priv.sdio;

	while (1) {
		if (kthread_should_stop()) {
			sdio_err("sdio bustx thread stop\n");
			break;
		}
		if (!wait_for_completion_interruptible(&bus->bustx_trgg)) {
			/* RNSBox: per-wakeup print removed */
			if ((int)(atomic_read(&sdiodev->tx_priv->tx_pktcnt) > 0) || (sdiodev->tx_priv->cmd_txstate == true))
				aicwf_sdio_tx_process(sdiodev);
		}
	}

	return 0;
}

static atomic_t rnsbox_bsp_rxwake_cnt = ATOMIC_INIT(0);
int aicwf_sdio_busrx_thread(void *data)
{
	struct aicwf_rx_priv *rx_priv = (struct aicwf_rx_priv *)data;
	struct aicwf_bus *bus_if = rx_priv->sdiodev->bus_if;

	while (1) {
		if (kthread_should_stop()) {
			sdio_err("sdio busrx thread stop\n");
			break;
		}
		if (!wait_for_completion_interruptible(&bus_if->busrx_trgg)) {
			aicwf_process_rxframes(rx_priv);
		}
	}

	return 0;
}

#if defined(CONFIG_SDIO_PWRCTRL)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
static void aicwf_sdio_bus_pwrctl(struct timer_list *t)
{
	struct aic_sdio_dev *sdiodev = from_timer(sdiodev, t, timer);
#else
static void aicwf_sdio_bus_pwrctl(ulong data)
{
	struct aic_sdio_dev *sdiodev = (struct aic_sdio_dev *) data;
#endif
	if (sdiodev->bus_if->state == BUS_DOWN_ST) {
		sdio_err("bus down\n");
		return;
	}

	if (sdiodev->pwrctl_tsk) {
		complete(&sdiodev->pwrctrl_trgg);
	}
}
#endif

static void aicwf_sdio_enq_rxpkt(struct aic_sdio_dev *sdiodev, struct sk_buff *pkt)
{
	struct aicwf_rx_priv *rx_priv = sdiodev->rx_priv;
	unsigned long flags = 0;

	spin_lock_irqsave(&rx_priv->rxqlock, flags);
	if (!aicwf_rxframe_enqueue(sdiodev->dev, &rx_priv->rxq, pkt)) {
		spin_unlock_irqrestore(&rx_priv->rxqlock, flags);
		printk(KERN_ERR "RNSBOX enq_rxpkt: queue full, dropped\n");
		aicwf_dev_skb_free(pkt);
		return;
	}
	spin_unlock_irqrestore(&rx_priv->rxqlock, flags);

	atomic_inc(&rx_priv->rx_cnt);
}

#define SDIO_OTHER_INTERRUPT (0x1ul << 7)

atomic_t rnsbox_bsp_irq_cnt = ATOMIC_INIT(0);
void aicwf_sdio_hal_irqhandler(struct sdio_func *func)
{
	struct aicwf_bus *bus_if = dev_get_drvdata(&func->dev);
	struct aic_sdio_dev *sdiodev = bus_if->bus_priv.sdio;
	u8 intstatus = 0;
	u8 byte_len = 0;
	struct sk_buff *pkt = NULL;
	int ret;

	if ((atomic_inc_return(&rnsbox_bsp_irq_cnt) & 0x3f) == 1)
		printk(KERN_ERR "RNSBOX bsp irq #%d\n",
		       atomic_read(&rnsbox_bsp_irq_cnt));

    //AICWFDBG(LOGDEBUG,"%s bsp enter \r\n", __func__);

    if(aicbsp_sdiodev->sdio_hal_irqhandler){
        aicbsp_sdiodev->sdio_hal_irqhandler(func);
        return;
    }

	if (!bus_if || bus_if->state == BUS_DOWN_ST) {
		sdio_err("bus err\n");
		return;
	}

    if (sdiodev->chipid  == PRODUCT_ID_AIC8801 || sdiodev->chipid  == PRODUCT_ID_AIC8800DC ||
        sdiodev->chipid  == PRODUCT_ID_AIC8800DW) {
    	ret = aicwf_sdio_readb(sdiodev, sdiodev->sdio_reg.block_cnt_reg, &intstatus);

    	while(intstatus){
    	    sdiodev->rx_priv->data_len = intstatus * SDIOWIFI_FUNC_BLOCKSIZE;
    	    if (intstatus > 0) {
    	        if(intstatus < 64) {
    	            pkt = aicwf_sdio_readframes(sdiodev, 0);
    	        } else {
    	            aicwf_sdio_intr_get_len_bytemode(sdiodev, &byte_len);//byte_len must<= 128
    	            sdio_info("byte mode len=%d\r\n", byte_len);
    	            pkt = aicwf_sdio_readframes(sdiodev, 0);
    	        }
    	    } else {
    	#ifndef CONFIG_PLATFORM_ALLWINNER
    	        sdio_err("Interrupt but no data\n");
    	#endif
    	    }

    	    if (pkt)
    	        aicwf_sdio_enq_rxpkt(sdiodev, pkt);

    	    ret = aicwf_sdio_readb(sdiodev, sdiodev->sdio_reg.block_cnt_reg, &intstatus);
    	}
    }else if (sdiodev->chipid  == PRODUCT_ID_AIC8800D80 || sdiodev->chipid  == PRODUCT_ID_AIC8800D80X2) {
        do {
            ret = aicwf_sdio_readb(sdiodev, sdiodev->sdio_reg.misc_int_status_reg, &intstatus);
            if (!ret) {
                break;
            }
            sdio_err("ret=%d, intstatus=%x\r\n",ret, intstatus);
        } while (1);
        if (intstatus & SDIO_OTHER_INTERRUPT) {
            u8 int_pending;
            ret = aicwf_sdio_readb(sdiodev, sdiodev->sdio_reg.sleep_reg, &int_pending);
            if (ret < 0) {
                sdio_err("reg:%d read failed!\n", sdiodev->sdio_reg.sleep_reg);
            }
            int_pending &= ~0x01; // dev to host soft irq
            ret = aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.sleep_reg, int_pending);
            if (ret < 0) {
                sdio_err("reg:%d write failed!\n", sdiodev->sdio_reg.sleep_reg);
            }
        }

        if ((atomic_read(&rnsbox_bsp_irq_cnt) & 0x3ff) == 1)
            printk(KERN_ERR "RNSBOX D80 irqstatus=%02x\n", intstatus);
        if (intstatus > 0) {
            uint8_t intmaskf2 = intstatus | (0x1UL << 3);
            if (intmaskf2 > 120U) { // func2
                if (intmaskf2 == 127U) { // byte mode
                    //aicwf_sdio_intr_get_len_bytemode(sdiodev, &byte_len, 1);//byte_len must<= 128
                    aicwf_sdio_intr_get_len_bytemode(sdiodev, &byte_len);//byte_len must<= 128
                    sdio_info("byte mode len=%d\r\n", byte_len);
                    pkt = aicwf_sdio_readframes(sdiodev, 1);
                } else { // block mode
                    sdiodev->rx_priv->data_len = (intstatus & 0x7U) * SDIOWIFI_FUNC_BLOCKSIZE;
                    pkt = aicwf_sdio_readframes(sdiodev, 1);
                }
            } else { // func1
                if (intstatus == 120U) { // byte mode
                    //aicwf_sdio_intr_get_len_bytemode(sdiodev, &byte_len, 0);//byte_len must<= 128
                    aicwf_sdio_intr_get_len_bytemode(sdiodev, &byte_len);//byte_len must<= 128
                    sdio_info("byte mode len=%d\r\n", byte_len);
                    pkt = aicwf_sdio_readframes(sdiodev, 0);
                } else { // block mode
                    sdiodev->rx_priv->data_len = (intstatus & 0x7FU) * SDIOWIFI_FUNC_BLOCKSIZE;
                    pkt = aicwf_sdio_readframes(sdiodev, 0);
                }
    		}
    } else {
    	#ifndef CONFIG_PLATFORM_ALLWINNER
            sdio_err("Interrupt but no data\n");
    	#endif
        }

        if (pkt)
            aicwf_sdio_enq_rxpkt(sdiodev, pkt);
    }

	complete(&bus_if->busrx_trgg);
}

void aicwf_sdio_hal_irqhandler_func2(struct sdio_func *func)
{
    struct aicwf_bus *bus_if = dev_get_drvdata(&func->dev);
    struct aic_sdio_dev *sdiodev = bus_if->bus_priv.sdio;
    u8 intstatus = 0;
    u8 byte_len = 0;
    #ifdef CONFIG_PREALLOC_RX_SKB
	struct rx_buff *pkt = NULL;
	#else
	struct sk_buff *pkt = NULL;
	#endif
    int ret;

    if (!bus_if || bus_if->state == BUS_DOWN_ST) {
        sdio_err("bus err\n");
        return;
    }

	#ifdef CONFIG_PREALLOC_RX_SKB
	if (list_empty(&aic_rx_buff_list.rxbuff_list)) {
        printk("%s %d, rxbuff list is empty\n", __func__, __LINE__);
        return;
    }
	#endif

    ret = aicwf_sdio_readb_func2(sdiodev, sdiodev->sdio_reg.block_cnt_reg, &intstatus);

	while(intstatus) {
	    sdiodev->rx_priv->data_len = intstatus * SDIOWIFI_FUNC_BLOCKSIZE;
	    if (intstatus > 0) {
	        if(intstatus < 64) {
	            pkt = aicwf_sdio_readframes(sdiodev,1);
	        } else {
	            sdio_info("byte mode len=%d\r\n", byte_len);

	            aicwf_sdio_intr_get_len_bytemode(sdiodev, &byte_len);//byte_len must<= 128
	            pkt = aicwf_sdio_readframes(sdiodev,1);
	        }
	    } else {
		#ifndef CONFIG_PLATFORM_ALLWINNER
	        sdio_err("Interrupt but no data\n");
		#endif
	    }

	    if (pkt){
	        aicwf_sdio_enq_rxpkt(sdiodev, pkt);
	    }
		ret = aicwf_sdio_readb_func2(sdiodev, sdiodev->sdio_reg.block_cnt_reg, &intstatus);
	}

	complete(&bus_if->busrx_trgg);
}

#if defined(CONFIG_SDIO_PWRCTRL)
void aicwf_sdio_pwrctl_timer(struct aic_sdio_dev *sdiodev, uint duration)
{
	uint timeout;

	if (sdiodev->bus_if->state == BUS_DOWN_ST && duration)
		return;

	spin_lock_bh(&sdiodev->pwrctl_lock);
	if (!duration) {
		if (timer_pending(&sdiodev->timer))
			del_timer_sync(&sdiodev->timer);
	} else {
		sdiodev->active_duration = duration;
		timeout = msecs_to_jiffies(sdiodev->active_duration);
		mod_timer(&sdiodev->timer, jiffies + timeout);
	}
	spin_unlock_bh(&sdiodev->pwrctl_lock);
}
#endif

static struct aicwf_bus_ops aicwf_sdio_bus_ops = {
	.stop = aicwf_sdio_bus_stop,
	.start = aicwf_sdio_bus_start,
	.txdata = aicwf_sdio_bus_txdata,
	.txmsg = aicwf_sdio_bus_txmsg,
};

void aicwf_sdio_release_func2(struct aic_sdio_dev *sdiodev)
{
	int ret = 0;
	sdio_dbg("%s\n", __func__);
	sdio_claim_host(sdiodev->func_msg);
	//disable sdio interrupt
	ret = aicwf_sdio_writeb_func2(sdiodev, sdiodev->sdio_reg.intr_config_reg, 0x0);
	if (ret < 0) {
		sdio_err("reg:%d write failed!\n", sdiodev->sdio_reg.intr_config_reg);
	}
	sdio_release_irq(sdiodev->func_msg);
	sdio_release_host(sdiodev->func_msg);

}

void aicwf_sdio_release(struct aic_sdio_dev *sdiodev)
{
	struct aicwf_bus *bus_if = NULL;
	struct aicwf_bus *bus_if_t = NULL;
	int ret = 0;

	sdio_dbg("%s\n", __func__);
	if (sdiodev->func == NULL) {
		printk("%s, NULL sdio func\n", __func__);
		return;
	}

	bus_if = aicbsp_get_drvdata(sdiodev->dev);
	if (bus_if)
		bus_if->state = BUS_DOWN_ST;

	bus_if_t = dev_get_drvdata(sdiodev->dev);

	if ((bus_if_t != NULL) && (sdiodev->bus_if == bus_if_t)) {
		sdio_dbg("%s bsp release\n", __func__);
		sdio_claim_host(sdiodev->func);
		//disable sdio interrupt
		ret = aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.intr_config_reg, 0x0);
		if (ret < 0) {
			sdio_err("reg:%d write failed!, ret=%d\n", sdiodev->sdio_reg.intr_config_reg, ret);
		}
		sdio_release_irq(sdiodev->func);
		sdio_release_host(sdiodev->func);
	}

	if(sdiodev->chipid == PRODUCT_ID_AIC8800DC || sdiodev->chipid == PRODUCT_ID_AIC8800DW){
		aicwf_sdio_release_func2(sdiodev);
	}

	if (sdiodev->dev)
		aicwf_bus_deinit(sdiodev->dev);

	if (sdiodev->tx_priv)
		aicwf_tx_deinit(sdiodev->tx_priv);

	if (sdiodev->rx_priv)
		aicwf_rx_deinit(sdiodev->rx_priv);

	rwnx_cmd_mgr_deinit(&sdiodev->cmd_mgr);
}



void aicwf_sdio_reg_init(struct aic_sdio_dev *sdiodev)
{
    sdio_dbg("%s\n", __func__);

    if(sdiodev->chipid == PRODUCT_ID_AIC8801 || sdiodev->chipid == PRODUCT_ID_AIC8800DC ||
       sdiodev->chipid == PRODUCT_ID_AIC8800DW){
        sdiodev->sdio_reg.bytemode_len_reg =       SDIOWIFI_BYTEMODE_LEN_REG;
        sdiodev->sdio_reg.intr_config_reg =        SDIOWIFI_INTR_CONFIG_REG;
        sdiodev->sdio_reg.sleep_reg =              SDIOWIFI_SLEEP_REG;
        sdiodev->sdio_reg.wakeup_reg =             SDIOWIFI_WAKEUP_REG;
        sdiodev->sdio_reg.flow_ctrl_reg =          SDIOWIFI_FLOW_CTRL_REG;
        sdiodev->sdio_reg.register_block =         SDIOWIFI_REGISTER_BLOCK;
        sdiodev->sdio_reg.bytemode_enable_reg =    SDIOWIFI_BYTEMODE_ENABLE_REG;
        sdiodev->sdio_reg.block_cnt_reg =          SDIOWIFI_BLOCK_CNT_REG;
        sdiodev->sdio_reg.rd_fifo_addr =           SDIOWIFI_RD_FIFO_ADDR;
        sdiodev->sdio_reg.wr_fifo_addr =           SDIOWIFI_WR_FIFO_ADDR;
	} else if (sdiodev->chipid == PRODUCT_ID_AIC8800D80 || sdiodev->chipid == PRODUCT_ID_AIC8800D80X2){
        sdiodev->sdio_reg.bytemode_len_reg =       SDIOWIFI_BYTEMODE_LEN_REG_V3;
        sdiodev->sdio_reg.intr_config_reg =        SDIOWIFI_INTR_ENABLE_REG_V3;
        sdiodev->sdio_reg.sleep_reg =              SDIOWIFI_INTR_PENDING_REG_V3;
        sdiodev->sdio_reg.wakeup_reg =             SDIOWIFI_INTR_TO_DEVICE_REG_V3;
        sdiodev->sdio_reg.flow_ctrl_reg =          SDIOWIFI_FLOW_CTRL_Q1_REG_V3;
        sdiodev->sdio_reg.bytemode_enable_reg =    SDIOWIFI_BYTEMODE_ENABLE_REG_V3;
        sdiodev->sdio_reg.misc_int_status_reg =    SDIOWIFI_MISC_INT_STATUS_REG_V3;
        sdiodev->sdio_reg.rd_fifo_addr =           SDIOWIFI_RD_FIFO_ADDR_V3;
        sdiodev->sdio_reg.wr_fifo_addr =           SDIOWIFI_WR_FIFO_ADDR_V3;
    }
}

int aicwf_sdio_func_init(struct aic_sdio_dev *sdiodev)
{
	struct mmc_host *host;
	u8 block_bit0 = 0x1;
	u8 byte_mode_disable = 0x1;//1: no byte mode
	int ret = 0;
	struct aicbsp_feature_t feature;

	aicbsp_get_feature(&feature, NULL);
    aicwf_sdio_reg_init(sdiodev);

	host = sdiodev->func->card->host;

	sdio_claim_host(sdiodev->func);
#if 0//SDIO PHASE SETTING
	sdiodev->func->card->quirks |= MMC_QUIRK_LENIENT_FN0;
	sdio_f0_writeb(sdiodev->func, feature.sdio_phase, 0x13, &ret);
	if (ret < 0) {
		sdio_err("write func0 fail %d\n", ret);
        sdio_release_host(sdiodev->func);
		return ret;
	}
#endif

	ret = sdio_set_block_size(sdiodev->func, SDIOWIFI_FUNC_BLOCKSIZE);
	if (ret < 0) {
		sdio_err("set blocksize fail %d\n", ret);
		sdio_release_host(sdiodev->func);
		return ret;
	}
	ret = sdio_enable_func(sdiodev->func);
	if (ret < 0) {
        sdio_err("enable func fail %d.\n", ret);
		sdio_release_host(sdiodev->func);
		return ret;
	}
    udelay(100);
#if 1//SDIO CLOCK SETTING
	if (feature.sdio_clock > 0) {
		{ u32 clk = feature.sdio_clock;
		  /* RNSBox: the mainline sd clock base is ~750 MHz, so the
		   * firmware's 150 MHz request lands at ~125 MHz; sdio reads
		   * CRC-fail there. Stay at the SDIO high-speed limit. */
		  if (clk > 50000000)
			clk = 50000000;
		  host->ios.clock = clk;
		  printk(KERN_ERR "RNSBOX sdio clock request=%u f_max=%u\n",
			 clk, host->f_max); }
		host->ops->set_ios(host, &host->ios);
		sdio_dbg("Set SDIO Clock %d MHz\n", host->ios.clock/1000000);
	}
#endif
	sdio_release_host(sdiodev->func);

	if(sdiodev->chipid == PRODUCT_ID_AIC8800DC || sdiodev->chipid == PRODUCT_ID_AIC8800DW){

	    sdio_claim_host(sdiodev->func_msg);

		//set sdio blocksize
	    ret = sdio_set_block_size(sdiodev->func_msg, SDIOWIFI_FUNC_BLOCKSIZE);
	    if (ret < 0) {
	        AICWFDBG(LOGERROR, "set func2 blocksize fail %d\n", ret);
	        sdio_release_host(sdiodev->func_msg);
	        return ret;
	    }

		//set sdio enable func
	    ret = sdio_enable_func(sdiodev->func_msg);
	    if (ret < 0) {
	        AICWFDBG(LOGERROR, "enable func2 fail %d.\n", ret);
	    }

	    sdio_release_host(sdiodev->func_msg);

	    ret = aicwf_sdio_writeb_func2(sdiodev, sdiodev->sdio_reg.register_block, block_bit0);
	    if (ret < 0) {
	        AICWFDBG(LOGERROR, "reg:%d write failed!\n", sdiodev->sdio_reg.register_block);
	        return ret;
	    }

	    //1: no byte mode
	    ret = aicwf_sdio_writeb_func2(sdiodev, sdiodev->sdio_reg.bytemode_enable_reg, byte_mode_disable);
	    if (ret < 0) {
	        AICWFDBG(LOGERROR, "reg:%d write failed!\n", sdiodev->sdio_reg.bytemode_enable_reg);
	        return ret;
	    }
	}

	ret = aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.register_block, block_bit0);
	if (ret < 0) {
		sdio_err("reg:%d write failed!\n", sdiodev->sdio_reg.register_block);
		return ret;
	}

	//1: no byte mode
	ret = aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.bytemode_enable_reg, byte_mode_disable);
	if (ret < 0) {
		sdio_err("reg:%d write failed!\n", sdiodev->sdio_reg.bytemode_enable_reg);
		return ret;
	}

	return ret;
}

int aicwf_sdiov3_func_init(struct aic_sdio_dev *sdiodev)
{
	struct mmc_host *host;
	u8 byte_mode_disable = 0x1;//1: no byte mode
	int ret = 0;
    //u8 val;
	struct aicbsp_feature_t feature;

	aicbsp_get_feature(&feature, NULL);
    aicwf_sdio_reg_init(sdiodev);

	host = sdiodev->func->card->host;

	sdio_claim_host(sdiodev->func);
	sdiodev->func->card->quirks |= MMC_QUIRK_LENIENT_FN0;

	ret = sdio_set_block_size(sdiodev->func, SDIOWIFI_FUNC_BLOCKSIZE);
	if (ret < 0) {
		sdio_err("set blocksize fail %d\n", ret);
		sdio_release_host(sdiodev->func);
		return ret;
	}
	ret = sdio_enable_func(sdiodev->func);
	if (ret < 0) {
        sdio_err("enable func fail %d.\n", ret);
		sdio_release_host(sdiodev->func);
		return ret;
	}

    sdio_f0_writeb(sdiodev->func, 0x7F, 0xF2, &ret);
    if (ret) {
        sdio_err("set fn0 0xF2 fail %d\n", ret);
        sdio_release_host(sdiodev->func);
        return ret;
    }

	/* RNSBox diagnostics: report what the mmc core negotiated before the
	 * first data transfer, and let insmod override clock / bus width. */
	{
		u8 cccr07;
		int e = 0;

		cccr07 = sdio_f0_readb(sdiodev->func, 0x07, &e);
		dev_info(&sdiodev->func->dev,
			 "RNSBOX sdio state: cccr07=%02x err=%d bus_width=%u clock=%u f_max=%u timing=%u max_blk=%u cur_blk=%u multi=%u hs=%u wide=%u\n",
			 cccr07, e, host->ios.bus_width, host->ios.clock,
			 host->f_max, host->ios.timing,
			 sdiodev->func->max_blksize,
			 sdiodev->func->cur_blksize,
			 sdiodev->func->card->cccr.multi_block,
			 sdiodev->func->card->cccr.high_speed,
			 sdiodev->func->card->cccr.wide_bus);
	}
	if (sdio_clk_mhz) {
		host->ios.clock = sdio_clk_mhz * 1000000;
		host->ops->set_ios(host, &host->ios);
		dev_info(&sdiodev->func->dev,
			 "RNSBOX forced sdio clock=%u actual=%u\n",
			 host->ios.clock, host->actual_clock);
	}
	if (sdio_1bit) {
		int e = 0;

		sdio_f0_writeb(sdiodev->func, 0x00, 0x07, &e);
		host->ios.bus_width = MMC_BUS_WIDTH_1;
		host->ops->set_ios(host, &host->ios);
		dev_info(&sdiodev->func->dev,
			 "RNSBOX forced 1-bit (cccr write err=%d)\n", e);
	}
#if 0
    if (host->ios.timing == MMC_TIMING_UHS_DDR50) {
        val = 0x21;//0x1D;//0x5;
    } else {
        val = 0x01;//0x19;//0x1;
    }
    val |= SDIOCLK_FREE_RUNNING_BIT;
    sdio_f0_writeb(sdiodev->func, val, 0xF0, &ret);
    if (ret) {
        sdio_err("set iopad ctrl fail %d\n", ret);
        sdio_release_host(sdiodev->func);
        return ret;
    }
    sdio_f0_writeb(sdiodev->func, 0x0, 0xF8, &ret);
    if (ret) {
        sdio_err("set iopad delay2 fail %d\n", ret);
        sdio_release_host(sdiodev->func);
        return ret;
    }
    sdio_f0_writeb(sdiodev->func, 0x40, 0xF1, &ret);
    if (ret) {
        sdio_err("set iopad delay1 fail %d\n", ret);
        sdio_release_host(sdiodev->func);
        return ret;
    }
    msleep(1);
#if 1//SDIO CLOCK SETTING
	if ((feature.sdio_clock > 0) && (host->ios.timing != MMC_TIMING_UHS_DDR50)) {
		{ u32 clk = feature.sdio_clock;
		  /* RNSBox bring-up: 25 MHz until the pad drive strength is
		   * sorted (50 MHz gives -EILSEQ on chip->host data reads). */
		  if (clk > 25000000)
			clk = 25000000;
		  host->ios.clock = clk; }
		host->ops->set_ios(host, &host->ios);
		sdio_dbg("Set SDIO Clock %d MHz\n", host->ios.clock/1000000);
	}
#endif
#endif
	sdio_release_host(sdiodev->func);

	//1: no byte mode
	ret = aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.bytemode_enable_reg, byte_mode_disable);
	if (ret < 0) {
		sdio_err("reg:%d write failed!\n", sdiodev->sdio_reg.bytemode_enable_reg);
		return ret;
	}

	return ret;
}

void aicwf_sdio_func_deinit(struct aic_sdio_dev *sdiodev)
{
	struct aicwf_bus *bus_if = NULL;

	if (sdiodev->func == NULL) {
		sdio_err("%s, NULL sdio func\n", __func__);
		return;
	}

	bus_if = dev_get_drvdata(sdiodev->dev);
	if (bus_if == NULL) {
		sdio_err("%s, bus_if is null\n", __func__);
		return;
	}

	if (sdiodev->bus_if == bus_if) {
		sdio_dbg("%s bsp disable\n", __func__);
		sdio_claim_host(sdiodev->func);
		sdio_disable_func(sdiodev->func);
		sdio_release_host(sdiodev->func);
	}

	if(sdiodev->chipid == PRODUCT_ID_AIC8800DC || sdiodev->chipid == PRODUCT_ID_AIC8800DW){
		sdio_claim_host(sdiodev->func_msg);
		sdio_disable_func(sdiodev->func_msg);
		sdio_release_host(sdiodev->func_msg);
	}

}


void *aicwf_sdio_bus_init(struct aic_sdio_dev *sdiodev)
{
	int ret;
	struct aicwf_bus *bus_if;
	struct aicwf_rx_priv *rx_priv;
	struct aicwf_tx_priv *tx_priv;

#if defined(CONFIG_SDIO_PWRCTRL)
	spin_lock_init(&sdiodev->pwrctl_lock);
	sema_init(&sdiodev->pwrctl_wakeup_sema, 1);
#endif

	bus_if = sdiodev->bus_if;
	bus_if->dev = sdiodev->dev;
	bus_if->ops = &aicwf_sdio_bus_ops;
	bus_if->state = BUS_DOWN_ST;
#if defined(CONFIG_SDIO_PWRCTRL)
	sdiodev->state = SDIO_SLEEP_ST;
	sdiodev->active_duration = SDIOWIFI_PWR_CTRL_INTERVAL;
#else
	sdiodev->state = SDIO_ACTIVE_ST;
#endif

	rx_priv = aicwf_rx_init(sdiodev);
	if (!rx_priv) {
		sdio_err("rx init fail\n");
		goto fail;
	}
	sdiodev->rx_priv = rx_priv;

	tx_priv = aicwf_tx_init(sdiodev);
	if (!tx_priv) {
		sdio_err("tx init fail\n");
		goto fail;
	}
	sdiodev->tx_priv = tx_priv;
	aicwf_frame_queue_init(&tx_priv->txq, 8, TXQLEN);
	spin_lock_init(&tx_priv->txqlock);
	sema_init(&tx_priv->txctl_sema, 1);
	sema_init(&tx_priv->cmd_txsema, 1);
	init_waitqueue_head(&tx_priv->cmd_txdone_wait);
	atomic_set(&tx_priv->tx_pktcnt, 0);

#if defined(CONFIG_SDIO_PWRCTRL)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	timer_setup(&sdiodev->timer, aicwf_sdio_bus_pwrctl, 0);
#else
	init_timer(&sdiodev->timer);
	sdiodev->timer.data = (ulong) sdiodev;
	sdiodev->timer.function = aicwf_sdio_bus_pwrctl;
#endif
	init_completion(&sdiodev->pwrctrl_trgg);
#endif
	ret = aicwf_bus_init(0, sdiodev->dev);
	if (ret < 0) {
		sdio_err("bus init fail\n");
		goto fail;
	}

	ret  = aicwf_bus_start(bus_if);
	if (ret != 0) {
		sdio_err("bus start fail\n");
		goto fail;
	}

	return sdiodev;

fail:
	aicwf_sdio_release(sdiodev);
	return NULL;
}

void get_fw_path(char* fw_path){
	if (strlen(aic_fw_path) > 0) {
		memcpy(fw_path, aic_fw_path, strlen(aic_fw_path));
	}else{
		memcpy(fw_path, aic_default_fw_path, strlen(aic_default_fw_path));
	}
}

int get_testmode(void){
	return testmode;
}

struct sdio_func *get_sdio_func(void){
    return aicbsp_sdiodev->func;
}

void set_irq_handler(void *fn){
    aicbsp_sdiodev->sdio_hal_irqhandler = (sdio_irq_handler_t *)fn;
}

extern int adap_test;
int get_adap_test(void){
    return adap_test;
}

uint8_t crc8_ponl_107(uint8_t *p_buffer, uint16_t cal_size)
{
    uint8_t i;
    uint8_t crc = 0;
    if (cal_size==0) {
        return crc;
    }
    while (cal_size--) {
        for (i = 0x80; i > 0; i /= 2) {
            if (crc & 0x80)  {
                crc *= 2;
                crc ^= 0x07; //polynomial X8 + X2 + X + 1,(0x107)
            } else {
                crc *= 2;
            }
            if ((*p_buffer) & i) {
                crc ^= 0x07;
            }
        }
        p_buffer++;
    }
    return crc;
}

EXPORT_SYMBOL(get_fw_path);
EXPORT_SYMBOL(get_testmode);
EXPORT_SYMBOL(get_sdio_func);
EXPORT_SYMBOL(set_irq_handler);
EXPORT_SYMBOL(get_adap_test);

