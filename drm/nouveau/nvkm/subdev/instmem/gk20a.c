/*
 * Copyright (c) 2015, NVIDIA CORPORATION. All rights reserved.
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
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * GK20A does not have dedicated video memory, and to accurately represent this
 * fact Nouveau will not create a RAM device for it. Therefore its instmem
 * implementation must be done directly on top of system memory, while providing
 * coherent read and write operations.
 *
 * Instmem can be allocated through two means:
 * 1) If an IOMMU mapping has been probed, the IOMMU API is used to make memory
 *    pages contiguous to the GPU. This is the preferred way.
 * 2) If no IOMMU mapping is probed, the DMA API is used to allocate physically
 *    contiguous memory.
 *
 * In both cases CPU read and writes are performed using PRAMIN (i.e. using the
 * GPU path) to ensure these operations are coherent for the GPU. This allows us
 * to use more "relaxed" allocation parameters when using the DMA API, since we
 * never need a kernel mapping.
 */

#include <subdev/fb.h>
#include <core/mm.h>

#ifdef __KERNEL__
#include <linux/dma-attrs.h>
#include <linux/iommu.h>
#include <nouveau_platform.h>
#endif

#include "priv.h"

struct gk20a_instobj {
	struct nvkm_instobj base;
	/* Must be second member here - see nouveau_gpuobj_map_vm() */
	struct nvkm_mem *mem;
	/* Pointed by mem */
	struct nvkm_mem _mem;
};

/*
 * Used for objects allocated using the DMA API
 */
struct gk20a_instobj_dma {
	struct gk20a_instobj base;

	void *cpuaddr;
	dma_addr_t handle;
	struct nvkm_mm_node r;
};

/*
 * Used for objects flattened using the IOMMU API
 */
struct gk20a_instobj_iommu {
	struct gk20a_instobj base;

	/* array of base.mem->size pages */
	struct page *pages[];
};

struct gk20a_instmem {
	struct nvkm_instmem base;
	spinlock_t lock;
	u64 addr;

	/* Only used if IOMMU if present */
	struct mutex *mm_mutex;
	struct nvkm_mm *mm;
	struct iommu_domain *domain;
	unsigned long iommu_pgshift;

	/* Only used by DMA API */
	struct dma_attrs attrs;
};

/*
 * Use PRAMIN to read/write data and avoid coherency issues.
 * PRAMIN uses the GPU path and ensures data will always be coherent.
 *
 * A dynamic mapping based solution would be desirable in the future, but
 * the issue remains of how to maintain coherency efficiently. On ARM it is
 * not easy (if possible at all?) to create uncached temporary mappings.
 */

static u32
gk20a_instobj_rd32(struct nvkm_object *object, u64 offset)
{
	struct gk20a_instmem *imem = (void *)nvkm_instmem(object);
	struct gk20a_instobj *node = (void *)object;
	struct nvkm_device *device = imem->base.subdev.device;
	unsigned long flags;
	u64 base = (node->mem->offset + offset) & 0xffffff00000ULL;
	u64 addr = (node->mem->offset + offset) & 0x000000fffffULL;
	u32 data;

	spin_lock_irqsave(&imem->lock, flags);
	if (unlikely(imem->addr != base)) {
		nvkm_wr32(device, 0x001700, base >> 16);
		imem->addr = base;
	}
	data = nvkm_rd32(device, 0x700000 + addr);
	spin_unlock_irqrestore(&imem->lock, flags);
	return data;
}

static void
gk20a_instobj_wr32(struct nvkm_object *object, u64 offset, u32 data)
{
	struct gk20a_instmem *imem = (void *)nvkm_instmem(object);
	struct gk20a_instobj *node = (void *)object;
	struct nvkm_device *device = imem->base.subdev.device;
	unsigned long flags;
	u64 base = (node->mem->offset + offset) & 0xffffff00000ULL;
	u64 addr = (node->mem->offset + offset) & 0x000000fffffULL;

	spin_lock_irqsave(&imem->lock, flags);
	if (unlikely(imem->addr != base)) {
		nvkm_wr32(device, 0x001700, base >> 16);
		imem->addr = base;
	}
	nvkm_wr32(device, 0x700000 + addr, data);
	spin_unlock_irqrestore(&imem->lock, flags);
}

static void
gk20a_instobj_dtor_dma(struct gk20a_instobj *_node)
{
	struct gk20a_instobj_dma *node = (void *)_node;
	struct gk20a_instmem *imem = (void *)nvkm_instmem(node);
	struct device *dev = nv_device_base(nv_device(imem));

	if (unlikely(!node->cpuaddr))
		return;

	dma_free_attrs(dev, _node->mem->size << PAGE_SHIFT, node->cpuaddr,
		       node->handle, &imem->attrs);
}

static void
gk20a_instobj_dtor_iommu(struct gk20a_instobj *_node)
{
	struct gk20a_instobj_iommu *node = (void *)_node;
	struct gk20a_instmem *imem = (void *)nvkm_instmem(node);
	struct nvkm_mm_node *r;
	int i;

	if (unlikely(list_empty(&_node->mem->regions)))
		return;

	r = list_first_entry(&_node->mem->regions, struct nvkm_mm_node,
			     rl_entry);

	/* clear bit 34 to unmap pages */
	r->offset &= ~BIT(34 - imem->iommu_pgshift);

	/* Unmap pages from GPU address space and free them */
	for (i = 0; i < _node->mem->size; i++) {
		iommu_unmap(imem->domain,
			    (r->offset + i) << imem->iommu_pgshift, PAGE_SIZE);
		__free_page(node->pages[i]);
	}

	/* Release area from GPU address space */
	mutex_lock(imem->mm_mutex);
	nvkm_mm_free(imem->mm, &r);
	mutex_unlock(imem->mm_mutex);
}

static void
gk20a_instobj_dtor(struct nvkm_object *object)
{
	struct gk20a_instobj *node = (void *)object;
	struct gk20a_instmem *imem = (void *)nvkm_instmem(node);

	if (imem->domain)
		gk20a_instobj_dtor_iommu(node);
	else
		gk20a_instobj_dtor_dma(node);

	nvkm_instobj_destroy(&node->base);
}

static int
gk20a_instobj_ctor_dma(struct nvkm_object *parent, struct nvkm_object *engine,
		       struct nvkm_oclass *oclass, u32 npages, u32 align,
		       struct gk20a_instobj **_node)
{
	struct gk20a_instobj_dma *node;
	struct gk20a_instmem *imem = (void *)nvkm_instmem(parent);
	struct nvkm_subdev *subdev = &imem->base.subdev;
	struct device *dev = nv_device_base(nv_device(parent));
	int ret;

	ret = nvkm_instobj_create_(parent, engine, oclass, sizeof(*node),
				   (void **)&node);
	*_node = &node->base;
	if (ret)
		return ret;

	node->cpuaddr = dma_alloc_attrs(dev, npages << PAGE_SHIFT,
					&node->handle, GFP_KERNEL,
					&imem->attrs);
	if (!node->cpuaddr) {
		nvkm_error(subdev, "cannot allocate DMA memory\n");
		return -ENOMEM;
	}

	/* alignment check */
	if (unlikely(node->handle & (align - 1)))
		nvkm_warn(subdev,
			  "memory not aligned as requested: %pad (0x%x)\n",
			  &node->handle, align);

	/* present memory for being mapped using small pages */
	node->r.type = 12;
	node->r.offset = node->handle >> 12;
	node->r.length = (npages << PAGE_SHIFT) >> 12;

	node->base._mem.offset = node->handle;

	INIT_LIST_HEAD(&node->base._mem.regions);
	list_add_tail(&node->r.rl_entry, &node->base._mem.regions);

	return 0;
}

static int
gk20a_instobj_ctor_iommu(struct nvkm_object *parent, struct nvkm_object *engine,
			 struct nvkm_oclass *oclass, u32 npages, u32 align,
			 struct gk20a_instobj **_node)
{
	struct gk20a_instobj_iommu *node;
	struct gk20a_instmem *imem = (void *)nvkm_instmem(parent);
	struct nvkm_subdev *subdev = &imem->base.subdev;
	struct nvkm_mm_node *r;
	int ret;
	int i;

	ret = nvkm_instobj_create_(parent, engine, oclass,
				sizeof(*node) + sizeof(node->pages[0]) * npages,
				(void **)&node);
	*_node = &node->base;
	if (ret)
		return ret;

	/* Allocate backing memory */
	for (i = 0; i < npages; i++) {
		struct page *p = alloc_page(GFP_KERNEL);

		if (p == NULL) {
			ret = -ENOMEM;
			goto free_pages;
		}
		node->pages[i] = p;
	}

	mutex_lock(imem->mm_mutex);
	/* Reserve area from GPU address space */
	ret = nvkm_mm_head(imem->mm, 0, 1, npages, npages,
			   align >> imem->iommu_pgshift, &r);
	mutex_unlock(imem->mm_mutex);
	if (ret) {
		nvkm_error(subdev, "virtual space is full!\n");
		goto free_pages;
	}

	/* Map into GPU address space */
	for (i = 0; i < npages; i++) {
		struct page *p = node->pages[i];
		u32 offset = (r->offset + i) << imem->iommu_pgshift;

		ret = iommu_map(imem->domain, offset, page_to_phys(p),
				PAGE_SIZE, IOMMU_READ | IOMMU_WRITE);
		if (ret < 0) {
			nvkm_error(subdev, "IOMMU mapping failure: %d\n", ret);

			while (i-- > 0) {
				offset -= PAGE_SIZE;
				iommu_unmap(imem->domain, offset, PAGE_SIZE);
			}
			goto release_area;
		}
	}

	/* Bit 34 tells that an address is to be resolved through the IOMMU */
	r->offset |= BIT(34 - imem->iommu_pgshift);

	node->base._mem.offset = ((u64)r->offset) << imem->iommu_pgshift;

	INIT_LIST_HEAD(&node->base._mem.regions);
	list_add_tail(&r->rl_entry, &node->base._mem.regions);

	return 0;

release_area:
	mutex_lock(imem->mm_mutex);
	nvkm_mm_free(imem->mm, &r);
	mutex_unlock(imem->mm_mutex);

free_pages:
	for (i = 0; i < npages && node->pages[i] != NULL; i++)
		__free_page(node->pages[i]);

	return ret;
}

static int
gk20a_instobj_ctor(struct nvkm_object *parent, struct nvkm_object *engine,
		   struct nvkm_oclass *oclass, void *data, u32 _size,
		   struct nvkm_object **pobject)
{
	struct nvkm_instobj_args *args = data;
	struct gk20a_instmem *imem = (void *)nvkm_instmem(parent);
	struct gk20a_instobj *node;
	struct nvkm_subdev *subdev = &imem->base.subdev;
	u32 size, align;
	int ret;

	nvkm_debug(subdev, "%s (%s): size: %x align: %x\n", __func__,
		   imem->domain ? "IOMMU" : "DMA", args->size, args->align);

	/* Round size and align to page bounds */
	size = max(roundup(args->size, PAGE_SIZE), PAGE_SIZE);
	align = max(roundup(args->align, PAGE_SIZE), PAGE_SIZE);

	if (imem->domain)
		ret = gk20a_instobj_ctor_iommu(parent, engine, oclass,
					      size >> PAGE_SHIFT, align, &node);
	else
		ret = gk20a_instobj_ctor_dma(parent, engine, oclass,
					     size >> PAGE_SHIFT, align, &node);
	*pobject = nv_object(node);
	if (ret)
		return ret;

	node->mem = &node->_mem;

	/* present memory for being mapped using small pages */
	node->mem->size = size >> 12;
	node->mem->memtype = 0;
	node->mem->page_shift = 12;

	node->base.addr = node->mem->offset;
	node->base.size = size;

	nvkm_debug(subdev, "alloc size: 0x%x, align: 0x%x, gaddr: 0x%llx\n",
		   size, align, node->mem->offset);

	return 0;
}

static struct nvkm_instobj_impl
gk20a_instobj_oclass = {
	.base.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = gk20a_instobj_ctor,
		.dtor = gk20a_instobj_dtor,
		.init = _nvkm_instobj_init,
		.fini = _nvkm_instobj_fini,
		.rd32 = gk20a_instobj_rd32,
		.wr32 = gk20a_instobj_wr32,
	},
};



static int
gk20a_instmem_fini(struct nvkm_object *object, bool suspend)
{
	struct gk20a_instmem *imem = (void *)object;
	imem->addr = ~0ULL;
	return nvkm_instmem_fini(&imem->base, suspend);
}

static int
gk20a_instmem_ctor(struct nvkm_object *parent, struct nvkm_object *engine,
		   struct nvkm_oclass *oclass, void *data, u32 size,
		   struct nvkm_object **pobject)
{
	struct nvkm_device *device = (void *)parent;
	struct gk20a_instmem *imem;
	int ret;

	ret = nvkm_instmem_create(parent, engine, oclass, &imem);
	*pobject = nv_object(imem);
	if (ret)
		return ret;

	spin_lock_init(&imem->lock);

	if (device->gpu->iommu.domain) {
		imem->domain = device->gpu->iommu.domain;
		imem->mm = device->gpu->iommu.mm;
		imem->iommu_pgshift = device->gpu->iommu.pgshift;
		imem->mm_mutex = &device->gpu->iommu.mutex;

		nvkm_info(&imem->base.subdev, "using IOMMU\n");
	} else {
		init_dma_attrs(&imem->attrs);
		/*
		 * We will access instmem through PRAMIN and thus do not need a
		 * consistent CPU pointer or kernel mapping
		 */
		dma_set_attr(DMA_ATTR_NON_CONSISTENT, &imem->attrs);
		dma_set_attr(DMA_ATTR_WEAK_ORDERING, &imem->attrs);
		dma_set_attr(DMA_ATTR_WRITE_COMBINE, &imem->attrs);
		dma_set_attr(DMA_ATTR_NO_KERNEL_MAPPING, &imem->attrs);

		nvkm_info(&imem->base.subdev, "using DMA API\n");
	}

	return 0;
}

struct nvkm_oclass *
gk20a_instmem_oclass = &(struct nvkm_instmem_impl) {
	.base.handle = NV_SUBDEV(INSTMEM, 0xea),
	.base.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = gk20a_instmem_ctor,
		.dtor = _nvkm_instmem_dtor,
		.init = _nvkm_instmem_init,
		.fini = gk20a_instmem_fini,
	},
	.instobj = &gk20a_instobj_oclass.base,
}.base;
