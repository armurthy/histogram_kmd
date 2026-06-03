// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __INTEL_HISTOGRAM_H__
#define __INTEL_HISTOGRAM_H__

#include <linux/types.h>
#include <linux/workqueue.h>

struct delayed_work;
struct drm_property_blob;
struct drm_histogram_config;
struct drm_histogram_caps;
struct intel_crtc;
struct intel_display;
enum pipe;

#define HISTOGRAM_BIN_COUNT                    32

struct intel_histogram {
	struct drm_histogram_caps *caps;
	struct intel_crtc *crtc;
	struct delayed_work work;
	bool enable;
	bool can_enable;
	u32 bin_data[HISTOGRAM_BIN_COUNT];
};

enum intel_global_hist_status {
	INTEL_HISTOGRAM_ENABLE,
	INTEL_HISTOGRAM_DISABLE,
};

enum intel_global_histogram {
	INTEL_HISTOGRAM,
};

enum intel_global_hist_lut {
	INTEL_HISTOGRAM_PIXEL_FACTOR,
};

void intel_histogram_irq_handler(struct intel_display *display, enum pipe pipe);
int intel_histogram_atomic_check(struct intel_crtc *intel_crtc);
int intel_histogram_update(struct intel_crtc *intel_crtc,
			   struct drm_histogram_config *config);
int intel_histogram_init(struct intel_crtc *intel_crtc);
void intel_histogram_finish(struct intel_crtc *intel_crtc);

#endif /* __INTEL_HISTOGRAM_H__ */
