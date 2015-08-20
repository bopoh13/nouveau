/*
 * Copyright 2012 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */
#include "nv04.h"

#include <core/ramht.h>
#include <engine/gr/nv40.h>

/******************************************************************************
 * instmem subdev implementation
 *****************************************************************************/

static u32
nv40_instmem_rd32(struct nvkm_instmem *obj, u32 addr)
{
	struct nv04_instmem *imem = container_of(obj, typeof(*imem), base);
	return ioread32_native(imem->iomem + addr);
}

static void
nv40_instmem_wr32(struct nvkm_instmem *obj, u32 addr, u32 data)
{
	struct nv04_instmem *imem = container_of(obj, typeof(*imem), base);
	iowrite32_native(data, imem->iomem + addr);
}

static const struct nvkm_instmem_func
nv40_instmem_func = {
	.rd32 = nv40_instmem_rd32,
	.wr32 = nv40_instmem_wr32,
};

static int
nv40_instmem_ctor(struct nvkm_object *parent, struct nvkm_object *engine,
		  struct nvkm_oclass *oclass, void *data, u32 size,
		  struct nvkm_object **pobject)
{
	struct nvkm_device *device = (void *)parent;
	struct nv04_instmem *imem;
	int ret, bar, vs;

	ret = nvkm_instmem_create(parent, engine, oclass, &imem);
	*pobject = nv_object(imem);
	if (ret)
		return ret;

	imem->base.func = &nv40_instmem_func;

	/* map bar */
	if (nv_device_resource_len(device, 2))
		bar = 2;
	else
		bar = 3;

	imem->iomem = ioremap(nv_device_resource_start(device, bar),
			      nv_device_resource_len(device, bar));
	if (!imem->iomem) {
		nvkm_error(&imem->base.subdev, "unable to map PRAMIN BAR\n");
		return -EFAULT;
	}

	/* PRAMIN aperture maps over the end of vram, reserve enough space
	 * to fit graphics contexts for every channel, the magics come
	 * from engine/gr/nv40.c
	 */
	vs = hweight8((nvkm_rd32(device, 0x001540) & 0x0000ff00) >> 8);
	if      (device->chipset == 0x40) imem->base.reserved = 0x6aa0 * vs;
	else if (device->chipset  < 0x43) imem->base.reserved = 0x4f00 * vs;
	else if (nv44_gr_class(imem))  imem->base.reserved = 0x4980 * vs;
	else				  imem->base.reserved = 0x4a40 * vs;
	imem->base.reserved += 16 * 1024;
	imem->base.reserved *= 32;		/* per-channel */
	imem->base.reserved += 512 * 1024;	/* pci(e)gart table */
	imem->base.reserved += 512 * 1024;	/* object storage */

	imem->base.reserved = round_up(imem->base.reserved, 4096);

	ret = nvkm_mm_init(&imem->heap, 0, imem->base.reserved, 1);
	if (ret)
		return ret;

	/* 0x00000-0x10000: reserve for probable vbios image */
	ret = nvkm_gpuobj_new(nv_object(imem), NULL, 0x10000, 0, 0,
			      &imem->vbios);
	if (ret)
		return ret;

	/* 0x10000-0x18000: reserve for RAMHT */
	ret = nvkm_ramht_new(nv_object(imem), NULL, 0x08000, 0, &imem->ramht);
	if (ret)
		return ret;

	/* 0x18000-0x18200: reserve for RAMRO
	 * 0x18200-0x20000: padding
	 */
	ret = nvkm_gpuobj_new(nv_object(imem), NULL, 0x08000, 0, 0,
			      &imem->ramro);
	if (ret)
		return ret;

	/* 0x20000-0x21000: reserve for RAMFC
	 * 0x21000-0x40000: padding and some unknown crap
	 */
	ret = nvkm_gpuobj_new(nv_object(imem), NULL, 0x20000, 0,
			      NVOBJ_FLAG_ZERO_ALLOC, &imem->ramfc);
	if (ret)
		return ret;

	return 0;
}

struct nvkm_oclass *
nv40_instmem_oclass = &(struct nvkm_instmem_impl) {
	.base.handle = NV_SUBDEV(INSTMEM, 0x40),
	.base.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = nv40_instmem_ctor,
		.dtor = nv04_instmem_dtor,
		.init = _nvkm_instmem_init,
		.fini = _nvkm_instmem_fini,
	},
	.instobj = &nv04_instobj_oclass.base,
}.base;
