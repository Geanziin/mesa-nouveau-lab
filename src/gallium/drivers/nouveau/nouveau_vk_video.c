#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>

#include "nouveau_screen.h"
#include "nouveau.h"
#include "nouveau_vk_video.h"
#include "nouveau_vk_video_h264.h"
#include "pipe/p_context.h"
#include "util/u_debug.h"
#include "util/u_dl.h"
#include "util/simple_mtx.h"
#include "util/u_inlines.h"
#include "vl/vl_video_buffer.h"
#include "frontend/winsys_handle.h"
#include "drm-uapi/drm_fourcc.h"

#define INSTANCE_FUNCTIONS(X) \
   X(DestroyInstance) X(EnumeratePhysicalDevices) \
   X(GetPhysicalDeviceProperties2) X(GetPhysicalDeviceMemoryProperties) \
   X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceVideoCapabilitiesKHR) \
   X(GetPhysicalDeviceVideoFormatPropertiesKHR) X(CreateDevice) X(GetDeviceProcAddr) \
   X(GetPhysicalDeviceFormatProperties2) X(GetPhysicalDeviceImageFormatProperties2) \
   X(GetPhysicalDeviceFeatures2)

#define SHARED_FUNCTIONS(X) \
   X(GetImageSubresourceLayout) X(GetImageDrmFormatModifierPropertiesEXT) \
   X(GetMemoryFdKHR) X(GetMemoryFdPropertiesKHR)

#define DEVICE_FUNCTIONS(X) \
   X(DestroyDevice) X(DeviceWaitIdle) X(GetDeviceQueue) \
   X(CreateVideoSessionKHR) X(DestroyVideoSessionKHR) \
   X(GetVideoSessionMemoryRequirementsKHR) X(BindVideoSessionMemoryKHR) \
   X(CreateVideoSessionParametersKHR) X(DestroyVideoSessionParametersKHR) \
   X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) \
   X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
   X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
   X(CreateImageView) X(DestroyImageView) X(CreateCommandPool) X(DestroyCommandPool) \
   X(AllocateCommandBuffers) X(ResetCommandPool) X(BeginCommandBuffer) X(EndCommandBuffer) \
   X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) \
   X(CreateSemaphore) X(DestroySemaphore) X(QueueSubmit) \
   X(CmdPipelineBarrier2) X(CmdBeginVideoCodingKHR) X(CmdControlVideoCodingKHR) \
   X(CmdDecodeVideoKHR) X(CmdEndVideoCodingKHR) X(CmdCopyImageToBuffer)

struct vk_buffer {
   VkBuffer buffer;
   VkDeviceMemory memory;
   void *map;
   VkDeviceSize size;
};

struct nouveau_vk_video;

struct vk_picture {
   struct nouveau_vk_video *decoder;
   VkImage image;
   VkImageView view;
   VkDeviceMemory memory;
   int slot;
   bool initialized;
   bool submitted;
};

struct nouveau_vk_video {
   struct pipe_video_codec base;
   struct util_dl_library *loader;
   PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
   VkInstance instance;
   VkPhysicalDevice physical;
   VkDevice device;
#define DECLARE(name) PFN_vk##name name;
   INSTANCE_FUNCTIONS(DECLARE)
   DEVICE_FUNCTIONS(DECLARE)
   SHARED_FUNCTIONS(DECLARE)
#undef DECLARE
   VkPhysicalDeviceMemoryProperties memory_properties;
   VkVideoDecodeH264ProfileInfoKHR h264_profile;
   VkVideoProfileInfoKHR profile;
   VkVideoProfileListInfoKHR profile_list;
   VkVideoDecodeH264CapabilitiesKHR h264_caps;
   VkVideoDecodeCapabilitiesKHR decode_caps;
   VkVideoCapabilitiesKHR caps;
   uint32_t families[2];
   VkQueue queues[2];
   VkCommandPool pools[2];
   VkCommandBuffer commands[2];
   VkVideoSessionKHR session;
   VkDeviceMemory session_memory[8];
   unsigned session_memory_count;
   VkSemaphore semaphore;
   VkFence fence;
   struct vk_buffer bitstream_buffer;
   struct vk_buffer readback;
   struct nouveau_vk_h264_bitstream bitstream;
   struct vk_picture *slots[17];
   VkExtent2D extent;
   bool started;
   bool failed;
   unsigned completed_frames;
   bool zero_copy;
   uint64_t cpu_frame_bytes;
};

struct shared_video_buffer {
   struct pipe_video_buffer base;
   struct pipe_video_buffer *planes;
   struct nouveau_vk_video *owner;
   VkImage image;
   VkDeviceMemory memory;
   VkExtent2D extent;
   VkSubresourceLayout layouts[2];
   uint64_t modifier;
   VkDeviceSize allocation_size;
   int fd;
};

static void destroy_shared_buffer(struct pipe_video_buffer *buffer);

struct caps_cache_entry {
   bool probed, supported;
   VkExtent2D max, min;
};
static simple_mtx_t caps_cache_lock = SIMPLE_MTX_INITIALIZER;
static struct caps_cache_entry caps_cache[3];

static simple_mtx_t owner_lock = SIMPLE_MTX_INITIALIZER;
static struct nouveau_screen *owner_screen;
static struct nouveau_vk_video *owner_dev;
static void release_owner(struct nouveau_vk_video *dev);
static unsigned owner_refs;

static bool
kernel_supported(struct nouveau_screen *screen)
{
   struct stat st;
   char path[128];

   if (fstat(screen->drm->fd, &st))
      return false;

   snprintf(path, sizeof(path),
            "/sys/dev/char/%u:%u/device/decoder_busy_percent",
            major(st.st_rdev), minor(st.st_rdev));
   return access(path, F_OK) == 0;
}

bool
nouveau_vk_video_enabled(struct nouveau_screen *screen)
{
   if (screen->device->chipset != 0x177)
      return false;

   if (debug_get_option("NOUVEAU_VK_VIDEO", NULL))
      return debug_get_bool_option("NOUVEAU_VK_VIDEO", false);

   return kernel_supported(screen);
}

static StdVideoH264ProfileIdc
profile_idc(enum pipe_video_profile profile)
{
   switch (profile) {
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE:
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE:
      return STD_VIDEO_H264_PROFILE_IDC_BASELINE;
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
      return STD_VIDEO_H264_PROFILE_IDC_MAIN;
   case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH:
      return STD_VIDEO_H264_PROFILE_IDC_HIGH;
   default:
      return STD_VIDEO_H264_PROFILE_IDC_INVALID;
   }
}

static int
memory_type(struct nouveau_vk_video *dec, uint32_t mask, VkMemoryPropertyFlags flags)
{
   for (unsigned i = 0; i < dec->memory_properties.memoryTypeCount; i++) {
      if ((mask & (1u << i)) &&
          (dec->memory_properties.memoryTypes[i].propertyFlags & flags) == flags)
         return i;
   }
   return -1;
}

static bool
allocate_memory(struct nouveau_vk_video *dec, VkMemoryRequirements req,
                VkMemoryPropertyFlags flags, VkDeviceMemory *memory)
{
   int type = memory_type(dec, req.memoryTypeBits, flags);
   if (type < 0)
      return false;
   VkMemoryAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   return dec->AllocateMemory(dec->device, &info, NULL, memory) == VK_SUCCESS;
}

static void
destroy_buffer(struct nouveau_vk_video *dec, struct vk_buffer *buffer)
{
   if (buffer->map)
      dec->UnmapMemory(dec->device, buffer->memory);
   if (buffer->buffer)
      dec->DestroyBuffer(dec->device, buffer->buffer, NULL);
   if (buffer->memory)
      dec->FreeMemory(dec->device, buffer->memory, NULL);
   memset(buffer, 0, sizeof(*buffer));
}

static bool
create_buffer(struct nouveau_vk_video *dec, VkDeviceSize size,
              VkBufferUsageFlags usage, struct vk_buffer *buffer)
{
   VkBufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext = usage & VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR ? &dec->profile_list : NULL,
      .size = size,
      .usage = usage,
   };
   if (dec->CreateBuffer(dec->device, &info, NULL, &buffer->buffer) != VK_SUCCESS)
      return false;
   VkMemoryRequirements req;
   dec->GetBufferMemoryRequirements(dec->device, buffer->buffer, &req);
   if (!allocate_memory(dec, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buffer->memory) ||
       dec->BindBufferMemory(dec->device, buffer->buffer, buffer->memory, 0) != VK_SUCCESS ||
       dec->MapMemory(dec->device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->map) != VK_SUCCESS) {
      destroy_buffer(dec, buffer);
      return false;
   }
   buffer->size = size;
   return true;
}

static void
destroy_decoder(struct pipe_video_codec *codec)
{
   struct nouveau_vk_video *dec = (struct nouveau_vk_video *)codec;
   if (dec->device) {
      if (dec->DeviceWaitIdle)
         dec->DeviceWaitIdle(dec->device);
      if (dec->session) {
         dec->DestroyVideoSessionKHR(dec->device, dec->session, NULL);
         fprintf(stderr, "nouveau-vk-video: H.264 session destroyed, completed_frames=%u, failed=%u, zero_copy=%u, cpu_frame_bytes=%" PRIu64 "\n",
                 dec->completed_frames, dec->failed, dec->zero_copy, dec->cpu_frame_bytes);
      }
      for (unsigned i = 0; i < dec->session_memory_count; i++)
         dec->FreeMemory(dec->device, dec->session_memory[i], NULL);
      destroy_buffer(dec, &dec->bitstream_buffer);
      destroy_buffer(dec, &dec->readback);
      if (dec->semaphore)
         dec->DestroySemaphore(dec->device, dec->semaphore, NULL);
      if (dec->fence)
         dec->DestroyFence(dec->device, dec->fence, NULL);
      for (unsigned i = 0; i < 2; i++) {
         if (dec->pools[i])
            dec->DestroyCommandPool(dec->device, dec->pools[i], NULL);
      }
      if (dec->DestroyDevice)
         dec->DestroyDevice(dec->device, NULL);
   }
   if (dec->instance && dec->DestroyInstance)
      dec->DestroyInstance(dec->instance, NULL);
   if (dec->loader)
      util_dl_close(dec->loader);
   free(dec->bitstream.data);
   free(dec);
}

static bool
supports_picture_format(struct nouveau_vk_video *dec)
{
   VkImageUsageFlags usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
                             VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   VkPhysicalDeviceVideoFormatInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
      .pNext = &dec->profile_list, .imageUsage = usage,
   };
   VkVideoFormatPropertiesKHR formats[8] = {0};
   for (unsigned i = 0; i < ARRAY_SIZE(formats); i++)
      formats[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
   uint32_t count = ARRAY_SIZE(formats);
   if (dec->GetPhysicalDeviceVideoFormatPropertiesKHR(dec->physical, &info, &count, formats) != VK_SUCCESS)
      return false;
   for (unsigned i = 0; i < count; i++) {
      if (formats[i].format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM &&
          formats[i].imageType == VK_IMAGE_TYPE_2D &&
          formats[i].imageTiling == VK_IMAGE_TILING_OPTIMAL &&
          (formats[i].imageUsageFlags & usage) == usage)
         return true;
   }
   return false;
}

static struct nouveau_vk_video *
create_device(struct nouveau_screen *screen, enum pipe_video_profile profile)
{
   StdVideoH264ProfileIdc idc = profile_idc(profile);
   if (idc == STD_VIDEO_H264_PROFILE_IDC_INVALID)
      return NULL;
   struct nouveau_vk_video *dec = calloc(1, sizeof(*dec));
   if (!dec)
      return NULL;
   dec->zero_copy = debug_get_bool_option("NOUVEAU_VK_VIDEO_ZERO_COPY", true);
   dec->loader = util_dl_open("libvulkan.so.1");
   if (!dec->loader)
      goto fail;
   dec->GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
      util_dl_get_proc_address(dec->loader, "vkGetInstanceProcAddr");
   if (!dec->GetInstanceProcAddr)
      goto fail;
   PFN_vkCreateInstance create = (PFN_vkCreateInstance)
      dec->GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
   if (!create)
      goto fail;
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "nouveau-vk-video",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   if (create(&instance_info, NULL, &dec->instance) != VK_SUCCESS)
      goto fail;
#define LOAD_INSTANCE(name) \
   dec->name = (PFN_vk##name)dec->GetInstanceProcAddr(dec->instance, "vk" #name); \
   if (!dec->name) goto fail;
   INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE

   struct stat drm_stat;
   if (fstat(screen->drm->fd, &drm_stat))
      goto fail;
   VkPhysicalDevice physical[16];
   uint32_t count = ARRAY_SIZE(physical);
   if (dec->EnumeratePhysicalDevices(dec->instance, &count, physical) != VK_SUCCESS)
      goto fail;
   for (unsigned i = 0; i < count; i++) {
      VkPhysicalDeviceDrmPropertiesEXT drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
      VkPhysicalDeviceDriverProperties driver = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = &drm,
      };
      VkPhysicalDeviceProperties2 props = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver,
      };
      dec->GetPhysicalDeviceProperties2(physical[i], &props);
      bool same_node = (drm.hasRender && drm.renderMajor == major(drm_stat.st_rdev) &&
                        drm.renderMinor == minor(drm_stat.st_rdev)) ||
                       (drm.hasPrimary && drm.primaryMajor == major(drm_stat.st_rdev) &&
                        drm.primaryMinor == minor(drm_stat.st_rdev));
      if (props.properties.vendorID == 0x10de && props.properties.deviceID == 0x2584 &&
          driver.driverID == VK_DRIVER_ID_MESA_NVK && same_node) {
         dec->physical = physical[i];
         break;
      }
   }
   if (!dec->physical)
      goto fail;
   dec->h264_profile = (VkVideoDecodeH264ProfileInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR,
      .stdProfileIdc = idc,
      .pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR,
   };
   dec->profile = (VkVideoProfileInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR, .pNext = &dec->h264_profile,
      .videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
      .chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
      .lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
      .chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
   };
   dec->profile_list = (VkVideoProfileListInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
      .profileCount = 1, .pProfiles = &dec->profile,
   };
   dec->h264_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;
   dec->decode_caps = (VkVideoDecodeCapabilitiesKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR, .pNext = &dec->h264_caps,
   };
   dec->caps = (VkVideoCapabilitiesKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, .pNext = &dec->decode_caps,
   };
   if (dec->GetPhysicalDeviceVideoCapabilitiesKHR(dec->physical, &dec->profile, &dec->caps) != VK_SUCCESS ||
       dec->caps.maxDpbSlots < 17 || dec->caps.maxActiveReferencePictures < 16 ||
       !(dec->decode_caps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR))
      goto fail;
   if (!supports_picture_format(dec))
      goto fail;
   VkQueueFamilyProperties families[16];
   count = ARRAY_SIZE(families);
   dec->GetPhysicalDeviceQueueFamilyProperties(dec->physical, &count, families);
   dec->families[0] = dec->families[1] = UINT32_MAX;
   for (unsigned i = 0; i < count; i++) {
      if (families[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)
         dec->families[0] = i;
      if (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
         dec->families[1] = i;
   }
   if (dec->families[0] == UINT32_MAX || dec->families[1] == UINT32_MAX ||
       dec->families[0] == dec->families[1])
      goto fail;
   const char *extensions[] = {
      VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
      VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
      VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
      VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
   };
   float priority = 0.5f;
   VkDeviceQueueCreateInfo queues[2];
   for (unsigned i = 0; i < 2; i++) {
      queues[i] = (VkDeviceQueueCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = dec->families[i], .queueCount = 1, .pQueuePriorities = &priority,
      };
   }
   VkPhysicalDeviceVideoMaintenance1FeaturesKHR maintenance = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR,
   };
   VkPhysicalDeviceSynchronization2Features sync = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
      .pNext = dec->zero_copy ? &maintenance : NULL,
   };
   VkPhysicalDeviceFeatures2 features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &sync,
   };
   dec->GetPhysicalDeviceFeatures2(dec->physical, &features);
   if (!sync.synchronization2 || (dec->zero_copy && !maintenance.videoMaintenance1))
      goto fail;
   VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &sync,
      .queueCreateInfoCount = 2, .pQueueCreateInfos = queues,
      .enabledExtensionCount = dec->zero_copy ? ARRAY_SIZE(extensions) : 3,
      .ppEnabledExtensionNames = extensions,
   };
   if (dec->CreateDevice(dec->physical, &device_info, NULL, &dec->device) != VK_SUCCESS)
      goto fail;
#define LOAD_DEVICE(name) \
   dec->name = (PFN_vk##name)dec->GetDeviceProcAddr(dec->device, "vk" #name); \
   if (!dec->name) goto fail;
   DEVICE_FUNCTIONS(LOAD_DEVICE)
   if (dec->zero_copy) {
      SHARED_FUNCTIONS(LOAD_DEVICE)
   }
#undef LOAD_DEVICE
   dec->GetPhysicalDeviceMemoryProperties(dec->physical, &dec->memory_properties);
   for (unsigned i = 0; i < 2; i++)
      dec->GetDeviceQueue(dec->device, dec->families[i], 0, &dec->queues[i]);
   return dec;

fail:
   if (dec->device && !dec->DeviceWaitIdle)
      dec->DeviceWaitIdle = (PFN_vkDeviceWaitIdle)dec->GetDeviceProcAddr(dec->device, "vkDeviceWaitIdle");
   if (dec->device && !dec->DestroyDevice)
      dec->DestroyDevice = (PFN_vkDestroyDevice)dec->GetDeviceProcAddr(dec->device, "vkDestroyDevice");
   destroy_decoder(&dec->base);
   return NULL;
}

int
nouveau_vk_video_get_param(struct nouveau_screen *screen, enum pipe_video_profile profile,
                          enum pipe_video_entrypoint entrypoint, enum pipe_video_cap param)
{
   if (entrypoint != PIPE_VIDEO_ENTRYPOINT_BITSTREAM ||
       profile_idc(profile) == STD_VIDEO_H264_PROFILE_IDC_INVALID)
      return 0;
   switch (param) {
   case PIPE_VIDEO_CAP_SUPPORTED:
   case PIPE_VIDEO_CAP_SUPPORTS_PROGRESSIVE:
   case PIPE_VIDEO_CAP_MAX_WIDTH:
   case PIPE_VIDEO_CAP_MAX_HEIGHT:
   case PIPE_VIDEO_CAP_MAX_MACROBLOCKS:
   case PIPE_VIDEO_CAP_MIN_WIDTH:
   case PIPE_VIDEO_CAP_MIN_HEIGHT:
      break;
   default:
      return 0;
   }
   const StdVideoH264ProfileIdc idc = profile_idc(profile);
   unsigned slot = idc == STD_VIDEO_H264_PROFILE_IDC_BASELINE ? 0 :
                   idc == STD_VIDEO_H264_PROFILE_IDC_MAIN ? 1 : 2;
   simple_mtx_lock(&caps_cache_lock);
   struct caps_cache_entry *entry = &caps_cache[slot];
   if (!entry->probed) {
      struct nouveau_vk_video *dec = create_device(screen, profile);
      entry->probed = true;
      entry->supported = dec != NULL;
      if (dec) {
         entry->max = dec->caps.maxCodedExtent;
         entry->min = dec->caps.minCodedExtent;
         destroy_decoder(&dec->base);
      }
   }
   const struct caps_cache_entry caps = *entry;
   simple_mtx_unlock(&caps_cache_lock);
   if (!caps.supported)
      return 0;
   switch (param) {
   case PIPE_VIDEO_CAP_SUPPORTED:
   case PIPE_VIDEO_CAP_SUPPORTS_PROGRESSIVE:
      return 1;
   case PIPE_VIDEO_CAP_MAX_WIDTH:
      return caps.max.width;
   case PIPE_VIDEO_CAP_MAX_HEIGHT:
      return caps.max.height;
   case PIPE_VIDEO_CAP_MAX_MACROBLOCKS:
      return (caps.max.width / 16) * (caps.max.height / 16);
   case PIPE_VIDEO_CAP_MIN_WIDTH:
      return caps.min.width;
   case PIPE_VIDEO_CAP_MIN_HEIGHT:
      return caps.min.height;
   default:
      return 0;
   }
}

static bool
create_session(struct nouveau_vk_video *dec)
{
   dec->extent = (VkExtent2D) {align(dec->base.width, 16), align(dec->base.height, 16)};
   if (dec->extent.width < dec->caps.minCodedExtent.width ||
       dec->extent.height < dec->caps.minCodedExtent.height ||
       dec->extent.width > dec->caps.maxCodedExtent.width ||
       dec->extent.height > dec->caps.maxCodedExtent.height)
      return false;
   VkVideoSessionCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
      .queueFamilyIndex = dec->families[0], .pVideoProfile = &dec->profile,
      .pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
      .maxCodedExtent = dec->extent,
      .referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
      .maxDpbSlots = 17, .maxActiveReferencePictures = 16,
      .pStdHeaderVersion = &dec->caps.stdHeaderVersion,
   };
   if (dec->CreateVideoSessionKHR(dec->device, &info, NULL, &dec->session) != VK_SUCCESS)
      return false;
   VkVideoSessionMemoryRequirementsKHR reqs[8];
   memset(reqs, 0, sizeof(reqs));
   for (unsigned i = 0; i < ARRAY_SIZE(reqs); i++)
      reqs[i].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
   uint32_t count = ARRAY_SIZE(reqs);
   if (dec->GetVideoSessionMemoryRequirementsKHR(dec->device, dec->session, &count, reqs) != VK_SUCCESS)
      return false;
   VkBindVideoSessionMemoryInfoKHR bindings[8];
   for (unsigned i = 0; i < count; i++) {
      if (!allocate_memory(dec, reqs[i].memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           &dec->session_memory[i]))
         return false;
      dec->session_memory_count++;
      bindings[i] = (VkBindVideoSessionMemoryInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR,
         .memoryBindIndex = reqs[i].memoryBindIndex, .memory = dec->session_memory[i],
         .memorySize = reqs[i].memoryRequirements.size,
      };
   }
   if (dec->BindVideoSessionMemoryKHR(dec->device, dec->session, count, bindings) != VK_SUCCESS)
      return false;
   for (unsigned i = 0; i < 2; i++) {
      VkCommandPoolCreateInfo pool = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = dec->families[i],
      };
      if (dec->CreateCommandPool(dec->device, &pool, NULL, &dec->pools[i]) != VK_SUCCESS)
         return false;
      VkCommandBufferAllocateInfo command = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = dec->pools[i], .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      if (dec->AllocateCommandBuffers(dec->device, &command, &dec->commands[i]) != VK_SUCCESS)
         return false;
   }
   VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (dec->CreateSemaphore(dec->device, &sem, NULL, &dec->semaphore) != VK_SUCCESS ||
       dec->CreateFence(dec->device, &fence, NULL, &dec->fence) != VK_SUCCESS)
      return false;
   if (dec->zero_copy)
      return true;
   VkDeviceSize readback_size = (VkDeviceSize)dec->extent.width * dec->extent.height * 3 / 2;
   return create_buffer(dec, readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dec->readback);
}

static bool
create_shared_image(struct nouveau_vk_video *dec, VkExtent2D extent,
                    uint64_t modifier, VkImage *image)
{
   VkExternalMemoryImageCreateInfo external = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   VkImageDrmFormatModifierListCreateInfoEXT mods = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
      .pNext = &external, .drmFormatModifierCount = 1, .pDrmFormatModifiers = &modifier,
   };
   VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &mods,
      .flags = VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
      .extent = {extent.width, extent.height, 1},
      .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
   };
   return dec->CreateImage(dec->device, &info, NULL, image) == VK_SUCCESS;
}

static bool
choose_shared_modifier(struct nouveau_vk_video *dec, struct pipe_screen *screen,
                       VkExtent2D extent, uint64_t *modifier)
{
   VkDrmFormatModifierPropertiesEXT mods[64];
   VkDrmFormatModifierPropertiesListEXT list = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
      .drmFormatModifierCount = ARRAY_SIZE(mods), .pDrmFormatModifierProperties = mods,
   };
   VkFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list};
   dec->GetPhysicalDeviceFormatProperties2(dec->physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &props);
   if (list.drmFormatModifierCount > ARRAY_SIZE(mods))
      return false;
   for (unsigned i = 0; i < list.drmFormatModifierCount; i++) {
      uint64_t mod = mods[i].drmFormatModifier;
      if (mods[i].drmFormatModifierPlaneCount != 2 || (mod & 0xf) != 1 ||
          !fourcc_mod_is_vendor(mod, NVIDIA) ||
          !screen->is_dmabuf_modifier_supported(screen, mod, PIPE_FORMAT_R8_UNORM, NULL) ||
          !screen->is_dmabuf_modifier_supported(screen, mod, PIPE_FORMAT_R8G8_UNORM, NULL))
         continue;
      VkPhysicalDeviceExternalImageFormatInfo external = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
         .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
      VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
         .pNext = &external, .drmFormatModifier = mod,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      VkPhysicalDeviceImageFormatInfo2 info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &drm,
         .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, .type = VK_IMAGE_TYPE_2D,
         .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
         .flags = VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR,
         .usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      };
      VkExternalImageFormatProperties ext_props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
      VkImageFormatProperties2 image_props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ext_props};
      if (dec->GetPhysicalDeviceImageFormatProperties2(dec->physical, &info, &image_props) != VK_SUCCESS)
         continue;
      VkExternalMemoryFeatureFlags required = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                                             VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
      if ((ext_props.externalMemoryProperties.externalMemoryFeatures & required) != required ||
          extent.width > image_props.imageFormatProperties.maxExtent.width ||
          extent.height > image_props.imageFormatProperties.maxExtent.height)
         continue;
      *modifier = mod;
      return true;
   }
   return false;
}

static void
shared_resources(struct pipe_video_buffer *buffer, struct pipe_resource **resources)
{
   struct shared_video_buffer *b = (struct shared_video_buffer *)buffer;
   b->planes->get_resources(b->planes, resources);
}

static struct pipe_sampler_view **
shared_planes(struct pipe_video_buffer *buffer)
{
   struct shared_video_buffer *b = (struct shared_video_buffer *)buffer;
   return b->planes->get_sampler_view_planes(b->planes);
}

static struct pipe_sampler_view **
shared_components(struct pipe_video_buffer *buffer)
{
   struct shared_video_buffer *b = (struct shared_video_buffer *)buffer;
   return b->planes->get_sampler_view_components(b->planes);
}

static struct pipe_surface *
shared_surfaces(struct pipe_video_buffer *buffer)
{
   struct shared_video_buffer *b = (struct shared_video_buffer *)buffer;
   return b->planes->get_surfaces(b->planes);
}

static void
destroy_shared_buffer(struct pipe_video_buffer *buffer)
{
   struct shared_video_buffer *b = (struct shared_video_buffer *)buffer;
   vl_video_buffer_set_associated_data(buffer, NULL, NULL, NULL);
   if (b->planes)
      b->planes->destroy(b->planes);
   if (b->owner) {
      if (b->image)
         b->owner->DestroyImage(b->owner->device, b->image, NULL);
      if (b->memory)
         b->owner->FreeMemory(b->owner->device, b->memory, NULL);
      release_owner(b->owner);
   }
   if (b->fd >= 0)
      close(b->fd);
   free(b);
}

static struct nouveau_vk_video *
acquire_owner(struct nouveau_screen *screen)
{
   simple_mtx_lock(&owner_lock);
   if (owner_dev && owner_screen != screen) {
      simple_mtx_unlock(&owner_lock);
      return create_device(screen, PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH);
   }
   if (!owner_dev) {
      owner_dev = create_device(screen, PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH);
      owner_screen = owner_dev ? screen : NULL;
   }
   struct nouveau_vk_video *dev = owner_dev;
   if (dev)
      owner_refs++;
   simple_mtx_unlock(&owner_lock);
   return dev;
}

static void
release_owner(struct nouveau_vk_video *dev)
{
   simple_mtx_lock(&owner_lock);
   if (dev == owner_dev) {
      if (--owner_refs == 0) {
         owner_dev = NULL;
         owner_screen = NULL;
         simple_mtx_unlock(&owner_lock);
         destroy_decoder(&dev->base);
         return;
      }
      simple_mtx_unlock(&owner_lock);
      return;
   }
   simple_mtx_unlock(&owner_lock);
   destroy_decoder(&dev->base);
}

struct pipe_video_buffer *
nouveau_vk_video_buffer_create(struct pipe_context *context, const struct pipe_video_buffer *templ)
{
   struct shared_video_buffer *b = calloc(1, sizeof(*b));
   if (!b)
      return NULL;
   b->fd = -1;
   b->base = *templ;
   b->base.width = align(templ->width, 16);
   b->base.height = align(templ->height, 16);
   b->base.context = context;
   b->base.destroy = destroy_shared_buffer;
   b->extent = (VkExtent2D) {b->base.width, b->base.height};
   b->owner = acquire_owner(nouveau_screen(context->screen));
   if (!b->owner || !b->owner->zero_copy ||
       !choose_shared_modifier(b->owner, context->screen, b->extent, &b->modifier) ||
       !create_shared_image(b->owner, b->extent, b->modifier, &b->image))
      goto fail;
   VkMemoryRequirements req;
   b->owner->GetImageMemoryRequirements(b->owner->device, b->image, &req);
   b->allocation_size = req.size;
   int type = memory_type(b->owner, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (type < 0)
      goto fail;
   VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = b->image,
   };
   VkExportMemoryAllocateInfo export = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &dedicated,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &export,
      .allocationSize = req.size, .memoryTypeIndex = type,
   };
   if (b->owner->AllocateMemory(b->owner->device, &allocation, NULL, &b->memory) != VK_SUCCESS ||
       b->owner->BindImageMemory(b->owner->device, b->image, b->memory, 0) != VK_SUCCESS)
      goto fail;
   VkMemoryGetFdInfoKHR fd = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .memory = b->memory, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   if (b->owner->GetMemoryFdKHR(b->owner->device, &fd, &b->fd) != VK_SUCCESS)
      goto fail;
   struct pipe_resource *resources[3] = {0};
   for (unsigned i = 0; i < 2; i++) {
      VkImageSubresource sub = {
         .aspectMask = i == 0 ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT : VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
      };
      b->owner->GetImageSubresourceLayout(b->owner->device, b->image, &sub, &b->layouts[i]);
      if (b->layouts[i].offset > UINT_MAX || b->layouts[i].rowPitch > UINT_MAX)
         goto fail_resources;
      struct winsys_handle handle = {
         .type = WINSYS_HANDLE_TYPE_FD, .handle = b->fd,
         .stride = b->layouts[i].rowPitch, .offset = b->layouts[i].offset,
         .modifier = b->modifier, .size = req.size,
      };
      struct pipe_resource resource = {
         .target = PIPE_TEXTURE_2D, .format = i ? PIPE_FORMAT_R8G8_UNORM : PIPE_FORMAT_R8_UNORM,
         .width0 = b->extent.width >> i, .height0 = b->extent.height >> i,
         .depth0 = 1, .array_size = 1,
         .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET | templ->bind,
      };
      resources[i] = context->screen->resource_from_handle(context->screen, &resource, &handle, 0);
      if (!resources[i])
         goto fail_resources;
   }
   struct stat original;
   if (fstat(b->fd, &original))
      goto fail_resources;
   for (unsigned i = 0; i < 2; i++) {
      struct winsys_handle handle = {.type = WINSYS_HANDLE_TYPE_FD, .handle = UINT_MAX};
      if (!context->screen->resource_get_handle(context->screen, context, resources[i], &handle, 0))
         goto fail_resources;
      struct stat exported;
      bool same = !fstat(handle.handle, &exported) && exported.st_ino == original.st_ino &&
                  exported.st_dev == original.st_dev && handle.offset == b->layouts[i].offset &&
                  handle.stride == b->layouts[i].rowPitch && handle.modifier == b->modifier;
      close(handle.handle);
      if (!same)
         goto fail_resources;
   }
   fprintf(stderr, "nouveau-vk-video: shared DMA-BUF aliases verified, inode=%ju, modifier=0x%" PRIx64
                   ", offsets=%" PRIu64 "/%" PRIu64 ", pitches=%" PRIu64 "/%" PRIu64 "\n",
           (uintmax_t)original.st_ino, b->modifier, b->layouts[0].offset, b->layouts[1].offset,
           b->layouts[0].rowPitch, b->layouts[1].rowPitch);
   b->planes = vl_video_buffer_create_ex2(context, &b->base, resources);
   if (!b->planes)
      goto fail_resources;
   b->base.get_resources = shared_resources;
   b->base.get_sampler_view_planes = shared_planes;
   b->base.get_sampler_view_components = shared_components;
   b->base.get_surfaces = shared_surfaces;
   return &b->base;
fail_resources:
   for (unsigned i = 0; i < 3; i++)
      pipe_resource_reference(&resources[i], NULL);
fail:
   destroy_shared_buffer(&b->base);
   return NULL;
}

static void
destroy_picture(void *data)
{
   struct vk_picture *pic = data;
   struct nouveau_vk_video *dec = pic->decoder;
   if (pic->slot >= 0 && dec->slots[pic->slot] == pic)
      dec->slots[pic->slot] = NULL;
   if (pic->view)
      dec->DestroyImageView(dec->device, pic->view, NULL);
   if (pic->image)
      dec->DestroyImage(dec->device, pic->image, NULL);
   if (pic->memory)
      dec->FreeMemory(dec->device, pic->memory, NULL);
   free(pic);
   struct pipe_video_codec *codec = &dec->base;
   pipe_video_codec_reference(&codec, NULL);
}

static bool
import_shared_picture(struct nouveau_vk_video *dec, struct pipe_video_buffer *buffer,
                      struct vk_picture *pic)
{
   if (buffer->destroy != destroy_shared_buffer)
      return false;
   struct shared_video_buffer *b = (struct shared_video_buffer *)buffer;
   if (b->extent.width != dec->extent.width || b->extent.height != dec->extent.height ||
       !create_shared_image(dec, b->extent, b->modifier, &pic->image))
      return false;
   VkMemoryRequirements req;
   dec->GetImageMemoryRequirements(dec->device, pic->image, &req);
   if (req.size != b->allocation_size)
      return false;
   for (unsigned i = 0; i < 2; i++) {
      VkImageSubresource sub = {
         .aspectMask = i ? VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT : VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
      };
      VkSubresourceLayout layout;
      dec->GetImageSubresourceLayout(dec->device, pic->image, &sub, &layout);
      if (layout.offset != b->layouts[i].offset || layout.rowPitch != b->layouts[i].rowPitch ||
          layout.size != b->layouts[i].size)
         return false;
   }
   int fd = dup(b->fd);
   if (fd < 0)
      return false;
   VkMemoryFdPropertiesKHR properties = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
   if (dec->GetMemoryFdPropertiesKHR(dec->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                     fd, &properties) != VK_SUCCESS) {
      close(fd);
      return false;
   }
   int type = memory_type(dec, req.memoryTypeBits & properties.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   if (type < 0) {
      close(fd);
      return false;
   }
   VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = pic->image,
   };
   VkImportMemoryFdInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &dedicated,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd,
   };
   VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
      .allocationSize = req.size, .memoryTypeIndex = type,
   };
   if (dec->AllocateMemory(dec->device, &allocation, NULL, &pic->memory) != VK_SUCCESS) {
      close(fd);
      return false;
   }
   return dec->BindImageMemory(dec->device, pic->image, pic->memory, 0) == VK_SUCCESS;
}

static struct vk_picture *
get_picture(struct nouveau_vk_video *dec, struct pipe_video_buffer *buffer)
{
   struct vk_picture *pic = vl_video_buffer_get_associated_data(buffer, &dec->base);
   if (pic)
      return pic;
   pic = calloc(1, sizeof(*pic));
   if (!pic)
      return NULL;
   pic->decoder = dec;
   pic->slot = -1;
   struct pipe_video_codec *hold = NULL;
   pipe_video_codec_reference(&hold, &dec->base);
   VkImageCreateInfo image = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &dec->profile_list,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
      .extent = {dec->extent.width, dec->extent.height, 1},
      .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_CONCURRENT, .queueFamilyIndexCount = 2,
      .pQueueFamilyIndices = dec->families,
   };
   if (dec->zero_copy) {
      if (!import_shared_picture(dec, buffer, pic))
         goto fail;
   } else {
      if (dec->CreateImage(dec->device, &image, NULL, &pic->image) != VK_SUCCESS)
         goto fail;
      VkMemoryRequirements req;
      dec->GetImageMemoryRequirements(dec->device, pic->image, &req);
      if (!allocate_memory(dec, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &pic->memory) ||
          dec->BindImageMemory(dec->device, pic->image, pic->memory, 0) != VK_SUCCESS)
         goto fail;
   }
   VkImageViewCreateInfo view = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = pic->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   if (dec->CreateImageView(dec->device, &view, NULL, &pic->view) != VK_SUCCESS)
      goto fail;
   vl_video_buffer_set_associated_data(buffer, &dec->base, pic, destroy_picture);
   return pic;
fail:
   destroy_picture(pic);
   return NULL;
}

static void
image_barrier(struct nouveau_vk_video *dec, VkCommandBuffer command, struct vk_picture *pic,
              VkImageLayout old_layout, VkImageLayout new_layout,
              VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
              VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
              uint32_t src_family, uint32_t dst_family)
{
   VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage, .srcAccessMask = src_access,
      .dstStageMask = dst_stage, .dstAccessMask = dst_access,
      .oldLayout = old_layout, .newLayout = new_layout,
      .srcQueueFamilyIndex = src_family,
      .dstQueueFamilyIndex = dst_family,
      .image = pic->image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   VkDependencyInfo dep = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier,
   };
   dec->CmdPipelineBarrier2(command, &dep);
}

static bool
finish_pipe(struct pipe_context *context)
{
   struct pipe_fence_handle *fence = NULL;
   context->flush(context, &fence, 0);
   bool done = fence && context->screen->fence_finish(context->screen, context, fence, 5000000000ull);
   context->screen->fence_reference(context->screen, &fence, NULL);
   return done;
}

static void
begin_frame(struct pipe_video_codec *codec, struct pipe_video_buffer *target,
            struct pipe_picture_desc *picture)
{
   struct nouveau_vk_video *dec = (struct nouveau_vk_video *)codec;
   dec->bitstream.size = 0;
   dec->bitstream.slice_count = 0;
   if (!target || target->buffer_format != PIPE_FORMAT_NV12 || target->interlaced ||
       picture->protected_playback)
      dec->failed = true;
}

static void
decode_bitstream(struct pipe_video_codec *codec, struct pipe_video_buffer *target,
                 struct pipe_picture_desc *picture, unsigned count,
                 const void *const *data, const unsigned *sizes)
{
   struct nouveau_vk_video *dec = (struct nouveau_vk_video *)codec;
   if (!dec->failed && !nouveau_vk_h264_append(&dec->bitstream, count, data, sizes))
      dec->failed = true;
}

static int
end_frame(struct pipe_video_codec *codec, struct pipe_video_buffer *target,
          struct pipe_picture_desc *picture)
{
   struct nouveau_vk_video *dec = (struct nouveau_vk_video *)codec;
   struct pipe_h264_picture_desc *h264 = (struct pipe_h264_picture_desc *)picture;
   struct nouveau_vk_h264_parameters params;
   VkVideoSessionParametersKHR parameters = VK_NULL_HANDLE;
   if (dec->failed || !target || target->buffer_format != PIPE_FORMAT_NV12 || target->interlaced ||
       target->width > dec->extent.width || target->height > dec->extent.height ||
       (target->width & 1) || (target->height & 1) ||
       !nouveau_vk_h264_index(&dec->bitstream, h264) ||
       !nouveau_vk_h264_translate(h264, &dec->bitstream, dec->h264_profile.stdProfileIdc,
                                  dec->h264_caps.maxLevelIdc, &params))
      goto fail;
   if ((params.sps.pic_width_in_mbs_minus1 + 1) * 16 != dec->extent.width ||
       (params.sps.pic_height_in_map_units_minus1 + 1) * 16 != dec->extent.height)
      goto fail;
   VkVideoDecodeH264SessionParametersAddInfoKHR add = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR,
      .stdSPSCount = 1, .pStdSPSs = &params.sps, .stdPPSCount = 1, .pStdPPSs = &params.pps,
   };
   VkVideoDecodeH264SessionParametersCreateInfoKHR h264_create = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR,
      .maxStdSPSCount = 1, .maxStdPPSCount = 1, .pParametersAddInfo = &add,
   };
   VkVideoSessionParametersCreateInfoKHR create = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
      .pNext = &h264_create, .videoSession = dec->session,
   };
   if (dec->CreateVideoSessionParametersKHR(dec->device, &create, NULL, &parameters) != VK_SUCCESS)
      goto fail;
   VkDeviceSize range = align64(dec->bitstream.size, dec->caps.minBitstreamBufferSizeAlignment);
   if (range + 256 > dec->bitstream_buffer.size) {
      destroy_buffer(dec, &dec->bitstream_buffer);
      if (!create_buffer(dec, range + 256, VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR,
                         &dec->bitstream_buffer))
         goto fail;
   }
   memset(dec->bitstream_buffer.map, 0, range + 256);
   memcpy(dec->bitstream_buffer.map, dec->bitstream.data, dec->bitstream.size);

   if (dec->zero_copy && !finish_pipe(codec->context))
      goto fail;

   struct vk_picture *dst = get_picture(dec, target);
   if (!dst)
      goto fail;
   struct vk_picture *refs[16];
   unsigned ref_count = 0;
   uint32_t used_slots = 0;
   StdVideoDecodeH264ReferenceInfo ref_info[16];
   VkVideoDecodeH264DpbSlotInfoKHR ref_ext[16];
   VkVideoPictureResourceInfoKHR ref_resources[17];
   VkVideoReferenceSlotInfoKHR ref_slots[17];
   for (unsigned i = 0; i < 16; i++) {
      if (!h264->ref[i])
         continue;
      struct vk_picture *ref = vl_video_buffer_get_associated_data(h264->ref[i], codec);
      if (!ref || ref == dst || !ref->submitted || ref->slot < 0 || dec->slots[ref->slot] != ref ||
          (used_slots & (1u << ref->slot)) ||
          !nouveau_vk_h264_reference(h264, i, &ref_info[ref_count]))
         goto fail;
      used_slots |= 1u << ref->slot;
      refs[ref_count] = ref;
      ref_resources[ref_count] = (VkVideoPictureResourceInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
         .codedExtent = dec->extent, .imageViewBinding = ref->view,
      };
      ref_ext[ref_count] = (VkVideoDecodeH264DpbSlotInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR,
         .pStdReferenceInfo = &ref_info[ref_count],
      };
      ref_slots[ref_count] = (VkVideoReferenceSlotInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
         .pNext = &ref_ext[ref_count], .slotIndex = ref->slot,
         .pPictureResource = &ref_resources[ref_count],
      };
      ref_count++;
   }
   int slot = -1;
   for (unsigned i = 0; i < 17; i++) {
      if (!(used_slots & (1u << i)) && dec->slots[i] == dst) {
         slot = i;
         break;
      }
   }
   if (slot < 0) {
      for (unsigned i = 0; i < 17; i++) {
         if (!(used_slots & (1u << i))) {
            slot = i;
            if (dec->slots[i])
               dec->slots[i]->slot = -1;
            dec->slots[i] = dst;
            dst->slot = i;
            break;
         }
      }
   }
   if (slot < 0)
      goto fail;
   ref_resources[ref_count] = (VkVideoPictureResourceInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
      .codedExtent = dec->extent, .imageViewBinding = dst->view,
   };
   ref_slots[ref_count] = (VkVideoReferenceSlotInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
      .slotIndex = -1, .pPictureResource = &ref_resources[ref_count],
   };
   VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   for (unsigned i = 0; i < (dec->zero_copy ? 1 : 2); i++) {
      if (dec->ResetCommandPool(dec->device, dec->pools[i], 0) != VK_SUCCESS ||
          dec->BeginCommandBuffer(dec->commands[i], &begin) != VK_SUCCESS)
         goto fail;
   }
   VkCommandBuffer command = dec->commands[0];
   for (unsigned i = 0; i < ref_count; i++)
      image_barrier(dec, command, refs[i], dec->zero_copy ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                    VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                    VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR,
                    dec->zero_copy ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED,
                    dec->zero_copy ? dec->families[0] : VK_QUEUE_FAMILY_IGNORED);
   image_barrier(dec, command, dst,
                 dst->initialized ? (dec->zero_copy ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR) : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                 VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
                 dec->zero_copy ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED,
                 dec->zero_copy ? dec->families[0] : VK_QUEUE_FAMILY_IGNORED);
   VkVideoBeginCodingInfoKHR coding = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
      .videoSession = dec->session, .videoSessionParameters = parameters,
      .referenceSlotCount = ref_count + 1, .pReferenceSlots = ref_slots,
   };
   dec->CmdBeginVideoCodingKHR(command, &coding);
   if (!dec->started) {
      VkVideoCodingControlInfoKHR reset = {
         .sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
         .flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR,
      };
      dec->CmdControlVideoCodingKHR(command, &reset);
   }
   StdVideoDecodeH264ReferenceInfo dst_std = {
      .FrameNum = params.picture.frame_num,
      .PicOrderCnt = {params.picture.PicOrderCnt[0], params.picture.PicOrderCnt[1]},
   };
   VkVideoDecodeH264DpbSlotInfoKHR dst_ext = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR, .pStdReferenceInfo = &dst_std,
   };
   VkVideoReferenceSlotInfoKHR setup = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
      .pNext = &dst_ext, .slotIndex = slot, .pPictureResource = &ref_resources[ref_count],
   };
   VkVideoDecodeH264PictureInfoKHR h264_info = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR,
      .pStdPictureInfo = &params.picture, .sliceCount = dec->bitstream.slice_count,
      .pSliceOffsets = dec->bitstream.slice_offsets,
   };
   VkVideoDecodeInfoKHR decode = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR, .pNext = &h264_info,
      .srcBuffer = dec->bitstream_buffer.buffer, .srcBufferRange = range,
      .dstPictureResource = ref_resources[ref_count], .pSetupReferenceSlot = &setup,
      .referenceSlotCount = ref_count, .pReferenceSlots = ref_slots,
   };
   dec->CmdDecodeVideoKHR(command, &decode);
   VkVideoEndCodingInfoKHR end = {.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
   dec->CmdEndVideoCodingKHR(command, &end);
   if (dec->zero_copy) {
      for (unsigned i = 0; i < ref_count; i++)
         image_barrier(dec, command, refs[i], VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                       VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                       VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR, VK_PIPELINE_STAGE_2_NONE, 0,
                       dec->families[0], VK_QUEUE_FAMILY_FOREIGN_EXT);
      image_barrier(dec, command, dst, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                    VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_NONE, 0,
                    dec->families[0], VK_QUEUE_FAMILY_FOREIGN_EXT);
      if (dec->EndCommandBuffer(command) != VK_SUCCESS ||
          dec->ResetFences(dec->device, 1, &dec->fence) != VK_SUCCESS)
         goto fail;
      VkSubmitInfo direct = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1, .pCommandBuffers = &command,
      };
      if (dec->QueueSubmit(dec->queues[0], 1, &direct, dec->fence) != VK_SUCCESS ||
          dec->WaitForFences(dec->device, 1, &dec->fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) {
         dec->DeviceWaitIdle(dec->device);
         goto fail;
      }
      goto gpu_done;
   }
   image_barrier(dec, command, dst, VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
                  VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_NONE, 0,
                  VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
   if (dec->EndCommandBuffer(command) != VK_SUCCESS)
      goto fail;

   command = dec->commands[1];
   VkBufferImageCopy regions[2] = {
      {.imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1},
       .imageExtent = {dec->extent.width, dec->extent.height, 1}},
      {.bufferOffset = (VkDeviceSize)dec->extent.width * dec->extent.height,
       .imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1},
       .imageExtent = {dec->extent.width / 2, dec->extent.height / 2, 1}},
   };
   dec->CmdCopyImageToBuffer(command, dst->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             dec->readback.buffer, 2, regions);
   image_barrier(dec, command, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_NONE, 0,
                  VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
   VkBufferMemoryBarrier2 host = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = dec->readback.buffer, .size = VK_WHOLE_SIZE,
   };
   VkDependencyInfo host_dep = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &host,
   };
   dec->CmdPipelineBarrier2(command, &host_dep);
   if (dec->EndCommandBuffer(command) != VK_SUCCESS ||
       dec->ResetFences(dec->device, 1, &dec->fence) != VK_SUCCESS)
      goto fail;
   VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
      .pCommandBuffers = &dec->commands[0], .signalSemaphoreCount = 1,
      .pSignalSemaphores = &dec->semaphore,
   };
   if (dec->QueueSubmit(dec->queues[0], 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
      goto fail;
   VkPipelineStageFlags wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   submit = (VkSubmitInfo) {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
      .pCommandBuffers = &dec->commands[1], .waitSemaphoreCount = 1,
      .pWaitSemaphores = &dec->semaphore, .pWaitDstStageMask = &wait,
   };
   if (dec->QueueSubmit(dec->queues[1], 1, &submit, dec->fence) != VK_SUCCESS ||
       dec->WaitForFences(dec->device, 1, &dec->fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) {
      dec->DeviceWaitIdle(dec->device);
      goto fail;
   }
gpu_done:
   dec->DestroyVideoSessionParametersKHR(dec->device, parameters, NULL);
   parameters = VK_NULL_HANDLE;
   dst->initialized = dst->submitted = true;
   dec->started = true;
   if (dec->zero_copy) {
      codec->context->memory_barrier(codec->context, PIPE_BARRIER_TEXTURE | PIPE_BARRIER_FRAMEBUFFER);
      if (!finish_pipe(codec->context))
         goto fail;
      dec->completed_frames++;
      return 0;
   }
   struct pipe_resource *resources[3] = {0};
   target->get_resources(target, resources);
   if (!resources[0] || !resources[1])
      goto fail;
   for (unsigned plane = 0; plane < 2; plane++) {
      struct pipe_box box = {
         .width = target->width >> plane, .height = target->height >> plane, .depth = 1,
      };
      const uint8_t *data = dec->readback.map;
      if (plane)
         data += dec->extent.width * dec->extent.height;
      codec->context->texture_subdata(codec->context, resources[plane], 0, PIPE_MAP_WRITE,
                                        &box, data, dec->extent.width, 0);
      dec->cpu_frame_bytes += (uint64_t)box.width * box.height * (plane ? 2 : 1);
   }
   struct pipe_fence_handle *fence = NULL;
   codec->context->flush(codec->context, &fence, 0);
   bool done = fence && codec->context->screen->fence_finish(codec->context->screen,
                                                            codec->context, fence, 5000000000ull);
   codec->context->screen->fence_reference(codec->context->screen, &fence, NULL);
   if (!done)
      goto fail;
   dec->completed_frames++;
   return 0;

fail:
   if (parameters)
      dec->DestroyVideoSessionParametersKHR(dec->device, parameters, NULL);
   dec->failed = true;
   return -1;
}

static void
flush_decoder(struct pipe_video_codec *codec)
{
}

struct pipe_video_codec *
nouveau_vk_video_create(struct pipe_context *context, const struct pipe_video_codec *templ)
{
   if (templ->entrypoint != PIPE_VIDEO_ENTRYPOINT_BITSTREAM || templ->max_references > 16)
      return NULL;
   struct nouveau_vk_video *dec = create_device(nouveau_screen(context->screen), templ->profile);
   if (!dec)
      return NULL;
   dec->base = *templ;
   pipe_reference_init(&dec->base.reference, 1);
   dec->base.context = context;
   dec->base.destroy = destroy_decoder;
   dec->base.begin_frame = begin_frame;
   dec->base.decode_bitstream = decode_bitstream;
   dec->base.end_frame = end_frame;
   dec->base.flush = flush_decoder;
   if (!create_session(dec)) {
      destroy_decoder(&dec->base);
      return NULL;
   }
   fprintf(stderr, "nouveau-vk-video: H.264 session initialized on NVK, %ux%u, %s\n",
           dec->extent.width, dec->extent.height,
           dec->zero_copy ? "direct DMA-BUF zero-copy" : "synchronous CPU frame transfer");
   return &dec->base;
}
