#include <math.h>
#include <fcntl.h>

#include "wrapper_private.h"
#include "wrapper_log.h"
#include "wrapper_trace.h"
#include "wrapper_entrypoints.h"
#include "wrapper_trampolines.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_dispatch_table.h"
#include "vk_extensions.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "wsi_common.h"
#include "util/os_misc.h"

static uint32_t
parse_vk_version_from_env()
{
   uint32_t apiVersion = 0, major = 0, minor = 0, patch = 0;

   const char *wrapper_vk_version = getenv("WRAPPER_VK_VERSION");

   if (wrapper_vk_version) {
      sscanf(wrapper_vk_version, "%d.%d.%d", &major, &minor, &patch);
      apiVersion = VK_MAKE_VERSION(major, minor, patch);
   }

   return apiVersion;
}

static char *
get_driver_version(const uint32_t driverVersion)
{
	char *driver_version;
	uint32_t major = 0, minor = 0, patch = 0;

	major = VK_API_VERSION_MAJOR(driverVersion);
	minor = VK_API_VERSION_MINOR(driverVersion);
	patch = VK_API_VERSION_PATCH(driverVersion);

	asprintf(&driver_version, "%d.%d.%d", major, minor, patch);

	return driver_version;
}

static VkResult
wrapper_setup_device_extensions(struct wrapper_physical_device *pdevice) {
   struct vk_device_extension_table *exts = &pdevice->vk.supported_extensions;
   VkExtensionProperties pdevice_extensions[VK_DEVICE_EXTENSION_COUNT];
   uint32_t pdevice_extension_count = VK_DEVICE_EXTENSION_COUNT;
   VkResult result;

   result = pdevice->dispatch_table.EnumerateDeviceExtensionProperties(
      pdevice->dispatch_handle, NULL, &pdevice_extension_count, pdevice_extensions);

   if (result != VK_SUCCESS)
      return result;

   *exts = wrapper_device_extensions;

   for (int i = 0; i < pdevice_extension_count; i++) {
      int idx;
      for (idx = 0; idx < VK_DEVICE_EXTENSION_COUNT; idx++) {
         if (strcmp(vk_device_extensions[idx].extensionName,
                     pdevice_extensions[i].extensionName) == 0)
            break;
      }

      if (idx >= VK_DEVICE_EXTENSION_COUNT)
         continue;

      if (wrapper_filter_extensions.extensions[idx])
         continue;

      pdevice->base_supported_extensions.extensions[idx] =
         exts->extensions[idx] = true;
   }

   exts->KHR_present_wait = exts->KHR_timeline_semaphore;

   return VK_SUCCESS;
}

static void
wrapper_apply_device_extension_blacklist(struct wrapper_physical_device *physical_device) {
   char *blacklist = getenv("WRAPPER_EXTENSION_BLACKLIST");
   if (!blacklist)
      return;
   char *extension = strtok(blacklist, ",");
   while (extension != NULL) {
      for (int i = 0; i < VK_DEVICE_EXTENSION_COUNT; i++) {
         if (strstr(extension, vk_device_extensions[i].extensionName)) {
            WRAPPER_LOG(info, "Blacklisting extension %s", extension);
            physical_device->vk.supported_extensions.extensions[i] = false;
         }
      }
      extension = strtok(NULL, ",");
   }
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wrapper_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdevice->instance, pName);
}

VkResult enumerate_physical_device(struct vk_instance *_instance)
{
   struct wrapper_instance *instance = (struct wrapper_instance *)_instance;
   VkPhysicalDevice *physical_devices;
   uint32_t physical_device_count;
   static int wrapper_disable_placed = -1;
   static int wrapper_dmaheap_cached = -1;
   static int wrapper_disable_present_wait = -1;
   int wrapper_emulate_bcn;
   VkResult result;

   result = instance->dispatch_table.EnumeratePhysicalDevices(
      instance->dispatch_handle, &physical_device_count, NULL);

   if (result != VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to retrieve physical devices count, res %d", result);
      return result;
   }

   physical_devices = malloc(physical_device_count * sizeof(VkPhysicalDevice));

   result = instance->dispatch_table.EnumeratePhysicalDevices(
      instance->dispatch_handle, &physical_device_count, physical_devices);
      
   if (result!= VK_SUCCESS) {
      WRAPPER_LOG(error, "Failed to enumerate physical devices, res %d", result);	
      return result;
   }
   
   for (int i = 0; i < physical_device_count; i++) {
      PFN_vkGetInstanceProcAddr get_instance_proc_addr;
      struct wrapper_physical_device *pdevice;

      pdevice = vk_zalloc(&_instance->alloc, sizeof(*pdevice), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
      if (!pdevice)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      struct vk_physical_device_dispatch_table dispatch_table;
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wrapper_physical_device_entrypoints, true);
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wsi_physical_device_entrypoints, false);
      vk_physical_device_dispatch_table_from_entrypoints(
         &dispatch_table, &wrapper_physical_device_trampolines, false);

      result = vk_physical_device_init(&pdevice->vk,
                                       &instance->vk,
                                       NULL, NULL, NULL,
                                       &dispatch_table);
      if (result != VK_SUCCESS) {
         vk_free(&_instance->alloc, pdevice);
         return result;
      }

      pdevice->instance = instance;
      pdevice->dispatch_handle = physical_devices[i];
      get_instance_proc_addr = instance->dispatch_table.GetInstanceProcAddr;

      vk_physical_device_dispatch_table_load(&pdevice->dispatch_table,
                                             get_instance_proc_addr,
                                             instance->dispatch_handle);

	  if (wrapper_disable_placed == -1)
          wrapper_disable_placed = getenv("WRAPPER_DISABLE_PLACED") ? atoi(getenv("WRAPPER_DISABLE_PLACED")) : 0;

      if (wrapper_disable_present_wait == -1)
         wrapper_disable_present_wait = getenv("WRAPPER_DISABLE_PRESENT_WAIT") && atoi(getenv("WRAPPER_DISABLE_PRESENT_WAIT"));

      wrapper_setup_device_extensions(pdevice);
      wrapper_apply_device_extension_blacklist(pdevice);
      wrapper_setup_device_features(pdevice);

      struct vk_features *supported_features = &pdevice->vk.supported_features;
      pdevice->base_supported_features = *supported_features;
      supported_features->presentId = true;
      supported_features->multiViewport = true;
      supported_features->depthClamp = true;
      supported_features->depthBiasClamp = true;
      if (!wrapper_disable_placed) {
        pdevice->vk.supported_extensions.EXT_map_memory_placed = true;
        pdevice->vk.supported_extensions.KHR_map_memory2 = true;
      	supported_features->memoryMapPlaced = true;
      	supported_features->memoryUnmapReserve = true;
      } else {
        WRAPPER_LOG(info, "Disabling VK_EXT_map_memory_placed");
      	pdevice->vk.supported_extensions.EXT_map_memory_placed = false;
        pdevice->vk.supported_extensions.KHR_map_memory2 = false;
      	supported_features->memoryMapPlaced = false;
      	supported_features->memoryUnmapReserve = false;
      }
      supported_features->textureCompressionBC = true;
      supported_features->fillModeNonSolid = true;
      supported_features->shaderClipDistance = true;
      supported_features->shaderCullDistance = true;

      /* DXVK's D3D11 path requires VK_EXT_memory_priority unconditionally, at
       * every feature level, so a driver without it gets no D3D11 device at
       * all -- which is what Tegra X1 hits.  The extension is a pure allocator
       * hint: VkMemoryPriorityAllocateInfoEXT only says which allocations to
       * evict first under pressure, and an implementation is free to ignore
       * the value.  Advertise it and drop the hint.
       *
       * Nothing leaks down to the base driver: the extension name is filtered
       * out by wrapper_filter_enabled_extensions (it is absent from
       * base_supported_extensions) and the feature struct is unlinked from the
       * device pNext chain.  VkMemoryPriorityAllocateInfoEXT is left alone in
       * vkAllocateMemory on purpose -- the spec requires every component to
       * skip extending structures it does not know, so rebuilding the chain on
       * a hot path would buy nothing. */
      if (!pdevice->base_supported_extensions.EXT_memory_priority) {
         WRAPPER_LOG(info, "Faking VK_EXT_memory_priority");
         pdevice->vk.supported_extensions.EXT_memory_priority = true;
         supported_features->memoryPriority = true;
      }

      /* Likewise VK_EXT_host_query_reset, required by DXVK from feature level
       * 9_1 up.  Unlike memory priority this one has real behaviour, so it is
       * emulated rather than ignored: wrapper_ResetQueryPool records
       * vkCmdResetQueryPool on an internal command buffer and waits for it. */
      if (!pdevice->base_supported_extensions.EXT_host_query_reset) {
         WRAPPER_LOG(info, "Emulating VK_EXT_host_query_reset");
         pdevice->vk.supported_extensions.EXT_host_query_reset = true;
         supported_features->hostQueryReset = true;
      }
      if (wrapper_disable_present_wait) {
         WRAPPER_LOG(info, "Disabling present wait");
         supported_features->presentWait = false;
         pdevice->vk.supported_extensions.KHR_present_wait = false;
      } else {
         supported_features->presentWait = supported_features->timelineSemaphore;
      }
      supported_features->swapchainMaintenance1 = true;
      supported_features->imageCompressionControlSwapchain = false;
      
      result = wsi_device_init(&pdevice->wsi_device,
                               wrapper_physical_device_to_handle(pdevice),
                               wrapper_wsi_proc_addr, &_instance->alloc, -1,
                               NULL, &(struct wsi_device_options){});
      if (result != VK_SUCCESS) {
         vk_physical_device_finish(&pdevice->vk);
         vk_free(&_instance->alloc, pdevice);
         return result;
      }
      pdevice->vk.wsi_device = &pdevice->wsi_device;
      pdevice->wsi_device.force_bgra8_unorm_first = true;

      pdevice->driver_properties = (VkPhysicalDeviceDriverProperties) {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
      };
      pdevice->properties2 = (VkPhysicalDeviceProperties2) {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
         .pNext = &pdevice->driver_properties,
      };
      pdevice->dispatch_table.GetPhysicalDeviceProperties2(
         pdevice->dispatch_handle, &pdevice->properties2);
         
      pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties(
         pdevice->dispatch_handle, &pdevice->memory_properties);

      /* NVIDIA's Android/Tegra ICD exposes neither EXT_memory_budget nor a
       * useful pressure signal to DXVK. Other mobile drivers therefore enter
       * DXVK's eviction path while Tegra keeps allocating unified system
       * memory until Android kills the process. Keep this workaround opt-in
       * while it is being validated on the Switch-specific wrapper+ICD pair. */
      const char *nvidia_budget = getenv("WRAPPER_NVIDIA_MEMORY_BUDGET");
      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY &&
          !pdevice->base_supported_extensions.EXT_memory_budget &&
          nvidia_budget && atoi(nvidia_budget) > 0) {
         pdevice->nvidia_memory_budget_enabled = true;
         pdevice->nvidia_memory_budget_bytes =
            (uint64_t)atoi(nvidia_budget) * 1048576ull;
         pdevice->vk.supported_extensions.EXT_memory_budget = true;
         WRAPPER_LOG(info, "Faking VK_EXT_memory_budget for NVIDIA: %s MiB",
                     nvidia_budget);
      }

      /* The Android Tegra ICD exposes a device-local heap backed by unified
       * system RAM but has no memory-budget feedback. Keep this experimental
       * admission limit NVIDIA-only: it makes allocation pressure observable
       * to DXVK before Android's LMK has to kill the whole foreground app. */
      const char *nvidia_limit = getenv("WRAPPER_NVIDIA_MEMORY_LIMIT");
      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY &&
          nvidia_limit && atoi(nvidia_limit) > 0) {
         pdevice->nvidia_memory_limit_enabled = true;
         pdevice->nvidia_memory_limit_bytes =
            (uint64_t)atoi(nvidia_limit) * 1048576ull;
         WRAPPER_LOG(info, "NVIDIA memory admission limit: %s MiB",
                     nvidia_limit);
      }

      const char *sanitize_priority =
         getenv("WRAPPER_NVIDIA_SANITIZE_MEMORY_PRIORITY");
      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY &&
          !pdevice->base_supported_extensions.EXT_memory_priority &&
          sanitize_priority && atoi(sanitize_priority) != 0) {
         pdevice->nvidia_sanitize_memory_priority = true;
         WRAPPER_LOG(info,
            "Sanitizing spoofed memory-priority allocation hints for NVIDIA");
      }

      /* Tegra reports device-only and persistently mapped host-visible memory
       * types in one large unified heap. DXVK therefore sizes both classes as
       * if they had the whole heap available, although Android, Wine and the
       * GPU compete for the same 4 GiB. An opt-in synthetic host heap changes
       * allocator policy only; memory type indices passed to the ICD remain
       * untouched. */
      const char *host_heap = getenv("WRAPPER_NVIDIA_HOST_HEAP");
      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY &&
          host_heap && atoi(host_heap) > 0) {
         pdevice->nvidia_host_heap_enabled = true;
         pdevice->nvidia_host_heap_bytes =
            (uint64_t)atoi(host_heap) * 1048576ull;
         WRAPPER_LOG(info, "NVIDIA synthetic host-visible heap: %s MiB",
                     host_heap);
      }

     WRAPPER_LOG(info, "GPU Name: %s", pdevice->properties2.properties.deviceName);
     WRAPPER_LOG(info, "Driver Version: %s", get_driver_version(pdevice->properties2.properties.driverVersion));

      const char *app_name = instance->vk.app_info.app_name
         ? instance->vk.app_info.app_name : "wrapper";

      const char *engine_name = instance->vk.app_info.engine_name
         ? instance->vk.app_info.engine_name : "wrapper";
      pdevice->wsi_device.engine_name = engine_name;

      const uint32_t engine_version = instance->vk.app_info.engine_version;
      const uint32_t driver_version = pdevice->properties2.properties.driverVersion;

      /* HACK: Specific prop drivers workarounds for Adreno and Mali GPUs */
      
      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
         if (strstr(engine_name, "DXVK")) {
            WRAPPER_LOG(info, "Disabling VK_EXT_line_rasterization");
            pdevice->vk.supported_extensions.EXT_line_rasterization = false;	
            if (engine_version >= VK_MAKE_VERSION(2, 7, 0)) {
               WRAPPER_LOG(info, "Faking VK_KHR_pipeline_library");
               pdevice->vk.supported_extensions.KHR_pipeline_library = true;
            }
         }
         
         if (driver_version > VK_MAKE_VERSION(512, 744, 0) &&
             strstr(app_name, "clvk")) {
            WRAPPER_LOG(info, "Disabling globalPriorityQueue feature");
            supported_features->globalPriorityQuery = false;    
         }

         WRAPPER_LOG(info, "Disabling VK_KHR_shader_float_controls");
         pdevice->vk.supported_extensions.KHR_shader_float_controls = false;
      }

      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_ARM_PROPRIETARY) {
         bool is_dxvk = strstr(engine_name, "DXVK");
         bool is_vkd3d = strstr(engine_name, "vkd3d");
         bool is_d3d = is_dxvk || is_vkd3d;
         pdevice->is_vkd3d = is_vkd3d;
         if (is_d3d) {
            WRAPPER_LOG(info, "Faking VK_EXT_robustness2 for engine '%s'", engine_name);
            pdevice->vk.supported_extensions.EXT_robustness2 = true;
            supported_features->robustBufferAccess2 = true;
            supported_features->nullDescriptor = true;
            if (is_dxvk && engine_version >= VK_MAKE_VERSION(2, 7, 0)) {
               WRAPPER_LOG(info, "Faking VK_KHR_pipeline_library");
               pdevice->vk.supported_extensions.KHR_pipeline_library = true;
            }
            supported_features->extendedDynamicState = true;
            supported_features->extendedDynamicState2 = true;
            supported_features->dualSrcBlend = true;
            supported_features->multiDrawIndirect = true;
         }
         /* Spoof both vertex_attribute_divisor extensions as supported. Mali r51
          * exposes KHR (we forward that); r44 exposes neither, so we advertise
          * it purely to get vkd3d past its requirement check. Advertising BOTH
          * (rather than hiding KHR) is fine -- the device-side filter below
          * forwards whichever alias the base driver actually has, and
          * process_pnext_chain aliases the feature struct EXT<->KHR to match. */
         WRAPPER_LOG(info, "Spoofing VK_EXT/KHR_vertex_attribute_divisor as supported");
         pdevice->vk.supported_extensions.EXT_vertex_attribute_divisor = true;
         pdevice->vk.supported_extensions.KHR_vertex_attribute_divisor = true;
         WRAPPER_LOG(info, "Disabling VK_EXT_calibrated_timestamps");
         pdevice->vk.supported_extensions.EXT_calibrated_timestamps = false;
         if (!is_d3d) {
            WRAPPER_LOG(info, "Disabling VK_EXT_extended_dynamic_state and VK_EXT_extended_dynamic_state2");
            pdevice->vk.supported_extensions.EXT_extended_dynamic_state = false;
            pdevice->vk.supported_extensions.EXT_extended_dynamic_state2 = false;
         }
      }

      /* Samsung Xclipse: vkd3d 3.x needs VK_EXT_dynamic_rendering_unused_attachments
       * for correct rendering (draws with unused/mismatched render targets get
       * dropped -> objects vanish, e.g. the player character), but Xclipse doesn't
       * expose it. Advertise it -- it is a validation relaxation the RDNA driver
       * tolerates; the feature is faked in Features2 and the struct is unlinked from
       * the real CreateDevice pNext. */
      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY) {
         bool samsung_d3d = strstr(engine_name, "DXVK") || strstr(engine_name, "vkd3d");
         pdevice->is_vkd3d = strstr(engine_name, "vkd3d") != NULL;
         if (samsung_d3d &&
             !pdevice->base_supported_extensions.EXT_dynamic_rendering_unused_attachments) {
            WRAPPER_LOG(info, "Faking VK_EXT_dynamic_rendering_unused_attachments for Xclipse");
            pdevice->vk.supported_extensions.EXT_dynamic_rendering_unused_attachments = true;
         }
      }

      /* DXVK > 2.4.1 requires VK_KHR_maintenance5. Some drivers (e.g. Xclipse)
       * don't expose it. Emulate it for any D3D client whose base driver lacks
       * it (NOT Samsung-gated -- the base-lacks guard limits it to drivers that
       * actually need it). Feature faked in Features2, struct unlinked from the
       * real CreateDevice, entrypoints implemented in wrapper_device.c. */
      {
         bool is_d3d = strstr(engine_name, "DXVK") || strstr(engine_name, "vkd3d");
         if (is_d3d && !pdevice->base_supported_extensions.KHR_maintenance5) {
            WRAPPER_LOG(info, "Faking VK_KHR_maintenance5 (base driver lacks it)");
            pdevice->vk.supported_extensions.KHR_maintenance5 = true;
         }
      }

      /* VK_KHR_push_descriptor: vkd3d/DXVK effectively require it, but some
       * drivers (e.g. Mali r44) don't expose it. Advertise + emulate it for D3D
       * clients whose base driver lacks it. maxPushDescriptors is reported in
       * wrapper_GetPhysicalDeviceProperties2; the entrypoints are emulated in
       * wrapper_device.c. */
      {
         bool is_d3d = strstr(engine_name, "DXVK") || strstr(engine_name, "vkd3d");
         if (is_d3d && !pdevice->base_supported_extensions.KHR_push_descriptor) {
            WRAPPER_LOG(info, "Faking VK_KHR_push_descriptor (base driver lacks it)");
            pdevice->vk.supported_extensions.KHR_push_descriptor = true;
         }
      }

      char *wrapper_emulate_bcn_env = getenv("WRAPPER_EMULATE_BCN");

      if (!wrapper_emulate_bcn_env) 
         wrapper_emulate_bcn = 3;
      else 
         wrapper_emulate_bcn = atoi(wrapper_emulate_bcn_env);

      if (wrapper_emulate_bcn >= 3)  {
         if (pdevice->driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY &&
             driver_version >= VK_MAKE_VERSION(512, 530, 0)) {
            wrapper_emulate_bcn = (pdevice->base_supported_features.textureCompressionBC) ?
               0 : 1;
         } else if (pdevice->driver_properties.driverID == VK_DRIVER_ID_MESA_TURNIP) {
            wrapper_emulate_bcn = 0;
         } else if (pdevice->base_supported_features.textureCompressionBC) {
            /* A driver that supports BC natively needs no emulation, whoever
             * wrote it. Before this, only Qualcomm and Turnip were asked; every
             * other driver transcoded BC to ASTC even when it could sample BC
             * directly.
             *
             * On Tegra (driverID NVIDIA_PROPRIETARY, textureCompressionBC true)
             * that cost a compute transcode per upload and doubled the memory
             * of every BC1 image, since BC1 is 4 bpp and ASTC 4x4 is 8: a
             * 256x256 BC1 image needed 32 KiB direct and 64 KiB wrapped.
             * Measured in docs/investigations/tegra-probe-lab-2026-08-12.md.
             */
            wrapper_emulate_bcn = 0;
         }
      }

      pdevice->emulate_bcn = wrapper_emulate_bcn;

      if (wrapper_dmaheap_cached == -1)
         wrapper_dmaheap_cached = getenv("WRAPPER_DMAHEAP_CACHED") && atoi(getenv("WRAPPER_DMAHEAP_CACHED"));

      if (wrapper_dmaheap_cached)
         pdevice->dma_heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
      else
         pdevice->dma_heap_fd = open("/dev/dma_heap/system-uncached", O_RDONLY | O_CLOEXEC);
         
      if (pdevice->dma_heap_fd < 0) {
         WRAPPER_LOG("info", "dmabuf heap not found, falling back to /dev/ion");
         pdevice->dma_heap_fd = open("/dev/ion", O_RDONLY);
      }

      char *wrapper_resource_type = getenv("WRAPPER_RESOURCE_TYPE");

      pdevice->resource_type = wrapper_resource_type ? wrapper_resource_type : "auto";

      list_addtail(&pdevice->vk.link, &_instance->physical_devices.list);
   }

   return VK_SUCCESS;
}

void destroy_physical_device(struct vk_physical_device *pdevice) {
   VK_FROM_HANDLE(wrapper_physical_device, wpdevice,
                  vk_physical_device_to_handle(pdevice));
   if (wpdevice->dma_heap_fd != -1)
      close(wpdevice->dma_heap_fd);
   wsi_device_finish(pdevice->wsi_device, &pdevice->instance->alloc);
   vk_physical_device_finish(pdevice);
   vk_free(&pdevice->instance->alloc, pdevice);
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                           const char* pLayerName,
                                           uint32_t* pPropertyCount,
                                           VkExtensionProperties* pProperties)
{
   return vk_common_EnumerateDeviceExtensionProperties(physicalDevice,
                                                       pLayerName,
                                                       pPropertyCount,
                                                       pProperties);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                  VkPhysicalDeviceFeatures* pFeatures) 
{
   return vk_common_GetPhysicalDeviceFeatures(physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                   VkPhysicalDeviceFeatures2* pFeatures) {
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   vk_common_GetPhysicalDeviceFeatures2(physicalDevice, pFeatures);

   if (pdevice->driver_properties.driverID == VK_DRIVER_ID_ARM_PROPRIETARY &&
       pdevice->vk.supported_extensions.EXT_robustness2) {
      vk_foreach_struct(s, pFeatures->pNext) {
         if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            VkPhysicalDeviceRobustness2FeaturesEXT *r2 =
               (VkPhysicalDeviceRobustness2FeaturesEXT *)s;
            r2->robustBufferAccess2 = VK_TRUE;
            r2->nullDescriptor = VK_TRUE;
            if (pdevice->is_vkd3d)
               r2->robustImageAccess2 = VK_TRUE;
         }
         if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT) {
            ((VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *)s)->extendedDynamicState = VK_TRUE;
         }
         if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT) {
            ((VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *)s)->extendedDynamicState2 = VK_TRUE;
         }
         if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT) {
            VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *vad =
               (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *)s;
            vad->vertexAttributeInstanceRateDivisor = VK_TRUE;
            vad->vertexAttributeInstanceRateZeroDivisor = VK_TRUE;
         }
      }
   }

   if (pdevice->driver_properties.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY) {
      vk_foreach_struct(s, pFeatures->pNext) {
         if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT &&
             pdevice->vk.supported_extensions.EXT_dynamic_rendering_unused_attachments)
            ((VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT *)s)
               ->dynamicRenderingUnusedAttachments = VK_TRUE;
         if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES &&
             pdevice->vk.supported_extensions.KHR_maintenance5)
            ((VkPhysicalDeviceMaintenance5Features *)s)->maintenance5 = VK_TRUE;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                    VkPhysicalDeviceProperties *pProperties)
{
   char *device_name;
   uint32_t device_id;
   uint32_t vendor_id;
   
   uint32_t api_version = parse_vk_version_from_env();
   
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceProperties(
      pdevice->dispatch_handle, pProperties);

   char *device_name_env = getenv("WRAPPER_DEVICE_NAME");
   asprintf(&device_name, "Wrapper(%s)", (device_name_env) ? device_name_env : pProperties->deviceName);
   strcpy(pProperties->deviceName, device_name);

   device_id = getenv("WRAPPER_DEVICE_ID") ? atoi(getenv("WRAPPER_DEVICE_ID")) : 0;
   vendor_id = getenv("WRAPPER_VENDOR_ID") ? atoi(getenv("WRAPPER_VENDOR_ID")) : 0;

   if (device_id > 0)
      pProperties->deviceID = device_id;

   if (vendor_id > 0)
      pProperties->vendorID = vendor_id;

   if (api_version > 0)
      pProperties->apiVersion = api_version;

   /* See wrapper_GetPhysicalDeviceProperties2: bump push constant size to the
    * DXVK-required minimum on Xclipse. */
   if (pdevice->driver_properties.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY &&
       pProperties->limits.maxPushConstantsSize < 256)
      pProperties->limits.maxPushConstantsSize = 256;
}

/* vkd3d's bindless model needs large UpdateAfterBind descriptor limits (D3D12
 * shader-visible heaps hold up to ~1M descriptors). Some drivers (e.g. Mali
 * r44) report only ~500k, so vkd3d aborts at bindless_state_init with
 * "Insufficient descriptor indexing support". Spoof the limits up to the D3D12
 * heap size for D3D clients. NOTE: unlike a feature toggle this is a REAL
 * hardware limit -- if the game actually binds more descriptors than the driver
 * can back, it will fault later (VK_EXT_device_fault will report where) rather
 * than being cleanly rejected here. Only bumps values that are below target. */
#define WRAPPER_D3D_BINDLESS_LIMIT (1u << 20)   /* 1048576 */
#define BUMP_UAB(p, t) do { \
   uint32_t _t = (t); \
   if ((p)->maxUpdateAfterBindDescriptorsInAllPools < _t)           (p)->maxUpdateAfterBindDescriptorsInAllPools = _t; \
   if ((p)->maxPerStageDescriptorUpdateAfterBindSamplers < _t)      (p)->maxPerStageDescriptorUpdateAfterBindSamplers = _t; \
   if ((p)->maxPerStageDescriptorUpdateAfterBindUniformBuffers < _t)(p)->maxPerStageDescriptorUpdateAfterBindUniformBuffers = _t; \
   if ((p)->maxPerStageDescriptorUpdateAfterBindStorageBuffers < _t)(p)->maxPerStageDescriptorUpdateAfterBindStorageBuffers = _t; \
   if ((p)->maxPerStageDescriptorUpdateAfterBindSampledImages < _t) (p)->maxPerStageDescriptorUpdateAfterBindSampledImages = _t; \
   if ((p)->maxPerStageDescriptorUpdateAfterBindStorageImages < _t) (p)->maxPerStageDescriptorUpdateAfterBindStorageImages = _t; \
   if ((p)->maxPerStageUpdateAfterBindResources < _t)              (p)->maxPerStageUpdateAfterBindResources = _t; \
   if ((p)->maxDescriptorSetUpdateAfterBindSamplers < _t)          (p)->maxDescriptorSetUpdateAfterBindSamplers = _t; \
   if ((p)->maxDescriptorSetUpdateAfterBindUniformBuffers < _t)    (p)->maxDescriptorSetUpdateAfterBindUniformBuffers = _t; \
   if ((p)->maxDescriptorSetUpdateAfterBindStorageBuffers < _t)    (p)->maxDescriptorSetUpdateAfterBindStorageBuffers = _t; \
   if ((p)->maxDescriptorSetUpdateAfterBindSampledImages < _t)     (p)->maxDescriptorSetUpdateAfterBindSampledImages = _t; \
   if ((p)->maxDescriptorSetUpdateAfterBindStorageImages < _t)     (p)->maxDescriptorSetUpdateAfterBindStorageImages = _t; \
} while (0)

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                     VkPhysicalDeviceProperties2* pProperties)
{
   uint32_t device_id;
   uint32_t vendor_id;
   char *device_name;
   char *driver_info;
   uint32_t driver_id;

   uint32_t api_version = parse_vk_version_from_env();

   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
   pdevice->dispatch_table.GetPhysicalDeviceProperties2(
      pdevice->dispatch_handle, pProperties);

   const char *eng = pdevice->instance->vk.app_info.engine_name;
   bool is_d3d = eng && (strstr(eng, "DXVK") || strstr(eng, "vkd3d"));

   char *device_name_env = getenv("WRAPPER_DEVICE_NAME");
   asprintf(&device_name, "Wrapper(%s)", (device_name_env) ? device_name_env : pProperties->properties.deviceName);   
   strcpy(pProperties->properties.deviceName, device_name);

   device_id = getenv("WRAPPER_DEVICE_ID") ? atoi(getenv("WRAPPER_DEVICE_ID")) : 0;
   vendor_id = getenv("WRAPPER_VENDOR_ID") ? atoi(getenv("WRAPPER_VENDOR_ID")) : 0;
   
   if (device_id > 0)
      pProperties->properties.deviceID = device_id;
   
   if (vendor_id > 0)
      pProperties->properties.vendorID = vendor_id;

   if (api_version > 0)
      pProperties->properties.apiVersion = api_version;

   /* DXVK 2.6+ requires maxPushConstantsSize >= 256 (its unified "push data"
    * model) and skips the adapter otherwise. Xclipse reports less; the RDNA
    * hardware underneath handles 256 (desktop AMD reports it), Samsung's driver
    * just reports conservatively -- bump it so DXVK proceeds. NOTE: unlike the
    * extension fakes, this is a real driver limit; if the driver enforces it in
    * vkCreatePipelineLayout this will surface as pipeline-creation failures. */
   if (pdevice->driver_properties.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY &&
       pProperties->properties.limits.maxPushConstantsSize < 256)
      pProperties->properties.limits.maxPushConstantsSize = 256;

   vk_foreach_struct(prop, pProperties->pNext) {
      switch (prop->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT:
      {
         VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR khr_vad = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR,
         };
         VkPhysicalDeviceProperties2 p2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &khr_vad,
         };
         pdevice->dispatch_table.GetPhysicalDeviceProperties2(
            pdevice->dispatch_handle, &p2);
         ((VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *)prop)->maxVertexAttribDivisor =
            khr_vad.maxVertexAttribDivisor;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT:
      {
         VkPhysicalDeviceMapMemoryPlacedPropertiesEXT *placed_prop =
               (VkPhysicalDeviceMapMemoryPlacedPropertiesEXT *)prop;
         uint64_t os_page_size;
         os_get_page_size(&os_page_size);
         placed_prop->minPlacedMemoryMapAlignment = os_page_size;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES_EXT:
      {
      	 VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT *texel_prop =
      	      (VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT *)prop;
      	 texel_prop->storageTexelBufferOffsetAlignmentBytes = 1;
      	 texel_prop->uniformTexelBufferOffsetAlignmentBytes = 1;
      	 break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES:
      {
         /* Only override when we're faking the extension; if the base driver has
          * it natively it already filled a real limit. */
         if (pdevice->vk.supported_extensions.KHR_push_descriptor &&
             !pdevice->base_supported_extensions.KHR_push_descriptor)
            ((VkPhysicalDevicePushDescriptorProperties *)prop)->maxPushDescriptors =
               WRAPPER_MAX_PUSH_DESCRIPTORS;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES_KHR:
      {
         if (pdevice->driver_properties.driverID != VK_DRIVER_ID_QUALCOMM_PROPRIETARY)
            break;
            
         VkPhysicalDeviceFloatControlsPropertiesKHR *float_prop =
              (VkPhysicalDeviceFloatControlsPropertiesKHR *)prop;
         
         float_prop->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
         float_prop->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;     
         float_prop->shaderDenormFlushToZeroFloat16 = false;
         float_prop->shaderDenormFlushToZeroFloat32 = false;
         float_prop->shaderRoundingModeRTEFloat16 = false;
         float_prop->shaderRoundingModeRTEFloat32 = false;
         float_prop->shaderSignedZeroInfNanPreserveFloat16 = false;
         float_prop->shaderSignedZeroInfNanPreserveFloat32 = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
      {
         VkPhysicalDeviceVulkan11Properties *vk11_prop =
              (VkPhysicalDeviceVulkan11Properties *)prop;
         vk11_prop->subgroupSupportedOperations = 0;
         vk11_prop->subgroupSupportedStages = 0;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
      {
         VkPhysicalDeviceVulkan12Properties *vk12_prop =
              (VkPhysicalDeviceVulkan12Properties *)prop;

         if (pdevice->driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
            vk12_prop->denormBehaviorIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
            vk12_prop->roundingModeIndependence = VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
            vk12_prop->shaderDenormFlushToZeroFloat16 = false;
            vk12_prop->shaderDenormFlushToZeroFloat32 = false;
            vk12_prop->shaderRoundingModeRTEFloat16 = false;
            vk12_prop->shaderRoundingModeRTEFloat32 = false;
            vk12_prop->shaderSignedZeroInfNanPreserveFloat16 = false;
            vk12_prop->shaderSignedZeroInfNanPreserveFloat32 = false;
         }

         driver_id = getenv("WRAPPER_DRIVER_ID") ? atoi(getenv("WRAPPER_DRIVER_ID")) : 0;

         if (driver_id > 0)
         	vk12_prop->driverID = driver_id;
         
         asprintf(&driver_info, "%d.%d.%d", 
            VK_VERSION_MAJOR(pProperties->properties.driverVersion),
            VK_VERSION_MINOR(pProperties->properties.driverVersion),
            VK_VERSION_PATCH(pProperties->properties.driverVersion));
         
         strcpy(vk12_prop->driverInfo, driver_info);
         strcpy(vk12_prop->driverName, "Wrapper driver");

         /* Vulkan12Properties folds in the descriptor-indexing limits. */
         if (is_d3d)
            BUMP_UAB(vk12_prop, WRAPPER_D3D_BINDLESS_LIMIT);
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:
      {
         if (is_d3d)
            BUMP_UAB((VkPhysicalDeviceDescriptorIndexingProperties *)prop,
                     WRAPPER_D3D_BINDLESS_LIMIT);
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES:
      {
         VkPhysicalDeviceVulkan13Properties *vk13_prop =
              (VkPhysicalDeviceVulkan13Properties *)prop;
         vk13_prop->storageTexelBufferOffsetAlignmentBytes = 1;
         vk13_prop->uniformTexelBufferOffsetAlignmentBytes = 1;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES:
      {
         VkPhysicalDeviceSubgroupProperties *subgroup_prop =
              (VkPhysicalDeviceSubgroupProperties *)prop;
         subgroup_prop->supportedOperations = 0;
         subgroup_prop->supportedStages = 0;
         break;
      }
      default:
         break;
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice,
	                                           VkFormat format,
	                                           VkImageType type,
	                                           VkImageTiling tiling,
	                                           VkImageUsageFlags usage,
	                                           VkImageCreateFlags flags,
	                                           VkImageFormatProperties *pImageFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
  
   switch(format) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK:                                    
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
   case VK_FORMAT_BC2_UNORM_BLOCK:
   case VK_FORMAT_BC2_SRGB_BLOCK:
   case VK_FORMAT_BC3_UNORM_BLOCK:
   case VK_FORMAT_BC3_SRGB_BLOCK:
   case VK_FORMAT_BC4_UNORM_BLOCK:
   case VK_FORMAT_BC4_SNORM_BLOCK:
   case VK_FORMAT_BC5_UNORM_BLOCK:
   case VK_FORMAT_BC5_SNORM_BLOCK:
   case VK_FORMAT_BC6H_UFLOAT_BLOCK:
   case VK_FORMAT_BC6H_SFLOAT_BLOCK:
   case VK_FORMAT_BC7_UNORM_BLOCK:
   case VK_FORMAT_BC7_SRGB_BLOCK:
      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY &&
          format <= 138 && pdevice->emulate_bcn == 3)
         break;
             
      if (pdevice->emulate_bcn < 1)
         break;
      
      if (type & VK_IMAGE_TYPE_1D) {
         pImageFormatProperties->maxExtent.width = pdevice->properties2.properties.limits.maxImageDimension1D;
         pImageFormatProperties->maxExtent.height = 1;
         pImageFormatProperties->maxExtent.depth = 1;
      }
      if (type & VK_IMAGE_TYPE_2D) {
         if (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) {
            pImageFormatProperties->maxExtent.width = pdevice->properties2.properties.limits.maxImageDimensionCube;
            pImageFormatProperties->maxExtent.height = pdevice->properties2.properties.limits.maxImageDimensionCube;
         }
         else {
            pImageFormatProperties->maxExtent.width = pdevice->properties2.properties.limits.maxImageDimension2D;
            pImageFormatProperties->maxExtent.height = pdevice->properties2.properties.limits.maxImageDimension2D;
         }
         pImageFormatProperties->maxExtent.depth = 1;
      }
      if (type & VK_IMAGE_TYPE_3D) {
         pImageFormatProperties->maxExtent.width = pdevice->properties2.properties.limits.maxImageDimension3D;
         pImageFormatProperties->maxExtent.height = pdevice->properties2.properties.limits.maxImageDimension3D;
         pImageFormatProperties->maxExtent.depth = pdevice->properties2.properties.limits.maxImageDimension3D;
      }
      if (tiling & VK_IMAGE_TILING_LINEAR ||
             tiling & VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ||
             flags & VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT)
             pImageFormatProperties->maxMipLevels = 1;
      else 
         pImageFormatProperties->maxMipLevels = log2(
            pImageFormatProperties->maxExtent.width > pImageFormatProperties->maxExtent.height ? pImageFormatProperties->maxExtent.width :  pImageFormatProperties->maxExtent.height 	
         );
    
      if (tiling & VK_IMAGE_TILING_LINEAR ||
            ((tiling & VK_IMAGE_TILING_OPTIMAL) && type & VK_IMAGE_TYPE_3D))
         pImageFormatProperties->maxArrayLayers = 1;
      else
         pImageFormatProperties->maxArrayLayers = pdevice->properties2.properties.limits.maxImageArrayLayers;
      // We do not handle any case here for now
      pImageFormatProperties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;      
      pImageFormatProperties->maxResourceSize = 562949953421312;
      return VK_SUCCESS;
   default:
      break;
   }
  
   return pdevice->dispatch_table.GetPhysicalDeviceImageFormatProperties(pdevice->dispatch_handle, 
      format, type, tiling, usage, flags, pImageFormatProperties);  
}	                                           

VKAPI_ATTR VkResult VKAPI_CALL
wrapper_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice,
                                                const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
                                                VkImageFormatProperties2* pImageFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);
  
   switch(pImageFormatInfo->format) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK:                                    
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
   case VK_FORMAT_BC2_UNORM_BLOCK:
   case VK_FORMAT_BC2_SRGB_BLOCK:
   case VK_FORMAT_BC3_UNORM_BLOCK:
   case VK_FORMAT_BC3_SRGB_BLOCK:
   case VK_FORMAT_BC4_UNORM_BLOCK:
   case VK_FORMAT_BC4_SNORM_BLOCK:
   case VK_FORMAT_BC5_UNORM_BLOCK:
   case VK_FORMAT_BC5_SNORM_BLOCK:
   case VK_FORMAT_BC6H_UFLOAT_BLOCK:
   case VK_FORMAT_BC6H_SFLOAT_BLOCK:
   case VK_FORMAT_BC7_UNORM_BLOCK:
   case VK_FORMAT_BC7_SRGB_BLOCK:
      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY &&
          pImageFormatInfo->format <= 138 && pdevice->emulate_bcn == 3)
         break;
         
      if (pdevice->emulate_bcn < 1)
         break;
      
      if (pImageFormatInfo->type & VK_IMAGE_TYPE_1D) {
         pImageFormatProperties->imageFormatProperties.maxExtent.width = pdevice->properties2.properties.limits.maxImageDimension1D;
         pImageFormatProperties->imageFormatProperties.maxExtent.height = 1;
         pImageFormatProperties->imageFormatProperties.maxExtent.depth = 1;
      }
      if (pImageFormatInfo->type & VK_IMAGE_TYPE_2D) {
         if (pImageFormatInfo->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) {
            pImageFormatProperties->imageFormatProperties.maxExtent.width = pdevice->properties2.properties.limits.maxImageDimensionCube;
            pImageFormatProperties->imageFormatProperties.maxExtent.height = pdevice->properties2.properties.limits.maxImageDimensionCube;
         }
         else {
            pImageFormatProperties->imageFormatProperties.maxExtent.width = pdevice->properties2.properties.limits.maxImageDimension2D;
            pImageFormatProperties->imageFormatProperties.maxExtent.height = pdevice->properties2.properties.limits.maxImageDimension2D;
         }
         pImageFormatProperties->imageFormatProperties.maxExtent.depth = 1;
      }
      if (pImageFormatInfo->type & VK_IMAGE_TYPE_3D) {
         pImageFormatProperties->imageFormatProperties.maxExtent.width = pdevice->properties2.properties.limits.maxImageDimension3D;
         pImageFormatProperties->imageFormatProperties.maxExtent.height = pdevice->properties2.properties.limits.maxImageDimension3D;
         pImageFormatProperties->imageFormatProperties.maxExtent.depth = pdevice->properties2.properties.limits.maxImageDimension3D;
      }
      // We do not handle the case where vkPhysicalDeviceImageFormatInfo pNext has
      // a handleType which does not require mipMaps
      if (pImageFormatInfo->tiling & VK_IMAGE_TILING_LINEAR ||
             pImageFormatInfo->tiling & VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ||
             pImageFormatInfo->flags & VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT)
             pImageFormatProperties->imageFormatProperties.maxMipLevels = 1;
      else 
         pImageFormatProperties->imageFormatProperties.maxMipLevels = log2(
            pImageFormatProperties->imageFormatProperties.maxExtent.width > pImageFormatProperties->imageFormatProperties.maxExtent.height ? pImageFormatProperties->imageFormatProperties.maxExtent.width :  pImageFormatProperties->imageFormatProperties.maxExtent.height 	
         );
    
      if (pImageFormatInfo->tiling & VK_IMAGE_TILING_LINEAR ||
            ((pImageFormatInfo->tiling & VK_IMAGE_TILING_OPTIMAL) && pImageFormatInfo->type & VK_IMAGE_TYPE_3D))
         pImageFormatProperties->imageFormatProperties.maxArrayLayers = 1;
      else
         pImageFormatProperties->imageFormatProperties.maxArrayLayers = pdevice->properties2.properties.limits.maxImageArrayLayers;
      // We do not handle any case here for now
      pImageFormatProperties->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;      
      pImageFormatProperties->imageFormatProperties.maxResourceSize = 562949953421312;
      return VK_SUCCESS;
   default:
      break;
   }
  
   return pdevice->dispatch_table.GetPhysicalDeviceImageFormatProperties2(pdevice->dispatch_handle,
      pImageFormatInfo, pImageFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice,
                                            VkFormat format,
                                            VkFormatProperties* pFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);

   /* Ask the driver first, always. The emulated BC formats then add what the
    * emulation provides on top of what the driver already does.
    *
    * This used to return early for those formats, so the driver was never
    * asked and the four bits below were OR-ed into an output structure nobody
    * had written. On a driver with real BC support that silently removed
    * capabilities it has: on Tegra, TRANSFER_SRC disappeared from optimal
    * tiling and linear tiling came back empty, so a caller using this
    * entry point could not copy out of a BC image the driver can copy out of.
    * The Properties2 path above already had the right shape; this one just
    * never got it.
    */
   pdevice->dispatch_table.GetPhysicalDeviceFormatProperties(pdevice->dispatch_handle,
      format, pFormatProperties);

   switch (format) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
   case VK_FORMAT_BC2_UNORM_BLOCK:
   case VK_FORMAT_BC2_SRGB_BLOCK:
   case VK_FORMAT_BC3_UNORM_BLOCK:
   case VK_FORMAT_BC3_SRGB_BLOCK:
   case VK_FORMAT_BC4_UNORM_BLOCK:
   case VK_FORMAT_BC4_SNORM_BLOCK:
   case VK_FORMAT_BC5_UNORM_BLOCK:
   case VK_FORMAT_BC5_SNORM_BLOCK:
   case VK_FORMAT_BC6H_UFLOAT_BLOCK:
   case VK_FORMAT_BC6H_SFLOAT_BLOCK:
   case VK_FORMAT_BC7_UNORM_BLOCK:
   case VK_FORMAT_BC7_SRGB_BLOCK:
      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY &&
          format <= 138 && pdevice->emulate_bcn == 3)
         break;

      if (pdevice->emulate_bcn > 0) {
         VkFormatFeatureFlags driver_optimal =
            pFormatProperties->optimalTilingFeatures;
         pFormatProperties->optimalTilingFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
         WRAPPER_TRACE("GetFormatProperties transform format=%s optimal=0x%x->0x%x "
                       "reason=bcn_emulation", wrapper_trace_format(format),
                       driver_optimal, pFormatProperties->optimalTilingFeatures);
      }
      break;
   default:
      break;
   }
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                           VkFormat format,
                                           VkFormatProperties2* pFormatProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);

   pdevice->dispatch_table.GetPhysicalDeviceFormatProperties2(pdevice->dispatch_handle,
      format, pFormatProperties);

   switch (format) {
   case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
   case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
   case VK_FORMAT_BC2_UNORM_BLOCK:
   case VK_FORMAT_BC2_SRGB_BLOCK:
   case VK_FORMAT_BC3_UNORM_BLOCK:
   case VK_FORMAT_BC3_SRGB_BLOCK:
   case VK_FORMAT_BC4_UNORM_BLOCK:
   case VK_FORMAT_BC4_SNORM_BLOCK:
   case VK_FORMAT_BC5_UNORM_BLOCK:
   case VK_FORMAT_BC5_SNORM_BLOCK:
   case VK_FORMAT_BC6H_UFLOAT_BLOCK:
   case VK_FORMAT_BC6H_SFLOAT_BLOCK:
   case VK_FORMAT_BC7_UNORM_BLOCK:
   case VK_FORMAT_BC7_SRGB_BLOCK:
      if (pdevice->driver_properties.driverID == VK_DRIVER_ID_SAMSUNG_PROPRIETARY &&
          format <= 138 && pdevice->emulate_bcn == 3)
         break;

      if (pdevice->emulate_bcn > 0) {
         VkFormatFeatureFlags bc = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
         WRAPPER_TRACE("GetFormatProperties2 transform format=%s optimal=0x%x->0x%x "
                       "reason=bcn_emulation", wrapper_trace_format(format),
                       pFormatProperties->formatProperties.optimalTilingFeatures,
                       pFormatProperties->formatProperties.optimalTilingFeatures | bc);
         pFormatProperties->formatProperties.optimalTilingFeatures |= bc;

         VkBaseOutStructure *s = (VkBaseOutStructure *)pFormatProperties->pNext;
         while (s) {
            if (s->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3)
               ((VkFormatProperties3 *)s)->optimalTilingFeatures |= bc;
            s = s->pNext;
         }
      }
      break;
   default:
      break;
   }
}

VKAPI_ATTR void VKAPI_CALL
wrapper_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
										  VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);

   static int wrapper_vmem_max_size = -1;
   
   pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties(
      pdevice->dispatch_handle, pMemoryProperties);

   if (pdevice->nvidia_host_heap_enabled &&
       pMemoryProperties->memoryHeapCount == 1 &&
       pMemoryProperties->memoryHeapCount < VK_MAX_MEMORY_HEAPS) {
      const uint32_t host_heap = pMemoryProperties->memoryHeapCount++;
      pMemoryProperties->memoryHeaps[host_heap] =
         pMemoryProperties->memoryHeaps[0];
      pMemoryProperties->memoryHeaps[host_heap].size = MIN2(
         pMemoryProperties->memoryHeaps[host_heap].size,
         pdevice->nvidia_host_heap_bytes);
      for (uint32_t i = 0; i < pMemoryProperties->memoryTypeCount; i++) {
         if (pMemoryProperties->memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            pMemoryProperties->memoryTypes[i].heapIndex = host_heap;
      }
   }

   if (wrapper_vmem_max_size == -1)
      wrapper_vmem_max_size = getenv("WRAPPER_VMEM_MAX_SIZE") ? atoi(getenv("WRAPPER_VMEM_MAX_SIZE")) : 0;

   /* The override may lower the reported heap, never raise it.
    *
    * Raising it is not a harmless hint: DXVK budgets against this number, and
    * on Tegra exhausting the real heap does not produce
    * VK_ERROR_OUT_OF_DEVICE_MEMORY -- it panics the kernel. GameNative's
    * container setting defaults to 4096 MiB while this device reports 2653, so
    * without the clamp every container starts by promising memory that does not
    * exist. Reporting less than exists is still allowed, which is what the knob
    * was for. See docs/investigations/vram-overcommit-2026-08-12.md. */
   if (wrapper_vmem_max_size > 0) {
      VkDeviceSize requested = (VkDeviceSize)wrapper_vmem_max_size * 1048576;
      if (requested < pMemoryProperties->memoryHeaps[0].size)
         pMemoryProperties->memoryHeaps[0].size = requested;
      else
         WRAPPER_LOG(info, "Ignoring WRAPPER_VMEM_MAX_SIZE=%d MiB: heap is %llu MiB",
                     wrapper_vmem_max_size,
                     (unsigned long long)(pMemoryProperties->memoryHeaps[0].size >> 20));
   }
}

VKAPI_ATTR void VKAPI_CALL                                                                                      
wrapper_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                           VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VK_FROM_HANDLE(wrapper_physical_device, pdevice, physicalDevice);

   static int wrapper_vmem_max_size = -1;

   pdevice->dispatch_table.GetPhysicalDeviceMemoryProperties2(
      pdevice->dispatch_handle, pMemoryProperties);

   if (pdevice->nvidia_host_heap_enabled &&
       pMemoryProperties->memoryProperties.memoryHeapCount == 1 &&
       pMemoryProperties->memoryProperties.memoryHeapCount <
          VK_MAX_MEMORY_HEAPS) {
      VkPhysicalDeviceMemoryProperties *memory =
         &pMemoryProperties->memoryProperties;
      const uint32_t host_heap = memory->memoryHeapCount++;
      memory->memoryHeaps[host_heap] = memory->memoryHeaps[0];
      memory->memoryHeaps[host_heap].size = MIN2(
         memory->memoryHeaps[host_heap].size,
         pdevice->nvidia_host_heap_bytes);
      for (uint32_t i = 0; i < memory->memoryTypeCount; i++) {
         if (memory->memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            memory->memoryTypes[i].heapIndex = host_heap;
      }
   }

   if (pdevice->nvidia_memory_budget_enabled) {
      vk_foreach_struct(prop, pMemoryProperties->pNext) {
         if (prop->sType !=
             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
            continue;

         VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget =
            (VkPhysicalDeviceMemoryBudgetPropertiesEXT *)prop;
         for (uint32_t i = 0;
              i < pMemoryProperties->memoryProperties.memoryHeapCount; i++) {
            budget->heapBudget[i] =
               pMemoryProperties->memoryProperties.memoryHeaps[i].size;
            budget->heapUsage[i] = 0;
         }
         budget->heapBudget[0] = MIN2(
            budget->heapBudget[0], pdevice->nvidia_memory_budget_bytes);
         budget->heapUsage[0] =
            p_atomic_read(&pdevice->wrapper_memory_live_bytes);
         WRAPPER_TRACE("NVIDIA memory budget=%llu usage=%llu",
                       (unsigned long long)budget->heapBudget[0],
                       (unsigned long long)budget->heapUsage[0]);
      }
   }

   if (wrapper_vmem_max_size == -1)
      wrapper_vmem_max_size = getenv("WRAPPER_VMEM_MAX_SIZE") ? atoi(getenv("WRAPPER_VMEM_MAX_SIZE")) : 0;

   /* Same clamp as the 1.0 entry point above; DXVK reads this one. */
   if (wrapper_vmem_max_size > 0) {
      VkDeviceSize requested = (VkDeviceSize)wrapper_vmem_max_size * 1048576;
      VkDeviceSize *heap = &pMemoryProperties->memoryProperties.memoryHeaps[0].size;
      if (requested < *heap)
         *heap = requested;
      else
         WRAPPER_LOG(info, "Ignoring WRAPPER_VMEM_MAX_SIZE=%d MiB: heap is %llu MiB",
                     wrapper_vmem_max_size, (unsigned long long)(*heap >> 20));
   }
}
