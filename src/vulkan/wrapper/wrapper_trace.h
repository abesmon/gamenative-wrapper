#ifndef WRAPPER_TRACE_H
#define WRAPPER_TRACE_H

/*
 * Structured tracing for the calls where the wrapper changes what the
 * application asked for.
 *
 * This is not another api_dump. That layer already exists behind
 * WRAPPER_DEBUG=apidump, prints every call, and is unusable on a running game.
 * What is missing when a title renders wrongly is narrower and needs to survive
 * a real workload: for the handful of calls the wrapper rewrites, what came in,
 * what went to the driver, and what came back.
 *
 * Enable with WRAPPER_DEBUG=trace. Each traced call emits up to three lines:
 *
 *   TRACE CreateImage request  format=BC1_RGBA_UNORM_BLOCK extent=256x256x1 ...
 *   TRACE CreateImage transform format=BC1_RGBA_UNORM_BLOCK->ASTC_4x4_UNORM_BLOCK reason=bcn_emulation
 *   TRACE CreateImage result   VK_SUCCESS
 *
 * The transform line appears only when something actually changed, so a grep
 * for it lists every rewrite a run performed.
 */

#include "wrapper_log.h"

#define WRAPPER_TRACE(fmt, ...) WRAPPER_LOG(trace, fmt, ##__VA_ARGS__)

#define WRAPPER_TRACE_ENABLED() (WRAPPER_LOG_LEVEL(trace) != 0)

static inline const char *
wrapper_trace_format_name(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
   case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
   case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
   case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
   case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
   case VK_FORMAT_R16G16B16_SFLOAT: return "R16G16B16_SFLOAT";
   case VK_FORMAT_R8_UNORM: return "R8_UNORM";
   case VK_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
   case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
   case VK_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return "BC1_RGB_UNORM_BLOCK";
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return "BC1_RGB_SRGB_BLOCK";
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return "BC1_RGBA_UNORM_BLOCK";
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return "BC1_RGBA_SRGB_BLOCK";
   case VK_FORMAT_BC2_UNORM_BLOCK: return "BC2_UNORM_BLOCK";
   case VK_FORMAT_BC2_SRGB_BLOCK: return "BC2_SRGB_BLOCK";
   case VK_FORMAT_BC3_UNORM_BLOCK: return "BC3_UNORM_BLOCK";
   case VK_FORMAT_BC3_SRGB_BLOCK: return "BC3_SRGB_BLOCK";
   case VK_FORMAT_BC4_UNORM_BLOCK: return "BC4_UNORM_BLOCK";
   case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5_UNORM_BLOCK";
   case VK_FORMAT_BC6H_UFLOAT_BLOCK: return "BC6H_UFLOAT_BLOCK";
   case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7_UNORM_BLOCK";
   case VK_FORMAT_BC7_SRGB_BLOCK: return "BC7_SRGB_BLOCK";
   case VK_FORMAT_ASTC_4x4_UNORM_BLOCK: return "ASTC_4x4_UNORM_BLOCK";
   case VK_FORMAT_ASTC_4x4_SRGB_BLOCK: return "ASTC_4x4_SRGB_BLOCK";
   case VK_FORMAT_ASTC_8x8_UNORM_BLOCK: return "ASTC_8x8_UNORM_BLOCK";
   case VK_FORMAT_ASTC_8x8_SRGB_BLOCK: return "ASTC_8x8_SRGB_BLOCK";
   case VK_FORMAT_UNDEFINED: return "UNDEFINED";
   default: return NULL;
   }
}

/* Named formats read at a glance; the rest are still identifiable by number,
 * which is what matters in a log nobody wants to widen a table for. */
static inline const char *
wrapper_trace_format(VkFormat format)
{
   static _Thread_local char fallback[24];
   const char *name = wrapper_trace_format_name(format);
   if (name)
      return name;
   snprintf(fallback, sizeof(fallback), "VkFormat(%d)", (int)format);
   return fallback;
}

static inline const char *
wrapper_trace_tiling(VkImageTiling tiling)
{
   switch (tiling) {
   case VK_IMAGE_TILING_OPTIMAL: return "OPTIMAL";
   case VK_IMAGE_TILING_LINEAR: return "LINEAR";
   case VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT: return "DRM_MODIFIER";
   default: return "?";
   }
}

#endif /* WRAPPER_TRACE_H */
