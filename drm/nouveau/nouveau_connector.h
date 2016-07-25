/*
 * Copyright (C) 2008 Maarten Maathuis.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef __NOUVEAU_CONNECTOR_H__
#define __NOUVEAU_CONNECTOR_H__

#include <nvif/notify.h>

#include <drm/drm_edid.h>
#include <drm/drm_dp_helper.h>
#include "nouveau_crtc.h"

struct nvkm_i2c_port;

struct nouveau_connector {
	struct drm_connector base;
	enum dcb_connector_type type;
	u8 index;
	u8 *dcb;

	struct nvif_notify hpd;

	struct drm_dp_aux aux;

	int dithering_mode;
	int dithering_depth;
	int scaling_mode;
	bool scaling_full;
	int underscan;
	u32 underscan_hborder;
	u32 underscan_vborder;

	struct nouveau_encoder *detected_encoder;
	struct edid *edid;
	struct drm_display_mode *native_mode;
};

static inline struct nouveau_connector *nouveau_connector(
						struct drm_connector *con)
{
	return container_of(con, struct nouveau_connector, base);
}

static inline struct nouveau_connector *
nouveau_crtc_connector_get(struct nouveau_crtc *nv_crtc)
{
	struct drm_device *dev = nv_crtc->base.dev;
	struct drm_connector *connector;
	struct drm_crtc *crtc = to_drm_crtc(nv_crtc);

	list_for_each_entry(connector, &dev->mode_config.connector_list, head) {
		if (connector->encoder && connector->encoder->crtc == crtc)
			return nouveau_connector(connector);
	}

	return NULL;
}

struct drm_connector *
nouveau_connector_create(struct drm_device *, int index);

extern int nouveau_tv_disable;
extern int nouveau_ignorelid;
extern int nouveau_duallink;
extern int nouveau_hdmimhz;

#include <drm/drm_crtc.h>
#define nouveau_conn_atom(p)                                                   \
	container_of((p), struct nouveau_conn_atom, state)

struct nouveau_conn_atom {
	struct drm_connector_state state;

	struct {
		/* The enum values specifically defined here match nv50/gf119
		 * hw values, and the code relies on this.
		 */
		enum {
			DITHERING_MODE_OFF = 0x00,
			DITHERING_MODE_ON = 0x01,
			DITHERING_MODE_DYNAMIC2X2 = 0x10 | DITHERING_MODE_ON,
			DITHERING_MODE_STATIC2X2 = 0x18 | DITHERING_MODE_ON,
			DITHERING_MODE_TEMPORAL = 0x20 | DITHERING_MODE_ON,
			DITHERING_MODE_AUTO
		} mode;
		enum {
			DITHERING_DEPTH_6BPC = 0x00,
			DITHERING_DEPTH_8BPC = 0x02,
			DITHERING_DEPTH_AUTO
		} depth;
	} dither;

	struct {
		int mode;	/* DRM_MODE_SCALE_* */
		struct {
			enum {
				UNDERSCAN_OFF,
				UNDERSCAN_ON,
				UNDERSCAN_AUTO,
			} mode;
			u32 hborder;
			u32 vborder;
		} underscan;
		bool full;
	} scaler;

	struct {
		int color_vibrance;
		int vibrant_hue;
	} procamp;

	union {
		struct {
			bool dither:1;
			bool scaler:1;
			bool procamp:1;
		};
		u8 mask;
	} set;
};

void nouveau_conn_attach_properties(struct drm_connector *);
void nouveau_conn_reset(struct drm_connector *);
struct drm_connector_state *
nouveau_conn_atomic_duplicate_state(struct drm_connector *);
void nouveau_conn_atomic_destroy_state(struct drm_connector *,
				       struct drm_connector_state *);
int nouveau_conn_atomic_set_property(struct drm_connector *,
				     struct drm_connector_state *,
				     struct drm_property *, u64);
int nouveau_conn_atomic_get_property(struct drm_connector *,
				     const struct drm_connector_state *,
				     struct drm_property *, u64 *);
struct drm_display_mode *nouveau_conn_native_mode(struct drm_connector *);
#endif /* __NOUVEAU_CONNECTOR_H__ */
