/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVKMD_H
#define NVKMD_H 1

#include "nv_device_info.h"
#include "util/u_atomic.h"

#include "vulkan/vulkan_core.h"
#include "nouveau_device.h"

#include <assert.h>
#include <stdbool.h>
#include <stdbool.h>
#include <sys/types.h>

struct nvkmd_ctx;
struct nvkmd_dev;
struct nvkmd_mem;
struct nvkmd_pdev;
struct nvkmd_va;

struct _drmDevice;
struct vk_object_base;

/*
 * Enums
 */

enum nvkmd_mem_flags {
   /** VRAM on discrete GPUs or GART on integrated */
   NVKMD_MEM_LOCAL      = 1 << 0,
   NVKMD_MEM_GART       = 1 << 1,
   NVKMD_MEM_CAN_MAP    = 1 << 2,
   NVKMD_MEM_NO_SHARE   = 1 << 3,
};

enum nvkmd_mem_map_flags {
   NVKMD_MEM_MAP_RD     = 1 << 0,
   NVKMD_MEM_MAP_WR     = 1 << 1,
   NVKMD_MEM_MAP_RDWR   = NVKMD_MEM_MAP_RD | NVKMD_MEM_MAP_WR,
   NVKMD_MEM_MAP_FIXED  = 1 << 2,
};

enum nvkmd_va_flags {
   /** This VA should be configured for sparse (soft faults) */
   NVKMD_VA_SPARSE = 1 << 0,

   /** This VA should come from the capture/replay pool */
   NVKMD_VA_REPLAY = 1 << 1,

   /** Attempt to place this VA at the requested address and fail otherwise */
   NVKMD_VA_ALLOC_FIXED = 1 << 2,
};

/*
 * Structs
 */

struct nvkmd_info {
   bool has_dma_buf;
   bool has_get_vram_used;
   bool has_alloc_tiled;
   bool has_map_fixed;
   bool has_overmap;
};

struct nvkmd_pdev_ops {
   void (*destroy)(struct nvkmd_pdev *pdev);

   uint64_t (*get_vram_used)(struct nvkmd_pdev *pdev);

   int (*get_drm_primary_fd)(struct nvkmd_pdev *pdev);

   VkResult (*create_dev)(struct nvkmd_pdev *pdev,
                          struct vk_object_base *log_obj,
                          struct nvkmd_dev **dev_out);
};

struct nvkmd_pdev {
   const struct nvkmd_pdev_ops *ops;

   struct nv_device_info dev_info;
   struct nvkmd_info kmd_info;

   struct {
      dev_t render_dev;
      dev_t primary_dev;
   } drm;

   const struct vk_sync_type *const *sync_types;
};

struct nvkmd_dev_ops {
   void (*destroy)(struct nvkmd_dev *dev);

   uint64_t (*get_gpu_timestamp)(struct nvkmd_dev *dev);

   int (*get_drm_fd)(struct nvkmd_dev *dev);

   VkResult (*alloc_mem)(struct nvkmd_dev *dev,
                         struct vk_object_base *log_obj,
                         uint64_t size_B, uint64_t align_B,
                         enum nvkmd_mem_flags flags,
                         struct nvkmd_mem **mem_out);

   VkResult (*alloc_tiled_mem)(struct nvkmd_dev *dev,
                               struct vk_object_base *log_obj,
                               uint64_t size_B, uint64_t align_B,
                               uint8_t pte_kind, uint16_t tile_mode,
                               enum nvkmd_mem_flags flags,
                               struct nvkmd_mem **mem_out);

   VkResult (*import_dma_buf)(struct nvkmd_dev *dev,
                              struct vk_object_base *log_obj,
                              int fd, struct nvkmd_mem **mem_out);

   VkResult (*alloc_va)(struct nvkmd_dev *dev,
                        struct vk_object_base *log_obj,
                        enum nvkmd_va_flags flags, uint8_t pte_kind,
                        uint64_t size_B, uint64_t align_B,
                        uint64_t fixed_addr, struct nvkmd_va **va_out);
};

struct nvkmd_dev {
   const struct nvkmd_dev_ops *ops;
};

struct nvkmd_mem_ops {
   void (*free)(struct nvkmd_mem *mem);

   VkResult (*map)(struct nvkmd_mem *mem,
                   struct vk_object_base *log_obj,
                   enum nvkmd_mem_map_flags flags,
                   void *fixed_addr);

   void (*unmap)(struct nvkmd_mem *mem);

   VkResult (*overmap)(struct nvkmd_mem *mem,
                       struct vk_object_base *log_obj);

   VkResult (*export_dma_buf)(struct nvkmd_mem *mem,
                              struct vk_object_base *log_obj,
                              int *fd_out);
};

struct nvkmd_mem {
   const struct nvkmd_mem_ops *ops;

   uint32_t refcnt;

   enum nvkmd_mem_flags flags;

   uint64_t size_B;
   const struct nvkmd_va *va;
   void *map;
};

struct nvkmd_va_ops {
   void (*free)(struct nvkmd_va *va);

   VkResult (*bind_mem)(struct nvkmd_va *va,
                        struct vk_object_base *log_obj,
                        uint64_t va_offset_B,
                        struct nvkmd_mem *mem,
                        uint64_t mem_offset_B,
                        uint64_t range_B);

   VkResult (*unbind)(struct nvkmd_va *va,
                      struct vk_object_base *log_obj,
                      uint64_t va_offset_B,
                      uint64_t range_B);
};

struct nvkmd_va {
   const struct nvkmd_va_ops *ops;
   enum nvkmd_va_flags flags;
   uint8_t pte_kind;
   uint64_t addr;
   uint64_t size_B;
};

/*
 * Macros
 *
 * All subclassed structs must be named nvkmd_<subcls>_<strct> where the
 * original struct is named nvkmd_<strct>
 */

#define NVKMD_DECL_SUBCLASS(strct, subcls)                                 \
   extern const struct nvkmd_##strct##_ops nvkmd_##subcls##_##strct##_ops; \
   static inline struct nvkmd_##subcls##_##strct *                         \
   nvkmd_##subcls##_##strct(struct nvkmd_##strct *nvkmd)                   \
   {                                                                       \
      assert(nvkmd->ops == &nvkmd_##subcls##_##strct##_ops);               \
      return container_of(nvkmd, struct nvkmd_##subcls##_##strct, base);   \
   }

/*
 * Methods
 *
 * Even though everything goes through a function pointer table, we always add
 * an inline wrapper in case we want to move something into "core" NVKMD.
 */

VkResult MUST_CHECK
nvkmd_try_create_pdev_for_drm(struct _drmDevice *drm_device,
                              struct vk_object_base *log_obj,
                              enum nvk_debug debug_flags,
                              struct nvkmd_pdev **pdev_out);

static inline void
nvkmd_pdev_destroy(struct nvkmd_pdev *pdev)
{
   pdev->ops->destroy(pdev);
}

static inline uint64_t
nvkmd_pdev_get_vram_used(struct nvkmd_pdev *pdev)
{
   return pdev->ops->get_vram_used(pdev);
}

static inline int
nvkmd_pdev_get_drm_primary_fd(struct nvkmd_pdev *pdev)
{
   if (pdev->ops->get_drm_primary_fd == NULL)
      return -1;

   return pdev->ops->get_drm_primary_fd(pdev);
}

static inline VkResult MUST_CHECK
nvkmd_pdev_create_dev(struct nvkmd_pdev *pdev,
                      struct vk_object_base *log_obj,
                      struct nvkmd_dev **dev_out)
{
   return pdev->ops->create_dev(pdev, log_obj, dev_out);
}

static inline void
nvkmd_dev_destroy(struct nvkmd_dev *dev)
{
   dev->ops->destroy(dev);
}

static inline uint64_t
nvkmd_dev_get_gpu_timestamp(struct nvkmd_dev *dev)
{
   return dev->ops->get_gpu_timestamp(dev);
}

static inline int
nvkmd_dev_get_drm_fd(struct nvkmd_dev *dev)
{
   if (dev->ops->get_drm_fd == NULL)
      return -1;

   return dev->ops->get_drm_fd(dev);
}

static inline VkResult MUST_CHECK
nvkmd_dev_alloc_mem(struct nvkmd_dev *dev,
                    struct vk_object_base *log_obj,
                    uint64_t size_B, uint64_t align_B,
                    enum nvkmd_mem_flags flags,
                    struct nvkmd_mem **mem_out)
{
   return dev->ops->alloc_mem(dev, log_obj, size_B, align_B, flags, mem_out);
}

static inline VkResult MUST_CHECK
nvkmd_dev_alloc_tiled_mem(struct nvkmd_dev *dev,
                          struct vk_object_base *log_obj,
                          uint64_t size_B, uint64_t align_B,
                          uint8_t pte_kind, uint16_t tile_mode,
                          enum nvkmd_mem_flags flags,
                          struct nvkmd_mem **mem_out)
{
   return dev->ops->alloc_tiled_mem(dev, log_obj, size_B, align_B,
                                    pte_kind, tile_mode, flags, mem_out);
}

/* Implies NVKMD_MEM_CAN_MAP */
VkResult MUST_CHECK
nvkmd_dev_alloc_mapped_mem(struct nvkmd_dev *dev,
                           struct vk_object_base *log_obj,
                           uint64_t size_B, uint64_t align_B,
                           enum nvkmd_mem_flags flags,
                           enum nvkmd_mem_map_flags map_flags,
                           struct nvkmd_mem **mem_out);

static inline VkResult MUST_CHECK
nvkmd_dev_import_dma_buf(struct nvkmd_dev *dev,
                         struct vk_object_base *log_obj,
                         int fd, struct nvkmd_mem **mem_out)
{
   return dev->ops->import_dma_buf(dev, log_obj, fd, mem_out);
}

static inline VkResult MUST_CHECK
nvkmd_dev_alloc_va(struct nvkmd_dev *dev,
                   struct vk_object_base *log_obj,
                   enum nvkmd_va_flags flags, uint8_t pte_kind,
                   uint64_t size_B, uint64_t align_B,
                   uint64_t fixed_addr, struct nvkmd_va **va_out)
{
   return dev->ops->alloc_va(dev, log_obj, flags, pte_kind, size_B, align_B,
                             fixed_addr, va_out);
}

static inline struct nvkmd_mem *
nvkmd_mem_ref(struct nvkmd_mem *mem)
{
   p_atomic_inc(&mem->refcnt);
   return mem;
}

void nvkmd_mem_unref(struct nvkmd_mem *mem);

static inline VkResult MUST_CHECK
nvkmd_mem_map(struct nvkmd_mem *mem, struct vk_object_base *log_obj,
              enum nvkmd_mem_map_flags flags, void *fixed_addr,
              void **map_out)
{
   assert(mem->map == NULL);

   VkResult result = mem->ops->map(mem, log_obj, flags, fixed_addr);
   if (result != VK_SUCCESS)
      return result;

   *map_out = mem->map;

   return VK_SUCCESS;
}

static inline void
nvkmd_mem_unmap(struct nvkmd_mem *mem)
{
   assert(mem->map != NULL);
   mem->ops->unmap(mem);
   assert(mem->map == NULL);
}

static inline VkResult MUST_CHECK
nvkmd_mem_overmap(struct nvkmd_mem *mem, struct vk_object_base *log_obj)
{
   assert(mem->map != NULL);
   VkResult result = mem->ops->overmap(mem, log_obj);
   assert(mem->map == NULL);
   return result;
}

static inline VkResult MUST_CHECK
nvkmd_mem_export_dma_buf(struct nvkmd_mem *mem,
                      struct vk_object_base *log_obj,
                      int *fd_out)
{
   assert(!(mem->flags & NVKMD_MEM_NO_SHARE));

   return mem->ops->export_dma_buf(mem, log_obj, fd_out);
}

static inline void
nvkmd_va_free(struct nvkmd_va *va)
{
   va->ops->free(va);
}

static inline VkResult MUST_CHECK
nvkmd_va_bind_mem(struct nvkmd_va *va,
                  struct vk_object_base *log_obj,
                  uint64_t va_offset_B,
                  struct nvkmd_mem *mem,
                  uint64_t mem_offset_B,
                  uint64_t range_B)
{
   assert(va_offset_B <= va->size_B);
   assert(va_offset_B + range_B <= va->size_B);
   assert(mem_offset_B <= mem->size_B);
   assert(mem_offset_B + range_B <= mem->size_B);

   return va->ops->bind_mem(va, log_obj, va_offset_B,
                            mem, mem_offset_B, range_B);
}

static inline VkResult MUST_CHECK
nvkmd_va_unbind(struct nvkmd_va *va,
                struct vk_object_base *log_obj,
                uint64_t va_offset_B,
                uint64_t range_B)
{
   assert(va_offset_B <= va->size_B);
   assert(va_offset_B + range_B <= va->size_B);

   return va->ops->unbind(va, log_obj, va_offset_B, range_B);
}

#endif /* NVKMD_H */