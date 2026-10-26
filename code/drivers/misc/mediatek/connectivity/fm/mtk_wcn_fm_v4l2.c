// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek WCN FM radio - V4L2 radio device, power management, rfkill.
 *
 * Port from Samsung/MTK downstream drivers/misc/mediatek/fmradio/core/
 * fm_module.c (char device, ioctl semantics, antenna switch GPIO) and
 * fm_main.c (power up/down and control flows).  The downstream character
 * device is replaced by a V4L2 radio device exporting the same hardware
 * operations.
 *
 * Copyright (C) 2015 MediaTek Inc.
 */

#include <linux/gpio.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/rfkill.h>
#include <linux/slab.h>

#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>

#include "mtk_wcn_fm.h"

#define MTK_WCN_FM_RDS_FIFO_SIZE \
	(sizeof(struct mtk_wcn_fm_rds_data) * MTK_WCN_FM_RDS_FIFO_RECORDS)

/* Downstream FM_UE_FREQ_MIN / FM_UE_FREQ_MAX etc., in 10 kHz units. */
static void mtk_wcn_fm_band_range(u8 band, u16 *min, u16 *max)
{
	switch (band) {
	case FM_BAND_JAPAN:
		*min = FM_JP_FREQ_MIN;
		*max = FM_JP_FREQ_MAX;
		break;
	case FM_BAND_JAPANW:
		*min = FM_JP_FREQ_MIN;
		*max = FM_UE_FREQ_MAX;
		break;
	case FM_BAND_SPECIAL:
		*min = FM_SPECIAL_FREQ_MIN;
		*max = FM_SPECIAL_FREQ_MAX;
		break;
	case FM_BAND_UE:
	default:
		*min = FM_UE_FREQ_MIN;
		*max = FM_UE_FREQ_MAX;
		break;
	}
}

/*
 * Power management
 */
/*
 * Bring the FM function up: WMT function on, MT6627 power up sequence and
 * the initial tune.  Downstream does the same on FM_IOCTL_POWERUP, which
 * calls fm_powerup() and then fm_tune() (core/fm_module.c:131-158).
 */
static int mtk_wcn_fm_power_on(struct mtk_wcn_fm_dev *fm)
{
	u16 min, max;
	int ret;

	if (fm->powered)
		return 0;

	/* Drop RDS records left over from a previous session
	 * (mt6627_RDS_Init_Data(), mt6627_fm_rds.c:231-240).
	 */
	kfifo_reset(&fm->rds_fifo);

	ret = mtk_wcn_fm_chip_power_up(fm);
	if (ret)
		return ret;

	mtk_wcn_fm_band_range(fm->band, &min, &max);
	if (fm->cur_freq < min || fm->cur_freq > max)
		fm->cur_freq = min;

	ret = mtk_wcn_fm_chip_tune(fm, fm->cur_freq);
	if (ret) {
		mtk_wcn_fm_chip_power_down(fm);
		return ret;
	}

	/* FM antenna switch: downstream drives it high while powered
	 * (core/fm_module.c:151-152).
	 */
	if (fm->ant_sel_valid)
		gpio_set_value_cansleep(fm->ant_sel_gpio, 1);

	/*
	 * The chip power up sequence leaves the I2S port enabled; let the ASoC
	 * glue close it again unless the route and a PCM ask for it.
	 */
	mtk_wcn_fm_i2s_power(fm, true);

	return 0;
}

/* fm_powerdown(), core/fm_main.c:612-636. */
static void mtk_wcn_fm_power_off(struct mtk_wcn_fm_dev *fm)
{
	if (!fm->powered)
		return;

	/* pwrdown_flow() disables RDS before the chip power down sequence
	 * (core/fm_main.c:592-601).
	 */
	if (fm->rds_on)
		mtk_wcn_fm_chip_rds(fm, false);

	/* core/fm_module.c:164-165 */
	if (fm->ant_sel_valid)
		gpio_set_value_cansleep(fm->ant_sel_gpio, 0);

	/* Disable the I2S port while its registers are still reachable. */
	mtk_wcn_fm_i2s_power(fm, false);

	mtk_wcn_fm_chip_power_down(fm);

	/* Wake up readers blocked in read()/poll(). */
	wake_up_interruptible_all(&fm->rds_wq);
}

/*
 * Restore the FM function after a connectivity SoC reset
 * (fm_subsys_reset_work_func(), core/fm_main.c:2188-2265).  The WMT reset
 * notification is only generated for whole chip resets, where the FM function
 * has already been switched off by WMT, so the chip is brought up again from
 * scratch and the user visible state is re-applied from the control values.
 */
static void mtk_wcn_fm_reset_work(struct work_struct *work)
{
	struct mtk_wcn_fm_dev *fm = container_of(work, struct mtk_wcn_fm_dev,
						 rst_work);
	bool restored = false;

	mutex_lock(&fm->lock);
	if (fm->powered) {
		dev_warn(fm->dev, "connectivity reset, restoring FM state\n");
		fm->powered = false;
		mtk_wcn_fm_i2s_power(fm, false);
		if (!mtk_wcn_fm_chip_power_up(fm) &&
		    !mtk_wcn_fm_chip_tune(fm, fm->cur_freq)) {
			restored = true;
			mtk_wcn_fm_i2s_power(fm, true);
		} else {
			dev_err(fm->dev, "FM restore after reset failed\n");
		}
	}
	mutex_unlock(&fm->lock);

	/* Re-apply volume, mute, RDS, de-emphasis, antenna and DSP settings. */
	if (restored)
		v4l2_ctrl_handler_setup(&fm->ctrl_handler);
}

/*
 * rfkill
 */
static int mtk_wcn_fm_rfkill_set_block(void *data, bool blocked)
{
	struct mtk_wcn_fm_dev *fm = data;
	bool apply = false;

	mutex_lock(&fm->lock);
	if (blocked) {
		if (fm->opened)
			mtk_wcn_fm_power_off(fm);
	} else if (fm->opened && !fm->powered) {
		if (!mtk_wcn_fm_power_on(fm))
			apply = true;
	}
	mutex_unlock(&fm->lock);

	if (apply)
		v4l2_ctrl_handler_setup(&fm->ctrl_handler);

	return 0;
}

static const struct rfkill_ops mtk_wcn_fm_rfkill_ops = {
	.set_block = mtk_wcn_fm_rfkill_set_block,
};

/*
 * V4L2 controls
 */
static int mtk_wcn_fm_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mtk_wcn_fm_dev *fm = container_of(ctrl->handler,
						 struct mtk_wcn_fm_dev,
						 ctrl_handler);
	u16 min, max;
	int ret = 0;

	/*
	 * Values programmed while the chip is off are stored by the control
	 * framework and written by v4l2_ctrl_handler_setup() after the next
	 * power up.
	 */
	if (!fm->powered)
		return 0;

	mutex_lock(&fm->lock);
	switch (ctrl->id) {
	case V4L2_CID_AUDIO_MUTE:
		ret = mtk_wcn_fm_chip_mute(fm, ctrl->val);
		break;
	case V4L2_CID_AUDIO_VOLUME:
		ret = mtk_wcn_fm_chip_vol_set(fm, ctrl->val);
		break;
	case V4L2_CID_TUNE_DEEMPHASIS:
		ret = mtk_wcn_fm_chip_deemphasis(fm,
						 ctrl->val == V4L2_DEEMPHASIS_75_uS);
		break;
	case V4L2_CID_RDS_RECEPTION:
		ret = mtk_wcn_fm_chip_rds(fm, ctrl->val);
		break;
	case V4L2_CID_MTK_WCN_FM_BAND:
		if (ctrl->val == fm->band)
			break;
		fm->band = ctrl->val;
		mtk_wcn_fm_band_range(fm->band, &min, &max);
		if (fm->cur_freq < min || fm->cur_freq > max) {
			fm->cur_freq = min;
			ret = mtk_wcn_fm_chip_tune(fm, fm->cur_freq);
			/* The retune re-enabled the chip I2S port. */
			if (!ret)
				mtk_wcn_fm_i2s_resync(fm);
		}
		break;
	case V4L2_CID_MTK_WCN_FM_ANTENNA:
		ret = mtk_wcn_fm_chip_antenna(fm, ctrl->val);
		break;
	case V4L2_CID_MTK_WCN_FM_SOFTMUTE_GAIN:
		ret = mtk_wcn_fm_chip_smg(fm, ctrl->val);
		break;
	case V4L2_CID_MTK_WCN_FM_BLEND:
		ret = mtk_wcn_fm_chip_blend(fm, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&fm->lock);

	return ret;
}

static const struct v4l2_ctrl_ops mtk_wcn_fm_ctrl_ops = {
	.s_ctrl = mtk_wcn_fm_s_ctrl,
};

/*
 * V4L2 ioctls
 */
static int mtk_wcn_fm_querycap(struct file *file, void *priv,
			       struct v4l2_capability *cap)
{
	strscpy(cap->driver, MTK_WCN_FM_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, MTK_WCN_FM_CARD_NAME, sizeof(cap->card));
	strscpy(cap->bus_info, "platform:mt_fmradio_mt6627",
		sizeof(cap->bus_info));

	return 0;
}

static int mtk_wcn_fm_g_tuner(struct file *file, void *priv,
			      struct v4l2_tuner *tuner)
{
	struct mtk_wcn_fm_dev *fm = video_drvdata(file);
	s32 dbm = 0;
	bool stereo = false;
	u16 min, max;
	int ret;

	if (tuner->index)
		return -EINVAL;

	mtk_wcn_fm_band_range(fm->band, &min, &max);

	strscpy(tuner->name, "FM", sizeof(tuner->name));
	tuner->type = V4L2_TUNER_RADIO;
	tuner->capability = V4L2_TUNER_CAP_LOW | V4L2_TUNER_CAP_STEREO |
			    V4L2_TUNER_CAP_RDS |
			    V4L2_TUNER_CAP_HWSEEK_BOUNDED |
			    V4L2_TUNER_CAP_HWSEEK_WRAP |
			    V4L2_TUNER_CAP_HWSEEK_PROG_LIM;
	tuner->rangelow = min * 160;
	tuner->rangehigh = max * 160;
	tuner->audmode = fm->mono ? V4L2_TUNER_MODE_MONO :
				    V4L2_TUNER_MODE_STEREO;
	tuner->afc = 0;

	mutex_lock(&fm->lock);
	if (!fm->powered) {
		mutex_unlock(&fm->lock);
		return -EIO;
	}
	ret = mtk_wcn_fm_chip_status(fm, &dbm, &stereo);
	mutex_unlock(&fm->lock);
	if (ret)
		return ret;

	/* V4L2 signal strength is 0..65535; the RSSI is reported in dBm
	 * (mt6627_GetCurRSSI(), mt6627_fm_lib.c:1277-1294).
	 */
	tuner->signal = (clamp_t(s32, dbm, -128, 127) + 128) * 257;
	tuner->rxsubchans = V4L2_TUNER_SUB_MONO;
	if (stereo)
		tuner->rxsubchans |= V4L2_TUNER_SUB_STEREO;
	if (fm->rds_on)
		tuner->rxsubchans |= V4L2_TUNER_SUB_RDS;

	return 0;
}

/* fm_monostereo_set(), core/fm_main.c:1242-1258: 1 forces mono, 0 is auto. */
static int mtk_wcn_fm_s_tuner(struct file *file, void *priv,
			      const struct v4l2_tuner *tuner)
{
	struct mtk_wcn_fm_dev *fm = video_drvdata(file);
	bool mono;
	int ret;

	if (tuner->index)
		return -EINVAL;

	/* Any mode other than mono keeps the chip's automatic selection. */
	mono = tuner->audmode == V4L2_TUNER_MODE_MONO;

	mutex_lock(&fm->lock);
	if (!fm->powered) {
		mutex_unlock(&fm->lock);
		return -EIO;
	}
	ret = mtk_wcn_fm_chip_mono(fm, mono);
	if (!ret)
		fm->mono = mono;
	mutex_unlock(&fm->lock);

	return ret;
}

static int mtk_wcn_fm_g_frequency(struct file *file, void *priv,
				  struct v4l2_frequency *f)
{
	struct mtk_wcn_fm_dev *fm = video_drvdata(file);

	if (f->tuner || f->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	mutex_lock(&fm->lock);
	if (!fm->powered) {
		mutex_unlock(&fm->lock);
		return -EIO;
	}
	f->frequency = FM_KHZ10_TO_V4L2(fm->cur_freq);
	mutex_unlock(&fm->lock);

	return 0;
}

/* fm_tune(), core/fm_main.c:1609-1695. */
static int mtk_wcn_fm_s_frequency(struct file *file, void *priv,
				  const struct v4l2_frequency *f)
{
	struct mtk_wcn_fm_dev *fm = video_drvdata(file);
	u16 freq;
	u16 min, max;
	int ret;

	if (f->tuner || f->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	freq = FM_V4L2_TO_KHZ10(f->frequency);
	mtk_wcn_fm_band_range(fm->band, &min, &max);
	if (freq < min || freq > max)
		return -EINVAL;

	mutex_lock(&fm->lock);
	if (!fm->powered) {
		ret = -EIO;
	} else {
		ret = mtk_wcn_fm_chip_tune(fm, freq);
		/* Every retune re-enables the I2S port; put it back into the
		 * state the route asked for.
		 */
		if (!ret)
			mtk_wcn_fm_i2s_resync(fm);
	}
	mutex_unlock(&fm->lock);

	return ret;
}

/* Seek resolution in Hz, mapped to the chip encoding (mt6627_fm_cmd.c:691). */
static int mtk_wcn_fm_seek_space(u32 spacing, u16 *space)
{
	switch (spacing) {
	case 0:
		*space = FM_MT6627_SEEK_SPACE_DEFAULT;
		return 0;
	case 50000:
		*space = FM_SPACE_50K;
		return 0;
	case 100000:
		*space = FM_SPACE_100K;
		return 0;
	case 200000:
		*space = FM_SPACE_200K;
		return 0;
	default:
		return -EINVAL;
	}
}

static int mtk_wcn_fm_s_hw_freq_seek(struct file *file, void *priv,
				     const struct v4l2_hw_freq_seek *seek)
{
	struct mtk_wcn_fm_dev *fm = video_drvdata(file);
	u16 min, max, space, result;
	int ret;

	if (seek->tuner || seek->type != V4L2_TUNER_RADIO)
		return -EINVAL;
	if (file->f_flags & O_NONBLOCK)
		return -EAGAIN;

	mtk_wcn_fm_band_range(fm->band, &min, &max);
	if (seek->rangelow)
		min = max_t(u16, min, FM_V4L2_TO_KHZ10(seek->rangelow));
	if (seek->rangehigh)
		max = min_t(u16, max, FM_V4L2_TO_KHZ10(seek->rangehigh));
	if (min >= max)
		return -EINVAL;

	ret = mtk_wcn_fm_seek_space(seek->spacing, &space);
	if (ret)
		return ret;

	mutex_lock(&fm->lock);
	if (!fm->powered)
		ret = -EIO;
	else
		ret = mtk_wcn_fm_chip_seek(fm, min, max, space,
					   seek->seek_upward ? FM_SEEK_UP :
							       FM_SEEK_DOWN,
					   seek->wrap_around, &result);
	mutex_unlock(&fm->lock);

	return ret;
}

static int mtk_wcn_fm_subscribe_event(struct v4l2_fh *fh,
				      const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subscribe_event(fh, sub);
	default:
		return -EINVAL;
	}
}

/*
 * Downstream private FM I2S ioctls (core/fm_module.c:294-315, 735-757).
 *
 * They are handled in the file operations instead of vidioc_default because
 * the downstream command word encodes _IOC_SIZE == 4 (the headers use a
 * pointer type in _IOWR, fm_ioctl.h:70,81) while the payload is 12 bytes.
 * video_ioctl2() would copy only the four bytes of _IOC_SIZE into the kernel
 * buffer handed to vidioc_default(); the downstream character driver instead
 * copied sizeof(struct) itself, which is what is done here.
 */
static int mtk_wcn_fm_ioctl_i2s_setting(struct mtk_wcn_fm_dev *fm,
					void __user *arg)
{
	struct fm_i2s_setting cfg;
	int ret;

	if (copy_from_user(&cfg, arg, sizeof(cfg)))
		return -EFAULT;

	if (cfg.onoff != FM_I2S_ON && cfg.onoff != FM_I2S_OFF)
		return -EINVAL;
	/*
	 * The MT6627 audio port is wired as the connectivity I2S master with a
	 * fixed 32 kHz frame (mt6627_fm_config.c:116-118) and there is no
	 * downstream register sequence that changes either
	 * (mt6627_I2s_Setting() has no body, mt6627_fm_lib.c:88-90).  A request
	 * for any other mode or rate is rejected rather than accepted and
	 * ignored.
	 */
	if (cfg.mode != FM_I2S_MASTER || cfg.sample != FM_I2S_32K)
		return -EOPNOTSUPP;

	mutex_lock(&fm->lock);
	if (!fm->powered) {
		ret = -EAGAIN;
	} else {
		/*
		 * Programs the chip port and keeps the ASoC route state in
		 * step.  Without CONFIG_SND_SOC this is the chip write itself;
		 * with ASoC but no machine link using the codec, an enable is
		 * rejected with -ENODEV because no PCM could reach the port.
		 */
		ret = mtk_wcn_fm_i2s_route(fm, cfg.onoff == FM_I2S_ON);
	}
	mutex_unlock(&fm->lock);

	return ret;
}

static int mtk_wcn_fm_ioctl_get_i2s_info(struct mtk_wcn_fm_dev *fm,
					 void __user *arg)
{
	struct fm_i2s_info info;
	int ret;

	memset(&info, 0, sizeof(info));

	mutex_lock(&fm->lock);
	ret = mtk_wcn_fm_chip_i2s_info(fm, &info);
	mutex_unlock(&fm->lock);

	if (ret)
		return ret;
	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

/*
 * Private ioctl dispatch.  Everything that is not an FM I2S ioctl goes back
 * to the V4L2 core.
 */
static long mtk_wcn_fm_fops_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	struct mtk_wcn_fm_dev *fm = video_drvdata(file);

	switch (cmd) {
	case FM_IOCTL_I2S_SETTING:
		return mtk_wcn_fm_ioctl_i2s_setting(fm, (void __user *)arg);
	case FM_IOCTL_GET_I2S_INFO:
		return mtk_wcn_fm_ioctl_get_i2s_info(fm, (void __user *)arg);
	default:
		return video_ioctl2(file, cmd, arg);
	}
}

static const struct v4l2_ioctl_ops mtk_wcn_fm_ioctl_ops = {
	.vidioc_querycap	= mtk_wcn_fm_querycap,
	.vidioc_g_tuner		= mtk_wcn_fm_g_tuner,
	.vidioc_s_tuner		= mtk_wcn_fm_s_tuner,
	.vidioc_g_frequency	= mtk_wcn_fm_g_frequency,
	.vidioc_s_frequency	= mtk_wcn_fm_s_frequency,
	.vidioc_s_hw_freq_seek	= mtk_wcn_fm_s_hw_freq_seek,
	.vidioc_subscribe_event	= mtk_wcn_fm_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/*
 * File operations
 */
static int mtk_wcn_fm_fops_open(struct file *file)
{
	struct mtk_wcn_fm_dev *fm = video_drvdata(file);
	int ret;

	ret = v4l2_fh_open(file);
	if (ret)
		return ret;

	/* The downstream driver tolerates multiple openers but only powers
	 * the chip for the first one (fm->ref, core/fm_main.c:254-330).
	 */
	if (!v4l2_fh_is_singular_file(file))
		return 0;

	mutex_lock(&fm->lock);
	if (rfkill_blocked(fm->rfkill)) {
		ret = -ERFKILL;
	} else {
		ret = mtk_wcn_fm_power_on(fm);
		if (!ret)
			fm->opened = true;
	}
	mutex_unlock(&fm->lock);

	if (ret) {
		v4l2_fh_release(file);
		return ret;
	}

	/* Program volume, mute, RDS, de-emphasis, antenna and DSP settings. */
	ret = v4l2_ctrl_handler_setup(&fm->ctrl_handler);
	if (ret) {
		mutex_lock(&fm->lock);
		fm->opened = false;
		mtk_wcn_fm_power_off(fm);
		mutex_unlock(&fm->lock);
		v4l2_fh_release(file);
	}

	return ret;
}

static int mtk_wcn_fm_fops_release(struct file *file)
{
	struct mtk_wcn_fm_dev *fm = video_drvdata(file);

	if (v4l2_fh_is_singular_file(file)) {
		mutex_lock(&fm->lock);
		if (fm->opened) {
			fm->opened = false;
			mtk_wcn_fm_power_off(fm);
		}
		mutex_unlock(&fm->lock);
	}

	return v4l2_fh_release(file);
}

/*
 * RDS capture.  The firmware pushes raw RDS groups on the FM channel, which
 * the link layer stores as records of sizeof(struct mtk_wcn_fm_rds_data);
 * read() hands whole records to userspace.
 */
static ssize_t mtk_wcn_fm_fops_read(struct file *file, char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct mtk_wcn_fm_dev *fm = video_drvdata(file);
	unsigned int copied = 0;
	size_t want;
	int ret;

	if (count < sizeof(struct mtk_wcn_fm_rds_data))
		return -EINVAL;

	if (kfifo_is_empty(&fm->rds_fifo)) {
		if (file->f_flags & O_NONBLOCK)
			return -EWOULDBLOCK;

		ret = wait_event_interruptible(fm->rds_wq,
					       !kfifo_is_empty(&fm->rds_fifo) ||
					       !fm->powered);
		if (ret)
			return ret;
		if (!fm->powered)
			return -EIO;
	}

	want = min_t(size_t, count, kfifo_len(&fm->rds_fifo));
	want -= want % sizeof(struct mtk_wcn_fm_rds_data);
	if (!want)
		return -EAGAIN;

	if (kfifo_to_user(&fm->rds_fifo, buf, want, &copied))
		return -EFAULT;

	return copied;
}

static __poll_t mtk_wcn_fm_fops_poll(struct file *file,
				     struct poll_table_struct *pts)
{
	struct mtk_wcn_fm_dev *fm = video_drvdata(file);
	__poll_t req = poll_requested_events(pts);
	__poll_t ret = v4l2_ctrl_poll(file, pts);

	if (req & (EPOLLIN | EPOLLRDNORM)) {
		poll_wait(file, &fm->rds_wq, pts);
		if (!kfifo_is_empty(&fm->rds_fifo))
			ret |= EPOLLIN | EPOLLRDNORM;
		if (!fm->powered)
			ret |= EPOLLHUP;
	}

	return ret;
}

static const struct v4l2_file_operations mtk_wcn_fm_fops = {
	.owner		= THIS_MODULE,
	.open		= mtk_wcn_fm_fops_open,
	.release	= mtk_wcn_fm_fops_release,
	.read		= mtk_wcn_fm_fops_read,
	.poll		= mtk_wcn_fm_fops_poll,
	.unlocked_ioctl	= mtk_wcn_fm_fops_ioctl,
#ifdef CONFIG_COMPAT
	/* Same layout for 32-bit callers (three s32 words, no pointers). */
	.compat_ioctl32	= mtk_wcn_fm_fops_ioctl,
#endif
};

/*
 * Platform driver
 */
static void mtk_wcn_fm_kfifo_free(void *data)
{
	struct mtk_wcn_fm_dev *fm = data;

	kfifo_free(&fm->rds_fifo);
}

static int mtk_wcn_fm_probe(struct platform_device *pdev)
{
	struct mtk_wcn_fm_dev *fm;
	int ret;

	fm = devm_kzalloc(&pdev->dev, sizeof(*fm), GFP_KERNEL);
	if (!fm)
		return -ENOMEM;

	fm->dev = &pdev->dev;
	platform_set_drvdata(pdev, fm);
	mutex_init(&fm->lock);
	mutex_init(&fm->cmd_lock);
	spin_lock_init(&fm->evt_lock);
	init_waitqueue_head(&fm->evt_wq);
	init_waitqueue_head(&fm->rds_wq);
	INIT_WORK(&fm->rst_work, mtk_wcn_fm_reset_work);

	fm->band = FM_BAND_DEFAULT;
	fm->antenna = 0;
	fm->deemphasis = FM_MT6627_DEEMPHASIS_DEFAULT;
	fm->vol = 15;
	fm->smg = FM_MT6627_SMG_DEFAULT;
	fm->blend = true;
	fm->cur_freq = FM_UE_FREQ_MIN;
	fm->ant_sel_gpio = -1;

	ret = kfifo_alloc(&fm->rds_fifo, MTK_WCN_FM_RDS_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(&pdev->dev, mtk_wcn_fm_kfifo_free, fm);
	if (ret)
		return ret;

	/* Antenna switch GPIO, downstream "fmradio_ant_sel"
	 * (core/fm_module.c:1382-1393, DTS node mt_fmradio_mt6627).
	 */
	ret = of_get_named_gpio(pdev->dev.of_node, "fmradio_ant_sel", 0);
	if (ret >= 0) {
		fm->ant_sel_gpio = ret;
		ret = devm_gpio_request_one(&pdev->dev, fm->ant_sel_gpio,
					    GPIOF_OUT_INIT_LOW, "fm_ant_sel");
		if (ret) {
			dev_warn(&pdev->dev, "no fm_ant_sel control (%d)\n",
				 ret);
			fm->ant_sel_gpio = -1;
		} else {
			fm->ant_sel_valid = true;
			dev_dbg(&pdev->dev, "fm_ant_sel GPIO %d\n",
				fm->ant_sel_gpio);
		}
	} else {
		fm->ant_sel_gpio = -1;
		dev_dbg(&pdev->dev, "no fm_ant_sel GPIO\n");
	}

	ret = v4l2_device_register(&pdev->dev, &fm->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "cannot register v4l2_device (%d)\n", ret);
		return ret;
	}

	v4l2_ctrl_handler_init(&fm->ctrl_handler, 8);
	v4l2_ctrl_new_std(&fm->ctrl_handler, &mtk_wcn_fm_ctrl_ops,
			  V4L2_CID_AUDIO_MUTE, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&fm->ctrl_handler, &mtk_wcn_fm_ctrl_ops,
			  V4L2_CID_AUDIO_VOLUME, 0, 15, 1, fm->vol);
	v4l2_ctrl_new_std_menu(&fm->ctrl_handler, &mtk_wcn_fm_ctrl_ops,
			       V4L2_CID_TUNE_DEEMPHASIS,
			       V4L2_DEEMPHASIS_75_uS,
			       1 << V4L2_DEEMPHASIS_DISABLED,
			       V4L2_DEEMPHASIS_50_uS);
	v4l2_ctrl_new_std(&fm->ctrl_handler, &mtk_wcn_fm_ctrl_ops,
			  V4L2_CID_RDS_RECEPTION, 0, 1, 1, 0);
	v4l2_ctrl_new_std_menu(&fm->ctrl_handler, &mtk_wcn_fm_ctrl_ops,
			       V4L2_CID_MTK_WCN_FM_BAND, FM_BAND_SPECIAL,
			       1 << 0 /* FM_BAND_UNKNOWN is not selectable */,
			       FM_BAND_DEFAULT);
	v4l2_ctrl_new_std_menu(&fm->ctrl_handler, &mtk_wcn_fm_ctrl_ops,
			       V4L2_CID_MTK_WCN_FM_ANTENNA, 1, 0, 0);
	v4l2_ctrl_new_std(&fm->ctrl_handler, &mtk_wcn_fm_ctrl_ops,
			  V4L2_CID_MTK_WCN_FM_SOFTMUTE_GAIN, 0,
			  FM_MT6627_SMG_MAX, 1, FM_MT6627_SMG_DEFAULT);
	v4l2_ctrl_new_std(&fm->ctrl_handler, &mtk_wcn_fm_ctrl_ops,
			  V4L2_CID_MTK_WCN_FM_BLEND, 0, 1, 1, 1);
	if (fm->ctrl_handler.error) {
		ret = fm->ctrl_handler.error;
		dev_err(&pdev->dev, "cannot create controls (%d)\n", ret);
		goto err_ctrl;
	}
	fm->v4l2_dev.ctrl_handler = &fm->ctrl_handler;

	strscpy(fm->vdev.name, MTK_WCN_FM_DRIVER_NAME, sizeof(fm->vdev.name));
	fm->vdev.v4l2_dev = &fm->v4l2_dev;
	fm->vdev.fops = &mtk_wcn_fm_fops;
	fm->vdev.ioctl_ops = &mtk_wcn_fm_ioctl_ops;
	fm->vdev.release = video_device_release_empty;
	fm->vdev.device_caps = V4L2_CAP_RADIO | V4L2_CAP_TUNER |
			       V4L2_CAP_HW_FREQ_SEEK | V4L2_CAP_RDS_CAPTURE |
			       V4L2_CAP_READWRITE;
	video_set_drvdata(&fm->vdev, fm);

	/*
	 * rfkill must exist before the radio device becomes visible, because
	 * open() queries the block state.
	 */
	fm->rfkill = rfkill_alloc(MTK_WCN_FM_DRIVER_NAME, &pdev->dev,
				  RFKILL_TYPE_FM, &mtk_wcn_fm_rfkill_ops, fm);
	if (!fm->rfkill) {
		ret = -ENOMEM;
		goto err_ctrl;
	}
	ret = rfkill_register(fm->rfkill);
	if (ret) {
		rfkill_destroy(fm->rfkill);
		fm->rfkill = NULL;
		goto err_ctrl;
	}

	ret = video_register_device(&fm->vdev, VFL_TYPE_RADIO, -1);
	if (ret) {
		dev_err(&pdev->dev, "cannot register radio device (%d)\n",
			ret);
		goto err_rfkill;
	}

	/* Register the STP event callback and the WMT message callback. */
	ret = mtk_wcn_fm_link_init(fm);
	if (ret)
		goto err_video;

	/*
	 * ASoC glue: the FM codec component only.  No sound card is registered
	 * here - the board machine driver owns the single card and may link
	 * this component.  A registration failure (for example the audio
	 * component being unavailable) only removes the ALSA route; tuning and
	 * the chip level I2S controls keep working.
	 */
	ret = mtk_wcn_fm_i2s_init(fm);
	if (ret)
		dev_warn(&pdev->dev,
			 "FM I2S ALSA route unavailable (%d), continuing\n",
			 ret);

	dev_info(&pdev->dev, "MT6627 FM radio on %s\n",
		 video_device_node_name(&fm->vdev));

	return 0;

err_video:
	video_unregister_device(&fm->vdev);
err_rfkill:
	rfkill_unregister(fm->rfkill);
	rfkill_destroy(fm->rfkill);
err_ctrl:
	v4l2_ctrl_handler_free(&fm->ctrl_handler);
	v4l2_device_unregister(&fm->v4l2_dev);

	return ret;
}

static void mtk_wcn_fm_remove(struct platform_device *pdev)
{
	struct mtk_wcn_fm_dev *fm = platform_get_drvdata(pdev);

	if (!fm)
		return;

	mutex_lock(&fm->lock);
	if (fm->opened) {
		fm->opened = false;
		mtk_wcn_fm_power_off(fm);
	}
	mutex_unlock(&fm->lock);

	/* Drop the codec component before the chip and its registers go away. */
	mtk_wcn_fm_i2s_exit(fm);

	/* Stop the reset worker before the WMT/STP callbacks go away. */
	cancel_work_sync(&fm->rst_work);
	mtk_wcn_fm_link_exit(fm);

	rfkill_unregister(fm->rfkill);
	rfkill_destroy(fm->rfkill);
	video_unregister_device(&fm->vdev);
	v4l2_ctrl_handler_free(&fm->ctrl_handler);
	v4l2_device_unregister(&fm->v4l2_dev);
}

static const struct of_device_id mtk_wcn_fm_of_match[] = {
	{ .compatible = "mediatek,mt_mt6627_fmradio" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_wcn_fm_of_match);

static struct platform_driver mtk_wcn_fm_driver = {
	.probe	= mtk_wcn_fm_probe,
	.remove	= mtk_wcn_fm_remove,
	.driver	= {
		.name		= MTK_WCN_FM_DRIVER_NAME,
		.of_match_table	= mtk_wcn_fm_of_match,
	},
};
module_platform_driver(mtk_wcn_fm_driver);

MODULE_DESCRIPTION("MediaTek WCN MT6627 FM radio driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" MTK_WCN_FM_DRIVER_NAME);
