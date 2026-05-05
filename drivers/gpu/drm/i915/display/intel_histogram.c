// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "intel_color_regs.h"
#include "intel_de.h"
#include "intel_display.h"
#include "intel_display_regs.h"
#include "intel_display_types.h"
#include "intel_display_utils.h"
#include "intel_dp.h"
#include "intel_histogram.h"
#include "intel_histogram_regs.h"
#include "intel_psr.h"

/* 3.0% of the pipe's current pixel count, hw does x4 */
#define HISTOGRAM_GUARDBAND_THRESHOLD_DEFAULT 300
/* Precision factor for threshold guardband */
#define HISTOGRAM_GUARDBAND_PRECISION_FACTOR 10000
#define HISTOGRAM_BIN_READ_RETRY_COUNT 5
#define IET_SAMPLE_FORMAT_1_INT_9_FRACT 0x1000009

static void intel_histogram_enable_dithering(struct intel_display *display,
					     enum pipe pipe)
{
	intel_de_rmw(display, PIPE_MISC(pipe), PIPE_MISC_DITHER_ENABLE,
		     PIPE_MISC_DITHER_ENABLE);
}

static void set_bin_index_0(struct intel_display *display, enum pipe pipe)
{
	if (DISPLAY_VER(display) >= 20)
		intel_de_rmw(display, DPST_IE_INDEX(pipe),
			     DPST_IE_BIN_INDEX_MASK, DPST_IE_BIN_INDEX(0));
	else
		intel_de_rmw(display, DPST_CTL(pipe),
			     DPST_CTL_BIN_REG_MASK,
			     DPST_CTL_BIN_REG_CLEAR);
}

static void write_iet(struct intel_display *display, enum pipe pipe,
			      u32 *data)
{
	int i;

	for (i = 0; i < HISTOGRAM_IET_LENGTH; i++) {
		if (DISPLAY_VER(display) >= 20)
			intel_de_rmw(display, DPST_IE_BIN(pipe),
				     DPST_IE_BIN_DATA_MASK,
				     DPST_IE_BIN_DATA(data[i]));
		else
			intel_de_rmw(display, DPST_BIN(pipe),
				     DPST_BIN_DATA_MASK,
				     DPST_BIN_DATA(data[i]));

		drm_dbg_atomic(display->drm, "iet_lut[%d]=%x\n",
			       i, data[i]);
	}
}

static bool intel_histogram_get_data(struct intel_crtc *intel_crtc)
{
	struct intel_display *display = to_intel_display(intel_crtc);
	struct intel_histogram *histogram = intel_crtc->histogram;
	int index;
	u32 dpstbin;

	if (DISPLAY_VER(display) >= 20)
		intel_de_rmw(display, DPST_HIST_INDEX(intel_crtc->pipe),
			     DPST_HIST_BIN_INDEX_MASK,
			     DPST_HIST_BIN_INDEX(0));
	else
		intel_de_rmw(display, DPST_CTL(intel_crtc->pipe),
			     DPST_CTL_BIN_REG_FUNC_SEL | DPST_CTL_BIN_REG_MASK, 0);

	for (index = 0; index < ARRAY_SIZE(histogram->bin_data); index++) {
		dpstbin = intel_de_read(display, (DISPLAY_VER(display) >= 20 ?
					DPST_HIST_BIN(intel_crtc->pipe) :
					DPST_BIN(intel_crtc->pipe)));
		if (!(dpstbin & DPST_BIN_BUSY)) {
			histogram->bin_data[index] = dpstbin & (DISPLAY_VER(display) >= 20 ?
								DPST_HIST_BIN_DATA_MASK :
								DPST_BIN_DATA_MASK);
		} else {
			drm_err(display->drm, "Histogram bin busy, retyring\n");
			fsleep(2);
			return false;
		}
	}
	return true;
}

static void intel_histogram_handle_int_work(struct work_struct *work)
{
	struct intel_histogram *histogram = container_of(work,
		struct intel_histogram, work.work);
	struct intel_crtc *intel_crtc = histogram->crtc;
	struct intel_display *display = to_intel_display(intel_crtc);
	struct intel_encoder *encoder = NULL;
	struct intel_dp *intel_dp = NULL;
	char *event[3] = {NULL, NULL, NULL};
	enum pipe pipe=intel_crtc->pipe;
	int retry;

	event[0] = "HISTOGRAM=1";
	event[1] = kasprintf(GFP_KERNEL, "PIPE=%d", intel_crtc->pipe);
	event[2] = NULL;

	/* Wa: 14014889975 */
	if (IS_DISPLAY_VER(display, 13, 14))
		intel_de_rmw(display, DPST_CTL(intel_crtc->pipe),
			     DPST_CTL_RESTORE, 0);

	for_each_intel_encoder_mask_with_psr(display->drm, encoder,
					     intel_crtc->config->uapi.encoder_mask)
		intel_dp = enc_to_intel_dp(encoder);

	/* If PSR is active, read-write the Palette LUT so as to trigger a PSR exit */
	if (intel_dp->psr.active) {
		u32 val = intel_de_read_fw(display, LGC_PALETTE(pipe, 0));
		intel_de_write_fw(display, LGC_PALETTE(pipe, 0), val);
	}

	/*
	 * Set DPST_CTL Bin Reg function select to TC
	 * Set DPST_CTL Bin Register Index to 0
	 */
	for (retry = 0; retry < HISTOGRAM_BIN_READ_RETRY_COUNT; retry++) {
		if (intel_histogram_get_data(intel_crtc)) {
			struct drm_histogram *hist;

			hist = kzalloc(sizeof(struct drm_histogram), GFP_KERNEL);
			if (!hist)
				return;
			hist->nr_elements = ARRAY_SIZE(histogram->bin_data);
			memcpy(hist->max_rgb, histogram->bin_data, sizeof(histogram->bin_data));

			/* TODO: fill the drm_histogram_config data back this drm_histogram struct */
			drm_property_replace_global_blob(display->drm,
				&intel_crtc->base.state->histogram_data,
				sizeof(struct drm_histogram),
				hist, &intel_crtc->base.base,
				intel_crtc->base.histogram_data_property);
			/* Notify user for Histogram readiness */
			if (kobject_uevent_env(&display->drm->primary->kdev->kobj,
					       KOBJ_CHANGE, event))
				drm_err(display->drm,
					"Sending HISTOGRAM event failed\n");
			break;
		}
	}
	if (retry >= HISTOGRAM_BIN_READ_RETRY_COUNT) {
		drm_err(display->drm, "Histogram bin read failed with max retry\n");
		return;
	}

	/* Wa: 14014889975 */
	if (IS_DISPLAY_VER(display, 13, 14))
		/* Write the value read from DPST_CTL to DPST_CTL.Interrupt Delay Counter(bit 23:16) */
		intel_de_rmw(display, DPST_CTL(intel_crtc->pipe),
			     DPST_CTL_GUARDBAND_INTERRUPT_DELAY_CNT |
			     DPST_CTL_RESTORE,
			     DPST_CTL_GUARDBAND_INTERRUPT_DELAY(0x0) |
			     DPST_CTL_RESTORE);

	/* Enable histogram interrupt */
	intel_de_rmw(display, DPST_GUARD(intel_crtc->pipe), DPST_GUARD_HIST_INT_EN,
		     DPST_GUARD_HIST_INT_EN);

	/* Clear histogram interrupt by setting histogram interrupt status bit*/
	intel_de_rmw(display, DPST_GUARD(intel_crtc->pipe),
		     DPST_GUARD_HIST_EVENT_STATUS, 1);
}

void intel_histogram_irq_handler(struct intel_display *display, enum pipe pipe)
{
	struct intel_crtc *intel_crtc =
		to_intel_crtc(drm_crtc_from_index(display->drm, pipe));
	struct intel_histogram *histogram = intel_crtc->histogram;

	if (!histogram->enable) {
		drm_err(display->drm,
			"Spurious interrupt, histogram not enabled\n");
		return;
	}

	queue_delayed_work(display->wq.unordered,
			   &histogram->work, 0);
}

int intel_histogram_atomic_check(struct intel_crtc *intel_crtc)
{
	struct intel_histogram *histogram = intel_crtc->histogram;

	/* TODO: Restrictions for enabling histogram */
	histogram->can_enable = true;

	return 0;
}

static int intel_histogram_enable(struct intel_crtc *intel_crtc, u8 mode)
{
	struct intel_display *display = to_intel_display(intel_crtc);
	struct intel_histogram *histogram = intel_crtc->histogram;
	int pipe = intel_crtc->pipe;
	u64 res;
	u32 gbandthreshold;

	if (!histogram || !histogram->can_enable)
		return -EINVAL;

	if (histogram->enable)
		return 0;

	/* Pipe Dithering should be enabled with histogram */
	intel_histogram_enable_dithering(display, pipe);

	 /* enable histogram, clear DPST_BIN reg and select TC function */
	if (DISPLAY_VER(display) >= 20)
		intel_de_rmw(display, DPST_CTL(pipe),
			     DPST_CTL_IE_HIST_EN |
			     DPST_CTL_HIST_MODE,
			     DPST_CTL_IE_HIST_EN |
			     DPST_CTL_HIST_MODE_HSV);
	else
		 /* enable histogram, clear DPST_CTL bin reg func select to TC */
		intel_de_rmw(display, DPST_CTL(pipe),
			     DPST_CTL_BIN_REG_FUNC_SEL | DPST_CTL_IE_HIST_EN |
			     DPST_CTL_HIST_MODE |
			     DPST_CTL_IE_TABLE_VALUE_FORMAT |
			     DPST_CTL_ENHANCEMENT_MODE_MASK |
			     DPST_CTL_IE_MODI_TABLE_EN,
			     ((mode == DRM_MODE_HISTOGRAM_HSV_MAX_RGB) ?
			      DPST_CTL_BIN_REG_FUNC_TC : 0) |
			     DPST_CTL_IE_HIST_EN |
			     DPST_CTL_HIST_MODE_HSV |
			     DPST_CTL_IE_TABLE_VALUE_FORMAT_1INT_9FRAC |
			     DPST_CTL_EN_MULTIPLICATIVE | DPST_CTL_IE_MODI_TABLE_EN);

	/* Re-Visit: check if wait for one vblank is required */
	drm_crtc_wait_one_vblank(&intel_crtc->base);

	/* TODO: Program GuardBand Threshold needs to be moved to modeset path */
	res = (intel_crtc->config->hw.adjusted_mode.vtotal *
	       intel_crtc->config->hw.adjusted_mode.htotal);

	gbandthreshold = (res *	HISTOGRAM_GUARDBAND_THRESHOLD_DEFAULT) /
			  HISTOGRAM_GUARDBAND_PRECISION_FACTOR;

	/* Enable histogram interrupt mode */
	intel_de_rmw(display, DPST_GUARD(pipe),
		     DPST_GUARD_THRESHOLD_GB_MASK |
		     DPST_GUARD_INTERRUPT_DELAY_MASK | DPST_GUARD_HIST_INT_EN,
		     DPST_GUARD_THRESHOLD_GB(gbandthreshold) |
		     DPST_GUARD_INTERRUPT_DELAY(0x04) |
		     DPST_GUARD_HIST_INT_EN);

	/* Clear pending interrupts has to be done on separate write */
	intel_de_rmw(display, DPST_GUARD(pipe),
		     DPST_GUARD_HIST_EVENT_STATUS, 1);

	histogram->enable = true;

	return 0;
}

static void intel_histogram_disable(struct intel_crtc *intel_crtc)
{
	struct intel_display *display = to_intel_display(intel_crtc);
	struct intel_histogram *histogram = intel_crtc->histogram;
	int pipe = intel_crtc->pipe;

	if (!histogram)
		return;

	/* If already disabled return */
	if (!histogram->enable)
		return;

	/* Clear pending interrupts and disable interrupts */
	intel_de_rmw(display, DPST_GUARD(pipe),
		     DPST_GUARD_HIST_INT_EN | DPST_GUARD_HIST_EVENT_STATUS, 0);

	/* disable DPST_CTL Histogram mode */
	intel_de_rmw(display, DPST_CTL(pipe),
		     DPST_CTL_IE_HIST_EN, 0);

	cancel_delayed_work(&histogram->work);
	histogram->enable = false;
}

int intel_histogram_update(struct intel_crtc *intel_crtc,
			   struct drm_histogram_config *config)
{
	struct intel_display *display = to_intel_display(intel_crtc);

	if (config->enable) {
		if (config->hist_mode != DRM_MODE_HISTOGRAM_HSV_MAX_RGB) {
			drm_err(display->drm,
				"Only max(RGB) mode is supported for histogram\n");
			return -EINVAL;
		}
		return intel_histogram_enable(intel_crtc, config->hist_mode);
	}

	intel_histogram_disable(intel_crtc);
	return 0;
}

int intel_histogram_set_iet_lut(struct intel_crtc *intel_crtc,
				struct drm_property_blob *blob)
{
	struct intel_histogram *histogram = intel_crtc->histogram;
	struct intel_display *display = to_intel_display(intel_crtc);
	struct intel_encoder *encoder = NULL;
	struct intel_connector *connector = NULL;
	struct intel_dp *intel_dp = NULL;
	int pipe = intel_crtc->pipe;
	struct drm_iet_1dlut_sample *iet;
	u32 *data;
	int ret;

	if (!histogram)
		return -EINVAL;

	if (!histogram->enable) {
		drm_err(display->drm, "histogram not enabled");
		return -EINVAL;
	}

	if (!data) {
	drm_err(display->drm, "enhancement LUT data is NULL");
		return -EINVAL;
	}


	if (DISPLAY_VER(display) < 20) {
		/* Set DPST_CTL Bin Reg function select to IE & wait for a vblabk */
		intel_de_rmw(display, DPST_CTL(pipe),
			     DPST_CTL_BIN_REG_FUNC_SEL,
			     DPST_CTL_BIN_REG_FUNC_IE);
	}

	set_bin_index_0(display, pipe);

	iet = (struct drm_iet_1dlut_sample *)blob->data;
	data = kzalloc(sizeof(data) * iet->nr_elements, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	ret = copy_from_user(data, (uint32_t __user *)(unsigned long)iet->iet_lut,
			     sizeof(uint32_t) * iet->nr_elements);
	if (ret)
		return ret;

	for_each_intel_encoder_mask_with_psr(display->drm, encoder,
					     intel_crtc->config->uapi.encoder_mask)
		intel_dp = enc_to_intel_dp(encoder);

	/* If PSR is active, read-write the Palette LUT so as to trigger a PSR exit */
	if (intel_dp->psr.active) {
		u32 val = intel_de_read_fw(display, LGC_PALETTE(pipe, 0));
		intel_de_write_fw(display, LGC_PALETTE(pipe, 0), val);
	}

	write_iet(display, pipe, data);

	if (intel_dp && intel_dp_is_edp(intel_dp) &&
	    iet->nr_elements == (HISTOGRAM_IET_LENGTH + 1)) {
		connector = intel_dp->attached_connector;
		connector->panel.backlight.funcs->set(connector->base.state, data[iet->nr_elements]);
	}

	kfree(data);
	drm_property_blob_put(intel_crtc->base.state->iet_lut);

	return 0;
}

void intel_histogram_finish(struct intel_crtc *intel_crtc)
{
	struct intel_histogram *histogram = intel_crtc->histogram;

	cancel_delayed_work_sync(&histogram->work);
	kfree(histogram);
}

int intel_histogram_init(struct intel_crtc *crtc)
{
	struct intel_histogram *histogram;
	struct drm_histogram_caps *histogram_caps;
	struct drm_iet_caps *iet_caps;
	u32 *iet_format;

	/* Allocate histogram internal struct */
	histogram = kzalloc(sizeof(*histogram), GFP_KERNEL);
	if (!histogram)
		return -ENOMEM;
	histogram_caps = kzalloc(sizeof(*histogram_caps), GFP_KERNEL);
	if (!histogram_caps)
		return -ENOMEM;

	histogram_caps->histogram_mode = DRM_MODE_HISTOGRAM_HSV_MAX_RGB;
	histogram_caps->bins_count = HISTOGRAM_BIN_COUNT;

	iet_caps = kzalloc(sizeof(*iet_caps), GFP_KERNEL);
	if (!iet_caps)
		return -ENOMEM;

	iet_caps->iet_mode = DRM_MODE_IET_MULTIPLICATIVE;
	iet_caps->nr_iet_sample_formats = 1;
	iet_caps->nr_iet_lut_entries = HISTOGRAM_IET_LENGTH;
	iet_format = kzalloc(sizeof(u32)*iet_caps->nr_iet_sample_formats,
			     GFP_KERNEL);
	*iet_format = IET_SAMPLE_FORMAT_1_INT_9_FRACT;
	iet_caps->iet_sample_format = *iet_format;

	crtc->histogram = histogram;
	histogram->crtc = crtc;
	histogram->can_enable = false;
	histogram->caps = histogram_caps;
	histogram->iet_caps = iet_caps;

	INIT_DEFERRABLE_WORK(&histogram->work,
			     intel_histogram_handle_int_work);

	return 0;
}
