/*
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "vulkan/vulkan_core.h"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <android/hardware/graphics/common/1.0/types.h>
#include <android/hardware_buffer.h>
#include <grallocusage/GrallocUsageConversion.h>
#include <graphicsenv/GraphicsEnv.h>
#include <hardware/gralloc.h>
#include <hardware/gralloc1.h>
#include <log/log.h>
#include <sync/sync.h>
#include <system/window.h>
#include <ui/BufferQueueDefs.h>
#include <utils/StrongPointer.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "driver.h"

using PixelFormat = aidl::android::hardware::graphics::common::PixelFormat;
using DataSpace = aidl::android::hardware::graphics::common::Dataspace;
using android::hardware::graphics::common::V1_0::BufferUsage;

namespace vulkan {
namespace driver {

namespace {

static uint64_t convertGralloc1ToBufferUsage(uint64_t producerUsage,
                                             uint64_t consumerUsage) {
    static_assert(uint64_t(GRALLOC1_CONSUMER_USAGE_CPU_READ_OFTEN) ==
                      uint64_t(GRALLOC1_PRODUCER_USAGE_CPU_READ_OFTEN),
                  "expected ConsumerUsage and ProducerUsage CPU_READ_OFTEN "
                  "bits to match");
    uint64_t merged = producerUsage | consumerUsage;
    if ((merged & (GRALLOC1_CONSUMER_USAGE_CPU_READ_OFTEN)) ==
        GRALLOC1_CONSUMER_USAGE_CPU_READ_OFTEN) {
        merged &= ~uint64_t(GRALLOC1_CONSUMER_USAGE_CPU_READ_OFTEN);
        merged |= BufferUsage::CPU_READ_OFTEN;
    }
    if ((merged & (GRALLOC1_PRODUCER_USAGE_CPU_WRITE_OFTEN)) ==
        GRALLOC1_PRODUCER_USAGE_CPU_WRITE_OFTEN) {
        merged &= ~uint64_t(GRALLOC1_PRODUCER_USAGE_CPU_WRITE_OFTEN);
        merged |= BufferUsage::CPU_WRITE_OFTEN;
    }
    return merged;
}

const VkSurfaceTransformFlagsKHR kSupportedTransforms =
    VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR |
    VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
    VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR |
    VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR |
    VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR |
    VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR |
    VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_180_BIT_KHR |
    VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR |
    VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;

VkSurfaceTransformFlagBitsKHR TranslateNativeToVulkanTransform(int native) {
    // Native and Vulkan transforms are isomorphic, but are represented
    // differently. Vulkan transforms are built up of an optional horizontal
    // mirror, followed by a clockwise 0/90/180/270-degree rotation. Native
    // transforms are built up from a horizontal flip, vertical flip, and
    // 90-degree rotation, all optional but always in that order.

    switch (native) {
        case 0:
            return VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        case NATIVE_WINDOW_TRANSFORM_FLIP_H:
            return VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR;
        case NATIVE_WINDOW_TRANSFORM_FLIP_V:
            return VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_180_BIT_KHR;
        case NATIVE_WINDOW_TRANSFORM_ROT_180:
            return VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR;
        case NATIVE_WINDOW_TRANSFORM_ROT_90:
            return VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
        case NATIVE_WINDOW_TRANSFORM_FLIP_H | NATIVE_WINDOW_TRANSFORM_ROT_90:
            return VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR;
        case NATIVE_WINDOW_TRANSFORM_FLIP_V | NATIVE_WINDOW_TRANSFORM_ROT_90:
            return VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR;
        case NATIVE_WINDOW_TRANSFORM_ROT_270:
            return VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
        case NATIVE_WINDOW_TRANSFORM_INVERSE_DISPLAY:
        default:
            return VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }
}

int TranslateVulkanToNativeTransform(VkSurfaceTransformFlagBitsKHR transform) {
    switch (transform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_ROT_90;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_ROT_180;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_ROT_270;
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_FLIP_H;
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_FLIP_H |
                   NATIVE_WINDOW_TRANSFORM_ROT_90;
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_180_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_FLIP_V;
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_FLIP_V |
                   NATIVE_WINDOW_TRANSFORM_ROT_90;
        case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:
        case VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR:
        default:
            return 0;
    }
}

int InvertTransformToNative(VkSurfaceTransformFlagBitsKHR transform) {
    switch (transform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_ROT_270;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_ROT_180;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_ROT_90;
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_FLIP_H;
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_FLIP_H |
                   NATIVE_WINDOW_TRANSFORM_ROT_90;
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_180_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_FLIP_V;
        case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR:
            return NATIVE_WINDOW_TRANSFORM_FLIP_V |
                   NATIVE_WINDOW_TRANSFORM_ROT_90;
        case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:
        case VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR:
        default:
            return 0;
    }
}

const static VkColorSpaceKHR colorSpaceSupportedByVkEXTSwapchainColorspace[] = {
    VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT,
    VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT,
    VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT,
    VK_COLOR_SPACE_BT709_LINEAR_EXT,
    VK_COLOR_SPACE_BT709_NONLINEAR_EXT,
    VK_COLOR_SPACE_BT2020_LINEAR_EXT,
    VK_COLOR_SPACE_HDR10_ST2084_EXT,
    VK_COLOR_SPACE_HDR10_HLG_EXT,
    VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT,
    VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT,
    VK_COLOR_SPACE_PASS_THROUGH_EXT,
    VK_COLOR_SPACE_DCI_P3_LINEAR_EXT};

const static VkColorSpaceKHR
    colorSpaceSupportedByVkEXTSwapchainColorspaceOnFP16SurfaceOnly[] = {
        VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT,
        VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT};

class TimingInfo {
   public:
    TimingInfo(const VkPresentTimeGOOGLE* qp, uint64_t nativeFrameId)
        : vals_{qp->presentID, qp->desiredPresentTime, 0, 0, 0},
          native_frame_id_(nativeFrameId) {}
    bool ready() const {
        return (timestamp_desired_present_time_ !=
                        NATIVE_WINDOW_TIMESTAMP_PENDING &&
                timestamp_actual_present_time_ !=
                        NATIVE_WINDOW_TIMESTAMP_PENDING &&
                timestamp_render_complete_time_ !=
                        NATIVE_WINDOW_TIMESTAMP_PENDING &&
                timestamp_composition_latch_time_ !=
                        NATIVE_WINDOW_TIMESTAMP_PENDING);
    }
    void calculate(int64_t rdur) {
        bool anyTimestampInvalid =
                (timestamp_actual_present_time_ ==
                        NATIVE_WINDOW_TIMESTAMP_INVALID) ||
                (timestamp_render_complete_time_ ==
                        NATIVE_WINDOW_TIMESTAMP_INVALID) ||
                (timestamp_composition_latch_time_ ==
                        NATIVE_WINDOW_TIMESTAMP_INVALID);
        if (anyTimestampInvalid) {
            ALOGE("Unexpectedly received invalid timestamp.");
            vals_.actualPresentTime = 0;
            vals_.earliestPresentTime = 0;
            vals_.presentMargin = 0;
            return;
        }

        vals_.actualPresentTime =
                static_cast<uint64_t>(timestamp_actual_present_time_);
        int64_t margin = (timestamp_composition_latch_time_ -
                           timestamp_render_complete_time_);
        // Calculate vals_.earliestPresentTime, and potentially adjust
        // vals_.presentMargin.  The initial value of vals_.earliestPresentTime
        // is vals_.actualPresentTime.  If we can subtract rdur (the duration
        // of a refresh cycle) from vals_.earliestPresentTime (and also from
        // vals_.presentMargin) and still leave a positive margin, then we can
        // report to the application that it could have presented earlier than
        // it did (per the extension specification).  If for some reason, we
        // can do this subtraction repeatedly, we do, since
        // vals_.earliestPresentTime really is supposed to be the "earliest".
        int64_t early_time = timestamp_actual_present_time_;
        while ((margin > rdur) &&
               ((early_time - rdur) > timestamp_composition_latch_time_)) {
            early_time -= rdur;
            margin -= rdur;
        }
        vals_.earliestPresentTime = static_cast<uint64_t>(early_time);
        vals_.presentMargin = static_cast<uint64_t>(margin);
    }
    void get_values(VkPastPresentationTimingGOOGLE* values) const {
        *values = vals_;
    }

   public:
    VkPastPresentationTimingGOOGLE vals_ { 0, 0, 0, 0, 0 };

    uint64_t native_frame_id_ { 0 };
    int64_t timestamp_desired_present_time_{ NATIVE_WINDOW_TIMESTAMP_PENDING };
    int64_t timestamp_actual_present_time_ { NATIVE_WINDOW_TIMESTAMP_PENDING };
    int64_t timestamp_render_complete_time_ { NATIVE_WINDOW_TIMESTAMP_PENDING };
    int64_t timestamp_composition_latch_time_
            { NATIVE_WINDOW_TIMESTAMP_PENDING };
};

struct Surface {
    android::sp<ANativeWindow> window;
    VkSwapchainKHR swapchain_handle;
    uint64_t consumer_usage;

    // Indicate whether this surface has been used by a swapchain, no matter the
    // swapchain is still current or has been destroyed.
    bool used_by_swapchain;
};

VkSurfaceKHR HandleFromSurface(Surface* surface) {
    return VkSurfaceKHR(reinterpret_cast<uint64_t>(surface));
}

Surface* SurfaceFromHandle(VkSurfaceKHR handle) {
    return reinterpret_cast<Surface*>(handle);
}

// Maximum number of TimingInfo structs to keep per swapchain:
enum { MAX_TIMING_INFOS = 10 };
// Minimum number of frames to look for in the past (so we don't cause
// syncronous requests to Surface Flinger):
enum { MIN_NUM_FRAMES_AGO = 5 };

bool IsSharedPresentMode(VkPresentModeKHR mode) {
    return mode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR ||
        mode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR;
}

struct Swapchain {
    Swapchain(Surface& surface_,
              uint32_t num_images_,
              VkPresentModeKHR present_mode,
              int pre_transform_,
              int64_t refresh_duration_)
        : surface(surface_),
          num_images(num_images_),
          mailbox_mode(present_mode == VK_PRESENT_MODE_MAILBOX_KHR),
          pre_transform(pre_transform_),
          frame_timestamps_enabled(false),
          refresh_duration(refresh_duration_),
          acquire_next_image_timeout(-1),
          shared(IsSharedPresentMode(present_mode)) {
    }

    VkResult get_refresh_duration(uint64_t& outRefreshDuration)
    {
        ANativeWindow* window = surface.window.get();
        int err = native_window_get_refresh_cycle_duration(
            window,
            &refresh_duration);
        if (err != android::OK) {
            ALOGE("%s:native_window_get_refresh_cycle_duration failed: %s (%d)",
                __func__, strerror(-err), err );
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        outRefreshDuration = refresh_duration;
        return VK_SUCCESS;
    }

    Surface& surface;
    uint32_t num_images;
    bool mailbox_mode;
    int pre_transform;
    bool frame_timestamps_enabled;
    int64_t refresh_duration;
    nsecs_t acquire_next_image_timeout;
    bool shared;

    struct Image {
        Image()
            : image(VK_NULL_HANDLE),
              dequeue_fence(-1),
              release_fence(-1),
              dequeued(false) {}
        VkImage image;
        // If the image is bound to memory, an sp to the underlying gralloc buffer.
        // Otherwise, nullptr; the image will be bound to memory as part of
        // AcquireNextImage.
        android::sp<ANativeWindowBuffer> buffer;
        // The fence is only valid when the buffer is dequeued, and should be
        // -1 any other time. When valid, we own the fd, and must ensure it is
        // closed: either by closing it explicitly when queueing the buffer,
        // or by passing ownership e.g. to ANativeWindow::cancelBuffer().
        int dequeue_fence;
        // This fence is a dup of the sync fd returned from the driver via
        // vkQueueSignalReleaseImageANDROID upon vkQueuePresentKHR. We must
        // ensure it is closed upon re-presenting or releasing the image.
        int release_fence;
        bool dequeued;
    } images[android::BufferQueueDefs::NUM_BUFFER_SLOTS];

    std::vector<TimingInfo> timing;
};

VkSwapchainKHR HandleFromSwapchain(Swapchain* swapchain) {
    return VkSwapchainKHR(reinterpret_cast<uint64_t>(swapchain));
}

Swapchain* SwapchainFromHandle(VkSwapchainKHR handle) {
    return reinterpret_cast<Swapchain*>(handle);
}

static bool IsFencePending(int fd) {
    if (fd < 0)
        return false;

    errno = 0;
    return sync_wait(fd, 0 /* timeout */) == -1 && errno == ETIME;
}

void ReleaseSwapchainImage(VkDevice device,
                           bool shared_present,
                           ANativeWindow* window,
                           int release_fence,
                           Swapchain::Image& image,
                           bool defer_if_pending) {
    ATRACE_CALL();

    ALOG_ASSERT(release_fence == -1 || image.dequeued,
                "ReleaseSwapchainImage: can't provide a release fence for "
                "non-dequeued images");

    if (image.dequeued) {
        if (release_fence >= 0) {
            // We get here from vkQueuePresentKHR. The application is
            // responsible for creating an execution dependency chain from
            // vkAcquireNextImage (dequeue_fence) to vkQueuePresentKHR
            // (release_fence), so we can drop the dequeue_fence here.
            if (image.dequeue_fence >= 0)
                close(image.dequeue_fence);
        } else {
            // We get here during swapchain destruction, or various serious
            // error cases e.g. when we can't create the release_fence during
            // vkQueuePresentKHR. In non-error cases, the dequeue_fence should
            // have already signalled, since the swapchain images are supposed
            // to be idle before the swapchain is destroyed. In error cases,
            // there may be rendering in flight to the image, but since we
            // weren't able to create a release_fence, waiting for the
            // dequeue_fence is about the best we can do.
            release_fence = image.dequeue_fence;
        }
        image.dequeue_fence = -1;

        // It's invalid to call cancelBuffer on a shared buffer
        if (window && !shared_present) {
            window->cancelBuffer(window, image.buffer.get(), release_fence);
        } else {
            if (release_fence >= 0) {
                sync_wait(release_fence, -1 /* forever */);
                close(release_fence);
            }
        }
        release_fence = -1;
        image.dequeued = false;
    }

    if (defer_if_pending && IsFencePending(image.release_fence))
        return;

    if (image.release_fence >= 0) {
        close(image.release_fence);
        image.release_fence = -1;
    }

    if (image.image) {
        ATRACE_BEGIN("DestroyImage");
        GetData(device).driver.DestroyImage(device, image.image, nullptr);
        ATRACE_END();
        image.image = VK_NULL_HANDLE;
    }

    image.buffer.clear();
}

void OrphanSwapchain(VkDevice device, Swapchain* swapchain) {
    if (swapchain->surface.swapchain_handle != HandleFromSwapchain(swapchain))
        return;
    for (uint32_t i = 0; i < swapchain->num_images; i++) {
        if (!swapchain->images[i].dequeued) {
            ReleaseSwapchainImage(device, swapchain->shared, nullptr, -1,
                                  swapchain->images[i], true);
        }
    }
    swapchain->surface.swapchain_handle = VK_NULL_HANDLE;
    swapchain->timing.clear();
}

uint32_t get_num_ready_timings(Swapchain& swapchain) {
    if (swapchain.timing.size() < MIN_NUM_FRAMES_AGO) {
        return 0;
    }

    uint32_t num_ready = 0;
    const size_t num_timings = swapchain.timing.size() - MIN_NUM_FRAMES_AGO + 1;
    for (uint32_t i = 0; i < num_timings; i++) {
        TimingInfo& ti = swapchain.timing[i];
        if (ti.ready()) {
            // This TimingInfo is ready to be reported to the user.  Add it
            // to the num_ready.
            num_ready++;
            continue;
        }
        // This TimingInfo is not yet ready to be reported to the user,
        // and so we should look for any available timestamps that
        // might make it ready.
        int64_t desired_present_time = 0;
        int64_t render_complete_time = 0;
        int64_t composition_latch_time = 0;
        int64_t actual_present_time = 0;
        // Obtain timestamps:
        int err = native_window_get_frame_timestamps(
            swapchain.surface.window.get(), ti.native_frame_id_,
            &desired_present_time, &render_complete_time,
            &composition_latch_time,
            nullptr,  //&first_composition_start_time,
            nullptr,  //&last_composition_start_time,
            nullptr,  //&composition_finish_time,
            &actual_present_time,
            nullptr,  //&dequeue_ready_time,
            nullptr /*&reads_done_time*/);

        if (err != android::OK) {
            continue;
        }

        // Record the timestamp(s) we received, and then see if this TimingInfo
        // is ready to be reported to the user:
        ti.timestamp_desired_present_time_ = desired_present_time;
        ti.timestamp_actual_present_time_ = actual_present_time;
        ti.timestamp_render_complete_time_ = render_complete_time;
        ti.timestamp_composition_latch_time_ = composition_latch_time;

        if (ti.ready()) {
            // The TimingInfo has received enough timestamps, and should now
            // use those timestamps to calculate the info that should be
            // reported to the user:
            ti.calculate(swapchain.refresh_duration);
            num_ready++;
        }
    }
    return num_ready;
}

void copy_ready_timings(Swapchain& swapchain,
                        uint32_t* count,
                        VkPastPresentationTimingGOOGLE* timings) {
    if (swapchain.timing.empty()) {
        *count = 0;
        return;
    }

    size_t last_ready = swapchain.timing.size() - 1;
    while (!swapchain.timing[last_ready].ready()) {
        if (last_ready == 0) {
            *count = 0;
            return;
        }
        last_ready--;
    }

    uint32_t num_copied = 0;
    int32_t num_to_remove = 0;
    for (uint32_t i = 0; i <= last_ready && num_copied < *count; i++) {
        const TimingInfo& ti = swapchain.timing[i];
        if (ti.ready()) {
            ti.get_values(&timings[num_copied]);
            num_copied++;
        }
        num_to_remove++;
    }

    // Discard old frames that aren't ready if newer frames are ready.
    // We don't expect to get the timing info for those old frames.
    swapchain.timing.erase(swapchain.timing.begin(),
                           swapchain.timing.begin() + num_to_remove);

    *count = num_copied;
}

PixelFormat GetNativePixelFormat(VkFormat format) {
    PixelFormat native_format = PixelFormat::RGBA_8888;
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            native_format = PixelFormat::RGBA_8888;
            break;
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
            native_format = PixelFormat::RGB_565;
            break;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            native_format = PixelFormat::RGBA_FP16;
            break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            native_format = PixelFormat::RGBA_1010102;
            break;
        case VK_FORMAT_R8_UNORM:
            native_format = PixelFormat::R_8;
            break;
        case VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16:
            native_format = PixelFormat::RGBA_10101010;
            break;
        default:
            ALOGV("unsupported swapchain format %d", format);
            break;
    }
    return native_format;
}

DataSpace GetNativeDataspace(VkColorSpaceKHR colorspace, VkFormat format) {
    switch (colorspace) {
        case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
            return DataSpace::SRGB;
        case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT:
            return DataSpace::DISPLAY_P3;
        case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
            return DataSpace::SCRGB_LINEAR;
        case VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT:
            return DataSpace::SCRGB;
        case VK_COLOR_SPACE_DCI_P3_LINEAR_EXT:
            return DataSpace::DCI_P3_LINEAR;
        case VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT:
            return DataSpace::DCI_P3;
        case VK_COLOR_SPACE_BT709_LINEAR_EXT:
            return DataSpace::SRGB_LINEAR;
        case VK_COLOR_SPACE_BT709_NONLINEAR_EXT:
            return DataSpace::SRGB;
        case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
            if (format == VK_FORMAT_R16G16B16A16_SFLOAT) {
                return DataSpace::BT2020_LINEAR_EXTENDED;
            } else {
                return DataSpace::BT2020_LINEAR;
            }
        case VK_COLOR_SPACE_HDR10_ST2084_EXT:
            return DataSpace::BT2020_PQ;
        case VK_COLOR_SPACE_DOLBYVISION_EXT:
            return DataSpace::BT2020_PQ;
        case VK_COLOR_SPACE_HDR10_HLG_EXT:
            return DataSpace::BT2020_HLG;
        case VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT:
            return DataSpace::ADOBE_RGB_LINEAR;
        case VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT:
            return DataSpace::ADOBE_RGB;
        // Pass through is intended to allow app to provide data that is passed
        // to the display system without modification.
        case VK_COLOR_SPACE_PASS_THROUGH_EXT:
            return DataSpace::ARBITRARY;

        default:
            // This indicates that we don't know about the
            // dataspace specified and we should indicate that
            // it's unsupported
            return DataSpace::UNKNOWN;
    }
}

}  // anonymous namespace

VKAPI_ATTR
VkResult CreateAndroidSurfaceKHR(
    VkInstance instance,
    const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* allocator,
    VkSurfaceKHR* out_surface) {
    ATRACE_CALL();

    if (!allocator)
        allocator = &GetData(instance).allocator;
    void* mem = allocator->pfnAllocation(allocator->pUserData, sizeof(Surface),
                                         alignof(Surface),
                                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (!mem)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    Surface* surface = new (mem) Surface;

    surface->window = pCreateInfo->window;
    surface->swapchain_handle = VK_NULL_HANDLE;
    surface->used_by_swapchain = false;
    int err = native_window_get_consumer_usage(surface->window.get(),
                                               &surface->consumer_usage);
    if (err != android::OK) {
        ALOGE("native_window_get_consumer_usage() failed: %s (%d)",
              strerror(-err), err);
        surface->~Surface();
        allocator->pfnFree(allocator->pUserData, surface);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    err =
        native_window_api_connect(surface->window.get(), NATIVE_WINDOW_API_EGL);
    if (err != android::OK) {
        ALOGE("native_window_api_connect() failed: %s (%d)", strerror(-err),
              err);
        surface->~Surface();
        allocator->pfnFree(allocator->pUserData, surface);
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    }

    *out_surface = HandleFromSurface(surface);
    return VK_SUCCESS;
}

VKAPI_ATTR
void DestroySurfaceKHR(VkInstance instance,
                       VkSurfaceKHR surface_handle,
                       const VkAllocationCallbacks* allocator) {
    ATRACE_CALL();

    Surface* surface = SurfaceFromHandle(surface_handle);
    if (!surface)
        return;
    native_window_api_disconnect(surface->window.get(), NATIVE_WINDOW_API_EGL);
    ALOGV_IF(surface->swapchain_handle != VK_NULL_HANDLE,
             "destroyed VkSurfaceKHR 0x%" PRIx64
             " has active VkSwapchainKHR 0x%" PRIx64,
             reinterpret_cast<uint64_t>(surface_handle),
             reinterpret_cast<uint64_t>(surface->swapchain_handle));
    surface->~Surface();
    if (!allocator)
        allocator = &GetData(instance).allocator;
    allocator->pfnFree(allocator->pUserData, surface);
}

VKAPI_ATTR
VkResult GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice /*pdev*/,
                                            uint32_t /*queue_family*/,
                                            VkSurfaceKHR /*surface_handle*/,
                                            VkBool32* supported) {
    *supported = VK_TRUE;
    return VK_SUCCESS;
}

VKAPI_ATTR
VkResult GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice pdev,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* capabilities) {
    ATRACE_CALL();

    // Implement in terms of GetPhysicalDeviceSurfaceCapabilities2KHR

    VkPhysicalDeviceSurfaceInfo2KHR info2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
        nullptr,
        surface
    };

    VkSurfaceCapabilities2KHR caps2 = {
        VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
        nullptr,
        {},
    };

    VkResult result = GetPhysicalDeviceSurfaceCapabilities2KHR(pdev, &info2, &caps2);
    *capabilities = caps2.surfaceCapabilities;
    return result;
}

// Does the call-twice and VK_INCOMPLETE handling for querying lists
// of things, where we already have the full set built in a vector.
template <typename T>
VkResult CopyWithIncomplete(std::vector<T> const& things,
        T* callerPtr, uint32_t* callerCount) {
    VkResult result = VK_SUCCESS;
    if (callerPtr) {
        if (things.size() > *callerCount)
            result = VK_INCOMPLETE;
        *callerCount = std::min(uint32_t(things.size()), *callerCount);
        std::copy(things.begin(), things.begin() + *callerCount, callerPtr);
    } else {
        *callerCount = things.size();
    }
    return result;
}

VKAPI_ATTR
VkResult GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pdev,
                                            VkSurfaceKHR surface_handle,
                                            uint32_t* count,
                                            VkSurfaceFormatKHR* formats) {
    ATRACE_CALL();

    const InstanceData& instance_data = GetData(pdev);

    uint64_t consumer_usage = 0;
    bool colorspace_ext =
        instance_data.hook_extensions.test(ProcHook::EXT_swapchain_colorspace);
    if (surface_handle == VK_NULL_HANDLE) {
        ProcHook::Extension surfaceless = ProcHook::GOOGLE_surfaceless_query;
        bool surfaceless_enabled =
            instance_data.hook_extensions.test(surfaceless);
        if (!surfaceless_enabled) {
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        // Support for VK_GOOGLE_surfaceless_query.

        // TODO(b/203826952): research proper value; temporarily use the
        // values seen on Pixel
        consumer_usage = AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    } else {
        Surface& surface = *SurfaceFromHandle(surface_handle);
        consumer_usage = surface.consumer_usage;
    }

    AHardwareBuffer_Desc desc = {};
    desc.width = 1;
    desc.height = 1;
    desc.layers = 1;
    desc.usage = consumer_usage | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;

    // We must support R8G8B8A8
    std::vector<VkSurfaceFormatKHR> all_formats = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };

    VkFormat format = VK_FORMAT_UNDEFINED;
    if (colorspace_ext) {
        for (VkColorSpaceKHR colorSpace :
             colorSpaceSupportedByVkEXTSwapchainColorspace) {
            format = VK_FORMAT_R8G8B8A8_UNORM;
            if (GetNativeDataspace(colorSpace, format) != DataSpace::UNKNOWN) {
                all_formats.emplace_back(
                    VkSurfaceFormatKHR{format, colorSpace});
            }

            format = VK_FORMAT_R8G8B8A8_SRGB;
            if (GetNativeDataspace(colorSpace, format) != DataSpace::UNKNOWN) {
                all_formats.emplace_back(
                    VkSurfaceFormatKHR{format, colorSpace});
            }
        }
    }

    // NOTE: Any new formats that are added must be coordinated across different
    // Android users.  This includes the ANGLE team (a layered implementation of
    // OpenGL-ES).

    format = VK_FORMAT_R5G6B5_UNORM_PACK16;
    desc.format = AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
    if (AHardwareBuffer_isSupported(&desc)) {
        all_formats.emplace_back(
            VkSurfaceFormatKHR{format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
        if (colorspace_ext) {
            for (VkColorSpaceKHR colorSpace :
                 colorSpaceSupportedByVkEXTSwapchainColorspace) {
                if (GetNativeDataspace(colorSpace, format) !=
                    DataSpace::UNKNOWN) {
                    all_formats.emplace_back(
                        VkSurfaceFormatKHR{format, colorSpace});
                }
            }
        }
    }

    format = VK_FORMAT_R16G16B16A16_SFLOAT;
    desc.format = AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT;
    if (AHardwareBuffer_isSupported(&desc)) {
        all_formats.emplace_back(
            VkSurfaceFormatKHR{format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
        if (colorspace_ext) {
            for (VkColorSpaceKHR colorSpace :
                 colorSpaceSupportedByVkEXTSwapchainColorspace) {
                if (GetNativeDataspace(colorSpace, format) !=
                    DataSpace::UNKNOWN) {
                    all_formats.emplace_back(
                        VkSurfaceFormatKHR{format, colorSpace});
                }
            }

            for (
                VkColorSpaceKHR colorSpace :
                colorSpaceSupportedByVkEXTSwapchainColorspaceOnFP16SurfaceOnly) {
                if (GetNativeDataspace(colorSpace, format) !=
                    DataSpace::UNKNOWN) {
                    all_formats.emplace_back(
                        VkSurfaceFormatKHR{format, colorSpace});
                }
            }
        }
    }

    format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    desc.format = AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
    if (AHardwareBuffer_isSupported(&desc)) {
        all_formats.emplace_back(
            VkSurfaceFormatKHR{format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
        if (colorspace_ext) {
            for (VkColorSpaceKHR colorSpace :
                 colorSpaceSupportedByVkEXTSwapchainColorspace) {
                if (GetNativeDataspace(colorSpace, format) !=
                    DataSpace::UNKNOWN) {
                    all_formats.emplace_back(
                        VkSurfaceFormatKHR{format, colorSpace});
                }
            }
        }
    }

    format = VK_FORMAT_R8_UNORM;
    desc.format = AHARDWAREBUFFER_FORMAT_R8_UNORM;
    if (AHardwareBuffer_isSupported(&desc)) {
        if (colorspace_ext) {
            all_formats.emplace_back(
                VkSurfaceFormatKHR{format, VK_COLOR_SPACE_PASS_THROUGH_EXT});
        }
    }

    bool rgba10x6_formats_ext = false;
    uint32_t exts_count;
    const auto& driver = GetData(pdev).driver;
    driver.EnumerateDeviceExtensionProperties(pdev, nullptr, &exts_count,
                                              nullptr);
    std::vector<VkExtensionProperties> props(exts_count);
    driver.EnumerateDeviceExtensionProperties(pdev, nullptr, &exts_count,
                                              props.data());
    for (uint32_t i = 0; i < exts_count; i++) {
        VkExtensionProperties prop = props[i];
        if (strcmp(prop.extensionName,
                   VK_EXT_RGBA10X6_FORMATS_EXTENSION_NAME) == 0) {
            rgba10x6_formats_ext = true;
        }
    }
    format = VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16;
    desc.format = AHARDWAREBUFFER_FORMAT_R10G10B10A10_UNORM;
    if (AHardwareBuffer_isSupported(&desc) && rgba10x6_formats_ext) {
        all_formats.emplace_back(
            VkSurfaceFormatKHR{format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
        if (colorspace_ext) {
            for (VkColorSpaceKHR colorSpace :
                 colorSpaceSupportedByVkEXTSwapchainColorspace) {
                if (GetNativeDataspace(colorSpace, format) !=
                    DataSpace::UNKNOWN) {
                    all_formats.emplace_back(
                        VkSurfaceFormatKHR{format, colorSpace});
                }
            }
        }
    }

    // NOTE: Any new formats that are added must be coordinated across different
    // Android users.  This includes the ANGLE team (a layered implementation of
    // OpenGL-ES).

    return CopyWithIncomplete(all_formats, formats, count);
}

VKAPI_ATTR
VkResult GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    VkSurfaceCapabilities2KHR* pSurfaceCapabilities) {
    ATRACE_CALL();

    auto surface = pSurfaceInfo->surface;
    auto capabilities = &pSurfaceCapabilities->surfaceCapabilities;

    VkSurfacePresentModeEXT const *pPresentMode = nullptr;
    for (auto pNext = reinterpret_cast<VkBaseInStructure const *>(pSurfaceInfo->pNext);
            pNext; pNext = reinterpret_cast<VkBaseInStructure const *>(pNext->pNext)) {
        switch (pNext->sType) {
            case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT:
                pPresentMode = reinterpret_cast<VkSurfacePresentModeEXT const *>(pNext);
                break;

            default:
                break;
        }
    }

    int err;
    int width, height;
    int transform_hint;
    int max_buffer_count;
    int min_undequeued_buffers;
    if (surface == VK_NULL_HANDLE) {
        const InstanceData& instance_data = GetData(physicalDevice);
        ProcHook::Extension surfaceless = ProcHook::GOOGLE_surfaceless_query;
        bool surfaceless_enabled =
            instance_data.hook_extensions.test(surfaceless);
        if (!surfaceless_enabled) {
            // It is an error to pass a surface==VK_NULL_HANDLE unless the
            // VK_GOOGLE_surfaceless_query extension is enabled
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        // Support for VK_GOOGLE_surfaceless_query.  The primary purpose of this
        // extension for this function is for
        // VkSurfaceProtectedCapabilitiesKHR::supportsProtected.  The following
        // four values cannot be known without a surface.  Default values will
        // be supplied anyway, but cannot be relied upon.
        width = 0xFFFFFFFF;
        height = 0xFFFFFFFF;
        transform_hint = VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;
        capabilities->minImageCount = 0xFFFFFFFF;
        capabilities->maxImageCount = 0xFFFFFFFF;
    } else {
        ANativeWindow* window = SurfaceFromHandle(surface)->window.get();

        err = window->query(window, NATIVE_WINDOW_DEFAULT_WIDTH, &width);
        if (err != android::OK) {
            ALOGE("NATIVE_WINDOW_DEFAULT_WIDTH query failed: %s (%d)",
                  strerror(-err), err);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        err = window->query(window, NATIVE_WINDOW_DEFAULT_HEIGHT, &height);
        if (err != android::OK) {
            ALOGE("NATIVE_WINDOW_DEFAULT_WIDTH query failed: %s (%d)",
                  strerror(-err), err);
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        err = window->query(window, NATIVE_WINDOW_TRANSFORM_HINT,
                            &transform_hint);
        if (err != android::OK) {
            ALOGE("NATIVE_WINDOW_TRANSFORM_HINT query failed: %s (%d)",
                  strerror(-err), err);
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        err = window->query(window, NATIVE_WINDOW_MAX_BUFFER_COUNT,
                            &max_buffer_count);
        if (err != android::OK) {
            ALOGE("NATIVE_WINDOW_MAX_BUFFER_COUNT query failed: %s (%d)",
                  strerror(-err), err);
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        err = window->query(window, NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
                            &min_undequeued_buffers);
        if (err != android::OK) {
            ALOGE("NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS query failed: %s (%d)",
                  strerror(-err), err);
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        // Additional buffer count over min_undequeued_buffers in vulkan came from 2 total
        // being technically enough for fifo (although a poor experience) vs 3 being the
        // absolute minimum for mailbox to be useful. So min_undequeued_buffers + 2 is sensible
        static constexpr int default_additional_buffers = 2;

        if(pPresentMode != nullptr) {
            switch (pPresentMode->presentMode) {
                case VK_PRESENT_MODE_IMMEDIATE_KHR:
                    ALOGE("Swapchain present mode VK_PRESENT_MODE_IMMEDIATE_KHR is not supported");
                    break;
                case VK_PRESENT_MODE_MAILBOX_KHR:
                case VK_PRESENT_MODE_FIFO_KHR:
                    capabilities->minImageCount = std::min(max_buffer_count,
                            min_undequeued_buffers + default_additional_buffers);
                    capabilities->maxImageCount = static_cast<uint32_t>(max_buffer_count);
                    break;
                case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
                    ALOGE("Swapchain present mode VK_PRESENT_MODE_FIFO_RELEAXED_KHR "
                          "is not supported");
                    break;
                case VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR:
                case VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR:
                    capabilities->minImageCount = 1;
                    capabilities->maxImageCount = 1;
                    break;

                default:
                    ALOGE("Unrecognized swapchain present mode %u is not supported",
                            pPresentMode->presentMode);
                    break;
            }
        } else {
            capabilities->minImageCount = std::min(max_buffer_count,
                    min_undequeued_buffers + default_additional_buffers);
            capabilities->maxImageCount = static_cast<uint32_t>(max_buffer_count);
        }
    }

    capabilities->currentExtent =
        VkExtent2D{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    // TODO(http://b/134182502): Figure out what the max extent should be.
    capabilities->minImageExtent = VkExtent2D{1, 1};
    capabilities->maxImageExtent = VkExtent2D{4096, 4096};

    if (capabilities->maxImageExtent.height <
        capabilities->currentExtent.height) {
        capabilities->maxImageExtent.height =
            capabilities->currentExtent.height;
    }

    if (capabilities->maxImageExtent.width <
        capabilities->currentExtent.width) {
        capabilities->maxImageExtent.width = capabilities->currentExtent.width;
    }

    capabilities->maxImageArrayLayers = 1;

    capabilities->supportedTransforms = kSupportedTransforms;
    capabilities->currentTransform =
        TranslateNativeToVulkanTransform(transform_hint);

    // On Android, window composition is a WindowManager property, not something
    // associated with the bufferqueue. It can't be changed from here.
    capabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

    capabilities->supportedUsageFlags =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

    for (auto pNext = reinterpret_cast<VkBaseOutStructure*>(pSurfaceCapabilities->pNext);
            pNext; pNext = reinterpret_cast<VkBaseOutStructure*>(pNext->pNext)) {

        switch (pNext->sType) {
            case VK_STRUCTURE_TYPE_SHARED_PRESENT_SURFACE_CAPABILITIES_KHR: {
                VkSharedPresentSurfaceCapabilitiesKHR* shared_caps =
                    reinterpret_cast<VkSharedPresentSurfaceCapabilitiesKHR*>(pNext);
                // Claim same set of usage flags are supported for
                // shared present modes as for other modes.
                shared_caps->sharedPresentSupportedUsageFlags =
                    pSurfaceCapabilities->surfaceCapabilities
                        .supportedUsageFlags;
            } break;

            case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
                VkSurfaceProtectedCapabilitiesKHR* protected_caps =
                    reinterpret_cast<VkSurfaceProtectedCapabilitiesKHR*>(pNext);
                protected_caps->supportsProtected = VK_TRUE;
            } break;

            case VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT: {
                VkSurfacePresentScalingCapabilitiesEXT* scaling_caps =
                    reinterpret_cast<VkSurfacePresentScalingCapabilitiesEXT*>(pNext);
                // By default, Android stretches the buffer to fit the window,
                // without preserving aspect ratio. Other modes are technically possible
                // but consult with CoGS team before exposing them here!
                scaling_caps->supportedPresentScaling = VK_PRESENT_SCALING_STRETCH_BIT_EXT;

                // Since we always scale, we don't support any gravity.
                scaling_caps->supportedPresentGravityX = 0;
                scaling_caps->supportedPresentGravityY = 0;

                // Scaled image limits are just the basic image limits
                scaling_caps->minScaledImageExtent = capabilities->minImageExtent;
                scaling_caps->maxScaledImageExtent = capabilities->maxImageExtent;
            } break;

            case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT: {
                VkSurfacePresentModeCompatibilityEXT* mode_caps =
                    reinterpret_cast<VkSurfacePresentModeCompatibilityEXT*>(pNext);

                ALOG_ASSERT(pPresentMode,
                        "querying VkSurfacePresentModeCompatibilityEXT "
                        "requires VkSurfacePresentModeEXT to be provided");
                std::vector<VkPresentModeKHR> compatibleModes;
                compatibleModes.push_back(pPresentMode->presentMode);

                switch (pPresentMode->presentMode) {
                    // Shared modes are both compatible with each other.
                    case VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR:
                        compatibleModes.push_back(VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR);
                        break;
                    case VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR:
                        compatibleModes.push_back(VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR);
                        break;
                    default:
                        // Other modes are only compatible with themselves.
                        // TODO: consider whether switching between FIFO and MAILBOX is reasonable
                        break;
                }

                // Note: this does not generate VK_INCOMPLETE since we're nested inside
                // a larger query and there would be no way to determine exactly where it came from.
                CopyWithIncomplete(compatibleModes, mode_caps->pPresentModes,
                        &mode_caps->presentModeCount);
            } break;

            default:
                // Ignore all other extension structs
                break;
        }
    }

    return VK_SUCCESS;
}

VKAPI_ATTR
VkResult GetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormat2KHR* pSurfaceFormats) {
    ATRACE_CALL();

    if (!pSurfaceFormats) {
        return GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice,
                                                  pSurfaceInfo->surface,
                                                  pSurfaceFormatCount, nullptr);
    }

    // temp vector for forwarding; we'll marshal it into the pSurfaceFormats
    // after the call.
    std::vector<VkSurfaceFormatKHR> surface_formats(*pSurfaceFormatCount);
    VkResult result = GetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice, pSurfaceInfo->surface, pSurfaceFormatCount,
        surface_formats.data());

    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return result;
    }

    const auto& driver = GetData(physicalDevice).driver;

    // marshal results individually due to stride difference.
    uint32_t formats_to_marshal = *pSurfaceFormatCount;
    for (uint32_t i = 0u; i < formats_to_marshal; i++) {
        pSurfaceFormats[i].surfaceFormat = surface_formats[i];

        // Query the compression properties for the surface format
        VkSurfaceFormat2KHR* pSurfaceFormat = &pSurfaceFormats[i];
        while (pSurfaceFormat->pNext) {
            pSurfaceFormat =
                reinterpret_cast<VkSurfaceFormat2KHR*>(pSurfaceFormat->pNext);
            switch (pSurfaceFormat->sType) {
                case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT: {
                    VkImageCompressionPropertiesEXT* surfaceCompressionProps =
                        reinterpret_cast<VkImageCompressionPropertiesEXT*>(
                            pSurfaceFormat);

                    if (surfaceCompressionProps &&
                        (driver.GetPhysicalDeviceImageFormatProperties2KHR ||
                         driver.GetPhysicalDeviceImageFormatProperties2)) {
                        VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {};
                        imageFormatInfo.sType =
                            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
                        imageFormatInfo.format =
                            pSurfaceFormats[i].surfaceFormat.format;
                        imageFormatInfo.type = VK_IMAGE_TYPE_2D;
                        imageFormatInfo.usage =
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                        imageFormatInfo.pNext = nullptr;

                        VkImageCompressionControlEXT compressionControl = {};
                        compressionControl.sType =
                            VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT;
                        compressionControl.pNext = imageFormatInfo.pNext;
                        compressionControl.flags =
                            VK_IMAGE_COMPRESSION_FIXED_RATE_DEFAULT_EXT;

                        imageFormatInfo.pNext = &compressionControl;

                        VkImageCompressionPropertiesEXT compressionProps = {};
                        compressionProps.sType =
                            VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT;
                        compressionProps.pNext = nullptr;

                        VkImageFormatProperties2KHR imageFormatProps = {};
                        imageFormatProps.sType =
                            VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2_KHR;
                        imageFormatProps.pNext = &compressionProps;

                        VkResult compressionRes =
                            GetPhysicalDeviceImageFormatProperties2(
                                physicalDevice, &imageFormatInfo,
                                &imageFormatProps);
                        if (compressionRes == VK_SUCCESS) {
                            surfaceCompressionProps->imageCompressionFlags =
                                compressionProps.imageCompressionFlags;
                            surfaceCompressionProps
                                ->imageCompressionFixedRateFlags =
                                compressionProps.imageCompressionFixedRateFlags;
                        } else if (compressionRes ==
                                       VK_ERROR_OUT_OF_HOST_MEMORY ||
                                   compressionRes ==
                                       VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                            return compressionRes;
                        } else {
                            // For any of the *_NOT_SUPPORTED errors we continue
                            // onto the next format
                            continue;
                        }
                    }
                } break;

                default:
                    // Ignore all other extension structs
                    break;
            }
        }
    }

    return result;
}

VKAPI_ATTR
VkResult GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pdev,
                                                 VkSurfaceKHR surface,
                                                 uint32_t* count,
                                                 VkPresentModeKHR* modes) {
    ATRACE_CALL();

    int err;
    int query_value;
    std::vector<VkPresentModeKHR> present_modes;
    if (surface == VK_NULL_HANDLE) {
        const InstanceData& instance_data = GetData(pdev);
        ProcHook::Extension surfaceless = ProcHook::GOOGLE_surfaceless_query;
        bool surfaceless_enabled =
            instance_data.hook_extensions.test(surfaceless);
        if (!surfaceless_enabled) {
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        // Support for VK_GOOGLE_surfaceless_query.  The primary purpose of this
        // extension for this function is for
        // VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR and
        // VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR.  We technically cannot
        // know if VK_PRESENT_MODE_SHARED_MAILBOX_KHR is supported without a
        // surface, and that cannot be relied upon.  Therefore, don't return it.
        present_modes.push_back(VK_PRESENT_MODE_FIFO_KHR);
    } else {
        ANativeWindow* window = SurfaceFromHandle(surface)->window.get();

        err = window->query(window, NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
                            &query_value);
        if (err != android::OK || query_value < 0) {
            ALOGE(
                "NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS query failed: %s (%d) "
                "value=%d",
                strerror(-err), err, query_value);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        uint32_t min_undequeued_buffers = static_cast<uint32_t>(query_value);

        err =
            window->query(window, NATIVE_WINDOW_MAX_BUFFER_COUNT, &query_value);
        if (err != android::OK || query_value < 0) {
            ALOGE(
                "NATIVE_WINDOW_MAX_BUFFER_COUNT query failed: %s (%d) value=%d",
                strerror(-err), err, query_value);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        uint32_t max_buffer_count = static_cast<uint32_t>(query_value);

        if (min_undequeued_buffers + 1 < max_buffer_count)
            present_modes.push_back(VK_PRESENT_MODE_MAILBOX_KHR);
        present_modes.push_back(VK_PRESENT_MODE_FIFO_KHR);
    }

    VkPhysicalDevicePresentationPropertiesANDROID present_properties;
    QueryPresentationProperties(pdev, &present_properties);
    if (present_properties.sharedImage) {
        present_modes.push_back(VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR);
        present_modes.push_back(VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR);
    }

    return CopyWithIncomplete(present_modes, modes, count);
}

VKAPI_ATTR
VkResult GetDeviceGroupPresentCapabilitiesKHR(
    VkDevice,
    VkDeviceGroupPresentCapabilitiesKHR* pDeviceGroupPresentCapabilities) {
    ATRACE_CALL();

    ALOGV_IF(pDeviceGroupPresentCapabilities->sType !=
                 VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_CAPABILITIES_KHR,
             "vkGetDeviceGroupPresentCapabilitiesKHR: invalid "
             "VkDeviceGroupPresentCapabilitiesKHR structure type %d",
             pDeviceGroupPresentCapabilities->sType);

    memset(pDeviceGroupPresentCapabilities->presentMask, 0,
           sizeof(pDeviceGroupPresentCapabilities->presentMask));

    // assume device group of size 1
    pDeviceGroupPresentCapabilities->presentMask[0] = 1 << 0;
    pDeviceGroupPresentCapabilities->modes =
        VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;

    return VK_SUCCESS;
}

VKAPI_ATTR
VkResult GetDeviceGroupSurfacePresentModesKHR(
    VkDevice,
    VkSurfaceKHR,
    VkDeviceGroupPresentModeFlagsKHR* pModes) {
    ATRACE_CALL();

    *pModes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

VKAPI_ATTR
VkResult GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice,
                                               VkSurfaceKHR surface,
                                               uint32_t* pRectCount,
                                               VkRect2D* pRects) {
    ATRACE_CALL();

    if (!pRects) {
        *pRectCount = 1;
    } else {
        uint32_t count = std::min(*pRectCount, 1u);
        bool incomplete = *pRectCount < 1;

        *pRectCount = count;

        if (incomplete) {
            return VK_INCOMPLETE;
        }

        int err;
        ANativeWindow* window = SurfaceFromHandle(surface)->window.get();

        int width = 0, height = 0;
        err = window->query(window, NATIVE_WINDOW_DEFAULT_WIDTH, &width);
        if (err != android::OK) {
            ALOGE("NATIVE_WINDOW_DEFAULT_WIDTH query failed: %s (%d)",
                  strerror(-err), err);
        }
        err = window->query(window, NATIVE_WINDOW_DEFAULT_HEIGHT, &height);
        if (err != android::OK) {
            ALOGE("NATIVE_WINDOW_DEFAULT_WIDTH query failed: %s (%d)",
                  strerror(-err), err);
        }

        pRects[0].offset.x = 0;
        pRects[0].offset.y = 0;
        pRects[0].extent = VkExtent2D{static_cast<uint32_t>(width),
                                      static_cast<uint32_t>(height)};
    }
    return VK_SUCCESS;
}

static void DestroySwapchainInternal(VkDevice device,
                                     VkSwapchainKHR swapchain_handle,
                                     const VkAllocationCallbacks* allocator) {
    ATRACE_CALL();

    const auto& dispatch = GetData(device).driver;
    Swapchain* swapchain = SwapchainFromHandle(swapchain_handle);
    if (!swapchain) {
        return;
    }

    bool active = swapchain->surface.swapchain_handle == swapchain_handle;
    ANativeWindow* window = active ? swapchain->surface.window.get() : nullptr;

    if (window && swapchain->frame_timestamps_enabled) {
        native_window_enable_frame_timestamps(window, false);
    }

    for (uint32_t i = 0; i < swapchain->num_images; i++) {
        ReleaseSwapchainImage(device, swapchain->shared, window, -1,
                              swapchain->images[i], false);
    }

    if (active) {
        swapchain->surface.swapchain_handle = VK_NULL_HANDLE;
    }

    if (!allocator) {
        allocator = &GetData(device).allocator;
    }

    swapchain->~Swapchain();
    allocator->pfnFree(allocator->pUserData, swapchain);
}

static VkResult getProducerUsageGPDIFP2(
    const VkPhysicalDevice& pdev,
    const VkSwapchainCreateInfoKHR* create_info,
    const VkSwapchainImageUsageFlagsANDROID swapchain_image_usage,
    bool create_protected_swapchain,
    uint64_t* producer_usage) {
    // Look through the create_info pNext chain passed to createSwapchainKHR
    // for an image compression control struct.
    // if one is found AND the appropriate extensions are enabled, create a
    // VkImageCompressionControlEXT structure to pass on to
    // GetPhysicalDeviceImageFormatProperties2
    void* compression_control_pNext = nullptr;
    VkImageCompressionControlEXT image_compression = {};
    const VkSwapchainCreateInfoKHR* create_infos = create_info;
    while (create_infos->pNext) {
        create_infos = reinterpret_cast<const VkSwapchainCreateInfoKHR*>(
            create_infos->pNext);
        switch (create_infos->sType) {
            case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: {
                const VkImageCompressionControlEXT* compression_infos =
                    reinterpret_cast<const VkImageCompressionControlEXT*>(
                        create_infos);
                image_compression = *compression_infos;
                image_compression.pNext = nullptr;
                compression_control_pNext = &image_compression;
            } break;
            default:
                // Ignore all other info structs
                break;
        }
    }

    // call GetPhysicalDeviceImageFormatProperties2KHR
    VkPhysicalDeviceExternalImageFormatInfo external_image_format_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = compression_control_pNext,
        .handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };

    // AHB does not have an sRGB format so we can't pass it to GPDIFP
    // We need to convert the format to unorm if it is srgb
    VkFormat format = create_info->imageFormat;
    if (format == VK_FORMAT_R8G8B8A8_SRGB) {
        format = VK_FORMAT_R8G8B8A8_UNORM;
    }

    VkPhysicalDeviceImageFormatInfo2 image_format_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_image_format_info,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = create_info->imageUsage,
        .flags =
            create_protected_swapchain ? VK_IMAGE_CREATE_PROTECTED_BIT : 0u,
    };

    // If supporting mutable format swapchain add the mutable format flag
    if (create_info->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
        image_format_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        image_format_info.flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT_KHR;
    }

    VkAndroidHardwareBufferUsageANDROID ahb_usage;
    ahb_usage.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID;
    ahb_usage.pNext = nullptr;

    VkImageFormatProperties2 image_format_properties;
    image_format_properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    image_format_properties.pNext = &ahb_usage;

    VkResult result = GetPhysicalDeviceImageFormatProperties2(
        pdev, &image_format_info, &image_format_properties);
    if (result != VK_SUCCESS) {
        ALOGE(
            "VkGetPhysicalDeviceImageFormatProperties2 for AHB usage "
            "failed: %d",
            result);
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    // Determine if USAGE_FRONT_BUFFER is needed.
    // GPDIFP2 has no means of using VkSwapchainImageUsageFlagsANDROID when
    // querying for producer_usage. So androidHardwareBufferUsage will not
    // contain USAGE_FRONT_BUFFER. We need to manually check for usage here.
    if (!(swapchain_image_usage &
          VK_SWAPCHAIN_IMAGE_USAGE_SHARED_BIT_ANDROID)) {
        *producer_usage = ahb_usage.androidHardwareBufferUsage;
        return VK_SUCCESS;
    }

    // Check if USAGE_FRONT_BUFFER is supported for this swapchain
    AHardwareBuffer_Desc ahb_desc = {
        .width = create_info->imageExtent.width,
        .height = create_info->imageExtent.height,
        .layers = create_info->imageArrayLayers,
        .format = create_info->imageFormat,
        .usage = ahb_usage.androidHardwareBufferUsage |
                 AHARDWAREBUFFER_USAGE_FRONT_BUFFER,
        .stride = 0,  // stride is always ignored when calling isSupported()
    };

    // If FRONT_BUFFER is not supported in the GPDIFP2 path
    // then we need to fallback to GetSwapchainGrallocUsageXAndroid
    if (AHardwareBuffer_isSupported(&ahb_desc)) {
        *producer_usage = ahb_usage.androidHardwareBufferUsage;
        *producer_usage |= AHARDWAREBUFFER_USAGE_FRONT_BUFFER;
        return VK_SUCCESS;
    }

    return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

static VkResult getProducerUsage(const VkDevice& device,
                                 const VkSwapchainCreateInfoKHR* create_info,
                                 const VkSwapchainImageUsageFlagsANDROID swapchain_image_usage,
                                 bool create_protected_swapchain,
                                 uint64_t* producer_usage) {
    // Get the physical device to query the appropriate producer usage
    const VkPhysicalDevice& pdev = GetData(device).driver_physical_device;
    const InstanceData& instance_data = GetData(pdev);
    const InstanceDriverTable& instance_dispatch = instance_data.driver;

    if (instance_dispatch.GetPhysicalDeviceImageFormatProperties2 ||
            instance_dispatch.GetPhysicalDeviceImageFormatProperties2KHR) {
        VkResult result =
            getProducerUsageGPDIFP2(pdev, create_info, swapchain_image_usage,
                                    create_protected_swapchain, producer_usage);
        if (result == VK_SUCCESS) {
            return VK_SUCCESS;
        }
        // Fall through to gralloc path on error
    }

    uint64_t native_usage = 0;
    void* usage_info_pNext = nullptr;
    VkResult result;
    VkImageCompressionControlEXT image_compression = {};
    const auto& dispatch = GetData(device).driver;
    if (dispatch.GetSwapchainGrallocUsage4ANDROID) {
        ATRACE_BEGIN("GetSwapchainGrallocUsage4ANDROID");
        VkGrallocUsageInfo2ANDROID gralloc_usage_info = {};
        gralloc_usage_info.sType =
            VK_STRUCTURE_TYPE_GRALLOC_USAGE_INFO_2_ANDROID;
        gralloc_usage_info.format = create_info->imageFormat;
        gralloc_usage_info.imageUsage = create_info->imageUsage;
        gralloc_usage_info.swapchainImageUsage = swapchain_image_usage;

        // Look through the pNext chain for an image compression control struct
        // if one is found AND the appropriate extensions are enabled,
        // append it to be the gralloc usage pNext chain
        const VkSwapchainCreateInfoKHR* create_infos = create_info;
        while (create_infos->pNext) {
            create_infos = reinterpret_cast<const VkSwapchainCreateInfoKHR*>(
                create_infos->pNext);
            switch (create_infos->sType) {
                case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: {
                    const VkImageCompressionControlEXT* compression_infos =
                        reinterpret_cast<const VkImageCompressionControlEXT*>(
                            create_infos);
                    image_compression = *compression_infos;
                    image_compression.pNext = nullptr;
                    usage_info_pNext = &image_compression;
                } break;

                default:
                    // Ignore all other info structs
                    break;
            }
        }
        gralloc_usage_info.pNext = usage_info_pNext;

        result = dispatch.GetSwapchainGrallocUsage4ANDROID(
            device, &gralloc_usage_info, &native_usage);
        ATRACE_END();
        if (result != VK_SUCCESS) {
            ALOGE("vkGetSwapchainGrallocUsage4ANDROID failed: %d", result);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
    } else if (dispatch.GetSwapchainGrallocUsage3ANDROID) {
        ATRACE_BEGIN("GetSwapchainGrallocUsage3ANDROID");
        VkGrallocUsageInfoANDROID gralloc_usage_info = {};
        gralloc_usage_info.sType = VK_STRUCTURE_TYPE_GRALLOC_USAGE_INFO_ANDROID;
        gralloc_usage_info.format = create_info->imageFormat;
        gralloc_usage_info.imageUsage = create_info->imageUsage;

        // Look through the pNext chain for an image compression control struct
        // if one is found AND the appropriate extensions are enabled,
        // append it to be the gralloc usage pNext chain
        const VkSwapchainCreateInfoKHR* create_infos = create_info;
        while (create_infos->pNext) {
            create_infos = reinterpret_cast<const VkSwapchainCreateInfoKHR*>(
                create_infos->pNext);
            switch (create_infos->sType) {
                case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: {
                    const VkImageCompressionControlEXT* compression_infos =
                        reinterpret_cast<const VkImageCompressionControlEXT*>(
                            create_infos);
                    image_compression = *compression_infos;
                    image_compression.pNext = nullptr;
                    usage_info_pNext = &image_compression;
                } break;

                default:
                    // Ignore all other info structs
                    break;
            }
        }
        gralloc_usage_info.pNext = usage_info_pNext;

        result = dispatch.GetSwapchainGrallocUsage3ANDROID(
            device, &gralloc_usage_info, &native_usage);
        ATRACE_END();
        if (result != VK_SUCCESS) {
            ALOGE("vkGetSwapchainGrallocUsage3ANDROID failed: %d", result);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
    } else if (dispatch.GetSwapchainGrallocUsage2ANDROID) {
        uint64_t consumer_usage, producer_usage;
        ATRACE_BEGIN("GetSwapchainGrallocUsage2ANDROID");
        result = dispatch.GetSwapchainGrallocUsage2ANDROID(
            device, create_info->imageFormat, create_info->imageUsage,
            swapchain_image_usage, &consumer_usage, &producer_usage);
        ATRACE_END();
        if (result != VK_SUCCESS) {
            ALOGE("vkGetSwapchainGrallocUsage2ANDROID failed: %d", result);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        native_usage =
            convertGralloc1ToBufferUsage(producer_usage, consumer_usage);
    } else if (dispatch.GetSwapchainGrallocUsageANDROID) {
        ATRACE_BEGIN("GetSwapchainGrallocUsageANDROID");
        int32_t legacy_usage = 0;
        result = dispatch.GetSwapchainGrallocUsageANDROID(
            device, create_info->imageFormat, create_info->imageUsage,
            &legacy_usage);
        ATRACE_END();
        if (result != VK_SUCCESS) {
            ALOGE("vkGetSwapchainGrallocUsageANDROID failed: %d", result);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        native_usage = static_cast<uint64_t>(legacy_usage);
    }
    *producer_usage = native_usage;

    return VK_SUCCESS;
}

VKAPI_ATTR
VkResult CreateSwapchainKHR(VkDevice device,
                            const VkSwapchainCreateInfoKHR* create_info,
                            const VkAllocationCallbacks* allocator,
                            VkSwapchainKHR* swapchain_handle) {
    ATRACE_CALL();

    int err;
    VkResult result = VK_SUCCESS;

    ALOGV("vkCreateSwapchainKHR: surface=0x%" PRIx64
          " minImageCount=%u imageFormat=%u imageColorSpace=%u"
          " imageExtent=%ux%u imageUsage=%#x preTransform=%u presentMode=%u"
          " oldSwapchain=0x%" PRIx64,
          reinterpret_cast<uint64_t>(create_info->surface),
          create_info->minImageCount, create_info->imageFormat,
          create_info->imageColorSpace, create_info->imageExtent.width,
          create_info->imageExtent.height, create_info->imageUsage,
          create_info->preTransform, create_info->presentMode,
          reinterpret_cast<uint64_t>(create_info->oldSwapchain));

    if (!allocator)
        allocator = &GetData(device).allocator;

    PixelFormat native_pixel_format =
        GetNativePixelFormat(create_info->imageFormat);
    DataSpace native_dataspace = GetNativeDataspace(
        create_info->imageColorSpace, create_info->imageFormat);
    if (native_dataspace == DataSpace::UNKNOWN) {
        ALOGE(
            "CreateSwapchainKHR(VkSwapchainCreateInfoKHR.imageColorSpace = %d) "
            "failed: Unsupported color space",
            create_info->imageColorSpace);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    ALOGV_IF(create_info->imageArrayLayers != 1,
             "swapchain imageArrayLayers=%u not supported",
             create_info->imageArrayLayers);
    ALOGV_IF((create_info->preTransform & ~kSupportedTransforms) != 0,
             "swapchain preTransform=%#x not supported",
             create_info->preTransform);
    ALOGV_IF(!(create_info->presentMode == VK_PRESENT_MODE_FIFO_KHR ||
               create_info->presentMode == VK_PRESENT_MODE_MAILBOX_KHR ||
               create_info->presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR ||
               create_info->presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR),
             "swapchain presentMode=%u not supported",
             create_info->presentMode);

    Surface& surface = *SurfaceFromHandle(create_info->surface);

    if (surface.swapchain_handle != create_info->oldSwapchain) {
        ALOGV("Can't create a swapchain for VkSurfaceKHR 0x%" PRIx64
              " because it already has active swapchain 0x%" PRIx64
              " but VkSwapchainCreateInfo::oldSwapchain=0x%" PRIx64,
              reinterpret_cast<uint64_t>(create_info->surface),
              reinterpret_cast<uint64_t>(surface.swapchain_handle),
              reinterpret_cast<uint64_t>(create_info->oldSwapchain));
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    }
    if (create_info->oldSwapchain != VK_NULL_HANDLE)
        OrphanSwapchain(device, SwapchainFromHandle(create_info->oldSwapchain));

    // -- Reset the native window --
    // The native window might have been used previously, and had its properties
    // changed from defaults. That will affect the answer we get for queries
    // like MIN_UNDEQUED_BUFFERS. Reset to a known/default state before we
    // attempt such queries.

    // The native window only allows dequeueing all buffers before any have
    // been queued, since after that point at least one is assumed to be in
    // non-FREE state at any given time. Disconnecting and re-connecting
    // orphans the previous buffers, getting us back to the state where we can
    // dequeue all buffers.
    //
    // This is not necessary if the surface was never used previously.
    //
    // TODO(http://b/134186185) recycle swapchain images more efficiently
    ANativeWindow* window = surface.window.get();
    if (surface.used_by_swapchain) {
        err = native_window_api_disconnect(window, NATIVE_WINDOW_API_EGL);
        ALOGW_IF(err != android::OK,
                 "native_window_api_disconnect failed: %s (%d)", strerror(-err),
                 err);
        err = native_window_api_connect(window, NATIVE_WINDOW_API_EGL);
        ALOGW_IF(err != android::OK,
                 "native_window_api_connect failed: %s (%d)", strerror(-err),
                 err);
    }

    err =
        window->perform(window, NATIVE_WINDOW_SET_DEQUEUE_TIMEOUT, nsecs_t{-1});
    if (err != android::OK) {
        ALOGE("window->perform(SET_DEQUEUE_TIMEOUT) failed: %s (%d)",
              strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    int swap_interval =
        create_info->presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? 0 : 1;
    err = window->setSwapInterval(window, swap_interval);
    if (err != android::OK) {
        ALOGE("native_window->setSwapInterval(1) failed: %s (%d)",
              strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    err = native_window_set_shared_buffer_mode(window, false);
    if (err != android::OK) {
        ALOGE("native_window_set_shared_buffer_mode(false) failed: %s (%d)",
              strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    err = native_window_set_auto_refresh(window, false);
    if (err != android::OK) {
        ALOGE("native_window_set_auto_refresh(false) failed: %s (%d)",
              strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    // -- Configure the native window --

    const auto& dispatch = GetData(device).driver;

    err = native_window_set_buffers_format(
        window, static_cast<int>(native_pixel_format));
    if (err != android::OK) {
        ALOGE("native_window_set_buffers_format(%s) failed: %s (%d)",
              toString(native_pixel_format).c_str(), strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    /* Respect consumer default dataspace upon HAL_DATASPACE_ARBITRARY. */
    if (native_dataspace != DataSpace::ARBITRARY) {
        err = native_window_set_buffers_data_space(
            window, static_cast<android_dataspace_t>(native_dataspace));
        if (err != android::OK) {
            ALOGE("native_window_set_buffers_data_space(%d) failed: %s (%d)",
                  native_dataspace, strerror(-err), err);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
    }

    err = native_window_set_buffers_dimensions(
        window, static_cast<int>(create_info->imageExtent.width),
        static_cast<int>(create_info->imageExtent.height));
    if (err != android::OK) {
        ALOGE("native_window_set_buffers_dimensions(%d,%d) failed: %s (%d)",
              create_info->imageExtent.width, create_info->imageExtent.height,
              strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    // VkSwapchainCreateInfo::preTransform indicates the transformation the app
    // applied during rendering. native_window_set_transform() expects the
    // inverse: the transform the app is requesting that the compositor perform
    // during composition. With native windows, pre-transform works by rendering
    // with the same transform the compositor is applying (as in Vulkan), but
    // then requesting the inverse transform, so that when the compositor does
    // it's job the two transforms cancel each other out and the compositor ends
    // up applying an identity transform to the app's buffer.
    err = native_window_set_buffers_transform(
        window, InvertTransformToNative(create_info->preTransform));
    if (err != android::OK) {
        ALOGE("native_window_set_buffers_transform(%d) failed: %s (%d)",
              InvertTransformToNative(create_info->preTransform),
              strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    err = native_window_set_scaling_mode(
        window, NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
    if (err != android::OK) {
        ALOGE("native_window_set_scaling_mode(SCALE_TO_WINDOW) failed: %s (%d)",
              strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    VkSwapchainImageUsageFlagsANDROID swapchain_image_usage = 0;
    if (IsSharedPresentMode(create_info->presentMode)) {
        swapchain_image_usage |= VK_SWAPCHAIN_IMAGE_USAGE_SHARED_BIT_ANDROID;
        err = native_window_set_shared_buffer_mode(window, true);
        if (err != android::OK) {
            ALOGE("native_window_set_shared_buffer_mode failed: %s (%d)", strerror(-err), err);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
    }

    if (create_info->presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR) {
        err = native_window_set_auto_refresh(window, true);
        if (err != android::OK) {
            ALOGE("native_window_set_auto_refresh failed: %s (%d)", strerror(-err), err);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
    }

    int query_value;
    // TODO: Now that we are calling into GPDSC2 directly, this query may be redundant
    //       the call to std::max(min_buffer_count, num_images) may be redundant as well
    err = window->query(window, NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
                        &query_value);
    if (err != android::OK || query_value < 0) {
        ALOGE("window->query failed: %s (%d) value=%d", strerror(-err), err,
              query_value);
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    const uint32_t min_undequeued_buffers = static_cast<uint32_t>(query_value);

    // Lower layer insists that we have at least min_undequeued_buffers + 1
    // buffers.  This is wasteful and we'd like to relax it in the shared case,
    // but not all the pieces are in place for that to work yet.  Note we only
    // lie to the lower layer--we don't want to give the app back a swapchain
    // with extra images (which they can't actually use!).
    const uint32_t min_buffer_count = min_undequeued_buffers + 1;

    // Call into GPDSC2 to get the minimum and maximum allowable buffer count for the surface of
    // interest. This step is only necessary if the app requests a number of images
    // (create_info->minImageCount) that is less or more than the surface capabilities.
    // An app should be calling GPDSC2 and using those values to set create_info, but in the
    // event that the app has hard-coded image counts an error can occur
    VkSurfacePresentModeEXT present_mode = {
        VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT,
        nullptr,
        create_info->presentMode
    };
    VkPhysicalDeviceSurfaceInfo2KHR surface_info2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
        &present_mode,
        create_info->surface
    };
    VkSurfaceCapabilities2KHR surface_capabilities2 = {
        VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
        nullptr,
        {},
    };
    result = GetPhysicalDeviceSurfaceCapabilities2KHR(GetData(device).driver_physical_device,
            &surface_info2, &surface_capabilities2);

    uint32_t num_images = create_info->minImageCount;
    num_images = std::clamp(num_images,
            surface_capabilities2.surfaceCapabilities.minImageCount,
            surface_capabilities2.surfaceCapabilities.maxImageCount);

    const uint32_t buffer_count = std::max(min_buffer_count, num_images);
    err = native_window_set_buffer_count(window, buffer_count);
    if (err != android::OK) {
        ALOGE("native_window_set_buffer_count(%d) failed: %s (%d)", buffer_count,
              strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    // In shared mode the num_images must be one regardless of how many
    // buffers were allocated for the buffer queue.
    if (swapchain_image_usage & VK_SWAPCHAIN_IMAGE_USAGE_SHARED_BIT_ANDROID) {
        num_images = 1;
    }

    VkImageFormatListCreateInfo extra_mutable_formats = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
    };
    VkImageFormatListCreateInfo* extra_mutable_formats_ptr;

    // Look through the create_info pNext chain passed to createSwapchainKHR
    // for an image compression control struct.
    // if one is found AND the appropriate extensions are enabled, create a
    // VkImageCompressionControlEXT structure to pass on to VkImageCreateInfo
    // TODO check for imageCompressionControlSwapchain feature is enabled
    void* usage_info_pNext = nullptr;
    VkImageCompressionControlEXT image_compression = {};
    const VkSwapchainCreateInfoKHR* create_infos = create_info;
    while (create_infos->pNext) {
        create_infos = reinterpret_cast<const VkSwapchainCreateInfoKHR*>(create_infos->pNext);
        switch (create_infos->sType) {
            case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: {
                const VkImageCompressionControlEXT* compression_infos =
                    reinterpret_cast<const VkImageCompressionControlEXT*>(create_infos);
                image_compression = *compression_infos;
                image_compression.pNext = nullptr;
                usage_info_pNext = &image_compression;
            } break;
            case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
                const VkImageFormatListCreateInfo* format_list =
                    reinterpret_cast<const VkImageFormatListCreateInfo*>(
                        create_infos);
                if (create_info->flags &
                    VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
                    if (format_list && format_list->viewFormatCount > 0 &&
                        format_list->pViewFormats) {
                        extra_mutable_formats.viewFormatCount =
                            format_list->viewFormatCount;
                        extra_mutable_formats.pViewFormats =
                            format_list->pViewFormats;
                        extra_mutable_formats_ptr = &extra_mutable_formats;
                    } else {
                        ALOGE(
                            "vk_swapchain_create_mutable_format_bit_khr was "
                            "set during swapchain creation but no valid "
                            "vkimageformatlistcreateinfo was found in the "
                            "pnext chain");
                        return VK_ERROR_INITIALIZATION_FAILED;
                    }
                }
            } break;
            default:
                // Ignore all other info structs
                break;
        }
    }

    // Get the appropriate native_usage for the images
    // Get the consumer usage
    uint64_t native_usage = surface.consumer_usage;
    // Determine if the swapchain is protected
    bool create_protected_swapchain = false;
    if (create_info->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) {
        create_protected_swapchain = true;
        native_usage |= BufferUsage::PROTECTED;
    }
    // Get the producer usage
    uint64_t producer_usage;
    result = getProducerUsage(device, create_info, swapchain_image_usage, create_protected_swapchain, &producer_usage);
    if (result != VK_SUCCESS) {
        return result;
    }
    native_usage |= producer_usage;

    err = native_window_set_usage(window, native_usage);
    if (err != android::OK) {
        ALOGE("native_window_set_usage failed: %s (%d)", strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    int transform_hint;
    err = window->query(window, NATIVE_WINDOW_TRANSFORM_HINT, &transform_hint);
    if (err != android::OK) {
        ALOGE("NATIVE_WINDOW_TRANSFORM_HINT query failed: %s (%d)",
              strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    int64_t refresh_duration;
    err = native_window_get_refresh_cycle_duration(window, &refresh_duration);
    if (err != android::OK) {
        ALOGE("native_window_get_refresh_cycle_duration query failed: %s (%d)",
              strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    // -- Allocate our Swapchain object --
    // After this point, we must deallocate the swapchain on error.

    void* mem = allocator->pfnAllocation(allocator->pUserData,
                                         sizeof(Swapchain), alignof(Swapchain),
                                         VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

    if (!mem)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    Swapchain* swapchain = new (mem)
        Swapchain(surface, num_images, create_info->presentMode,
                  TranslateVulkanToNativeTransform(create_info->preTransform),
                  refresh_duration);
    VkSwapchainImageCreateInfoANDROID swapchain_image_create = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_IMAGE_CREATE_INFO_ANDROID,
#pragma clang diagnostic pop
        .pNext = usage_info_pNext,
        .usage = swapchain_image_usage,
    };
    VkNativeBufferANDROID image_native_buffer = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
        .sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID,
#pragma clang diagnostic pop
        .pNext = &swapchain_image_create,
    };

    VkImageCreateInfo image_create = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = create_protected_swapchain ? VK_IMAGE_CREATE_PROTECTED_BIT : 0u,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = create_info->imageFormat,
        .extent = {
            create_info->imageExtent.width,
            create_info->imageExtent.height,
            1
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = create_info->imageUsage,
        .sharingMode = create_info->imageSharingMode,
        .queueFamilyIndexCount = create_info->queueFamilyIndexCount,
        .pQueueFamilyIndices = create_info->pQueueFamilyIndices,
    };

    if (create_info->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
        image_create.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        image_create.flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT_KHR;
    }

    // Note: don't do deferred allocation for shared present modes. There's only one buffer
    // involved so very little benefit.
    if ((create_info->flags & VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_EXT) &&
            !IsSharedPresentMode(create_info->presentMode)) {
        // Don't want to touch the underlying gralloc buffers yet;
        // instead just create unbound VkImages which will later be bound to memory inside
        // AcquireNextImage.
        VkImageSwapchainCreateInfoKHR image_swapchain_create = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR,
            .pNext = extra_mutable_formats_ptr,
            .swapchain = HandleFromSwapchain(swapchain),
        };
        image_create.pNext = &image_swapchain_create;

        for (uint32_t i = 0; i < num_images; i++) {
            Swapchain::Image& img = swapchain->images[i];
            img.buffer = nullptr;
            img.dequeued = false;

            result = dispatch.CreateImage(device, &image_create, nullptr, &img.image);
            if (result != VK_SUCCESS) {
                ALOGD("vkCreateImage w/ for deferred swapchain image failed: %u", result);
                break;
            }
        }
    } else {
        // -- Dequeue all buffers and create a VkImage for each --
        // Any failures during or after this must cancel the dequeued buffers.

        for (uint32_t i = 0; i < num_images; i++) {
            Swapchain::Image& img = swapchain->images[i];

            ANativeWindowBuffer* buffer;
            err = window->dequeueBuffer(window, &buffer, &img.dequeue_fence);
            if (err != android::OK) {
                ALOGE("dequeueBuffer[%u] failed: %s (%d)", i, strerror(-err), err);
                switch (-err) {
                    case ENOMEM:
                        result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                        break;
                    default:
                        result = VK_ERROR_SURFACE_LOST_KHR;
                        break;
                }
                break;
            }
            img.buffer = buffer;
            img.dequeued = true;

            image_native_buffer.handle = img.buffer->handle;
            image_native_buffer.stride = img.buffer->stride;
            image_native_buffer.format = img.buffer->format;
            image_native_buffer.usage = int(img.buffer->usage);
            android_convertGralloc0To1Usage(int(img.buffer->usage),
                &image_native_buffer.usage2.producer,
                &image_native_buffer.usage2.consumer);
            image_native_buffer.usage3 = img.buffer->usage;
            image_native_buffer.ahb =
                ANativeWindowBuffer_getHardwareBuffer(img.buffer.get());
            image_create.pNext = &image_native_buffer;

            if (extra_mutable_formats_ptr) {
                extra_mutable_formats_ptr->pNext = image_create.pNext;
                image_create.pNext = extra_mutable_formats_ptr;
            }

            ATRACE_BEGIN("CreateImage");
            result =
                dispatch.CreateImage(device, &image_create, nullptr, &img.image);
            ATRACE_END();
            if (result != VK_SUCCESS) {
                ALOGD("vkCreateImage w/ native buffer failed: %u", result);
                break;
            }
        }

        // -- Cancel all buffers, returning them to the queue --
        // If an error occurred before, also destroy the VkImage and release the
        // buffer reference. Otherwise, we retain a strong reference to the buffer.
        for (uint32_t i = 0; i < num_images; i++) {
            Swapchain::Image& img = swapchain->images[i];
            if (img.dequeued) {
                if (!swapchain->shared) {
                    window->cancelBuffer(window, img.buffer.get(),
                                         img.dequeue_fence);
                    img.dequeue_fence = -1;
                    img.dequeued = false;
                }
            }
        }
    }

    if (result != VK_SUCCESS) {
        DestroySwapchainInternal(device, HandleFromSwapchain(swapchain),
                                 allocator);
        return result;
    }

    if (transform_hint != swapchain->pre_transform) {
        // Log that the app is not doing pre-rotation.
        android::GraphicsEnv::getInstance().setTargetStats(
            android::GpuStatsInfo::Stats::FALSE_PREROTATION);
    }

    // Set stats for creating a Vulkan swapchain
    android::GraphicsEnv::getInstance().setTargetStats(
        android::GpuStatsInfo::Stats::CREATED_VULKAN_SWAPCHAIN);

    surface.used_by_swapchain = true;
    surface.swapchain_handle = HandleFromSwapchain(swapchain);
    *swapchain_handle = surface.swapchain_handle;
    return VK_SUCCESS;
}

VKAPI_ATTR
void DestroySwapchainKHR(VkDevice device,
                         VkSwapchainKHR swapchain_handle,
                         const VkAllocationCallbacks* allocator) {
    ATRACE_CALL();

    DestroySwapchainInternal(device, swapchain_handle, allocator);
}

VKAPI_ATTR
VkResult GetSwapchainImagesKHR(VkDevice,
                               VkSwapchainKHR swapchain_handle,
                               uint32_t* count,
                               VkImage* images) {
    ATRACE_CALL();

    Swapchain& swapchain = *SwapchainFromHandle(swapchain_handle);
    ALOGW_IF(swapchain.surface.swapchain_handle != swapchain_handle,
             "getting images for non-active swapchain 0x%" PRIx64
             "; only dequeued image handles are valid",
             reinterpret_cast<uint64_t>(swapchain_handle));
    VkResult result = VK_SUCCESS;
    if (images) {
        uint32_t n = swapchain.num_images;
        if (*count < swapchain.num_images) {
            n = *count;
            result = VK_INCOMPLETE;
        }
        for (uint32_t i = 0; i < n; i++)
            images[i] = swapchain.images[i].image;
        *count = n;
    } else {
        *count = swapchain.num_images;
    }
    return result;
}

VKAPI_ATTR
VkResult AcquireNextImageKHR(VkDevice device,
                             VkSwapchainKHR swapchain_handle,
                             uint64_t timeout,
                             VkSemaphore semaphore,
                             VkFence vk_fence,
                             uint32_t* image_index) {
    ATRACE_CALL();

    Swapchain& swapchain = *SwapchainFromHandle(swapchain_handle);
    ANativeWindow* window = swapchain.surface.window.get();
    VkResult result;
    int err;

    if (swapchain.surface.swapchain_handle != swapchain_handle)
        return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain.shared) {
        // In shared mode, we keep the buffer dequeued all the time, so we don't
        // want to dequeue a buffer here. Instead, just ask the driver to ensure
        // the semaphore and fence passed to us will be signalled.
        *image_index = 0;
        result = GetData(device).driver.AcquireImageANDROID(
                device, swapchain.images[*image_index].image, -1, semaphore, vk_fence);
        return result;
    }

    const nsecs_t acquire_next_image_timeout =
        timeout > (uint64_t)std::numeric_limits<nsecs_t>::max() ? -1 : timeout;
    if (acquire_next_image_timeout != swapchain.acquire_next_image_timeout) {
        // Cache the timeout to avoid the duplicate binder cost.
        err = window->perform(window, NATIVE_WINDOW_SET_DEQUEUE_TIMEOUT,
                              acquire_next_image_timeout);
        if (err != android::OK) {
            ALOGE("window->perform(SET_DEQUEUE_TIMEOUT) failed: %s (%d)",
                  strerror(-err), err);
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        swapchain.acquire_next_image_timeout = acquire_next_image_timeout;
    }

    ANativeWindowBuffer* buffer;
    int fence_fd;
    err = window->dequeueBuffer(window, &buffer, &fence_fd);
    if (err == android::TIMED_OUT || err == android::INVALID_OPERATION) {
        ALOGW("dequeueBuffer timed out: %s (%d)", strerror(-err), err);
        return timeout ? VK_TIMEOUT : VK_NOT_READY;
    } else if (err != android::OK) {
        ALOGE("dequeueBuffer failed: %s (%d)", strerror(-err), err);
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    uint32_t idx;
    for (idx = 0; idx < swapchain.num_images; idx++) {
        if (swapchain.images[idx].buffer.get() == buffer) {
            swapchain.images[idx].dequeued = true;
            swapchain.images[idx].dequeue_fence = fence_fd;
            break;
        }
    }

    // If this is a deferred alloc swapchain, this may be the first time we've
    // seen a particular buffer. If so, there should be an empty slot. Find it,
    // and bind the gralloc buffer to the VkImage for that slot. If there is no
    // empty slot, then we dequeued an unexpected buffer. Non-deferred swapchains
    // will also take this path, but will never have an empty slot since we
    // populated them all upfront.
    if (idx == swapchain.num_images) {
        for (idx = 0; idx < swapchain.num_images; idx++) {
            if (!swapchain.images[idx].buffer) {
                // Note: this structure is technically required for
                // Vulkan correctness, even though the driver is probably going
                // to use everything from the VkNativeBufferANDROID below.
                // This is kindof silly, but it's how we did the ANB
                // side of VK_KHR_swapchain v69, so we're stuck with it unless
                // we want to go tinkering with the ANB spec some more.
                VkBindImageMemorySwapchainInfoKHR bimsi = {
                    .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR,
                    .pNext = nullptr,
                    .swapchain = swapchain_handle,
                    .imageIndex = idx,
                };
                VkNativeBufferANDROID nb = {
                    .sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID,
                    .pNext = &bimsi,
                    .handle = buffer->handle,
                    .stride = buffer->stride,
                    .format = buffer->format,
                    .usage = int(buffer->usage),
                    .usage3 = buffer->usage,
                    .ahb = ANativeWindowBuffer_getHardwareBuffer(buffer),
                };
                android_convertGralloc0To1Usage(int(buffer->usage),
                                                &nb.usage2.producer,
                                                &nb.usage2.consumer);
                VkBindImageMemoryInfo bimi = {
                    .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
                    .pNext = &nb,
                    .image = swapchain.images[idx].image,
                    .memory = VK_NULL_HANDLE,
                    .memoryOffset = 0,
                };
                result = GetData(device).driver.BindImageMemory2(device, 1, &bimi);
                if (result != VK_SUCCESS) {
                    // This shouldn't really happen. If it does, something is probably
                    // unrecoverably wrong with the swapchain and its images. Cancel
                    // the buffer and declare the swapchain broken.
                    ALOGE("failed to do deferred gralloc buffer bind");
                    window->cancelBuffer(window, buffer, fence_fd);
                    return VK_ERROR_OUT_OF_DATE_KHR;
                }

                swapchain.images[idx].dequeued = true;
                swapchain.images[idx].dequeue_fence = fence_fd;
                swapchain.images[idx].buffer = buffer;
                break;
            }
        }
    }

    // The buffer doesn't match any slot. This shouldn't normally happen, but is
    // possible if the bufferqueue is reconfigured behind libvulkan's back. If this
    // happens, just declare the swapchain to be broken and the app will recreate it.
    if (idx == swapchain.num_images) {
        ALOGE("dequeueBuffer returned unrecognized buffer");
        window->cancelBuffer(window, buffer, fence_fd);
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    int fence_clone = -1;
    if (fence_fd != -1) {
        fence_clone = dup(fence_fd);
        if (fence_clone == -1) {
            ALOGE("dup(fence) failed, stalling until signalled: %s (%d)",
                  strerror(errno), errno);
            sync_wait(fence_fd, -1 /* forever */);
        }
    }

    result = GetData(device).driver.AcquireImageANDROID(
        device, swapchain.images[idx].image, fence_clone, semaphore, vk_fence);
    if (result != VK_SUCCESS) {
        // NOTE: we're relying on AcquireImageANDROID to close fence_clone,
        // even if the call fails. We could close it ourselves on failure, but
        // that would create a race condition if the driver closes it on a
        // failure path: some other thread might create an fd with the same
        // number between the time the driver closes it and the time we close
        // it. We must assume one of: the driver *always* closes it even on
        // failure, or *never* closes it on failure.
        window->cancelBuffer(window, buffer, fence_fd);
        swapchain.images[idx].dequeued = false;
        swapchain.images[idx].dequeue_fence = -1;
        return result;
    }

    *image_index = idx;
    return VK_SUCCESS;
}

VKAPI_ATTR
VkResult AcquireNextImage2KHR(VkDevice device,
                              const VkAcquireNextImageInfoKHR* pAcquireInfo,
                              uint32_t* pImageIndex) {
    ATRACE_CALL();

    return AcquireNextImageKHR(device, pAcquireInfo->swapchain,
                               pAcquireInfo->timeout, pAcquireInfo->semaphore,
                               pAcquireInfo->fence, pImageIndex);
}

static VkResult WorstPresentResult(VkResult a, VkResult b) {
    // See the error ranking for vkQueuePresentKHR at the end of section 29.6
    // (in spec version 1.0.14).
    static const VkResult kWorstToBest[] = {
        VK_ERROR_DEVICE_LOST,
        VK_ERROR_SURFACE_LOST_KHR,
        VK_ERROR_OUT_OF_DATE_KHR,
        VK_ERROR_OUT_OF_DEVICE_MEMORY,
        VK_ERROR_OUT_OF_HOST_MEMORY,
        VK_SUBOPTIMAL_KHR,
    };
    for (auto result : kWorstToBest) {
        if (a == result || b == result)
            return result;
    }
    ALOG_ASSERT(a == VK_SUCCESS, "invalid vkQueuePresentKHR result %d", a);
    ALOG_ASSERT(b == VK_SUCCESS, "invalid vkQueuePresentKHR result %d", b);
    return a != VK_SUCCESS ? a : b;
}

// KHR_incremental_present aspect of QueuePresentKHR
static void SetSwapchainSurfaceDamage(ANativeWindow *window, const VkPresentRegionKHR *pRegion) {
    std::vector<android_native_rect_t> rects(pRegion->rectangleCount);
    for (auto i = 0u; i < pRegion->rectangleCount; i++) {
        auto const& rect = pRegion->pRectangles[i];
        if (rect.layer > 0) {
            ALOGV("vkQueuePresentKHR ignoring invalid layer (%u); using layer 0 instead",
                rect.layer);
        }

        rects[i].left = rect.offset.x;
        rects[i].bottom = rect.offset.y;
        rects[i].right = rect.offset.x + rect.extent.width;
        rects[i].top = rect.offset.y + rect.extent.height;
    }
    native_window_set_surface_damage(window, rects.data(), rects.size());
}

// GOOGLE_display_timing aspect of QueuePresentKHR
static void SetSwapchainFrameTimestamp(Swapchain &swapchain, const VkPresentTimeGOOGLE *pTime) {
    ANativeWindow *window = swapchain.surface.window.get();

    // We don't know whether the app will actually use GOOGLE_display_timing
    // with a particular swapchain until QueuePresent; enable it on the BQ
    // now if needed
    if (!swapchain.frame_timestamps_enabled) {
        ALOGV("Calling native_window_enable_frame_timestamps(true)");
        native_window_enable_frame_timestamps(window, true);
        swapchain.frame_timestamps_enabled = true;
    }

    // Record the nativeFrameId so it can be later correlated to
    // this present.
    uint64_t nativeFrameId = 0;
    int err = native_window_get_next_frame_id(
            window, &nativeFrameId);
    if (err != android::OK) {
        ALOGE("Failed to get next native frame ID.");
    }

    // Add a new timing record with the user's presentID and
    // the nativeFrameId.
    swapchain.timing.emplace_back(pTime, nativeFrameId);
    if (swapchain.timing.size() > MAX_TIMING_INFOS) {
        swapchain.timing.erase(
            swapchain.timing.begin(),
            swapchain.timing.begin() + swapchain.timing.size() - MAX_TIMING_INFOS);
    }
    if (pTime->desiredPresentTime) {
        ALOGV(
            "Calling native_window_set_buffers_timestamp(%" PRId64 ")",
            pTime->desiredPresentTime);
        native_window_set_buffers_timestamp(
            window,
            static_cast<int64_t>(pTime->desiredPresentTime));
    }
}

// EXT_swapchain_maintenance1 present mode change
static bool SetSwapchainPresentMode(ANativeWindow *window, VkPresentModeKHR mode) {
    // There is no dynamic switching between non-shared present modes.
    // All we support is switching between demand and continuous refresh.
    if (!IsSharedPresentMode(mode))
        return true;

    int err = native_window_set_auto_refresh(window,
            mode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR);
    if (err != android::OK) {
        ALOGE("native_window_set_auto_refresh() failed: %s (%d)",
              strerror(-err), err);
        return false;
    }

    return true;
}

static VkResult PresentOneSwapchain(
        VkQueue queue,
        Swapchain& swapchain,
        uint32_t imageIndex,
        const VkPresentRegionKHR *pRegion,
        const VkPresentTimeGOOGLE *pTime,
        VkFence presentFence,
        const VkPresentModeKHR *pPresentMode,
        uint32_t waitSemaphoreCount,
        const VkSemaphore *pWaitSemaphores) {

    VkDevice device = GetData(queue).driver_device;
    const auto& dispatch = GetData(queue).driver;

    Swapchain::Image& img = swapchain.images[imageIndex];
    VkResult swapchain_result = VK_SUCCESS;
    VkResult result;
    int err;

    // XXX: long standing issue: QueueSignalReleaseImageANDROID consumes the
    // wait semaphores, so this doesn't actually work for the multiple swapchain
    // case.
    int fence = -1;
    result = dispatch.QueueSignalReleaseImageANDROID(
        queue, waitSemaphoreCount,
        pWaitSemaphores, img.image, &fence);
    if (result != VK_SUCCESS) {
        ALOGE("QueueSignalReleaseImageANDROID failed: %d", result);
        swapchain_result = result;
    }
    if (img.release_fence >= 0)
        close(img.release_fence);
    img.release_fence = fence < 0 ? -1 : dup(fence);

    if (swapchain.surface.swapchain_handle == HandleFromSwapchain(&swapchain)) {
        ANativeWindow* window = swapchain.surface.window.get();
        if (swapchain_result == VK_SUCCESS) {

            if (presentFence != VK_NULL_HANDLE) {
                int fence_copy = fence < 0 ? -1 : dup(fence);
                VkImportFenceFdInfoKHR iffi = {
                    VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
                    nullptr,
                    presentFence,
                    VK_FENCE_IMPORT_TEMPORARY_BIT,
                    VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
                    fence_copy,
                };
                if (VK_SUCCESS != dispatch.ImportFenceFdKHR(device, &iffi) && fence_copy >= 0) {
                    // ImportFenceFdKHR takes ownership only if it succeeds
                    close(fence_copy);
                }
            }

            if (pRegion) {
                SetSwapchainSurfaceDamage(window, pRegion);
            }
            if (pTime) {
                SetSwapchainFrameTimestamp(swapchain, pTime);
            }
            if (pPresentMode) {
                if (!SetSwapchainPresentMode(window, *pPresentMode))
                    swapchain_result = WorstPresentResult(swapchain_result,
                        VK_ERROR_SURFACE_LOST_KHR);
            }

            err = window->queueBuffer(window, img.buffer.get(), fence);
            // queueBuffer always closes fence, even on error
            if (err != android::OK) {
                ALOGE("queueBuffer failed: %s (%d)", strerror(-err), err);
                swapchain_result = WorstPresentResult(
                    swapchain_result, VK_ERROR_SURFACE_LOST_KHR);
            } else {
                if (img.dequeue_fence >= 0) {
                    close(img.dequeue_fence);
                    img.dequeue_fence = -1;
                }
                img.dequeued = false;
            }

            // If the swapchain is in shared mode, immediately dequeue the
            // buffer so it can be presented again without an intervening
            // call to AcquireNextImageKHR. We expect to get the same buffer
            // back from every call to dequeueBuffer in this mode.
            if (swapchain.shared && swapchain_result == VK_SUCCESS) {
                ANativeWindowBuffer* buffer;
                int fence_fd;
                err = window->dequeueBuffer(window, &buffer, &fence_fd);
                if (err != android::OK) {
                    ALOGE("dequeueBuffer failed: %s (%d)", strerror(-err), err);
                    swapchain_result = WorstPresentResult(swapchain_result,
                        VK_ERROR_SURFACE_LOST_KHR);
                } else if (img.buffer != buffer) {
                    ALOGE("got wrong image back for shared swapchain");
                    swapchain_result = WorstPresentResult(swapchain_result,
                        VK_ERROR_SURFACE_LOST_KHR);
                } else {
                    img.dequeue_fence = fence_fd;
                    img.dequeued = true;
                }
            }
        }
        if (swapchain_result != VK_SUCCESS) {
            OrphanSwapchain(device, &swapchain);
        }
        // Android will only return VK_SUBOPTIMAL_KHR for vkQueuePresentKHR,
        // and only when the window's transform/rotation changes.  Extent
        // changes will not cause VK_SUBOPTIMAL_KHR because of the
        // application issues that were caused when the following transform
        // change was added.
        int window_transform_hint;
        err = window->query(window, NATIVE_WINDOW_TRANSFORM_HINT,
                            &window_transform_hint);
        if (err != android::OK) {
            ALOGE("NATIVE_WINDOW_TRANSFORM_HINT query failed: %s (%d)",
                  strerror(-err), err);
            swapchain_result = WorstPresentResult(
                swapchain_result, VK_ERROR_SURFACE_LOST_KHR);
        }
        if (swapchain.pre_transform != window_transform_hint) {
            swapchain_result =
                WorstPresentResult(swapchain_result, VK_SUBOPTIMAL_KHR);
        }
    } else {
        ReleaseSwapchainImage(device, swapchain.shared, nullptr, fence,
                              img, true);
        swapchain_result = VK_ERROR_OUT_OF_DATE_KHR;
    }

    return swapchain_result;
}

VKAPI_ATTR
VkResult QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* present_info) {
    ATRACE_CALL();

    ALOGV_IF(present_info->sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
             "vkQueuePresentKHR: invalid VkPresentInfoKHR structure type %d",
             present_info->sType);

    VkResult final_result = VK_SUCCESS;

    // Look at the pNext chain for supported extension structs:
    const VkPresentRegionsKHR* present_regions = nullptr;
    const VkPresentTimesInfoGOOGLE* present_times = nullptr;
    const VkSwapchainPresentFenceInfoEXT* present_fences = nullptr;
    const VkSwapchainPresentModeInfoEXT* present_modes = nullptr;

    const VkPresentRegionsKHR* next =
        reinterpret_cast<const VkPresentRegionsKHR*>(present_info->pNext);
    while (next) {
        switch (next->sType) {
            case VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR:
                present_regions = next;
                break;
            case VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE:
                present_times =
                    reinterpret_cast<const VkPresentTimesInfoGOOGLE*>(next);
                break;
            case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT:
                present_fences =
                    reinterpret_cast<const VkSwapchainPresentFenceInfoEXT*>(next);
                break;
            case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT:
                present_modes =
                    reinterpret_cast<const VkSwapchainPresentModeInfoEXT*>(next);
                break;
            default:
                ALOGV("QueuePresentKHR ignoring unrecognized pNext->sType = %x",
                      next->sType);
                break;
        }
        next = reinterpret_cast<const VkPresentRegionsKHR*>(next->pNext);
    }
    ALOGV_IF(
        present_regions &&
            present_regions->swapchainCount != present_info->swapchainCount,
        "VkPresentRegions::swapchainCount != VkPresentInfo::swapchainCount");
    ALOGV_IF(present_times &&
                 present_times->swapchainCount != present_info->swapchainCount,
             "VkPresentTimesInfoGOOGLE::swapchainCount != "
             "VkPresentInfo::swapchainCount");
    ALOGV_IF(present_fences &&
             present_fences->swapchainCount != present_info->swapchainCount,
             "VkSwapchainPresentFenceInfoEXT::swapchainCount != "
             "VkPresentInfo::swapchainCount");
    ALOGV_IF(present_modes &&
             present_modes->swapchainCount != present_info->swapchainCount,
             "VkSwapchainPresentModeInfoEXT::swapchainCount != "
             "VkPresentInfo::swapchainCount");

    const VkPresentRegionKHR* regions =
        (present_regions) ? present_regions->pRegions : nullptr;
    const VkPresentTimeGOOGLE* times =
        (present_times) ? present_times->pTimes : nullptr;

    for (uint32_t sc = 0; sc < present_info->swapchainCount; sc++) {
        Swapchain& swapchain =
            *SwapchainFromHandle(present_info->pSwapchains[sc]);

        VkResult swapchain_result = PresentOneSwapchain(
            queue,
            swapchain,
            present_info->pImageIndices[sc],
            (regions && !swapchain.mailbox_mode) ? &regions[sc] : nullptr,
            times ? &times[sc] : nullptr,
            present_fences ? present_fences->pFences[sc] : VK_NULL_HANDLE,
            present_modes ? &present_modes->pPresentModes[sc] : nullptr,
            present_info->waitSemaphoreCount,
            present_info->pWaitSemaphores);

        if (present_info->pResults)
            present_info->pResults[sc] = swapchain_result;

        if (swapchain_result != final_result)
            final_result = WorstPresentResult(final_result, swapchain_result);
    }

    return final_result;
}

VKAPI_ATTR
VkResult GetRefreshCycleDurationGOOGLE(
    VkDevice,
    VkSwapchainKHR swapchain_handle,
    VkRefreshCycleDurationGOOGLE* pDisplayTimingProperties) {
    ATRACE_CALL();

    Swapchain& swapchain = *SwapchainFromHandle(swapchain_handle);
    VkResult result = swapchain.get_refresh_duration(pDisplayTimingProperties->refreshDuration);

    return result;
}

VKAPI_ATTR
VkResult GetPastPresentationTimingGOOGLE(
    VkDevice,
    VkSwapchainKHR swapchain_handle,
    uint32_t* count,
    VkPastPresentationTimingGOOGLE* timings) {
    ATRACE_CALL();

    Swapchain& swapchain = *SwapchainFromHandle(swapchain_handle);
    if (swapchain.surface.swapchain_handle != swapchain_handle) {
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    ANativeWindow* window = swapchain.surface.window.get();
    VkResult result = VK_SUCCESS;

    if (!swapchain.frame_timestamps_enabled) {
        ALOGV("Calling native_window_enable_frame_timestamps(true)");
        native_window_enable_frame_timestamps(window, true);
        swapchain.frame_timestamps_enabled = true;
    }

    if (timings) {
        // Get the latest ready timing count before copying, since the copied
        // timing info will be erased in copy_ready_timings function.
        uint32_t n = get_num_ready_timings(swapchain);
        copy_ready_timings(swapchain, count, timings);
        // Check the *count here against the recorded ready timing count, since
        // *count can be overwritten per spec describes.
        if (*count < n) {
            result = VK_INCOMPLETE;
        }
    } else {
        *count = get_num_ready_timings(swapchain);
    }

    return result;
}

VKAPI_ATTR
VkResult GetSwapchainStatusKHR(
    VkDevice,
    VkSwapchainKHR swapchain_handle) {
    ATRACE_CALL();

    Swapchain& swapchain = *SwapchainFromHandle(swapchain_handle);
    VkResult result = VK_SUCCESS;

    if (swapchain.surface.swapchain_handle != swapchain_handle) {
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    // TODO(b/143296009): Implement this function properly

    return result;
}

VKAPI_ATTR void SetHdrMetadataEXT(
    VkDevice,
    uint32_t swapchainCount,
    const VkSwapchainKHR* pSwapchains,
    const VkHdrMetadataEXT* pHdrMetadataEXTs) {
    ATRACE_CALL();

    for (uint32_t idx = 0; idx < swapchainCount; idx++) {
        Swapchain* swapchain = SwapchainFromHandle(pSwapchains[idx]);
        if (!swapchain)
            continue;

        if (swapchain->surface.swapchain_handle != pSwapchains[idx]) continue;

        ANativeWindow* window = swapchain->surface.window.get();

        VkHdrMetadataEXT vulkanMetadata = pHdrMetadataEXTs[idx];
        const android_smpte2086_metadata smpteMetdata = {
            {vulkanMetadata.displayPrimaryRed.x,
             vulkanMetadata.displayPrimaryRed.y},
            {vulkanMetadata.displayPrimaryGreen.x,
             vulkanMetadata.displayPrimaryGreen.y},
            {vulkanMetadata.displayPrimaryBlue.x,
             vulkanMetadata.displayPrimaryBlue.y},
            {vulkanMetadata.whitePoint.x, vulkanMetadata.whitePoint.y},
            vulkanMetadata.maxLuminance,
            vulkanMetadata.minLuminance};
        native_window_set_buffers_smpte2086_metadata(window, &smpteMetdata);

        const android_cta861_3_metadata cta8613Metadata = {
            vulkanMetadata.maxContentLightLevel,
            vulkanMetadata.maxFrameAverageLightLevel};
        native_window_set_buffers_cta861_3_metadata(window, &cta8613Metadata);
    }

    return;
}

static void InterceptBindImageMemory2(
    uint32_t bind_info_count,
    const VkBindImageMemoryInfo* bind_infos,
    std::vector<VkNativeBufferANDROID>* out_native_buffers,
    std::vector<VkBindImageMemoryInfo>* out_bind_infos) {
    out_native_buffers->clear();
    out_bind_infos->clear();

    if (!bind_info_count)
        return;

    std::unordered_set<uint32_t> intercepted_indexes;

    for (uint32_t idx = 0; idx < bind_info_count; idx++) {
        auto info = reinterpret_cast<const VkBindImageMemorySwapchainInfoKHR*>(
            bind_infos[idx].pNext);
        while (info &&
               info->sType !=
                   VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR) {
            info = reinterpret_cast<const VkBindImageMemorySwapchainInfoKHR*>(
                info->pNext);
        }

        if (!info)
            continue;

        ALOG_ASSERT(info->swapchain != VK_NULL_HANDLE,
                    "swapchain handle must not be NULL");
        const Swapchain* swapchain = SwapchainFromHandle(info->swapchain);
        ALOG_ASSERT(
            info->imageIndex < swapchain->num_images,
            "imageIndex must be less than the number of images in swapchain");

        ANativeWindowBuffer* buffer =
            swapchain->images[info->imageIndex].buffer.get();
        VkNativeBufferANDROID native_buffer = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
            .sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID,
#pragma clang diagnostic pop
            .pNext = bind_infos[idx].pNext,
            .handle = buffer->handle,
            .stride = buffer->stride,
            .format = buffer->format,
            .usage = int(buffer->usage),
            .usage3 = buffer->usage,
            .ahb = ANativeWindowBuffer_getHardwareBuffer(buffer),
        };
        android_convertGralloc0To1Usage(int(buffer->usage),
                                        &native_buffer.usage2.producer,
                                        &native_buffer.usage2.consumer);
        // Reserve enough space to avoid letting re-allocation invalidate the
        // addresses of the elements inside.
        out_native_buffers->reserve(bind_info_count);
        out_native_buffers->emplace_back(native_buffer);

        // Reserve the space now since we know how much is needed now.
        out_bind_infos->reserve(bind_info_count);
        out_bind_infos->emplace_back(bind_infos[idx]);
        out_bind_infos->back().pNext = &out_native_buffers->back();

        intercepted_indexes.insert(idx);
    }

    if (intercepted_indexes.empty())
        return;

    for (uint32_t idx = 0; idx < bind_info_count; idx++) {
        if (intercepted_indexes.count(idx))
            continue;
        out_bind_infos->emplace_back(bind_infos[idx]);
    }
}

VKAPI_ATTR
VkResult BindImageMemory2(VkDevice device,
                          uint32_t bindInfoCount,
                          const VkBindImageMemoryInfo* pBindInfos) {
    ATRACE_CALL();

    // out_native_buffers is for maintaining the lifecycle of the constructed
    // VkNativeBufferANDROID objects inside InterceptBindImageMemory2.
    std::vector<VkNativeBufferANDROID> out_native_buffers;
    std::vector<VkBindImageMemoryInfo> out_bind_infos;
    InterceptBindImageMemory2(bindInfoCount, pBindInfos, &out_native_buffers,
                              &out_bind_infos);
    return GetData(device).driver.BindImageMemory2(
        device, bindInfoCount,
        out_bind_infos.empty() ? pBindInfos : out_bind_infos.data());
}

VKAPI_ATTR
VkResult BindImageMemory2KHR(VkDevice device,
                             uint32_t bindInfoCount,
                             const VkBindImageMemoryInfo* pBindInfos) {
    ATRACE_CALL();

    std::vector<VkNativeBufferANDROID> out_native_buffers;
    std::vector<VkBindImageMemoryInfo> out_bind_infos;
    InterceptBindImageMemory2(bindInfoCount, pBindInfos, &out_native_buffers,
                              &out_bind_infos);
    return GetData(device).driver.BindImageMemory2KHR(
        device, bindInfoCount,
        out_bind_infos.empty() ? pBindInfos : out_bind_infos.data());
}

VKAPI_ATTR
VkResult ReleaseSwapchainImagesEXT(VkDevice /*device*/,
                                   const VkReleaseSwapchainImagesInfoEXT* pReleaseInfo) {
    ATRACE_CALL();

    Swapchain& swapchain = *SwapchainFromHandle(pReleaseInfo->swapchain);
    ANativeWindow* window = swapchain.surface.window.get();

    // If in shared present mode, don't actually release the image back to the BQ.
    // Both sides share it forever.
    if (swapchain.shared)
        return VK_SUCCESS;

    for (uint32_t i = 0; i < pReleaseInfo->imageIndexCount; i++) {
        Swapchain::Image& img = swapchain.images[pReleaseInfo->pImageIndices[i]];
        window->cancelBuffer(window, img.buffer.get(), img.dequeue_fence);

        // cancelBuffer has taken ownership of the dequeue fence
        img.dequeue_fence = -1;
        // if we're still holding a release fence, get rid of it now
        if (img.release_fence >= 0) {
           close(img.release_fence);
           img.release_fence = -1;
        }
        img.dequeued = false;
    }

    return VK_SUCCESS;
}

}  // namespace driver
}  // namespace vulkan
