// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Implementation of primary ALSA driver code base for NVIDIA Tegra HDA.
 *
 * Copyright (c) 2019-2022, NVIDIA CORPORATION. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/clocksource.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/string.h>
#include <linux/pm_runtime.h>

#include <soc/tegra/fuse.h>
#include <sound/core.h>
#include <sound/initval.h>

#include <sound/hda_codec.h>
#include "hda_controller.h"
#include "hda_jack.h"

#if IS_ENABLED(CONFIG_TEGRA_DC)
#include <video/tegra_hdmi_audio.h>
#endif

/* Defines for Nvidia Tegra HDA support */
#define HDA_BAR0           0x8000
#define HDA_DFPCI_CFG      0x1000

#define HDA_CFG_CMD        0x1004
#define HDA_CFG_BAR0       0x1010

#define HDA_ENABLE_IO_SPACE       (1 << 0)
#define HDA_ENABLE_MEM_SPACE      (1 << 1)
#define HDA_ENABLE_BUS_MASTER     (1 << 2)
#define HDA_ENABLE_SERR           (1 << 8)
#define HDA_DISABLE_INTR          (1 << 10)
#define HDA_BAR0_INIT_PROGRAM     0xFFFFFFFF
#define HDA_BAR0_FINAL_PROGRAM    (1 << 14)

/* IPFS */
#define HDA_IPFS_CONFIG           0x180
#define HDA_IPFS_EN_FPCI          0x1

#define HDA_IPFS_FPCI_BAR0        0x80
#define HDA_FPCI_BAR0_START       0x40

#define HDA_IPFS_INTR_MASK        0x188
#define HDA_IPFS_EN_INTR          (1 << 16)

/* FPCI */
#define FPCI_DBG_CFG_2		  0x10F4
#define FPCI_GCAP_NSDO_SHIFT	  18
#define FPCI_GCAP_NSDO_MASK	  (0x3 << FPCI_GCAP_NSDO_SHIFT)

/* max number of SDs */
#define NUM_CAPTURE_SD 1
#define NUM_PLAYBACK_SD 1

/* GSC_ID register */
#define HDA_GSC_REG		0x1e0
#define HDA_GSC_ID		10

#if IS_ENABLED(CONFIG_TEGRA_DC)
#define CHAR_BUF_SIZE_MAX	50
struct hda_pcm_devices {
	struct azx_pcm *apcm;
	struct kobject *kobj;
	struct kobj_attribute pcm_attr;
	struct kobj_attribute name_attr;
	char switch_name[CHAR_BUF_SIZE_MAX];
	int dev_id;
};
#endif

/*
 * Tegra194 does not reflect correct number of SDO lines. Below macro
 * is used to update the GCAP register to workaround the issue.
 */
#define TEGRA194_NUM_SDO_LINES	  4
#define JACKPOLL_INTERVAL	msecs_to_jiffies(5000)

struct hda_tegra {
	struct azx chip;
	struct device *dev;
	struct clk_bulk_data *clocks;
	unsigned int nclocks;
	void __iomem *regs;
	void __iomem *regs_fpci;
	struct work_struct probe_work;
	struct delayed_work jack_work;
#if IS_ENABLED(CONFIG_TEGRA_DC)
	int num_codecs;
	struct kobject *kobj;
	struct hda_pcm_devices *hda_pcm_dev;
#endif
};

#ifdef CONFIG_PM
static int power_save = CONFIG_SND_HDA_POWER_SAVE_DEFAULT;
module_param(power_save, bint, 0644);
MODULE_PARM_DESC(power_save,
		 "Automatic power-saving timeout (in seconds, 0 = disable).");
#else
#define power_save	0
#endif

static const struct hda_controller_ops hda_tegra_ops; /* nothing special */

static void hda_tegra_init(struct hda_tegra *hda)
{
	u32 v;

	/* Enable PCI access */
	v = readl(hda->regs + HDA_IPFS_CONFIG);
	v |= HDA_IPFS_EN_FPCI;
	writel(v, hda->regs + HDA_IPFS_CONFIG);

	/* Enable MEM/IO space and bus master */
	v = readl(hda->regs + HDA_CFG_CMD);
	v &= ~HDA_DISABLE_INTR;
	v |= HDA_ENABLE_MEM_SPACE | HDA_ENABLE_IO_SPACE |
		HDA_ENABLE_BUS_MASTER | HDA_ENABLE_SERR;
	writel(v, hda->regs + HDA_CFG_CMD);

	writel(HDA_BAR0_INIT_PROGRAM, hda->regs + HDA_CFG_BAR0);
	writel(HDA_BAR0_FINAL_PROGRAM, hda->regs + HDA_CFG_BAR0);
	writel(HDA_FPCI_BAR0_START, hda->regs + HDA_IPFS_FPCI_BAR0);

	v = readl(hda->regs + HDA_IPFS_INTR_MASK);
	v |= HDA_IPFS_EN_INTR;
	writel(v, hda->regs + HDA_IPFS_INTR_MASK);

	/* program HDA_GSC_ID to get access to APR */
	switch (tegra_get_chip_id()) {
	case TEGRA194:
	case TEGRA234:
		writel(HDA_GSC_ID, hda->regs + HDA_GSC_REG);
		break;
	default:
		break;
	}
}

/*
 * power management
 */
static int __maybe_unused hda_tegra_suspend(struct device *dev)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct azx *chip = card->private_data;
	struct hda_tegra *hda = container_of(chip, struct hda_tegra, chip);
	int rc;

	cancel_delayed_work_sync(&hda->jack_work);
	rc = pm_runtime_force_suspend(dev);
	if (rc < 0)
		return rc;
	snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);

	return 0;
}

static int __maybe_unused hda_tegra_resume(struct device *dev)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct azx *chip = card->private_data;
	struct hda_tegra *hda = container_of(chip, struct hda_tegra, chip);
	int rc;

	rc = pm_runtime_force_resume(dev);
	if (rc < 0)
		return rc;
	snd_power_change_state(card, SNDRV_CTL_POWER_D0);

	schedule_delayed_work(&hda->jack_work, JACKPOLL_INTERVAL);

	return 0;
}

static int __maybe_unused hda_tegra_runtime_suspend(struct device *dev)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct azx *chip = card->private_data;
	struct hda_tegra *hda = container_of(chip, struct hda_tegra, chip);

	if (chip && chip->running) {
		/* enable controller wake up event */
		azx_writew(chip, WAKEEN, azx_readw(chip, WAKEEN) |
			   STATESTS_INT_MASK);

		azx_stop_chip(chip);
		azx_enter_link_reset(chip);
	}
	clk_bulk_disable_unprepare(hda->nclocks, hda->clocks);

	return 0;
}

static int __maybe_unused hda_tegra_runtime_resume(struct device *dev)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct azx *chip = card->private_data;
	struct hda_tegra *hda = container_of(chip, struct hda_tegra, chip);
	int rc;

	rc = clk_bulk_prepare_enable(hda->nclocks, hda->clocks);
	if (rc != 0)
		return rc;
	if (chip && chip->running) {
		hda_tegra_init(hda);
		azx_init_chip(chip, 1);
		/* disable controller wake up event*/
		azx_writew(chip, WAKEEN, azx_readw(chip, WAKEEN) &
			   ~STATESTS_INT_MASK);
	}

	return 0;
}

static const struct dev_pm_ops hda_tegra_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(hda_tegra_suspend, hda_tegra_resume)
	SET_RUNTIME_PM_OPS(hda_tegra_runtime_suspend,
			   hda_tegra_runtime_resume,
			   NULL)
};

static void  hda_tegra_jack_work(struct work_struct *work)
{
	struct hda_tegra *hda =
			container_of(work, struct hda_tegra, jack_work.work);
	struct azx *chip = &hda->chip;
	struct hda_codec *codec;

	if (!chip->running)
		return;

	list_for_each_codec(codec, &chip->bus) {
		if (snd_hdac_is_power_on(&codec->core))
			continue;

		snd_hda_power_up_pm(codec);
		snd_hda_jack_set_dirty_all(codec);
		snd_hda_jack_poll_all(codec);
		snd_hda_power_down_pm(codec);
	}

	schedule_delayed_work(&hda->jack_work, JACKPOLL_INTERVAL);
}

static int hda_tegra_dev_disconnect(struct snd_device *device)
{
	struct azx *chip = device->device_data;

	chip->bus.shutdown = 1;
	return 0;
}

/*
 * destructor
 */
static int hda_tegra_dev_free(struct snd_device *device)
{
	struct azx *chip = device->device_data;
	struct hda_tegra *hda = container_of(chip, struct hda_tegra, chip);

	cancel_work_sync(&hda->probe_work);
	cancel_delayed_work_sync(&hda->jack_work);
	if (azx_bus(chip)->chip_init) {
		azx_stop_all_streams(chip);
		azx_stop_chip(chip);
	}

	azx_free_stream_pages(chip);
	azx_free_streams(chip);
	snd_hdac_bus_exit(azx_bus(chip));

	return 0;
}

static int hda_tegra_init_chip(struct azx *chip, struct platform_device *pdev)
{
	struct hda_tegra *hda = container_of(chip, struct hda_tegra, chip);
	struct hdac_bus *bus = azx_bus(chip);
	struct device *dev = hda->dev;
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	hda->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(hda->regs))
		return PTR_ERR(hda->regs);

	bus->remap_addr = hda->regs + HDA_BAR0;
	bus->addr = res->start + HDA_BAR0;
	hda->regs_fpci = hda->regs + HDA_DFPCI_CFG;

	hda_tegra_init(hda);

	return 0;
}

static int hda_tegra_first_init(struct azx *chip, struct platform_device *pdev)
{
	struct hda_tegra *hda = container_of(chip, struct hda_tegra, chip);
	struct hdac_bus *bus = azx_bus(chip);
	struct snd_card *card = chip->card;
	int err;
	unsigned short gcap;
	int irq_id = platform_get_irq(pdev, 0);
	const char *sname, *drv_name = "tegra-hda";
	struct device_node *np = pdev->dev.of_node;
#ifdef CONFIG_ANDROID
	cpumask_t mask;
#endif /* #ifdef CONFIG_ANDROID */

	if (irq_id < 0)
		return irq_id;

	err = hda_tegra_init_chip(chip, pdev);
	if (err)
		return err;

	err = devm_request_irq(chip->card->dev, irq_id, azx_interrupt,
			     IRQF_SHARED, KBUILD_MODNAME, chip);
	if (err) {
		dev_err(chip->card->dev,
			"unable to request IRQ %d, disabling device\n",
			irq_id);
		return err;
	}
#ifdef CONFIG_ANDROID
	/* We want to run on all but CPU0 */
	cpumask_setall(&mask);
	cpumask_clear_cpu(0, &mask);
	irq_set_affinity_hint(irq_id, &mask);
#endif /* #ifdef CONFIG_ANDROID */
	bus->irq = irq_id;
	bus->dma_stop_delay = 100;
	card->sync_irq = bus->irq;

	/*
	 * Tegra194 has 4 SDO lines and the STRIPE can be used to
	 * indicate how many of the SDO lines the stream should be
	 * striped. But GCAP register does not reflect the true
	 * capability of HW. Below workaround helps to fix this.
	 *
	 * GCAP_NSDO is bits 19:18 in T_AZA_DBG_CFG_2,
	 * 0 for 1 SDO, 1 for 2 SDO, 2 for 4 SDO lines.
	 */
	if (of_device_is_compatible(np, "nvidia,tegra194-hda")) {
		u32 val;

		dev_info(card->dev, "Override SDO lines to %u\n",
			 TEGRA194_NUM_SDO_LINES);

		val = readl(hda->regs + FPCI_DBG_CFG_2) & ~FPCI_GCAP_NSDO_MASK;
		val |= (TEGRA194_NUM_SDO_LINES >> 1) << FPCI_GCAP_NSDO_SHIFT;
		writel(val, hda->regs + FPCI_DBG_CFG_2);
	}

	gcap = azx_readw(chip, GCAP);
	dev_dbg(card->dev, "chipset global capabilities = 0x%x\n", gcap);

	chip->align_buffer_size = 1;

	/* read number of streams from GCAP register instead of using
	 * hardcoded value
	 */
	chip->capture_streams = (gcap >> 8) & 0x0f;

	/* The GCAP register on T23x implies no Input Streams(ISS) supported,
	 * but the HW output stream descriptor programming should start with
	 * offset 0x20*4 from base stream descriptor address. This will be a
	 * problem while calculating the offset for output stream descriptor
	 * which will be considering input stream also. So here output stream
	 * starts with offset 0 which is wrong as HW register for output stream
	 * offset starts with 4.
	 */
	if (of_device_is_compatible(np, "nvidia,tegra23x-hda"))
		chip->capture_streams = 4;

	chip->playback_streams = (gcap >> 12) & 0x0f;
	if (!chip->playback_streams && !chip->capture_streams) {
		/* gcap didn't give any info, switching to old method */
		chip->playback_streams = NUM_PLAYBACK_SD;
		chip->capture_streams = NUM_CAPTURE_SD;
	}
	chip->capture_index_offset = 0;
	chip->playback_index_offset = chip->capture_streams;
	chip->num_streams = chip->playback_streams + chip->capture_streams;

	/* initialize streams */
	err = azx_init_streams(chip);
	if (err < 0) {
		dev_err(card->dev, "failed to initialize streams: %d\n", err);
		return err;
	}

	err = azx_alloc_stream_pages(chip);
	if (err < 0) {
		dev_err(card->dev, "failed to allocate stream pages: %d\n",
			err);
		return err;
	}

	/* initialize chip */
	azx_init_chip(chip, 1);

	/*
	 * Playback (for 44.1K/48K, 2-channel, 16-bps) fails with
	 * 4 SDO lines due to legacy design limitation. Following
	 * is, from HD Audio Specification (Revision 1.0a), used to
	 * control striping of the stream across multiple SDO lines
	 * for sample rates <= 48K.
	 *
	 * { ((num_channels * bits_per_sample) / number of SDOs) >= 8 }
	 *
	 * Due to legacy design issue it is recommended that above
	 * ratio must be greater than 8. Since number of SDO lines is
	 * in powers of 2, next available ratio is 16 which can be
	 * used as a limiting factor here.
	 */
	if (of_device_is_compatible(np, "nvidia,tegra30-hda"))
		chip->bus.core.sdo_limit = 16;

	/* codec detection */
	if (!bus->codec_mask) {
		dev_err(card->dev, "no codecs found!\n");
		return -ENODEV;
	}

	/* driver name */
	strncpy(card->driver, drv_name, sizeof(card->driver));
	/* shortname for card */
	sname = of_get_property(np, "nvidia,model", NULL);
	if (!sname)
		sname = drv_name;
	if (strlen(sname) > sizeof(card->shortname))
		dev_info(card->dev, "truncating shortname for card\n");
	strncpy(card->shortname, sname, sizeof(card->shortname));

	/* longname for card */
	snprintf(card->longname, sizeof(card->longname),
		 "%s at 0x%lx irq %i",
		 card->shortname, bus->addr, bus->irq);

	return 0;
}

/*
 * constructor
 */

static void hda_tegra_probe_work(struct work_struct *work);

static int hda_tegra_create(struct snd_card *card,
			    unsigned int driver_caps,
			    struct hda_tegra *hda)
{
	static const struct snd_device_ops ops = {
		.dev_disconnect = hda_tegra_dev_disconnect,
		.dev_free = hda_tegra_dev_free,
	};
	struct azx *chip;
	int err;

	chip = &hda->chip;

	mutex_init(&chip->open_mutex);
	chip->card = card;
	chip->ops = &hda_tegra_ops;
	chip->driver_caps = driver_caps;
	chip->driver_type = driver_caps & 0xff;
	chip->dev_index = 0;
	INIT_LIST_HEAD(&chip->pcm_list);

	chip->codec_probe_mask = -1;

	chip->single_cmd = false;
	chip->snoop = true;

	INIT_WORK(&hda->probe_work, hda_tegra_probe_work);
	INIT_DELAYED_WORK(&hda->jack_work, hda_tegra_jack_work);

	err = azx_bus_init(chip, NULL);
	if (err < 0)
		return err;

	chip->bus.core.sync_write = 0;
	chip->bus.core.needs_damn_long_delay = 1;
	chip->bus.core.aligned_mmio = 1;

	err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops);
	if (err < 0) {
		dev_err(card->dev, "Error creating device\n");
		return err;
	}

	return 0;
}

static const struct of_device_id hda_tegra_match[] = {
	{ .compatible = "nvidia,tegra30-hda" },
	{ .compatible = "nvidia,tegra194-hda" },
	{ .compatible = "nvidia,tegra23x-hda" },
	{},
};
MODULE_DEVICE_TABLE(of, hda_tegra_match);

static int hda_tegra_probe(struct platform_device *pdev)
{
	const unsigned int driver_flags = AZX_DCAPS_CORBRP_SELF_CLEAR |
					  AZX_DCAPS_PM_RUNTIME |
					  AZX_DCAPS_4K_BDLE_BOUNDARY;
	struct snd_card *card;
	struct azx *chip;
	struct hda_tegra *hda;
	struct device_node *np;
	struct property *prop;
	const char *name;
	int err, num, i = 0;

	hda = devm_kzalloc(&pdev->dev, sizeof(*hda), GFP_KERNEL);
	if (!hda)
		return -ENOMEM;
	hda->dev = &pdev->dev;
	chip = &hda->chip;
	np = hda->dev->of_node;

	num = of_property_count_strings(np, "clock-names");
	if (num < 0) {
		dev_err(&pdev->dev, "No hda clocks specified\n");
		return -EINVAL;
	}
	hda->nclocks = num;
	hda->clocks = devm_kzalloc(&pdev->dev,
				num * sizeof(struct clk_bulk_data *),
				GFP_KERNEL);
	if (!hda->clocks)
		return -ENOMEM;

	of_property_for_each_string(np, "clock-names", prop, name)
		hda->clocks[i++].id = name;

	err = snd_card_new(&pdev->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
			   THIS_MODULE, 0, &card);
	if (err < 0) {
		dev_err(&pdev->dev, "Error creating card!\n");
		return err;
	}

	err = devm_clk_bulk_get(&pdev->dev, hda->nclocks, hda->clocks);
	if (err < 0)
		goto out_free;

	err = hda_tegra_create(card, driver_flags, hda);
	if (err < 0)
		goto out_free;
	card->private_data = chip;

	dev_set_drvdata(&pdev->dev, card);

	pm_runtime_enable(hda->dev);
	if (!azx_has_pm_runtime(chip))
		pm_runtime_forbid(hda->dev);

	schedule_work(&hda->probe_work);

	return 0;

out_free:
	snd_card_free(card);
	return err;
}

#if IS_ENABLED(CONFIG_TEGRA_DC)
static ssize_t hda_get_pcm_device_id(struct kobject *kobj,
	struct kobj_attribute *attr,
	char *buf)
{
	struct hda_pcm_devices *pcm_dev = container_of(attr,
		struct hda_pcm_devices, pcm_attr);
	return snprintf(buf, PAGE_SIZE, "%d\n", pcm_dev->apcm->info->device);
}

static ssize_t hda_get_pcm_switch_name(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	struct hda_pcm_devices *pcm_dev = container_of(attr,
		struct hda_pcm_devices, name_attr);
	return snprintf(buf, PAGE_SIZE, "%s\n", pcm_dev->switch_name);
}

static int hda_tegra_create_sysfs(struct hda_tegra *hda)
{
	struct azx *chip = &hda->chip;
	char dirname[CHAR_BUF_SIZE_MAX] = "hda_pcm_map";
	int dev_count = 0, ret = 0;
	struct azx_pcm *apcm;
	struct kobject *parent = hda->dev->kobj.parent;

	/* maintains list of all hda codecs */
	hda->hda_pcm_dev = devm_kzalloc(hda->dev,
		sizeof(struct hda_pcm_devices) * azx_bus(chip)->num_codecs,
		GFP_KERNEL);
	if (!hda->hda_pcm_dev)
		return -ENOMEM;

	hda->kobj = kobject_create_and_add(dirname, parent);
	if (!hda->kobj)
		return -ENOMEM;

	list_for_each_entry(apcm, &chip->pcm_list, list) {
		char subdirname[CHAR_BUF_SIZE_MAX];
		struct hda_pcm_devices *pcm_dev = &hda->hda_pcm_dev[dev_count];

		pcm_dev->apcm = apcm;
		snprintf(subdirname, sizeof(subdirname), "hda%d", dev_count);
		pcm_dev->kobj = kobject_create_and_add(subdirname, hda->kobj);
		if (!pcm_dev->kobj)
			return -ENOMEM;

		/* attributes for pcm device ID */
		sysfs_attr_init(&(pcm_dev->pcm_attr.attr));
		pcm_dev->pcm_attr.attr.name = "pcm_dev_id";
		pcm_dev->pcm_attr.attr.mode = 0644;
		pcm_dev->pcm_attr.show = hda_get_pcm_device_id;

		/* attributes for switch name */
		sysfs_attr_init(&(pcm_dev->name_attr.attr));
		pcm_dev->name_attr.attr.name = "switch_name";
		pcm_dev->name_attr.attr.mode = 0644;
		pcm_dev->name_attr.show = hda_get_pcm_switch_name;

		/* gets registered switch name for give dev ID
		 * TODO: may be we can create extcon node here itself and
		 * not rely on display driver
		 */
		pcm_dev->dev_id = (apcm->codec->core.vendor_id) & 0xffff;
		if (tegra_hda_get_switch_name(pcm_dev->dev_id,
			pcm_dev->switch_name) < 0) {
			dev_dbg(hda->dev, "error in getting switch name"
				" for hda_pcm_id(%d)\n", apcm->info->device);
			kobject_put(pcm_dev->kobj);
			pcm_dev->kobj = NULL;
			continue;
		}

		/* create files for read from userspace */
		ret = sysfs_create_file(pcm_dev->kobj,
			&(pcm_dev->pcm_attr.attr));
		if (ret < 0)
			break;

		ret = sysfs_create_file(pcm_dev->kobj,
			&(pcm_dev->name_attr.attr));
		if (ret < 0)
			break;

		dev_count++;
	}
	return ret;
}

static void hda_tegra_remove_sysfs(struct device *dev)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct azx *chip = card->private_data;
	struct hda_tegra *hda = container_of(chip, struct hda_tegra, chip);
	int i;

	if (!hda || !hda->hda_pcm_dev || !hda->kobj)
		return;

	for (i = 0; i < azx_bus(chip)->num_codecs; i++) {
		struct hda_pcm_devices *pcm_dev = &hda->hda_pcm_dev[i];

		if (pcm_dev->kobj) {
			sysfs_remove_file(pcm_dev->kobj,
				&pcm_dev->pcm_attr.attr);
			sysfs_remove_file(pcm_dev->kobj,
				&pcm_dev->name_attr.attr);
			kobject_put(pcm_dev->kobj);
			pcm_dev->kobj = NULL;
		}
	}
	kobject_put(hda->kobj);
	hda->kobj = NULL;
}
#endif

static void hda_tegra_probe_work(struct work_struct *work)
{
	struct hda_tegra *hda = container_of(work, struct hda_tegra, probe_work);
	struct azx *chip = &hda->chip;
	struct platform_device *pdev = to_platform_device(hda->dev);
	int err;

	pm_runtime_get_sync(hda->dev);
	err = hda_tegra_first_init(chip, pdev);
	if (err < 0)
		goto out_free;

	/* create codec instances */
	err = azx_probe_codecs(chip, 8);
	if (err < 0)
		goto out_free;

	err = azx_codec_configure(chip);
	if (err < 0)
		goto out_free;

	err = snd_card_register(chip->card);
	if (err < 0)
		goto out_free;

	chip->running = 1;
	snd_hda_set_power_save(&chip->bus, power_save * 1000);

#if IS_ENABLED(CONFIG_TEGRA_DC)
	/* export pcm device mapping to userspace - needed for android */
	err = hda_tegra_create_sysfs(hda);
	if (err < 0) {
		dev_err(&pdev->dev,
			"error:%d in creating sysfs nodes for hda\n", err);
		/* free allocated resources */
		hda_tegra_remove_sysfs(&pdev->dev);
	}
#endif
out_free:
	pm_runtime_put(hda->dev);
	schedule_delayed_work(&hda->jack_work, JACKPOLL_INTERVAL);
	return; /* no error return from async probe */
}

static int hda_tegra_remove(struct platform_device *pdev)
{
	struct snd_card *card = dev_get_drvdata(&pdev->dev);
	struct azx *chip = card->private_data;
	struct hda_tegra *hda = container_of(chip, struct hda_tegra, chip);
	int ret;

#if IS_ENABLED(CONFIG_TEGRA_DC)
	/* remove sysfs files */
	hda_tegra_remove_sysfs(&pdev->dev);
#endif

	cancel_delayed_work_sync(&hda->jack_work);
	ret = snd_card_free(dev_get_drvdata(&pdev->dev));
	pm_runtime_disable(&pdev->dev);

	return ret;
}

static void hda_tegra_shutdown(struct platform_device *pdev)
{
	struct snd_card *card = dev_get_drvdata(&pdev->dev);
	struct azx *chip;
	struct hda_tegra *hda;

	if (!card)
		return;
	chip = card->private_data;
	hda = container_of(chip, struct hda_tegra, chip);
	cancel_delayed_work_sync(&hda->jack_work);
	if (chip && chip->running)
		azx_stop_chip(chip);
}

static struct platform_driver tegra_platform_hda = {
	.driver = {
		.name = "tegra-hda",
		.pm = &hda_tegra_pm,
		.of_match_table = hda_tegra_match,
	},
	.probe = hda_tegra_probe,
	.remove = hda_tegra_remove,
	.shutdown = hda_tegra_shutdown,
};
module_platform_driver(tegra_platform_hda);

MODULE_DESCRIPTION("Tegra HDA bus driver");
MODULE_LICENSE("GPL v2");
