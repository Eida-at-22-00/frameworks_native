
/*
 * Copyright 2024 The Android Open Source Project
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
 *
 */

// clang-format off

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif

#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <map>
#include "vulkan_profiles.h"

#include <cstdio>

#ifndef VP_DEBUG_MESSAGE_CALLBACK
#if defined(ANDROID) || defined(__ANDROID__)
#include <android/log.h>
#define VP_DEBUG_MESSAGE_CALLBACK(MSG)     __android_log_print(ANDROID_LOG_ERROR, "vkprofiles ERROR", "%s", MSG); \
    __android_log_print(ANDROID_LOG_DEBUG, "vkprofiles WARNING", "%s", MSG)
#else
#define VP_DEBUG_MESSAGE_CALLBACK(MSG) fprintf(stderr, "%s\n", MSG)
#endif
#else
void VP_DEBUG_MESSAGE_CALLBACK(const char*);
#endif

#define VP_DEBUG_MSG(MSG) VP_DEBUG_MESSAGE_CALLBACK(MSG)
#define VP_DEBUG_MSGF(MSGFMT, ...) { char msg[1024]; snprintf(msg, sizeof(msg) - 1, (MSGFMT), __VA_ARGS__); VP_DEBUG_MESSAGE_CALLBACK(msg); }
#define VP_DEBUG_COND_MSG(COND, MSG) if (COND) VP_DEBUG_MSG(MSG)
#define VP_DEBUG_COND_MSGF(COND, MSGFMT, ...) if (COND) VP_DEBUG_MSGF(MSGFMT, __VA_ARGS__)

#include <string>

namespace detail {

VPAPI_ATTR std::string vpGetDeviceAndDriverInfoString(VkPhysicalDevice physicalDevice,
                                                      PFN_vkGetPhysicalDeviceProperties2KHR pfnGetPhysicalDeviceProperties2) {
    VkPhysicalDeviceDriverPropertiesKHR driverProps{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2KHR deviceProps{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR, &driverProps };
    pfnGetPhysicalDeviceProperties2(physicalDevice, &deviceProps);
    return std::string("deviceName=") + std::string(&deviceProps.properties.deviceName[0])
                    + ", driverName=" + std::string(&driverProps.driverName[0])
                    + ", driverInfo=" + std::string(&driverProps.driverInfo[0]);
}

}

namespace detail {


VPAPI_ATTR std::string FormatString(const char* message, ...) {
    std::size_t const STRING_BUFFER(4096);

    assert(message != nullptr);
    assert(strlen(message) >= 1 && strlen(message) < STRING_BUFFER);

    char buffer[STRING_BUFFER];
    va_list list;

    va_start(list, message);
    vsnprintf(buffer, STRING_BUFFER, message, list);
    va_end(list);

    return buffer;
}

VPAPI_ATTR const void* vpGetStructure(const void* pNext, VkStructureType type) {
    const VkBaseOutStructure* p = static_cast<const VkBaseOutStructure*>(pNext);
    while (p != nullptr) {
        if (p->sType == type) return p;
        p = p->pNext;
    }
    return nullptr;
}

VPAPI_ATTR void* vpGetStructure(void* pNext, VkStructureType type) {
    VkBaseOutStructure* p = static_cast<VkBaseOutStructure*>(pNext);
    while (p != nullptr) {
        if (p->sType == type) return p;
        p = p->pNext;
    }
    return nullptr;
}

VPAPI_ATTR VkBaseOutStructure* vpExtractStructure(VkPhysicalDeviceFeatures2KHR* pFeatures, VkStructureType structureType) {
    if (structureType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR) {
        return nullptr;
    }

    VkBaseOutStructure* current = reinterpret_cast<VkBaseOutStructure*>(pFeatures);
    VkBaseOutStructure* previous = nullptr;
    VkBaseOutStructure* found = nullptr;

    while (current != nullptr) {
        if (structureType == current->sType) {
            found = current;
            if (previous != nullptr) {
                previous->pNext = current->pNext;
            }
            current = nullptr;
        } else {
            previous = current;
            current = current->pNext;
        }
    }

    if (found != nullptr) {
        found->pNext = nullptr;
        return found;
    } else {
        return nullptr;
    }
}

VPAPI_ATTR void GatherStructureTypes(std::vector<VkStructureType>& structureTypes, VkBaseOutStructure* pNext) {
    while (pNext) {
        if (std::find(structureTypes.begin(), structureTypes.end(), pNext->sType) == structureTypes.end()) {
            structureTypes.push_back(pNext->sType);
        }

        pNext = pNext->pNext;
    }
}

VPAPI_ATTR bool isMultiple(double source, double multiple) {
    double mod = std::fmod(source, multiple);
    return std::abs(mod) < 0.0001;
}

VPAPI_ATTR bool isPowerOfTwo(double source) {
    double mod = std::fmod(source, 1.0);
    if (std::abs(mod) >= 0.0001) return false;

    std::uint64_t value = static_cast<std::uint64_t>(std::abs(source));
    return !(value & (value - static_cast<std::uint64_t>(1)));
}

using PFN_vpStructFiller = void(*)(VkBaseOutStructure* p);
using PFN_vpStructComparator = bool(*)(VkBaseOutStructure* p);
using PFN_vpStructChainerCb = void(*)(VkBaseOutStructure* p, void* pUser);
using PFN_vpStructChainer = void(*)(VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb);
using PFN_vpStructArrayChainerCb = void(*)(uint32_t count, VkBaseOutStructure* p, void* pUser);
using PFN_vpStructArrayChainer = void(*)(uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb);

struct VpFeatureDesc {
    PFN_vpStructFiller              pfnFiller;
    PFN_vpStructComparator          pfnComparator;
};

struct VpPropertyDesc {
    PFN_vpStructFiller              pfnFiller;
    PFN_vpStructComparator          pfnComparator;
};

struct VpQueueFamilyDesc {
    PFN_vpStructFiller              pfnFiller;
    PFN_vpStructComparator          pfnComparator;
};

struct VpFormatDesc {
    VkFormat                        format;
    PFN_vpStructFiller              pfnFiller;
    PFN_vpStructComparator          pfnComparator;
};

struct VpStructChainerDesc {
    PFN_vpStructChainer             pfnFeature;
    PFN_vpStructChainer             pfnProperty;
    PFN_vpStructArrayChainer        pfnQueueFamily;
    PFN_vpStructChainer             pfnFormat;
};

struct VpVideoProfileInfoDesc {
    PFN_vpStructFiller              pfnFiller;
    PFN_vpStructComparator          pfnComparator;
};

struct VpVideoCapabilityDesc {
    PFN_vpStructFiller              pfnFiller;
    PFN_vpStructComparator          pfnComparator;
};

struct VpVideoFormatDesc {
    PFN_vpStructFiller              pfnFiller;
    PFN_vpStructComparator          pfnComparator;
};

struct VpVideoProfileStructChainerDesc {
    PFN_vpStructChainer             pfnInfo;
    PFN_vpStructChainer             pfnCapability;
    PFN_vpStructArrayChainer        pfnFormat;
};

struct VpVideoProfileDesc {
    VpVideoProfileProperties properties;

    uint32_t infoStructTypeCount;
    const VkStructureType* pInfoStructTypes;
    VpVideoProfileInfoDesc info;

    uint32_t capabilityStructTypeCount;
    const VkStructureType* pCapabilityStructTypes;
    VpVideoCapabilityDesc capability;

    uint32_t formatStructTypeCount;
    const VkStructureType* pFormatStructTypes;
    uint32_t formatCount;
    const VpVideoFormatDesc* pFormats;

    VpVideoProfileStructChainerDesc chainers;
};

struct VpVariantDesc {
    char blockName[VP_MAX_PROFILE_NAME_SIZE];

    uint32_t instanceExtensionCount;
    const VkExtensionProperties* pInstanceExtensions;

    uint32_t deviceExtensionCount;
    const VkExtensionProperties* pDeviceExtensions;

    uint32_t featureStructTypeCount;
    const VkStructureType* pFeatureStructTypes;
    VpFeatureDesc feature;

    uint32_t propertyStructTypeCount;
    const VkStructureType* pPropertyStructTypes;
    VpPropertyDesc property;

    uint32_t queueFamilyStructTypeCount;
    const VkStructureType* pQueueFamilyStructTypes;
    uint32_t queueFamilyCount;
    const VpQueueFamilyDesc* pQueueFamilies;

    uint32_t formatStructTypeCount;
    const VkStructureType* pFormatStructTypes;
    uint32_t formatCount;
    const VpFormatDesc* pFormats;

    VpStructChainerDesc chainers;

    uint32_t videoProfileCount;
    const VpVideoProfileDesc* pVideoProfiles;
};

struct VpCapabilitiesDesc {
    uint32_t variantCount;
    const VpVariantDesc* pVariants;
};

struct VpProfileDesc {
    VpProfileProperties             props;
    uint32_t                        minApiVersion;

    const detail::VpVariantDesc*    pMergedCapabilities;

    uint32_t                        requiredProfileCount;
    const VpProfileProperties*      pRequiredProfiles;

    uint32_t                        requiredCapabilityCount;
    const VpCapabilitiesDesc*       pRequiredCapabilities;

    uint32_t                        fallbackCount;
    const VpProfileProperties*      pFallbacks;
};

template <typename T>
VPAPI_ATTR bool vpCheckFlags(const T& actual, const uint64_t expected) {
    return (actual & expected) == expected;
}


#ifdef VK_KHR_video_queue
VPAPI_ATTR void vpForEachMatchingVideoProfiles(
    VkVideoProfileInfoKHR*                      pVideoProfileInfo,
    void*                                       pUser,
    PFN_vpStructChainerCb                       pfnCb) {
    const VkVideoChromaSubsamplingFlagsKHR chroma_subsampling_list[] = {
        VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
        VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR,
        VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR,
        VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR
    };
    const VkVideoComponentBitDepthFlagsKHR bit_depth_list[] = {
        VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR,
        VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR
    };
    for (size_t chromaSubsampling_idx = 0; chromaSubsampling_idx < std::size(chroma_subsampling_list); ++chromaSubsampling_idx) {
        pVideoProfileInfo->chromaSubsampling = chroma_subsampling_list[chromaSubsampling_idx];
        for (size_t lumaBitDepth_idx = 0; lumaBitDepth_idx < std::size(bit_depth_list); ++lumaBitDepth_idx) {
            pVideoProfileInfo->lumaBitDepth = bit_depth_list[lumaBitDepth_idx];
            for (size_t chromaBitDepth_idx = 0; chromaBitDepth_idx < std::size(bit_depth_list); ++chromaBitDepth_idx) {
                pVideoProfileInfo->chromaBitDepth = bit_depth_list[chromaBitDepth_idx];
                {
                    pVideoProfileInfo->pNext = nullptr;
                    pVideoProfileInfo->videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
                    VkVideoDecodeH264ProfileInfoKHR var_VideoDecodeH264ProfileInfoKHR = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR };
                    var_VideoDecodeH264ProfileInfoKHR.pNext = pVideoProfileInfo->pNext;
                    pVideoProfileInfo->pNext = &var_VideoDecodeH264ProfileInfoKHR;
                    var_VideoDecodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
                    var_VideoDecodeH264ProfileInfoKHR.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
                    var_VideoDecodeH264ProfileInfoKHR.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_INTERLACED_INTERLEAVED_LINES_BIT_KHR;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
                    var_VideoDecodeH264ProfileInfoKHR.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_INTERLACED_SEPARATE_PLANES_BIT_KHR;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                }
                {
                    pVideoProfileInfo->pNext = nullptr;
                    pVideoProfileInfo->videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
                    VkVideoDecodeH265ProfileInfoKHR var_VideoDecodeH265ProfileInfoKHR = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR };
                    var_VideoDecodeH265ProfileInfoKHR.pNext = pVideoProfileInfo->pNext;
                    pVideoProfileInfo->pNext = &var_VideoDecodeH265ProfileInfoKHR;
                    var_VideoDecodeH265ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH265ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH265ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_STILL_PICTURE;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH265ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeH265ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_SCC_EXTENSIONS;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                }
                {
                    pVideoProfileInfo->pNext = nullptr;
                    pVideoProfileInfo->videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
                    VkVideoDecodeAV1ProfileInfoKHR var_VideoDecodeAV1ProfileInfoKHR = { VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR };
                    var_VideoDecodeAV1ProfileInfoKHR.pNext = pVideoProfileInfo->pNext;
                    pVideoProfileInfo->pNext = &var_VideoDecodeAV1ProfileInfoKHR;
                    var_VideoDecodeAV1ProfileInfoKHR.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
                    var_VideoDecodeAV1ProfileInfoKHR.filmGrainSupport = VK_TRUE;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeAV1ProfileInfoKHR.stdProfile = STD_VIDEO_AV1_PROFILE_HIGH;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeAV1ProfileInfoKHR.stdProfile = STD_VIDEO_AV1_PROFILE_PROFESSIONAL;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeAV1ProfileInfoKHR.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
                    var_VideoDecodeAV1ProfileInfoKHR.filmGrainSupport = VK_FALSE;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeAV1ProfileInfoKHR.stdProfile = STD_VIDEO_AV1_PROFILE_HIGH;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoDecodeAV1ProfileInfoKHR.stdProfile = STD_VIDEO_AV1_PROFILE_PROFESSIONAL;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                }
                {
                    pVideoProfileInfo->pNext = nullptr;
                    pVideoProfileInfo->videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
                    VkVideoEncodeH264ProfileInfoKHR var_VideoEncodeH264ProfileInfoKHR = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR };
                    var_VideoEncodeH264ProfileInfoKHR.pNext = pVideoProfileInfo->pNext;
                    pVideoProfileInfo->pNext = &var_VideoEncodeH264ProfileInfoKHR;
                    var_VideoEncodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoEncodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoEncodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoEncodeH264ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH_444_PREDICTIVE;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                }
                {
                    pVideoProfileInfo->pNext = nullptr;
                    pVideoProfileInfo->videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
                    VkVideoEncodeH265ProfileInfoKHR var_VideoEncodeH265ProfileInfoKHR = { VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR };
                    var_VideoEncodeH265ProfileInfoKHR.pNext = pVideoProfileInfo->pNext;
                    pVideoProfileInfo->pNext = &var_VideoEncodeH265ProfileInfoKHR;
                    var_VideoEncodeH265ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoEncodeH265ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoEncodeH265ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_STILL_PICTURE;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoEncodeH265ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_FORMAT_RANGE_EXTENSIONS;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                    var_VideoEncodeH265ProfileInfoKHR.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_SCC_EXTENSIONS;
                    pfnCb(reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo), pUser);
                }
            }
        }
    }
}
#endif  // VK_KHR_video_queue
#ifdef VP_ANDROID_15_minimums
namespace VP_ANDROID_15_MINIMUMS {

static const VkStructureType featureStructTypes[] = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG,
};

static const VkStructureType propertyStructTypes[] = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
};

static const VkStructureType formatStructTypes[] = {
    VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR,
    VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR,
};

namespace blocks {
namespace MUST {

static const VkExtensionProperties instanceExtensions[] = {
    VkExtensionProperties{ VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_GOOGLE_SURFACELESS_QUERY_EXTENSION_NAME, 1 },
};

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_ANDROID_EXTERNAL_FORMAT_RESOLVE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_4444_FORMATS_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_DEVICE_MEMORY_REPORT_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_EXTERNAL_MEMORY_ACQUIRE_UNMODIFIED_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_16BIT_STORAGE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_MAINTENANCE_5_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    s->features.drawIndirectFirstInstance = VK_TRUE;
                    s->features.samplerAnisotropy = VK_TRUE;
                    s->features.shaderImageGatherExtended = VK_TRUE;
                    s->features.shaderStorageImageExtendedFormats = VK_TRUE;
                    s->features.shaderStorageImageReadWithoutFormat = VK_TRUE;
                    s->features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                    VkPhysicalDeviceVulkan12Features* s = static_cast<VkPhysicalDeviceVulkan12Features*>(static_cast<void*>(p));
                    s->shaderFloat16 = VK_TRUE;
                    s->shaderInt8 = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: {
                    VkPhysicalDeviceCustomBorderColorFeaturesEXT* s = static_cast<VkPhysicalDeviceCustomBorderColorFeaturesEXT*>(static_cast<void*>(p));
                    s->customBorderColors = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT: {
                    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT* s = static_cast<VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT*>(static_cast<void*>(p));
                    s->primitiveTopologyListRestart = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT: {
                    VkPhysicalDeviceProvokingVertexFeaturesEXT* s = static_cast<VkPhysicalDeviceProvokingVertexFeaturesEXT*>(static_cast<void*>(p));
                    s->provokingVertexLast = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT: {
                    VkPhysicalDeviceIndexTypeUint8FeaturesEXT* s = static_cast<VkPhysicalDeviceIndexTypeUint8FeaturesEXT*>(static_cast<void*>(p));
                    s->indexTypeUint8 = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR: {
                    VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR* s = static_cast<VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR*>(static_cast<void*>(p));
                    s->vertexAttributeInstanceRateDivisor = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES: {
                    VkPhysicalDeviceSamplerYcbcrConversionFeatures* s = static_cast<VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(static_cast<void*>(p));
                    s->samplerYcbcrConversion = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: {
                    VkPhysicalDeviceShaderFloat16Int8Features* s = static_cast<VkPhysicalDeviceShaderFloat16Int8Features*>(static_cast<void*>(p));
                    s->shaderFloat16 = VK_TRUE;
                    s->shaderInt8 = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES: {
                    VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures* s = static_cast<VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures*>(static_cast<void*>(p));
                    s->shaderSubgroupExtendedTypes = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: {
                    VkPhysicalDevice8BitStorageFeatures* s = static_cast<VkPhysicalDevice8BitStorageFeatures*>(static_cast<void*>(p));
                    s->storageBuffer8BitAccess = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
                    VkPhysicalDevice16BitStorageFeatures* s = static_cast<VkPhysicalDevice16BitStorageFeatures*>(static_cast<void*>(p));
                    s->storageBuffer16BitAccess = VK_TRUE;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->features.drawIndirectFirstInstance == VK_TRUE);
                    ret = ret && (s->features.samplerAnisotropy == VK_TRUE);
                    ret = ret && (s->features.shaderImageGatherExtended == VK_TRUE);
                    ret = ret && (s->features.shaderStorageImageExtendedFormats == VK_TRUE);
                    ret = ret && (s->features.shaderStorageImageReadWithoutFormat == VK_TRUE);
                    ret = ret && (s->features.shaderStorageImageWriteWithoutFormat == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                    VkPhysicalDeviceVulkan12Features* s = static_cast<VkPhysicalDeviceVulkan12Features*>(static_cast<void*>(p));
                    ret = ret && (s->shaderFloat16 == VK_TRUE);
                    ret = ret && (s->shaderInt8 == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: {
                    VkPhysicalDeviceCustomBorderColorFeaturesEXT* s = static_cast<VkPhysicalDeviceCustomBorderColorFeaturesEXT*>(static_cast<void*>(p));
                    ret = ret && (s->customBorderColors == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT: {
                    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT* s = static_cast<VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT*>(static_cast<void*>(p));
                    ret = ret && (s->primitiveTopologyListRestart == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT: {
                    VkPhysicalDeviceProvokingVertexFeaturesEXT* s = static_cast<VkPhysicalDeviceProvokingVertexFeaturesEXT*>(static_cast<void*>(p));
                    ret = ret && (s->provokingVertexLast == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT: {
                    VkPhysicalDeviceIndexTypeUint8FeaturesEXT* s = static_cast<VkPhysicalDeviceIndexTypeUint8FeaturesEXT*>(static_cast<void*>(p));
                    ret = ret && (s->indexTypeUint8 == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR: {
                    VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR* s = static_cast<VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR*>(static_cast<void*>(p));
                    ret = ret && (s->vertexAttributeInstanceRateDivisor == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES: {
                    VkPhysicalDeviceSamplerYcbcrConversionFeatures* s = static_cast<VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->samplerYcbcrConversion == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: {
                    VkPhysicalDeviceShaderFloat16Int8Features* s = static_cast<VkPhysicalDeviceShaderFloat16Int8Features*>(static_cast<void*>(p));
                    ret = ret && (s->shaderFloat16 == VK_TRUE);
                    ret = ret && (s->shaderInt8 == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES: {
                    VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures* s = static_cast<VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->shaderSubgroupExtendedTypes == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: {
                    VkPhysicalDevice8BitStorageFeatures* s = static_cast<VkPhysicalDevice8BitStorageFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->storageBuffer8BitAccess == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
                    VkPhysicalDevice16BitStorageFeatures* s = static_cast<VkPhysicalDevice16BitStorageFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->storageBuffer16BitAccess == VK_TRUE);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR: {
                    VkPhysicalDeviceProperties2KHR* s = static_cast<VkPhysicalDeviceProperties2KHR*>(static_cast<void*>(p));
                    s->properties.limits.maxColorAttachments = 8;
                    s->properties.limits.maxPerStageDescriptorSampledImages = 128;
                    s->properties.limits.maxPerStageDescriptorSamplers = 128;
                    s->properties.limits.maxPerStageDescriptorStorageBuffers = 12;
                    s->properties.limits.maxPerStageDescriptorUniformBuffers = 13;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
                    VkPhysicalDeviceVulkan11Properties* s = static_cast<VkPhysicalDeviceVulkan11Properties*>(static_cast<void*>(p));
                    s->subgroupSupportedOperations |= (VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT);
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR: {
                    VkPhysicalDeviceProperties2KHR* s = static_cast<VkPhysicalDeviceProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->properties.limits.maxColorAttachments >= 8);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorSampledImages >= 128);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorSamplers >= 128);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorStorageBuffers >= 12);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorUniformBuffers >= 13);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
                    VkPhysicalDeviceVulkan11Properties* s = static_cast<VkPhysicalDeviceVulkan11Properties*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->subgroupSupportedOperations, (VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT)));
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpFormatDesc formatDesc[] = {
    {
        VK_FORMAT_A4B4G4R4_UNORM_PACK16_EXT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan12Features physicalDeviceVulkan12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, nullptr };
        VkPhysicalDeviceCustomBorderColorFeaturesEXT physicalDeviceCustomBorderColorFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, &physicalDeviceVulkan12Features };
        VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT physicalDevicePrimitiveTopologyListRestartFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT, &physicalDeviceCustomBorderColorFeaturesEXT };
        VkPhysicalDeviceProvokingVertexFeaturesEXT physicalDeviceProvokingVertexFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, &physicalDevicePrimitiveTopologyListRestartFeaturesEXT };
        VkPhysicalDeviceIndexTypeUint8FeaturesEXT physicalDeviceIndexTypeUint8FeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, &physicalDeviceProvokingVertexFeaturesEXT };
        VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR physicalDeviceVertexAttributeDivisorFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR, &physicalDeviceIndexTypeUint8FeaturesEXT };
        VkPhysicalDeviceSamplerYcbcrConversionFeatures physicalDeviceSamplerYcbcrConversionFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, &physicalDeviceVertexAttributeDivisorFeaturesKHR };
        VkPhysicalDeviceShaderFloat16Int8Features physicalDeviceShaderFloat16Int8Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, &physicalDeviceSamplerYcbcrConversionFeatures };
        VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures physicalDeviceShaderSubgroupExtendedTypesFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, &physicalDeviceShaderFloat16Int8Features };
        VkPhysicalDevice8BitStorageFeatures physicalDevice8BitStorageFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, &physicalDeviceShaderSubgroupExtendedTypesFeatures };
        VkPhysicalDevice16BitStorageFeatures physicalDevice16BitStorageFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, &physicalDevice8BitStorageFeatures };
        VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT physicalDevicePrimitivesGeneratedQueryFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT, &physicalDevice16BitStorageFeatures };
        VkPhysicalDeviceLineRasterizationFeaturesEXT physicalDeviceLineRasterizationFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, &physicalDevicePrimitivesGeneratedQueryFeaturesEXT };
        VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG physicalDeviceRelaxedLineRasterizationFeaturesIMG{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG, &physicalDeviceLineRasterizationFeaturesEXT };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceRelaxedLineRasterizationFeaturesIMG));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan11Properties physicalDeviceVulkan11Properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceVulkan11Properties));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkFormatProperties3KHR formatProperties3KHR{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&formatProperties3KHR));
        pfnCb(p, pUser);
    },
};
} // namespace MUST
namespace primitivesGeneratedQuery {

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_EXT_PRIMITIVES_GENERATED_QUERY_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT: {
                    VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT* s = static_cast<VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT*>(static_cast<void*>(p));
                    s->primitivesGeneratedQuery = VK_TRUE;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT: {
                    VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT* s = static_cast<VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT*>(static_cast<void*>(p));
                    ret = ret && (s->primitivesGeneratedQuery == VK_TRUE);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
        return ret;
    }
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan12Features physicalDeviceVulkan12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, nullptr };
        VkPhysicalDeviceCustomBorderColorFeaturesEXT physicalDeviceCustomBorderColorFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, &physicalDeviceVulkan12Features };
        VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT physicalDevicePrimitiveTopologyListRestartFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT, &physicalDeviceCustomBorderColorFeaturesEXT };
        VkPhysicalDeviceProvokingVertexFeaturesEXT physicalDeviceProvokingVertexFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, &physicalDevicePrimitiveTopologyListRestartFeaturesEXT };
        VkPhysicalDeviceIndexTypeUint8FeaturesEXT physicalDeviceIndexTypeUint8FeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, &physicalDeviceProvokingVertexFeaturesEXT };
        VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR physicalDeviceVertexAttributeDivisorFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR, &physicalDeviceIndexTypeUint8FeaturesEXT };
        VkPhysicalDeviceSamplerYcbcrConversionFeatures physicalDeviceSamplerYcbcrConversionFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, &physicalDeviceVertexAttributeDivisorFeaturesKHR };
        VkPhysicalDeviceShaderFloat16Int8Features physicalDeviceShaderFloat16Int8Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, &physicalDeviceSamplerYcbcrConversionFeatures };
        VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures physicalDeviceShaderSubgroupExtendedTypesFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, &physicalDeviceShaderFloat16Int8Features };
        VkPhysicalDevice8BitStorageFeatures physicalDevice8BitStorageFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, &physicalDeviceShaderSubgroupExtendedTypesFeatures };
        VkPhysicalDevice16BitStorageFeatures physicalDevice16BitStorageFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, &physicalDevice8BitStorageFeatures };
        VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT physicalDevicePrimitivesGeneratedQueryFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT, &physicalDevice16BitStorageFeatures };
        VkPhysicalDeviceLineRasterizationFeaturesEXT physicalDeviceLineRasterizationFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, &physicalDevicePrimitivesGeneratedQueryFeaturesEXT };
        VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG physicalDeviceRelaxedLineRasterizationFeaturesIMG{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG, &physicalDeviceLineRasterizationFeaturesEXT };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceRelaxedLineRasterizationFeaturesIMG));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan11Properties physicalDeviceVulkan11Properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceVulkan11Properties));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkFormatProperties3KHR formatProperties3KHR{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&formatProperties3KHR));
        pfnCb(p, pUser);
    },
};
} // namespace primitivesGeneratedQuery
namespace pipelineStatisticsQuery {

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    s->features.pipelineStatisticsQuery = VK_TRUE;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->features.pipelineStatisticsQuery == VK_TRUE);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
        return ret;
    }
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan12Features physicalDeviceVulkan12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, nullptr };
        VkPhysicalDeviceCustomBorderColorFeaturesEXT physicalDeviceCustomBorderColorFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, &physicalDeviceVulkan12Features };
        VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT physicalDevicePrimitiveTopologyListRestartFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT, &physicalDeviceCustomBorderColorFeaturesEXT };
        VkPhysicalDeviceProvokingVertexFeaturesEXT physicalDeviceProvokingVertexFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, &physicalDevicePrimitiveTopologyListRestartFeaturesEXT };
        VkPhysicalDeviceIndexTypeUint8FeaturesEXT physicalDeviceIndexTypeUint8FeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, &physicalDeviceProvokingVertexFeaturesEXT };
        VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR physicalDeviceVertexAttributeDivisorFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR, &physicalDeviceIndexTypeUint8FeaturesEXT };
        VkPhysicalDeviceSamplerYcbcrConversionFeatures physicalDeviceSamplerYcbcrConversionFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, &physicalDeviceVertexAttributeDivisorFeaturesKHR };
        VkPhysicalDeviceShaderFloat16Int8Features physicalDeviceShaderFloat16Int8Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, &physicalDeviceSamplerYcbcrConversionFeatures };
        VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures physicalDeviceShaderSubgroupExtendedTypesFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, &physicalDeviceShaderFloat16Int8Features };
        VkPhysicalDevice8BitStorageFeatures physicalDevice8BitStorageFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, &physicalDeviceShaderSubgroupExtendedTypesFeatures };
        VkPhysicalDevice16BitStorageFeatures physicalDevice16BitStorageFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, &physicalDevice8BitStorageFeatures };
        VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT physicalDevicePrimitivesGeneratedQueryFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT, &physicalDevice16BitStorageFeatures };
        VkPhysicalDeviceLineRasterizationFeaturesEXT physicalDeviceLineRasterizationFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, &physicalDevicePrimitivesGeneratedQueryFeaturesEXT };
        VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG physicalDeviceRelaxedLineRasterizationFeaturesIMG{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG, &physicalDeviceLineRasterizationFeaturesEXT };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceRelaxedLineRasterizationFeaturesIMG));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan11Properties physicalDeviceVulkan11Properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceVulkan11Properties));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkFormatProperties3KHR formatProperties3KHR{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&formatProperties3KHR));
        pfnCb(p, pUser);
    },
};
} // namespace pipelineStatisticsQuery
namespace swBresenhamLines {

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT: {
                    VkPhysicalDeviceLineRasterizationFeaturesEXT* s = static_cast<VkPhysicalDeviceLineRasterizationFeaturesEXT*>(static_cast<void*>(p));
                    s->bresenhamLines = VK_TRUE;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT: {
                    VkPhysicalDeviceLineRasterizationFeaturesEXT* s = static_cast<VkPhysicalDeviceLineRasterizationFeaturesEXT*>(static_cast<void*>(p));
                    ret = ret && (s->bresenhamLines == VK_TRUE);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
        return ret;
    }
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan12Features physicalDeviceVulkan12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, nullptr };
        VkPhysicalDeviceCustomBorderColorFeaturesEXT physicalDeviceCustomBorderColorFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, &physicalDeviceVulkan12Features };
        VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT physicalDevicePrimitiveTopologyListRestartFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT, &physicalDeviceCustomBorderColorFeaturesEXT };
        VkPhysicalDeviceProvokingVertexFeaturesEXT physicalDeviceProvokingVertexFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, &physicalDevicePrimitiveTopologyListRestartFeaturesEXT };
        VkPhysicalDeviceIndexTypeUint8FeaturesEXT physicalDeviceIndexTypeUint8FeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, &physicalDeviceProvokingVertexFeaturesEXT };
        VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR physicalDeviceVertexAttributeDivisorFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR, &physicalDeviceIndexTypeUint8FeaturesEXT };
        VkPhysicalDeviceSamplerYcbcrConversionFeatures physicalDeviceSamplerYcbcrConversionFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, &physicalDeviceVertexAttributeDivisorFeaturesKHR };
        VkPhysicalDeviceShaderFloat16Int8Features physicalDeviceShaderFloat16Int8Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, &physicalDeviceSamplerYcbcrConversionFeatures };
        VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures physicalDeviceShaderSubgroupExtendedTypesFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, &physicalDeviceShaderFloat16Int8Features };
        VkPhysicalDevice8BitStorageFeatures physicalDevice8BitStorageFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, &physicalDeviceShaderSubgroupExtendedTypesFeatures };
        VkPhysicalDevice16BitStorageFeatures physicalDevice16BitStorageFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, &physicalDevice8BitStorageFeatures };
        VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT physicalDevicePrimitivesGeneratedQueryFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT, &physicalDevice16BitStorageFeatures };
        VkPhysicalDeviceLineRasterizationFeaturesEXT physicalDeviceLineRasterizationFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, &physicalDevicePrimitivesGeneratedQueryFeaturesEXT };
        VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG physicalDeviceRelaxedLineRasterizationFeaturesIMG{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG, &physicalDeviceLineRasterizationFeaturesEXT };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceRelaxedLineRasterizationFeaturesIMG));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan11Properties physicalDeviceVulkan11Properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceVulkan11Properties));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkFormatProperties3KHR formatProperties3KHR{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&formatProperties3KHR));
        pfnCb(p, pUser);
    },
};
} // namespace swBresenhamLines
namespace hwBresenhamLines {

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_IMG_RELAXED_LINE_RASTERIZATION_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG: {
                    VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG* s = static_cast<VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG*>(static_cast<void*>(p));
                    s->relaxedLineRasterization = VK_TRUE;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG: {
                    VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG* s = static_cast<VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG*>(static_cast<void*>(p));
                    ret = ret && (s->relaxedLineRasterization == VK_TRUE);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
        return ret;
    }
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan12Features physicalDeviceVulkan12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, nullptr };
        VkPhysicalDeviceCustomBorderColorFeaturesEXT physicalDeviceCustomBorderColorFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, &physicalDeviceVulkan12Features };
        VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT physicalDevicePrimitiveTopologyListRestartFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT, &physicalDeviceCustomBorderColorFeaturesEXT };
        VkPhysicalDeviceProvokingVertexFeaturesEXT physicalDeviceProvokingVertexFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, &physicalDevicePrimitiveTopologyListRestartFeaturesEXT };
        VkPhysicalDeviceIndexTypeUint8FeaturesEXT physicalDeviceIndexTypeUint8FeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, &physicalDeviceProvokingVertexFeaturesEXT };
        VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR physicalDeviceVertexAttributeDivisorFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR, &physicalDeviceIndexTypeUint8FeaturesEXT };
        VkPhysicalDeviceSamplerYcbcrConversionFeatures physicalDeviceSamplerYcbcrConversionFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, &physicalDeviceVertexAttributeDivisorFeaturesKHR };
        VkPhysicalDeviceShaderFloat16Int8Features physicalDeviceShaderFloat16Int8Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, &physicalDeviceSamplerYcbcrConversionFeatures };
        VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures physicalDeviceShaderSubgroupExtendedTypesFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, &physicalDeviceShaderFloat16Int8Features };
        VkPhysicalDevice8BitStorageFeatures physicalDevice8BitStorageFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, &physicalDeviceShaderSubgroupExtendedTypesFeatures };
        VkPhysicalDevice16BitStorageFeatures physicalDevice16BitStorageFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, &physicalDevice8BitStorageFeatures };
        VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT physicalDevicePrimitivesGeneratedQueryFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT, &physicalDevice16BitStorageFeatures };
        VkPhysicalDeviceLineRasterizationFeaturesEXT physicalDeviceLineRasterizationFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, &physicalDevicePrimitivesGeneratedQueryFeaturesEXT };
        VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG physicalDeviceRelaxedLineRasterizationFeaturesIMG{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG, &physicalDeviceLineRasterizationFeaturesEXT };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceRelaxedLineRasterizationFeaturesIMG));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan11Properties physicalDeviceVulkan11Properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceVulkan11Properties));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkFormatProperties3KHR formatProperties3KHR{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&formatProperties3KHR));
        pfnCb(p, pUser);
    },
};
} // namespace hwBresenhamLines
} // namespace blocks
} // namespace VP_ANDROID_15_MINIMUMS
#endif // VP_ANDROID_15_minimums

#ifdef VP_ANDROID_16_minimums
namespace VP_ANDROID_16_MINIMUMS {

static const VkStructureType featureStructTypes[] = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR,
};

static const VkStructureType propertyStructTypes[] = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
};

namespace blocks {
namespace MUST {

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_PIPELINE_PROTECTED_ACCESS_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_8BIT_STORAGE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_MAINTENANCE_6_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_MAP_MEMORY_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SHADER_EXPECT_ASSUME_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SHADER_FLOAT_CONTROLS_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SHADER_SUBGROUP_ROTATE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    s->features.fullDrawIndexUint32 = VK_TRUE;
                    s->features.shaderInt16 = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                    VkPhysicalDeviceVulkan12Features* s = static_cast<VkPhysicalDeviceVulkan12Features*>(static_cast<void*>(p));
                    s->samplerMirrorClampToEdge = VK_TRUE;
                    s->scalarBlockLayout = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES: {
                    VkPhysicalDeviceProtectedMemoryFeatures* s = static_cast<VkPhysicalDeviceProtectedMemoryFeatures*>(static_cast<void*>(p));
                    s->protectedMemory = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES: {
                    VkPhysicalDeviceShaderIntegerDotProductFeatures* s = static_cast<VkPhysicalDeviceShaderIntegerDotProductFeatures*>(static_cast<void*>(p));
                    s->shaderIntegerDotProduct = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: {
                    VkPhysicalDeviceTransformFeedbackFeaturesEXT* s = static_cast<VkPhysicalDeviceTransformFeedbackFeaturesEXT*>(static_cast<void*>(p));
                    s->transformFeedback = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT: {
                    VkPhysicalDeviceImage2DViewOf3DFeaturesEXT* s = static_cast<VkPhysicalDeviceImage2DViewOf3DFeaturesEXT*>(static_cast<void*>(p));
                    s->image2DViewOf3D = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR: {
                    VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR* s = static_cast<VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR*>(static_cast<void*>(p));
                    s->shaderSubgroupUniformControlFlow = VK_TRUE;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->features.fullDrawIndexUint32 == VK_TRUE);
                    ret = ret && (s->features.shaderInt16 == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                    VkPhysicalDeviceVulkan12Features* s = static_cast<VkPhysicalDeviceVulkan12Features*>(static_cast<void*>(p));
                    ret = ret && (s->samplerMirrorClampToEdge == VK_TRUE);
                    ret = ret && (s->scalarBlockLayout == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES: {
                    VkPhysicalDeviceProtectedMemoryFeatures* s = static_cast<VkPhysicalDeviceProtectedMemoryFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->protectedMemory == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES: {
                    VkPhysicalDeviceShaderIntegerDotProductFeatures* s = static_cast<VkPhysicalDeviceShaderIntegerDotProductFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->shaderIntegerDotProduct == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: {
                    VkPhysicalDeviceTransformFeedbackFeaturesEXT* s = static_cast<VkPhysicalDeviceTransformFeedbackFeaturesEXT*>(static_cast<void*>(p));
                    ret = ret && (s->transformFeedback == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT: {
                    VkPhysicalDeviceImage2DViewOf3DFeaturesEXT* s = static_cast<VkPhysicalDeviceImage2DViewOf3DFeaturesEXT*>(static_cast<void*>(p));
                    ret = ret && (s->image2DViewOf3D == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR: {
                    VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR* s = static_cast<VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR*>(static_cast<void*>(p));
                    ret = ret && (s->shaderSubgroupUniformControlFlow == VK_TRUE);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR: {
                    VkPhysicalDeviceProperties2KHR* s = static_cast<VkPhysicalDeviceProperties2KHR*>(static_cast<void*>(p));
                    s->properties.limits.bufferImageGranularity = 4096;
                    s->properties.limits.lineWidthGranularity = 0.5f;
                    s->properties.limits.maxColorAttachments = 8;
                    s->properties.limits.maxComputeWorkGroupInvocations = 256;
                    s->properties.limits.maxComputeWorkGroupSize[0] = 256;
                    s->properties.limits.maxComputeWorkGroupSize[1] = 256;
                    s->properties.limits.maxComputeWorkGroupSize[2] = 64;
                    s->properties.limits.maxDescriptorSetStorageBuffers = 96;
                    s->properties.limits.maxDescriptorSetUniformBuffers = 90;
                    s->properties.limits.maxFragmentCombinedOutputResources = 16;
                    s->properties.limits.maxImageArrayLayers = 2048;
                    s->properties.limits.maxImageDimension1D = 8192;
                    s->properties.limits.maxImageDimension2D = 8192;
                    s->properties.limits.maxImageDimensionCube = 8192;
                    s->properties.limits.maxPerStageDescriptorUniformBuffers = 15;
                    s->properties.limits.maxPerStageResources = 200;
                    s->properties.limits.maxSamplerLodBias = 14;
                    s->properties.limits.maxUniformBufferRange = 65536;
                    s->properties.limits.maxVertexOutputComponents = 72;
                    s->properties.limits.mipmapPrecisionBits = 6;
                    s->properties.limits.pointSizeGranularity = 0.125f;
                    s->properties.limits.standardSampleLocations = VK_TRUE;
                    s->properties.limits.subTexelPrecisionBits = 8;
                    s->properties.limits.timestampComputeAndGraphics = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES: {
                    VkPhysicalDeviceFloatControlsProperties* s = static_cast<VkPhysicalDeviceFloatControlsProperties*>(static_cast<void*>(p));
                    s->shaderSignedZeroInfNanPreserveFloat16 = VK_TRUE;
                    s->shaderSignedZeroInfNanPreserveFloat32 = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
                    VkPhysicalDeviceVulkan11Properties* s = static_cast<VkPhysicalDeviceVulkan11Properties*>(static_cast<void*>(p));
                    s->subgroupSupportedStages |= (VK_SHADER_STAGE_COMPUTE_BIT);
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR: {
                    VkPhysicalDeviceProperties2KHR* s = static_cast<VkPhysicalDeviceProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->properties.limits.bufferImageGranularity <= 4096);
                    ret = ret && ((4096 % s->properties.limits.bufferImageGranularity) == 0);
                    ret = ret && (s->properties.limits.lineWidthGranularity <= 0.5);
                    ret = ret && (isMultiple(0.5, s->properties.limits.lineWidthGranularity));
                    ret = ret && (s->properties.limits.maxColorAttachments >= 8);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupInvocations >= 256);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupSize[0] >= 256);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupSize[1] >= 256);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupSize[2] >= 64);
                    ret = ret && (s->properties.limits.maxDescriptorSetStorageBuffers >= 96);
                    ret = ret && (s->properties.limits.maxDescriptorSetUniformBuffers >= 90);
                    ret = ret && (s->properties.limits.maxFragmentCombinedOutputResources >= 16);
                    ret = ret && (s->properties.limits.maxImageArrayLayers >= 2048);
                    ret = ret && (s->properties.limits.maxImageDimension1D >= 8192);
                    ret = ret && (s->properties.limits.maxImageDimension2D >= 8192);
                    ret = ret && (s->properties.limits.maxImageDimensionCube >= 8192);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorUniformBuffers >= 15);
                    ret = ret && (s->properties.limits.maxPerStageResources >= 200);
                    ret = ret && (s->properties.limits.maxSamplerLodBias >= 14);
                    ret = ret && (s->properties.limits.maxUniformBufferRange >= 65536);
                    ret = ret && (s->properties.limits.maxVertexOutputComponents >= 72);
                    ret = ret && (s->properties.limits.mipmapPrecisionBits >= 6);
                    ret = ret && (s->properties.limits.pointSizeGranularity <= 0.125);
                    ret = ret && (isMultiple(0.125, s->properties.limits.pointSizeGranularity));
                    ret = ret && (s->properties.limits.standardSampleLocations == VK_TRUE);
                    ret = ret && (s->properties.limits.subTexelPrecisionBits >= 8);
                    ret = ret && (vpCheckFlags(s->properties.limits.timestampComputeAndGraphics, VK_TRUE));
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES: {
                    VkPhysicalDeviceFloatControlsProperties* s = static_cast<VkPhysicalDeviceFloatControlsProperties*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->shaderSignedZeroInfNanPreserveFloat16, VK_TRUE));
                    ret = ret && (vpCheckFlags(s->shaderSignedZeroInfNanPreserveFloat32, VK_TRUE));
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
                    VkPhysicalDeviceVulkan11Properties* s = static_cast<VkPhysicalDeviceVulkan11Properties*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->subgroupSupportedStages, (VK_SHADER_STAGE_COMPUTE_BIT)));
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan12Features physicalDeviceVulkan12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, nullptr };
        VkPhysicalDeviceProtectedMemoryFeatures physicalDeviceProtectedMemoryFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, &physicalDeviceVulkan12Features };
        VkPhysicalDeviceShaderIntegerDotProductFeatures physicalDeviceShaderIntegerDotProductFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, &physicalDeviceProtectedMemoryFeatures };
        VkPhysicalDeviceTransformFeedbackFeaturesEXT physicalDeviceTransformFeedbackFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, &physicalDeviceShaderIntegerDotProductFeatures };
        VkPhysicalDeviceImage2DViewOf3DFeaturesEXT physicalDeviceImage2DViewOf3DFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT, &physicalDeviceTransformFeedbackFeaturesEXT };
        VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR physicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR, &physicalDeviceImage2DViewOf3DFeaturesEXT };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceFloatControlsProperties physicalDeviceFloatControlsProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES, nullptr };
        VkPhysicalDeviceVulkan11Properties physicalDeviceVulkan11Properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &physicalDeviceFloatControlsProperties };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceVulkan11Properties));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        pfnCb(p, pUser);
    },
};
} // namespace MUST
namespace multisampledToSingleSampled {

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_EXT_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
        return ret;
    }
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan12Features physicalDeviceVulkan12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, nullptr };
        VkPhysicalDeviceProtectedMemoryFeatures physicalDeviceProtectedMemoryFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, &physicalDeviceVulkan12Features };
        VkPhysicalDeviceShaderIntegerDotProductFeatures physicalDeviceShaderIntegerDotProductFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, &physicalDeviceProtectedMemoryFeatures };
        VkPhysicalDeviceTransformFeedbackFeaturesEXT physicalDeviceTransformFeedbackFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, &physicalDeviceShaderIntegerDotProductFeatures };
        VkPhysicalDeviceImage2DViewOf3DFeaturesEXT physicalDeviceImage2DViewOf3DFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT, &physicalDeviceTransformFeedbackFeaturesEXT };
        VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR physicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR, &physicalDeviceImage2DViewOf3DFeaturesEXT };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceFloatControlsProperties physicalDeviceFloatControlsProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES, nullptr };
        VkPhysicalDeviceVulkan11Properties physicalDeviceVulkan11Properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &physicalDeviceFloatControlsProperties };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceVulkan11Properties));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        pfnCb(p, pUser);
    },
};
} // namespace multisampledToSingleSampled
namespace shaderStencilExport {

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
        return ret;
    }
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceVulkan12Features physicalDeviceVulkan12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, nullptr };
        VkPhysicalDeviceProtectedMemoryFeatures physicalDeviceProtectedMemoryFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, &physicalDeviceVulkan12Features };
        VkPhysicalDeviceShaderIntegerDotProductFeatures physicalDeviceShaderIntegerDotProductFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, &physicalDeviceProtectedMemoryFeatures };
        VkPhysicalDeviceTransformFeedbackFeaturesEXT physicalDeviceTransformFeedbackFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, &physicalDeviceShaderIntegerDotProductFeatures };
        VkPhysicalDeviceImage2DViewOf3DFeaturesEXT physicalDeviceImage2DViewOf3DFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT, &physicalDeviceTransformFeedbackFeaturesEXT };
        VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR physicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR, &physicalDeviceImage2DViewOf3DFeaturesEXT };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceFloatControlsProperties physicalDeviceFloatControlsProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES, nullptr };
        VkPhysicalDeviceVulkan11Properties physicalDeviceVulkan11Properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &physicalDeviceFloatControlsProperties };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceVulkan11Properties));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        pfnCb(p, pUser);
    },
};
} // namespace shaderStencilExport
} // namespace blocks
} // namespace VP_ANDROID_16_MINIMUMS
#endif // VP_ANDROID_16_minimums

#ifdef VP_ANDROID_baseline_2021
namespace VP_ANDROID_BASELINE_2021 {

static const VkStructureType featureStructTypes[] = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
};

static const VkStructureType propertyStructTypes[] = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR,
};

static const VkStructureType formatStructTypes[] = {
    VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR,
    VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR,
};

static const VkExtensionProperties instanceExtensions[] = {
    VkExtensionProperties{ VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SURFACE_EXTENSION_NAME, 1 },
};

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_MAINTENANCE_1_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SWAPCHAIN_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    s->features.depthBiasClamp = VK_TRUE;
                    s->features.fragmentStoresAndAtomics = VK_TRUE;
                    s->features.fullDrawIndexUint32 = VK_TRUE;
                    s->features.imageCubeArray = VK_TRUE;
                    s->features.independentBlend = VK_TRUE;
                    s->features.robustBufferAccess = VK_TRUE;
                    s->features.sampleRateShading = VK_TRUE;
                    s->features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
                    s->features.textureCompressionASTC_LDR = VK_TRUE;
                    s->features.textureCompressionETC2 = VK_TRUE;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->features.depthBiasClamp == VK_TRUE);
                    ret = ret && (s->features.fragmentStoresAndAtomics == VK_TRUE);
                    ret = ret && (s->features.fullDrawIndexUint32 == VK_TRUE);
                    ret = ret && (s->features.imageCubeArray == VK_TRUE);
                    ret = ret && (s->features.independentBlend == VK_TRUE);
                    ret = ret && (s->features.robustBufferAccess == VK_TRUE);
                    ret = ret && (s->features.sampleRateShading == VK_TRUE);
                    ret = ret && (s->features.shaderSampledImageArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderStorageImageArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderUniformBufferArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.textureCompressionASTC_LDR == VK_TRUE);
                    ret = ret && (s->features.textureCompressionETC2 == VK_TRUE);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
        return ret;
    }
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(nullptr));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(nullptr));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkFormatProperties3KHR formatProperties3KHR{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&formatProperties3KHR));
        pfnCb(p, pUser);
    },
};

namespace blocks {
namespace baseline {

static const VkExtensionProperties instanceExtensions[] = {
    VkExtensionProperties{ VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SURFACE_EXTENSION_NAME, 1 },
};

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_MAINTENANCE_1_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SWAPCHAIN_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    s->features.depthBiasClamp = VK_TRUE;
                    s->features.fragmentStoresAndAtomics = VK_TRUE;
                    s->features.fullDrawIndexUint32 = VK_TRUE;
                    s->features.imageCubeArray = VK_TRUE;
                    s->features.independentBlend = VK_TRUE;
                    s->features.robustBufferAccess = VK_TRUE;
                    s->features.sampleRateShading = VK_TRUE;
                    s->features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
                    s->features.textureCompressionASTC_LDR = VK_TRUE;
                    s->features.textureCompressionETC2 = VK_TRUE;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->features.depthBiasClamp == VK_TRUE);
                    ret = ret && (s->features.fragmentStoresAndAtomics == VK_TRUE);
                    ret = ret && (s->features.fullDrawIndexUint32 == VK_TRUE);
                    ret = ret && (s->features.imageCubeArray == VK_TRUE);
                    ret = ret && (s->features.independentBlend == VK_TRUE);
                    ret = ret && (s->features.robustBufferAccess == VK_TRUE);
                    ret = ret && (s->features.sampleRateShading == VK_TRUE);
                    ret = ret && (s->features.shaderSampledImageArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderStorageImageArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderUniformBufferArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.textureCompressionASTC_LDR == VK_TRUE);
                    ret = ret && (s->features.textureCompressionETC2 == VK_TRUE);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR: {
                    VkPhysicalDeviceProperties2KHR* s = static_cast<VkPhysicalDeviceProperties2KHR*>(static_cast<void*>(p));
                    s->properties.limits.discreteQueuePriorities = 2;
                    s->properties.limits.framebufferColorSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.framebufferDepthSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.framebufferNoAttachmentsSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.framebufferStencilSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.maxBoundDescriptorSets = 4;
                    s->properties.limits.maxColorAttachments = 4;
                    s->properties.limits.maxComputeSharedMemorySize = 16384;
                    s->properties.limits.maxComputeWorkGroupCount[0] = 65535;
                    s->properties.limits.maxComputeWorkGroupCount[1] = 65535;
                    s->properties.limits.maxComputeWorkGroupCount[2] = 65535;
                    s->properties.limits.maxComputeWorkGroupInvocations = 128;
                    s->properties.limits.maxComputeWorkGroupSize[0] = 128;
                    s->properties.limits.maxComputeWorkGroupSize[1] = 128;
                    s->properties.limits.maxComputeWorkGroupSize[2] = 64;
                    s->properties.limits.maxDescriptorSetInputAttachments = 4;
                    s->properties.limits.maxDescriptorSetSampledImages = 48;
                    s->properties.limits.maxDescriptorSetSamplers = 48;
                    s->properties.limits.maxDescriptorSetStorageBuffers = 24;
                    s->properties.limits.maxDescriptorSetStorageBuffersDynamic = 4;
                    s->properties.limits.maxDescriptorSetStorageImages = 12;
                    s->properties.limits.maxDescriptorSetUniformBuffers = 36;
                    s->properties.limits.maxDescriptorSetUniformBuffersDynamic = 8;
                    s->properties.limits.maxDrawIndexedIndexValue = 4294967295;
                    s->properties.limits.maxDrawIndirectCount = 1;
                    s->properties.limits.maxFragmentCombinedOutputResources = 8;
                    s->properties.limits.maxFragmentInputComponents = 64;
                    s->properties.limits.maxFragmentOutputAttachments = 4;
                    s->properties.limits.maxFramebufferHeight = 4096;
                    s->properties.limits.maxFramebufferLayers = 256;
                    s->properties.limits.maxFramebufferWidth = 4096;
                    s->properties.limits.maxImageArrayLayers = 256;
                    s->properties.limits.maxImageDimension1D = 4096;
                    s->properties.limits.maxImageDimension2D = 4096;
                    s->properties.limits.maxImageDimension3D = 512;
                    s->properties.limits.maxImageDimensionCube = 4096;
                    s->properties.limits.maxInterpolationOffset = 0.4375f;
                    s->properties.limits.maxMemoryAllocationCount = 4096;
                    s->properties.limits.maxPerStageDescriptorInputAttachments = 4;
                    s->properties.limits.maxPerStageDescriptorSampledImages = 16;
                    s->properties.limits.maxPerStageDescriptorSamplers = 16;
                    s->properties.limits.maxPerStageDescriptorStorageBuffers = 4;
                    s->properties.limits.maxPerStageDescriptorStorageImages = 4;
                    s->properties.limits.maxPerStageDescriptorUniformBuffers = 12;
                    s->properties.limits.maxPerStageResources = 44;
                    s->properties.limits.maxPushConstantsSize = 128;
                    s->properties.limits.maxSampleMaskWords = 1;
                    s->properties.limits.maxSamplerAllocationCount = 4000;
                    s->properties.limits.maxSamplerAnisotropy = 1.0f;
                    s->properties.limits.maxSamplerLodBias = 2.0f;
                    s->properties.limits.maxStorageBufferRange = 134217728;
                    s->properties.limits.maxTexelBufferElements = 65536;
                    s->properties.limits.maxTexelOffset = 7;
                    s->properties.limits.maxUniformBufferRange = 16384;
                    s->properties.limits.maxVertexInputAttributeOffset = 2047;
                    s->properties.limits.maxVertexInputAttributes = 16;
                    s->properties.limits.maxVertexInputBindingStride = 2048;
                    s->properties.limits.maxVertexInputBindings = 16;
                    s->properties.limits.maxVertexOutputComponents = 64;
                    s->properties.limits.maxViewportDimensions[0] = 4096;
                    s->properties.limits.maxViewportDimensions[1] = 4096;
                    s->properties.limits.maxViewports = 1;
                    s->properties.limits.minInterpolationOffset = -0.5f;
                    s->properties.limits.minMemoryMapAlignment = 4096;
                    s->properties.limits.minStorageBufferOffsetAlignment = 256;
                    s->properties.limits.minTexelBufferOffsetAlignment = 256;
                    s->properties.limits.minTexelOffset = -8;
                    s->properties.limits.minUniformBufferOffsetAlignment = 256;
                    s->properties.limits.mipmapPrecisionBits = 4;
                    s->properties.limits.pointSizeGranularity = 1;
                    s->properties.limits.sampledImageColorSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.sampledImageDepthSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.sampledImageIntegerSampleCounts |= (VK_SAMPLE_COUNT_1_BIT);
                    s->properties.limits.sampledImageStencilSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.standardSampleLocations = VK_TRUE;
                    s->properties.limits.storageImageSampleCounts |= (VK_SAMPLE_COUNT_1_BIT);
                    s->properties.limits.subPixelInterpolationOffsetBits = 4;
                    s->properties.limits.subPixelPrecisionBits = 4;
                    s->properties.limits.subTexelPrecisionBits = 4;
                    s->properties.limits.viewportBoundsRange[0] = -8192;
                    s->properties.limits.viewportBoundsRange[1] = 8191;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR: {
                    VkPhysicalDeviceProperties2KHR* s = static_cast<VkPhysicalDeviceProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->properties.limits.discreteQueuePriorities >= 2);
                    ret = ret && (vpCheckFlags(s->properties.limits.framebufferColorSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.framebufferDepthSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.framebufferNoAttachmentsSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.framebufferStencilSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (s->properties.limits.maxBoundDescriptorSets >= 4);
                    ret = ret && (s->properties.limits.maxColorAttachments >= 4);
                    ret = ret && (s->properties.limits.maxComputeSharedMemorySize >= 16384);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupCount[0] >= 65535);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupCount[1] >= 65535);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupCount[2] >= 65535);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupInvocations >= 128);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupSize[0] >= 128);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupSize[1] >= 128);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupSize[2] >= 64);
                    ret = ret && (s->properties.limits.maxDescriptorSetInputAttachments >= 4);
                    ret = ret && (s->properties.limits.maxDescriptorSetSampledImages >= 48);
                    ret = ret && (s->properties.limits.maxDescriptorSetSamplers >= 48);
                    ret = ret && (s->properties.limits.maxDescriptorSetStorageBuffers >= 24);
                    ret = ret && (s->properties.limits.maxDescriptorSetStorageBuffersDynamic >= 4);
                    ret = ret && (s->properties.limits.maxDescriptorSetStorageImages >= 12);
                    ret = ret && (s->properties.limits.maxDescriptorSetUniformBuffers >= 36);
                    ret = ret && (s->properties.limits.maxDescriptorSetUniformBuffersDynamic >= 8);
                    ret = ret && (s->properties.limits.maxDrawIndexedIndexValue >= 4294967295);
                    ret = ret && (s->properties.limits.maxDrawIndirectCount >= 1);
                    ret = ret && (s->properties.limits.maxFragmentCombinedOutputResources >= 8);
                    ret = ret && (s->properties.limits.maxFragmentInputComponents >= 64);
                    ret = ret && (s->properties.limits.maxFragmentOutputAttachments >= 4);
                    ret = ret && (s->properties.limits.maxFramebufferHeight >= 4096);
                    ret = ret && (s->properties.limits.maxFramebufferLayers >= 256);
                    ret = ret && (s->properties.limits.maxFramebufferWidth >= 4096);
                    ret = ret && (s->properties.limits.maxImageArrayLayers >= 256);
                    ret = ret && (s->properties.limits.maxImageDimension1D >= 4096);
                    ret = ret && (s->properties.limits.maxImageDimension2D >= 4096);
                    ret = ret && (s->properties.limits.maxImageDimension3D >= 512);
                    ret = ret && (s->properties.limits.maxImageDimensionCube >= 4096);
                    ret = ret && (s->properties.limits.maxInterpolationOffset >= 0.4375);
                    ret = ret && (s->properties.limits.maxMemoryAllocationCount >= 4096);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorInputAttachments >= 4);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorSampledImages >= 16);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorSamplers >= 16);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorStorageBuffers >= 4);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorStorageImages >= 4);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorUniformBuffers >= 12);
                    ret = ret && (s->properties.limits.maxPerStageResources >= 44);
                    ret = ret && (s->properties.limits.maxPushConstantsSize >= 128);
                    ret = ret && (s->properties.limits.maxSampleMaskWords >= 1);
                    ret = ret && (s->properties.limits.maxSamplerAllocationCount >= 4000);
                    ret = ret && (s->properties.limits.maxSamplerAnisotropy >= 1.0);
                    ret = ret && (s->properties.limits.maxSamplerLodBias >= 2.0);
                    ret = ret && (s->properties.limits.maxStorageBufferRange >= 134217728);
                    ret = ret && (s->properties.limits.maxTexelBufferElements >= 65536);
                    ret = ret && (s->properties.limits.maxTexelOffset >= 7);
                    ret = ret && (s->properties.limits.maxUniformBufferRange >= 16384);
                    ret = ret && (s->properties.limits.maxVertexInputAttributeOffset >= 2047);
                    ret = ret && (s->properties.limits.maxVertexInputAttributes >= 16);
                    ret = ret && (s->properties.limits.maxVertexInputBindingStride >= 2048);
                    ret = ret && (s->properties.limits.maxVertexInputBindings >= 16);
                    ret = ret && (s->properties.limits.maxVertexOutputComponents >= 64);
                    ret = ret && (s->properties.limits.maxViewportDimensions[0] >= 4096);
                    ret = ret && (s->properties.limits.maxViewportDimensions[1] >= 4096);
                    ret = ret && (s->properties.limits.maxViewports >= 1);
                    ret = ret && (s->properties.limits.minInterpolationOffset <= -0.5);
                    ret = ret && (s->properties.limits.minMemoryMapAlignment <= 4096);
                    ret = ret && ((s->properties.limits.minMemoryMapAlignment & (s->properties.limits.minMemoryMapAlignment - 1)) == 0);
                    ret = ret && (s->properties.limits.minStorageBufferOffsetAlignment <= 256);
                    ret = ret && ((s->properties.limits.minStorageBufferOffsetAlignment & (s->properties.limits.minStorageBufferOffsetAlignment - 1)) == 0);
                    ret = ret && (s->properties.limits.minTexelBufferOffsetAlignment <= 256);
                    ret = ret && ((s->properties.limits.minTexelBufferOffsetAlignment & (s->properties.limits.minTexelBufferOffsetAlignment - 1)) == 0);
                    ret = ret && (s->properties.limits.minTexelOffset <= -8);
                    ret = ret && (s->properties.limits.minUniformBufferOffsetAlignment <= 256);
                    ret = ret && ((s->properties.limits.minUniformBufferOffsetAlignment & (s->properties.limits.minUniformBufferOffsetAlignment - 1)) == 0);
                    ret = ret && (s->properties.limits.mipmapPrecisionBits >= 4);
                    ret = ret && (s->properties.limits.pointSizeGranularity <= 1);
                    ret = ret && (isMultiple(1, s->properties.limits.pointSizeGranularity));
                    ret = ret && (vpCheckFlags(s->properties.limits.sampledImageColorSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.sampledImageDepthSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.sampledImageIntegerSampleCounts, (VK_SAMPLE_COUNT_1_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.sampledImageStencilSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (s->properties.limits.standardSampleLocations == VK_TRUE);
                    ret = ret && (vpCheckFlags(s->properties.limits.storageImageSampleCounts, (VK_SAMPLE_COUNT_1_BIT)));
                    ret = ret && (s->properties.limits.subPixelInterpolationOffsetBits >= 4);
                    ret = ret && (s->properties.limits.subPixelPrecisionBits >= 4);
                    ret = ret && (s->properties.limits.subTexelPrecisionBits >= 4);
                    ret = ret && (s->properties.limits.viewportBoundsRange[0] <= -8192);
                    ret = ret && (s->properties.limits.viewportBoundsRange[1] >= 8191);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpFormatDesc formatDesc[] = {
    {
        VK_FORMAT_A1R5G5B5_UNORM_PACK16,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A2B10G10R10_UINT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_SINT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_SNORM_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_SRGB_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_UINT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_UNORM_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x10_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x10_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x5_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x5_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x6_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x6_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x8_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x8_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_12x10_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_12x10_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_12x12_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_12x12_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_5x4_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_5x4_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_5x5_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_5x5_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_6x5_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_6x5_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x5_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x5_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x6_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x6_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x8_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x8_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_B4G4R4A4_UNORM_PACK16,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_B8G8R8A8_SRGB,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_B8G8R8A8_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_D16_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_D32_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_EAC_R11G11_SNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_EAC_R11G11_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_EAC_R11_SNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_EAC_R11_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16B16A16_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16B16A16_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16B16A16_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16B16A16_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32B32A32_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32B32A32_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32B32A32_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R5G6B5_UNORM_PACK16,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_SRGB,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(nullptr));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(nullptr));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkFormatProperties3KHR formatProperties3KHR{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&formatProperties3KHR));
        pfnCb(p, pUser);
    },
};
} // namespace baseline
} // namespace blocks
} // namespace VP_ANDROID_BASELINE_2021
#endif // VP_ANDROID_baseline_2021

#ifdef VP_ANDROID_baseline_2021_cpu_only
namespace VP_ANDROID_BASELINE_2021_CPU_ONLY {

static const VkStructureType featureStructTypes[] = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
};

static const VkStructureType propertyStructTypes[] = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR,
};

static const VkStructureType formatStructTypes[] = {
    VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR,
    VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR,
};

static const VkExtensionProperties instanceExtensions[] = {
    VkExtensionProperties{ VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SURFACE_EXTENSION_NAME, 1 },
};

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_MAINTENANCE_1_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SWAPCHAIN_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    s->features.depthBiasClamp = VK_TRUE;
                    s->features.fragmentStoresAndAtomics = VK_TRUE;
                    s->features.fullDrawIndexUint32 = VK_TRUE;
                    s->features.imageCubeArray = VK_TRUE;
                    s->features.independentBlend = VK_TRUE;
                    s->features.robustBufferAccess = VK_TRUE;
                    s->features.sampleRateShading = VK_TRUE;
                    s->features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
                    s->features.textureCompressionASTC_LDR = VK_TRUE;
                    s->features.textureCompressionETC2 = VK_TRUE;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->features.depthBiasClamp == VK_TRUE);
                    ret = ret && (s->features.fragmentStoresAndAtomics == VK_TRUE);
                    ret = ret && (s->features.fullDrawIndexUint32 == VK_TRUE);
                    ret = ret && (s->features.imageCubeArray == VK_TRUE);
                    ret = ret && (s->features.independentBlend == VK_TRUE);
                    ret = ret && (s->features.robustBufferAccess == VK_TRUE);
                    ret = ret && (s->features.sampleRateShading == VK_TRUE);
                    ret = ret && (s->features.shaderSampledImageArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderStorageImageArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderUniformBufferArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.textureCompressionASTC_LDR == VK_TRUE);
                    ret = ret && (s->features.textureCompressionETC2 == VK_TRUE);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
        return ret;
    }
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(nullptr));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(nullptr));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkFormatProperties3KHR formatProperties3KHR{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&formatProperties3KHR));
        pfnCb(p, pUser);
    },
};

namespace blocks {
namespace baseline {

static const VkExtensionProperties instanceExtensions[] = {
    VkExtensionProperties{ VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SURFACE_EXTENSION_NAME, 1 },
};

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_MAINTENANCE_1_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SWAPCHAIN_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    s->features.depthBiasClamp = VK_TRUE;
                    s->features.fragmentStoresAndAtomics = VK_TRUE;
                    s->features.fullDrawIndexUint32 = VK_TRUE;
                    s->features.imageCubeArray = VK_TRUE;
                    s->features.independentBlend = VK_TRUE;
                    s->features.robustBufferAccess = VK_TRUE;
                    s->features.sampleRateShading = VK_TRUE;
                    s->features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
                    s->features.textureCompressionASTC_LDR = VK_TRUE;
                    s->features.textureCompressionETC2 = VK_TRUE;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->features.depthBiasClamp == VK_TRUE);
                    ret = ret && (s->features.fragmentStoresAndAtomics == VK_TRUE);
                    ret = ret && (s->features.fullDrawIndexUint32 == VK_TRUE);
                    ret = ret && (s->features.imageCubeArray == VK_TRUE);
                    ret = ret && (s->features.independentBlend == VK_TRUE);
                    ret = ret && (s->features.robustBufferAccess == VK_TRUE);
                    ret = ret && (s->features.sampleRateShading == VK_TRUE);
                    ret = ret && (s->features.shaderSampledImageArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderStorageImageArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderUniformBufferArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.textureCompressionASTC_LDR == VK_TRUE);
                    ret = ret && (s->features.textureCompressionETC2 == VK_TRUE);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR: {
                    VkPhysicalDeviceProperties2KHR* s = static_cast<VkPhysicalDeviceProperties2KHR*>(static_cast<void*>(p));
                    s->properties.limits.discreteQueuePriorities = 2;
                    s->properties.limits.framebufferColorSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.framebufferDepthSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.framebufferNoAttachmentsSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.framebufferStencilSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.maxBoundDescriptorSets = 4;
                    s->properties.limits.maxColorAttachments = 4;
                    s->properties.limits.maxComputeSharedMemorySize = 16384;
                    s->properties.limits.maxComputeWorkGroupCount[0] = 65535;
                    s->properties.limits.maxComputeWorkGroupCount[1] = 65535;
                    s->properties.limits.maxComputeWorkGroupCount[2] = 65535;
                    s->properties.limits.maxComputeWorkGroupInvocations = 128;
                    s->properties.limits.maxComputeWorkGroupSize[0] = 128;
                    s->properties.limits.maxComputeWorkGroupSize[1] = 128;
                    s->properties.limits.maxComputeWorkGroupSize[2] = 64;
                    s->properties.limits.maxDescriptorSetInputAttachments = 4;
                    s->properties.limits.maxDescriptorSetSampledImages = 48;
                    s->properties.limits.maxDescriptorSetSamplers = 48;
                    s->properties.limits.maxDescriptorSetStorageBuffers = 24;
                    s->properties.limits.maxDescriptorSetStorageBuffersDynamic = 4;
                    s->properties.limits.maxDescriptorSetStorageImages = 12;
                    s->properties.limits.maxDescriptorSetUniformBuffers = 36;
                    s->properties.limits.maxDescriptorSetUniformBuffersDynamic = 8;
                    s->properties.limits.maxDrawIndexedIndexValue = 4294967295;
                    s->properties.limits.maxDrawIndirectCount = 1;
                    s->properties.limits.maxFragmentCombinedOutputResources = 8;
                    s->properties.limits.maxFragmentInputComponents = 64;
                    s->properties.limits.maxFragmentOutputAttachments = 4;
                    s->properties.limits.maxFramebufferHeight = 4096;
                    s->properties.limits.maxFramebufferLayers = 256;
                    s->properties.limits.maxFramebufferWidth = 4096;
                    s->properties.limits.maxImageArrayLayers = 256;
                    s->properties.limits.maxImageDimension1D = 4096;
                    s->properties.limits.maxImageDimension2D = 4096;
                    s->properties.limits.maxImageDimension3D = 512;
                    s->properties.limits.maxImageDimensionCube = 4096;
                    s->properties.limits.maxInterpolationOffset = 0.4375f;
                    s->properties.limits.maxMemoryAllocationCount = 4096;
                    s->properties.limits.maxPerStageDescriptorInputAttachments = 4;
                    s->properties.limits.maxPerStageDescriptorSampledImages = 16;
                    s->properties.limits.maxPerStageDescriptorSamplers = 16;
                    s->properties.limits.maxPerStageDescriptorStorageBuffers = 4;
                    s->properties.limits.maxPerStageDescriptorStorageImages = 4;
                    s->properties.limits.maxPerStageDescriptorUniformBuffers = 12;
                    s->properties.limits.maxPerStageResources = 44;
                    s->properties.limits.maxPushConstantsSize = 128;
                    s->properties.limits.maxSampleMaskWords = 1;
                    s->properties.limits.maxSamplerAllocationCount = 4000;
                    s->properties.limits.maxSamplerAnisotropy = 1.0f;
                    s->properties.limits.maxSamplerLodBias = 2.0f;
                    s->properties.limits.maxStorageBufferRange = 134217728;
                    s->properties.limits.maxTexelBufferElements = 65536;
                    s->properties.limits.maxTexelOffset = 7;
                    s->properties.limits.maxUniformBufferRange = 16384;
                    s->properties.limits.maxVertexInputAttributeOffset = 2047;
                    s->properties.limits.maxVertexInputAttributes = 16;
                    s->properties.limits.maxVertexInputBindingStride = 2048;
                    s->properties.limits.maxVertexInputBindings = 16;
                    s->properties.limits.maxVertexOutputComponents = 64;
                    s->properties.limits.maxViewportDimensions[0] = 4096;
                    s->properties.limits.maxViewportDimensions[1] = 4096;
                    s->properties.limits.maxViewports = 1;
                    s->properties.limits.minInterpolationOffset = -0.5f;
                    s->properties.limits.minMemoryMapAlignment = 4096;
                    s->properties.limits.minStorageBufferOffsetAlignment = 256;
                    s->properties.limits.minTexelBufferOffsetAlignment = 256;
                    s->properties.limits.minTexelOffset = -8;
                    s->properties.limits.minUniformBufferOffsetAlignment = 256;
                    s->properties.limits.mipmapPrecisionBits = 4;
                    s->properties.limits.sampledImageColorSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.sampledImageDepthSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.sampledImageIntegerSampleCounts |= (VK_SAMPLE_COUNT_1_BIT);
                    s->properties.limits.sampledImageStencilSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.standardSampleLocations = VK_TRUE;
                    s->properties.limits.storageImageSampleCounts |= (VK_SAMPLE_COUNT_1_BIT);
                    s->properties.limits.subPixelInterpolationOffsetBits = 4;
                    s->properties.limits.subPixelPrecisionBits = 4;
                    s->properties.limits.subTexelPrecisionBits = 4;
                    s->properties.limits.viewportBoundsRange[0] = -8192;
                    s->properties.limits.viewportBoundsRange[1] = 8191;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR: {
                    VkPhysicalDeviceProperties2KHR* s = static_cast<VkPhysicalDeviceProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->properties.limits.discreteQueuePriorities >= 2);
                    ret = ret && (vpCheckFlags(s->properties.limits.framebufferColorSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.framebufferDepthSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.framebufferNoAttachmentsSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.framebufferStencilSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (s->properties.limits.maxBoundDescriptorSets >= 4);
                    ret = ret && (s->properties.limits.maxColorAttachments >= 4);
                    ret = ret && (s->properties.limits.maxComputeSharedMemorySize >= 16384);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupCount[0] >= 65535);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupCount[1] >= 65535);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupCount[2] >= 65535);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupInvocations >= 128);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupSize[0] >= 128);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupSize[1] >= 128);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupSize[2] >= 64);
                    ret = ret && (s->properties.limits.maxDescriptorSetInputAttachments >= 4);
                    ret = ret && (s->properties.limits.maxDescriptorSetSampledImages >= 48);
                    ret = ret && (s->properties.limits.maxDescriptorSetSamplers >= 48);
                    ret = ret && (s->properties.limits.maxDescriptorSetStorageBuffers >= 24);
                    ret = ret && (s->properties.limits.maxDescriptorSetStorageBuffersDynamic >= 4);
                    ret = ret && (s->properties.limits.maxDescriptorSetStorageImages >= 12);
                    ret = ret && (s->properties.limits.maxDescriptorSetUniformBuffers >= 36);
                    ret = ret && (s->properties.limits.maxDescriptorSetUniformBuffersDynamic >= 8);
                    ret = ret && (s->properties.limits.maxDrawIndexedIndexValue >= 4294967295);
                    ret = ret && (s->properties.limits.maxDrawIndirectCount >= 1);
                    ret = ret && (s->properties.limits.maxFragmentCombinedOutputResources >= 8);
                    ret = ret && (s->properties.limits.maxFragmentInputComponents >= 64);
                    ret = ret && (s->properties.limits.maxFragmentOutputAttachments >= 4);
                    ret = ret && (s->properties.limits.maxFramebufferHeight >= 4096);
                    ret = ret && (s->properties.limits.maxFramebufferLayers >= 256);
                    ret = ret && (s->properties.limits.maxFramebufferWidth >= 4096);
                    ret = ret && (s->properties.limits.maxImageArrayLayers >= 256);
                    ret = ret && (s->properties.limits.maxImageDimension1D >= 4096);
                    ret = ret && (s->properties.limits.maxImageDimension2D >= 4096);
                    ret = ret && (s->properties.limits.maxImageDimension3D >= 512);
                    ret = ret && (s->properties.limits.maxImageDimensionCube >= 4096);
                    ret = ret && (s->properties.limits.maxInterpolationOffset >= 0.4375);
                    ret = ret && (s->properties.limits.maxMemoryAllocationCount >= 4096);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorInputAttachments >= 4);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorSampledImages >= 16);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorSamplers >= 16);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorStorageBuffers >= 4);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorStorageImages >= 4);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorUniformBuffers >= 12);
                    ret = ret && (s->properties.limits.maxPerStageResources >= 44);
                    ret = ret && (s->properties.limits.maxPushConstantsSize >= 128);
                    ret = ret && (s->properties.limits.maxSampleMaskWords >= 1);
                    ret = ret && (s->properties.limits.maxSamplerAllocationCount >= 4000);
                    ret = ret && (s->properties.limits.maxSamplerAnisotropy >= 1.0);
                    ret = ret && (s->properties.limits.maxSamplerLodBias >= 2.0);
                    ret = ret && (s->properties.limits.maxStorageBufferRange >= 134217728);
                    ret = ret && (s->properties.limits.maxTexelBufferElements >= 65536);
                    ret = ret && (s->properties.limits.maxTexelOffset >= 7);
                    ret = ret && (s->properties.limits.maxUniformBufferRange >= 16384);
                    ret = ret && (s->properties.limits.maxVertexInputAttributeOffset >= 2047);
                    ret = ret && (s->properties.limits.maxVertexInputAttributes >= 16);
                    ret = ret && (s->properties.limits.maxVertexInputBindingStride >= 2048);
                    ret = ret && (s->properties.limits.maxVertexInputBindings >= 16);
                    ret = ret && (s->properties.limits.maxVertexOutputComponents >= 64);
                    ret = ret && (s->properties.limits.maxViewportDimensions[0] >= 4096);
                    ret = ret && (s->properties.limits.maxViewportDimensions[1] >= 4096);
                    ret = ret && (s->properties.limits.maxViewports >= 1);
                    ret = ret && (s->properties.limits.minInterpolationOffset <= -0.5);
                    ret = ret && (s->properties.limits.minMemoryMapAlignment <= 4096);
                    ret = ret && ((s->properties.limits.minMemoryMapAlignment & (s->properties.limits.minMemoryMapAlignment - 1)) == 0);
                    ret = ret && (s->properties.limits.minStorageBufferOffsetAlignment <= 256);
                    ret = ret && ((s->properties.limits.minStorageBufferOffsetAlignment & (s->properties.limits.minStorageBufferOffsetAlignment - 1)) == 0);
                    ret = ret && (s->properties.limits.minTexelBufferOffsetAlignment <= 256);
                    ret = ret && ((s->properties.limits.minTexelBufferOffsetAlignment & (s->properties.limits.minTexelBufferOffsetAlignment - 1)) == 0);
                    ret = ret && (s->properties.limits.minTexelOffset <= -8);
                    ret = ret && (s->properties.limits.minUniformBufferOffsetAlignment <= 256);
                    ret = ret && ((s->properties.limits.minUniformBufferOffsetAlignment & (s->properties.limits.minUniformBufferOffsetAlignment - 1)) == 0);
                    ret = ret && (s->properties.limits.mipmapPrecisionBits >= 4);
                    ret = ret && (vpCheckFlags(s->properties.limits.sampledImageColorSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.sampledImageDepthSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.sampledImageIntegerSampleCounts, (VK_SAMPLE_COUNT_1_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.sampledImageStencilSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (s->properties.limits.standardSampleLocations == VK_TRUE);
                    ret = ret && (vpCheckFlags(s->properties.limits.storageImageSampleCounts, (VK_SAMPLE_COUNT_1_BIT)));
                    ret = ret && (s->properties.limits.subPixelInterpolationOffsetBits >= 4);
                    ret = ret && (s->properties.limits.subPixelPrecisionBits >= 4);
                    ret = ret && (s->properties.limits.subTexelPrecisionBits >= 4);
                    ret = ret && (s->properties.limits.viewportBoundsRange[0] <= -8192);
                    ret = ret && (s->properties.limits.viewportBoundsRange[1] >= 8191);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpFormatDesc formatDesc[] = {
    {
        VK_FORMAT_A1R5G5B5_UNORM_PACK16,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A2B10G10R10_UINT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_SINT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_SNORM_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_SRGB_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_UINT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_UNORM_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x10_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x10_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x5_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x5_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x6_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x6_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x8_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x8_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_12x10_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_12x10_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_12x12_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_12x12_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_5x4_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_5x4_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_5x5_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_5x5_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_6x5_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_6x5_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x5_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x5_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x6_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x6_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x8_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x8_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_B4G4R4A4_UNORM_PACK16,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_B8G8R8A8_SRGB,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_B8G8R8A8_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_D16_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_D32_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_EAC_R11G11_SNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_EAC_R11G11_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_EAC_R11_SNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_EAC_R11_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16B16A16_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16B16A16_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16B16A16_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16B16A16_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32B32A32_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32B32A32_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32B32A32_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R5G6B5_UNORM_PACK16,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_SRGB,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(nullptr));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(nullptr));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkFormatProperties3KHR formatProperties3KHR{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&formatProperties3KHR));
        pfnCb(p, pUser);
    },
};
} // namespace baseline
} // namespace blocks
} // namespace VP_ANDROID_BASELINE_2021_CPU_ONLY
#endif // VP_ANDROID_baseline_2021_cpu_only

#ifdef VP_ANDROID_baseline_2022
namespace VP_ANDROID_BASELINE_2022 {

static const VkStructureType featureStructTypes[] = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,
};

static const VkStructureType propertyStructTypes[] = {
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,
};

static const VkStructureType formatStructTypes[] = {
    VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR,
    VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR,
};

static const VkExtensionProperties instanceExtensions[] = {
    VkExtensionProperties{ VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SURFACE_EXTENSION_NAME, 1 },
};

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_MAINTENANCE_1_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SWAPCHAIN_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    s->features.depthBiasClamp = VK_TRUE;
                    s->features.fragmentStoresAndAtomics = VK_TRUE;
                    s->features.fullDrawIndexUint32 = VK_TRUE;
                    s->features.imageCubeArray = VK_TRUE;
                    s->features.independentBlend = VK_TRUE;
                    s->features.largePoints = VK_TRUE;
                    s->features.robustBufferAccess = VK_TRUE;
                    s->features.sampleRateShading = VK_TRUE;
                    s->features.shaderInt16 = VK_TRUE;
                    s->features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
                    s->features.textureCompressionASTC_LDR = VK_TRUE;
                    s->features.textureCompressionETC2 = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES: {
                    VkPhysicalDeviceMultiviewFeatures* s = static_cast<VkPhysicalDeviceMultiviewFeatures*>(static_cast<void*>(p));
                    s->multiview = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES: {
                    VkPhysicalDeviceSamplerYcbcrConversionFeatures* s = static_cast<VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(static_cast<void*>(p));
                    s->samplerYcbcrConversion = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: {
                    VkPhysicalDeviceShaderDrawParametersFeatures* s = static_cast<VkPhysicalDeviceShaderDrawParametersFeatures*>(static_cast<void*>(p));
                    s->shaderDrawParameters = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES: {
                    VkPhysicalDeviceVariablePointersFeatures* s = static_cast<VkPhysicalDeviceVariablePointersFeatures*>(static_cast<void*>(p));
                    s->variablePointers = VK_TRUE;
                    s->variablePointersStorageBuffer = VK_TRUE;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->features.depthBiasClamp == VK_TRUE);
                    ret = ret && (s->features.fragmentStoresAndAtomics == VK_TRUE);
                    ret = ret && (s->features.fullDrawIndexUint32 == VK_TRUE);
                    ret = ret && (s->features.imageCubeArray == VK_TRUE);
                    ret = ret && (s->features.independentBlend == VK_TRUE);
                    ret = ret && (s->features.largePoints == VK_TRUE);
                    ret = ret && (s->features.robustBufferAccess == VK_TRUE);
                    ret = ret && (s->features.sampleRateShading == VK_TRUE);
                    ret = ret && (s->features.shaderInt16 == VK_TRUE);
                    ret = ret && (s->features.shaderSampledImageArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderStorageBufferArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderStorageImageArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderUniformBufferArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.textureCompressionASTC_LDR == VK_TRUE);
                    ret = ret && (s->features.textureCompressionETC2 == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES: {
                    VkPhysicalDeviceMultiviewFeatures* s = static_cast<VkPhysicalDeviceMultiviewFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->multiview == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES: {
                    VkPhysicalDeviceSamplerYcbcrConversionFeatures* s = static_cast<VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->samplerYcbcrConversion == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: {
                    VkPhysicalDeviceShaderDrawParametersFeatures* s = static_cast<VkPhysicalDeviceShaderDrawParametersFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->shaderDrawParameters == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES: {
                    VkPhysicalDeviceVariablePointersFeatures* s = static_cast<VkPhysicalDeviceVariablePointersFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->variablePointers == VK_TRUE);
                    ret = ret && (s->variablePointersStorageBuffer == VK_TRUE);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
        return ret;
    }
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceMultiviewFeatures physicalDeviceMultiviewFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, nullptr };
        VkPhysicalDeviceSamplerYcbcrConversionFeatures physicalDeviceSamplerYcbcrConversionFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, &physicalDeviceMultiviewFeatures };
        VkPhysicalDeviceShaderDrawParametersFeatures physicalDeviceShaderDrawParametersFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, &physicalDeviceSamplerYcbcrConversionFeatures };
        VkPhysicalDeviceVariablePointersFeatures physicalDeviceVariablePointersFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, &physicalDeviceShaderDrawParametersFeatures };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceVariablePointersFeatures));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceMultiviewProperties physicalDeviceMultiviewProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceMultiviewProperties));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkFormatProperties3KHR formatProperties3KHR{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&formatProperties3KHR));
        pfnCb(p, pUser);
    },
};

namespace blocks {
namespace baseline {

static const VkExtensionProperties instanceExtensions[] = {
    VkExtensionProperties{ VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SURFACE_EXTENSION_NAME, 1 },
};

static const VkExtensionProperties deviceExtensions[] = {
    VkExtensionProperties{ VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_MAINTENANCE_1_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_STORAGE_BUFFER_STORAGE_CLASS_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_SWAPCHAIN_EXTENSION_NAME, 1 },
    VkExtensionProperties{ VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME, 1 },
};

static const VpFeatureDesc featureDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    s->features.depthBiasClamp = VK_TRUE;
                    s->features.fragmentStoresAndAtomics = VK_TRUE;
                    s->features.fullDrawIndexUint32 = VK_TRUE;
                    s->features.imageCubeArray = VK_TRUE;
                    s->features.independentBlend = VK_TRUE;
                    s->features.largePoints = VK_TRUE;
                    s->features.robustBufferAccess = VK_TRUE;
                    s->features.sampleRateShading = VK_TRUE;
                    s->features.shaderInt16 = VK_TRUE;
                    s->features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
                    s->features.shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
                    s->features.textureCompressionASTC_LDR = VK_TRUE;
                    s->features.textureCompressionETC2 = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES: {
                    VkPhysicalDeviceMultiviewFeatures* s = static_cast<VkPhysicalDeviceMultiviewFeatures*>(static_cast<void*>(p));
                    s->multiview = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES: {
                    VkPhysicalDeviceSamplerYcbcrConversionFeatures* s = static_cast<VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(static_cast<void*>(p));
                    s->samplerYcbcrConversion = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: {
                    VkPhysicalDeviceShaderDrawParametersFeatures* s = static_cast<VkPhysicalDeviceShaderDrawParametersFeatures*>(static_cast<void*>(p));
                    s->shaderDrawParameters = VK_TRUE;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES: {
                    VkPhysicalDeviceVariablePointersFeatures* s = static_cast<VkPhysicalDeviceVariablePointersFeatures*>(static_cast<void*>(p));
                    s->variablePointers = VK_TRUE;
                    s->variablePointersStorageBuffer = VK_TRUE;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR: {
                    VkPhysicalDeviceFeatures2KHR* s = static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->features.depthBiasClamp == VK_TRUE);
                    ret = ret && (s->features.fragmentStoresAndAtomics == VK_TRUE);
                    ret = ret && (s->features.fullDrawIndexUint32 == VK_TRUE);
                    ret = ret && (s->features.imageCubeArray == VK_TRUE);
                    ret = ret && (s->features.independentBlend == VK_TRUE);
                    ret = ret && (s->features.largePoints == VK_TRUE);
                    ret = ret && (s->features.robustBufferAccess == VK_TRUE);
                    ret = ret && (s->features.sampleRateShading == VK_TRUE);
                    ret = ret && (s->features.shaderInt16 == VK_TRUE);
                    ret = ret && (s->features.shaderSampledImageArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderStorageBufferArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderStorageImageArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.shaderUniformBufferArrayDynamicIndexing == VK_TRUE);
                    ret = ret && (s->features.textureCompressionASTC_LDR == VK_TRUE);
                    ret = ret && (s->features.textureCompressionETC2 == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES: {
                    VkPhysicalDeviceMultiviewFeatures* s = static_cast<VkPhysicalDeviceMultiviewFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->multiview == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES: {
                    VkPhysicalDeviceSamplerYcbcrConversionFeatures* s = static_cast<VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->samplerYcbcrConversion == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: {
                    VkPhysicalDeviceShaderDrawParametersFeatures* s = static_cast<VkPhysicalDeviceShaderDrawParametersFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->shaderDrawParameters == VK_TRUE);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES: {
                    VkPhysicalDeviceVariablePointersFeatures* s = static_cast<VkPhysicalDeviceVariablePointersFeatures*>(static_cast<void*>(p));
                    ret = ret && (s->variablePointers == VK_TRUE);
                    ret = ret && (s->variablePointersStorageBuffer == VK_TRUE);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpPropertyDesc propertyDesc = {
    [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR: {
                    VkPhysicalDeviceProperties2KHR* s = static_cast<VkPhysicalDeviceProperties2KHR*>(static_cast<void*>(p));
                    s->properties.limits.discreteQueuePriorities = 2;
                    s->properties.limits.framebufferColorSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.framebufferDepthSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.framebufferNoAttachmentsSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.framebufferStencilSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.maxBoundDescriptorSets = 4;
                    s->properties.limits.maxColorAttachments = 4;
                    s->properties.limits.maxComputeSharedMemorySize = 16384;
                    s->properties.limits.maxComputeWorkGroupCount[0] = 65535;
                    s->properties.limits.maxComputeWorkGroupCount[1] = 65535;
                    s->properties.limits.maxComputeWorkGroupCount[2] = 65535;
                    s->properties.limits.maxComputeWorkGroupInvocations = 128;
                    s->properties.limits.maxComputeWorkGroupSize[0] = 128;
                    s->properties.limits.maxComputeWorkGroupSize[1] = 128;
                    s->properties.limits.maxComputeWorkGroupSize[2] = 64;
                    s->properties.limits.maxDescriptorSetInputAttachments = 4;
                    s->properties.limits.maxDescriptorSetSampledImages = 48;
                    s->properties.limits.maxDescriptorSetSamplers = 48;
                    s->properties.limits.maxDescriptorSetStorageBuffers = 24;
                    s->properties.limits.maxDescriptorSetStorageBuffersDynamic = 4;
                    s->properties.limits.maxDescriptorSetStorageImages = 12;
                    s->properties.limits.maxDescriptorSetUniformBuffers = 36;
                    s->properties.limits.maxDescriptorSetUniformBuffersDynamic = 8;
                    s->properties.limits.maxDrawIndexedIndexValue = 4294967295;
                    s->properties.limits.maxDrawIndirectCount = 1;
                    s->properties.limits.maxFragmentCombinedOutputResources = 8;
                    s->properties.limits.maxFragmentInputComponents = 64;
                    s->properties.limits.maxFragmentOutputAttachments = 4;
                    s->properties.limits.maxFramebufferHeight = 4096;
                    s->properties.limits.maxFramebufferLayers = 256;
                    s->properties.limits.maxFramebufferWidth = 4096;
                    s->properties.limits.maxImageArrayLayers = 256;
                    s->properties.limits.maxImageDimension1D = 4096;
                    s->properties.limits.maxImageDimension2D = 4096;
                    s->properties.limits.maxImageDimension3D = 512;
                    s->properties.limits.maxImageDimensionCube = 4096;
                    s->properties.limits.maxInterpolationOffset = 0.4375f;
                    s->properties.limits.maxMemoryAllocationCount = 4096;
                    s->properties.limits.maxPerStageDescriptorInputAttachments = 4;
                    s->properties.limits.maxPerStageDescriptorSampledImages = 16;
                    s->properties.limits.maxPerStageDescriptorSamplers = 16;
                    s->properties.limits.maxPerStageDescriptorStorageBuffers = 4;
                    s->properties.limits.maxPerStageDescriptorStorageImages = 4;
                    s->properties.limits.maxPerStageDescriptorUniformBuffers = 12;
                    s->properties.limits.maxPerStageResources = 44;
                    s->properties.limits.maxPushConstantsSize = 128;
                    s->properties.limits.maxSampleMaskWords = 1;
                    s->properties.limits.maxSamplerAllocationCount = 4000;
                    s->properties.limits.maxSamplerAnisotropy = 1.0f;
                    s->properties.limits.maxSamplerLodBias = 2.0f;
                    s->properties.limits.maxStorageBufferRange = 134217728;
                    s->properties.limits.maxTexelBufferElements = 65536;
                    s->properties.limits.maxTexelOffset = 7;
                    s->properties.limits.maxUniformBufferRange = 16384;
                    s->properties.limits.maxVertexInputAttributeOffset = 2047;
                    s->properties.limits.maxVertexInputAttributes = 16;
                    s->properties.limits.maxVertexInputBindingStride = 2048;
                    s->properties.limits.maxVertexInputBindings = 16;
                    s->properties.limits.maxVertexOutputComponents = 64;
                    s->properties.limits.maxViewportDimensions[0] = 4096;
                    s->properties.limits.maxViewportDimensions[1] = 4096;
                    s->properties.limits.maxViewports = 1;
                    s->properties.limits.minInterpolationOffset = -0.5f;
                    s->properties.limits.minMemoryMapAlignment = 4096;
                    s->properties.limits.minStorageBufferOffsetAlignment = 256;
                    s->properties.limits.minTexelBufferOffsetAlignment = 256;
                    s->properties.limits.minTexelOffset = -8;
                    s->properties.limits.minUniformBufferOffsetAlignment = 256;
                    s->properties.limits.mipmapPrecisionBits = 4;
                    s->properties.limits.pointSizeGranularity = 1;
                    s->properties.limits.pointSizeRange[0] = 1.0f;
                    s->properties.limits.pointSizeRange[1] = 511;
                    s->properties.limits.sampledImageColorSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.sampledImageDepthSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.sampledImageIntegerSampleCounts |= (VK_SAMPLE_COUNT_1_BIT);
                    s->properties.limits.sampledImageStencilSampleCounts |= (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT);
                    s->properties.limits.standardSampleLocations = VK_TRUE;
                    s->properties.limits.storageImageSampleCounts |= (VK_SAMPLE_COUNT_1_BIT);
                    s->properties.limits.subPixelInterpolationOffsetBits = 4;
                    s->properties.limits.subPixelPrecisionBits = 4;
                    s->properties.limits.subTexelPrecisionBits = 4;
                    s->properties.limits.viewportBoundsRange[0] = -8192;
                    s->properties.limits.viewportBoundsRange[1] = 8191;
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES: {
                    VkPhysicalDeviceMultiviewProperties* s = static_cast<VkPhysicalDeviceMultiviewProperties*>(static_cast<void*>(p));
                    s->maxMultiviewInstanceIndex = 134217727;
                    s->maxMultiviewViewCount = 6;
                } break;
                default: break;
            }
    },
    [](VkBaseOutStructure* p) -> bool { (void)p;
        bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR: {
                    VkPhysicalDeviceProperties2KHR* s = static_cast<VkPhysicalDeviceProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (s->properties.limits.discreteQueuePriorities >= 2);
                    ret = ret && (vpCheckFlags(s->properties.limits.framebufferColorSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.framebufferDepthSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.framebufferNoAttachmentsSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.framebufferStencilSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (s->properties.limits.maxBoundDescriptorSets >= 4);
                    ret = ret && (s->properties.limits.maxColorAttachments >= 4);
                    ret = ret && (s->properties.limits.maxComputeSharedMemorySize >= 16384);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupCount[0] >= 65535);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupCount[1] >= 65535);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupCount[2] >= 65535);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupInvocations >= 128);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupSize[0] >= 128);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupSize[1] >= 128);
                    ret = ret && (s->properties.limits.maxComputeWorkGroupSize[2] >= 64);
                    ret = ret && (s->properties.limits.maxDescriptorSetInputAttachments >= 4);
                    ret = ret && (s->properties.limits.maxDescriptorSetSampledImages >= 48);
                    ret = ret && (s->properties.limits.maxDescriptorSetSamplers >= 48);
                    ret = ret && (s->properties.limits.maxDescriptorSetStorageBuffers >= 24);
                    ret = ret && (s->properties.limits.maxDescriptorSetStorageBuffersDynamic >= 4);
                    ret = ret && (s->properties.limits.maxDescriptorSetStorageImages >= 12);
                    ret = ret && (s->properties.limits.maxDescriptorSetUniformBuffers >= 36);
                    ret = ret && (s->properties.limits.maxDescriptorSetUniformBuffersDynamic >= 8);
                    ret = ret && (s->properties.limits.maxDrawIndexedIndexValue >= 4294967295);
                    ret = ret && (s->properties.limits.maxDrawIndirectCount >= 1);
                    ret = ret && (s->properties.limits.maxFragmentCombinedOutputResources >= 8);
                    ret = ret && (s->properties.limits.maxFragmentInputComponents >= 64);
                    ret = ret && (s->properties.limits.maxFragmentOutputAttachments >= 4);
                    ret = ret && (s->properties.limits.maxFramebufferHeight >= 4096);
                    ret = ret && (s->properties.limits.maxFramebufferLayers >= 256);
                    ret = ret && (s->properties.limits.maxFramebufferWidth >= 4096);
                    ret = ret && (s->properties.limits.maxImageArrayLayers >= 256);
                    ret = ret && (s->properties.limits.maxImageDimension1D >= 4096);
                    ret = ret && (s->properties.limits.maxImageDimension2D >= 4096);
                    ret = ret && (s->properties.limits.maxImageDimension3D >= 512);
                    ret = ret && (s->properties.limits.maxImageDimensionCube >= 4096);
                    ret = ret && (s->properties.limits.maxInterpolationOffset >= 0.4375);
                    ret = ret && (s->properties.limits.maxMemoryAllocationCount >= 4096);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorInputAttachments >= 4);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorSampledImages >= 16);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorSamplers >= 16);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorStorageBuffers >= 4);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorStorageImages >= 4);
                    ret = ret && (s->properties.limits.maxPerStageDescriptorUniformBuffers >= 12);
                    ret = ret && (s->properties.limits.maxPerStageResources >= 44);
                    ret = ret && (s->properties.limits.maxPushConstantsSize >= 128);
                    ret = ret && (s->properties.limits.maxSampleMaskWords >= 1);
                    ret = ret && (s->properties.limits.maxSamplerAllocationCount >= 4000);
                    ret = ret && (s->properties.limits.maxSamplerAnisotropy >= 1.0);
                    ret = ret && (s->properties.limits.maxSamplerLodBias >= 2.0);
                    ret = ret && (s->properties.limits.maxStorageBufferRange >= 134217728);
                    ret = ret && (s->properties.limits.maxTexelBufferElements >= 65536);
                    ret = ret && (s->properties.limits.maxTexelOffset >= 7);
                    ret = ret && (s->properties.limits.maxUniformBufferRange >= 16384);
                    ret = ret && (s->properties.limits.maxVertexInputAttributeOffset >= 2047);
                    ret = ret && (s->properties.limits.maxVertexInputAttributes >= 16);
                    ret = ret && (s->properties.limits.maxVertexInputBindingStride >= 2048);
                    ret = ret && (s->properties.limits.maxVertexInputBindings >= 16);
                    ret = ret && (s->properties.limits.maxVertexOutputComponents >= 64);
                    ret = ret && (s->properties.limits.maxViewportDimensions[0] >= 4096);
                    ret = ret && (s->properties.limits.maxViewportDimensions[1] >= 4096);
                    ret = ret && (s->properties.limits.maxViewports >= 1);
                    ret = ret && (s->properties.limits.minInterpolationOffset <= -0.5);
                    ret = ret && (s->properties.limits.minMemoryMapAlignment <= 4096);
                    ret = ret && ((s->properties.limits.minMemoryMapAlignment & (s->properties.limits.minMemoryMapAlignment - 1)) == 0);
                    ret = ret && (s->properties.limits.minStorageBufferOffsetAlignment <= 256);
                    ret = ret && ((s->properties.limits.minStorageBufferOffsetAlignment & (s->properties.limits.minStorageBufferOffsetAlignment - 1)) == 0);
                    ret = ret && (s->properties.limits.minTexelBufferOffsetAlignment <= 256);
                    ret = ret && ((s->properties.limits.minTexelBufferOffsetAlignment & (s->properties.limits.minTexelBufferOffsetAlignment - 1)) == 0);
                    ret = ret && (s->properties.limits.minTexelOffset <= -8);
                    ret = ret && (s->properties.limits.minUniformBufferOffsetAlignment <= 256);
                    ret = ret && ((s->properties.limits.minUniformBufferOffsetAlignment & (s->properties.limits.minUniformBufferOffsetAlignment - 1)) == 0);
                    ret = ret && (s->properties.limits.mipmapPrecisionBits >= 4);
                    ret = ret && (s->properties.limits.pointSizeGranularity <= 1);
                    ret = ret && (isMultiple(1, s->properties.limits.pointSizeGranularity));
                    ret = ret && (s->properties.limits.pointSizeRange[0] <= 1.0);
                    ret = ret && (s->properties.limits.pointSizeRange[1] >= 511);
                    ret = ret && (vpCheckFlags(s->properties.limits.sampledImageColorSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.sampledImageDepthSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.sampledImageIntegerSampleCounts, (VK_SAMPLE_COUNT_1_BIT)));
                    ret = ret && (vpCheckFlags(s->properties.limits.sampledImageStencilSampleCounts, (VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT)));
                    ret = ret && (s->properties.limits.standardSampleLocations == VK_TRUE);
                    ret = ret && (vpCheckFlags(s->properties.limits.storageImageSampleCounts, (VK_SAMPLE_COUNT_1_BIT)));
                    ret = ret && (s->properties.limits.subPixelInterpolationOffsetBits >= 4);
                    ret = ret && (s->properties.limits.subPixelPrecisionBits >= 4);
                    ret = ret && (s->properties.limits.subTexelPrecisionBits >= 4);
                    ret = ret && (s->properties.limits.viewportBoundsRange[0] <= -8192);
                    ret = ret && (s->properties.limits.viewportBoundsRange[1] >= 8191);
                } break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES: {
                    VkPhysicalDeviceMultiviewProperties* s = static_cast<VkPhysicalDeviceMultiviewProperties*>(static_cast<void*>(p));
                    ret = ret && (s->maxMultiviewInstanceIndex >= 134217727);
                    ret = ret && (s->maxMultiviewViewCount >= 6);
                } break;
                default: break;
            }
        return ret;
    }
};

static const VpFormatDesc formatDesc[] = {
    {
        VK_FORMAT_A1R5G5B5_UNORM_PACK16,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A2B10G10R10_UINT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_SINT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_SNORM_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_SRGB_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_UINT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_A8B8G8R8_UNORM_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x10_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x10_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x5_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x5_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x6_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x6_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x8_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_10x8_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_12x10_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_12x10_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_12x12_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_12x12_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_5x4_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_5x4_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_5x5_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_5x5_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_6x5_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_6x5_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x5_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x5_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x6_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x6_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x8_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ASTC_8x8_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_B4G4R4A4_UNORM_PACK16,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_B8G8R8A8_SRGB,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_B8G8R8A8_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_D16_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_D32_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_EAC_R11G11_SNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_EAC_R11G11_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_EAC_R11_SNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_EAC_R11_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16B16A16_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16B16A16_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16B16A16_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16B16A16_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16G16_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R16_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32B32A32_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32B32A32_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32B32A32_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32G32_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32_SFLOAT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R32_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R5G6B5_UNORM_PACK16,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_SRGB,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8B8A8_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8G8_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8_SINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8_SNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8_UINT,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
    {
        VK_FORMAT_R8_UNORM,
        [](VkBaseOutStructure* p) { (void)p;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    s->formatProperties.bufferFeatures |= (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                    s->formatProperties.linearTilingFeatures |= (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                    s->formatProperties.optimalTilingFeatures |= (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
                } break;
                default: break;
            }
        },
        [](VkBaseOutStructure* p) -> bool { (void)p;
            bool ret = true;
            switch (p->sType) {
                case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR: {
                    VkFormatProperties2KHR* s = static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p));
                    ret = ret && (vpCheckFlags(s->formatProperties.bufferFeatures, (VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.linearTilingFeatures, (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                    ret = ret && (vpCheckFlags(s->formatProperties.optimalTilingFeatures, (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)));
                } break;
                default: break;
            }
            return ret;
        }
    },
};

static const VpStructChainerDesc chainerDesc = {
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceMultiviewFeatures physicalDeviceMultiviewFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, nullptr };
        VkPhysicalDeviceSamplerYcbcrConversionFeatures physicalDeviceSamplerYcbcrConversionFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, &physicalDeviceMultiviewFeatures };
        VkPhysicalDeviceShaderDrawParametersFeatures physicalDeviceShaderDrawParametersFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, &physicalDeviceSamplerYcbcrConversionFeatures };
        VkPhysicalDeviceVariablePointersFeatures physicalDeviceVariablePointersFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, &physicalDeviceShaderDrawParametersFeatures };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceVariablePointersFeatures));
        pfnCb(p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkPhysicalDeviceMultiviewProperties physicalDeviceMultiviewProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&physicalDeviceMultiviewProperties));
        pfnCb(p, pUser);
    },
    [](uint32_t count, VkBaseOutStructure* p, void* pUser, PFN_vpStructArrayChainerCb pfnCb) {
        pfnCb(count, p, pUser);
    },
    [](VkBaseOutStructure* p, void* pUser, PFN_vpStructChainerCb pfnCb) {
        VkFormatProperties3KHR formatProperties3KHR{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR, nullptr };
        p->pNext = static_cast<VkBaseOutStructure*>(static_cast<void*>(&formatProperties3KHR));
        pfnCb(p, pUser);
    },
};
} // namespace baseline
} // namespace blocks
} // namespace VP_ANDROID_BASELINE_2022
#endif // VP_ANDROID_baseline_2022


#ifdef VP_ANDROID_15_minimums
namespace VP_ANDROID_15_MINIMUMS {
    namespace blocks {
        namespace MUST {
            static const VpVariantDesc variants[] = {
                {
                    "MUST",
                    static_cast<uint32_t>(std::size(blocks::MUST::instanceExtensions)), blocks::MUST::instanceExtensions,
                    static_cast<uint32_t>(std::size(blocks::MUST::deviceExtensions)), blocks::MUST::deviceExtensions,
                    static_cast<uint32_t>(std::size(featureStructTypes)), featureStructTypes,
                    blocks::MUST::featureDesc,
                    static_cast<uint32_t>(std::size(propertyStructTypes)), propertyStructTypes,
                    blocks::MUST::propertyDesc,
                    0, nullptr,
                    0, nullptr,
                    static_cast<uint32_t>(std::size(formatStructTypes)), formatStructTypes,
                    static_cast<uint32_t>(std::size(blocks::MUST::formatDesc)), blocks::MUST::formatDesc,
                    blocks::MUST::chainerDesc,
                    0, nullptr,
                },
            };
            static const uint32_t variantCount = static_cast<uint32_t>(std::size(variants));
        } // namespace MUST

        namespace primitivesGeneratedQuery_pipelineStatisticsQuery_ {
            static const VpVariantDesc variants[] = {
                {
                    "primitivesGeneratedQuery",
                    0, nullptr,
                    static_cast<uint32_t>(std::size(blocks::primitivesGeneratedQuery::deviceExtensions)), blocks::primitivesGeneratedQuery::deviceExtensions,
                    static_cast<uint32_t>(std::size(featureStructTypes)), featureStructTypes,
                    blocks::primitivesGeneratedQuery::featureDesc,
                    0, nullptr,
                    blocks::primitivesGeneratedQuery::propertyDesc,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    blocks::primitivesGeneratedQuery::chainerDesc,
                    0, nullptr,
                },
                {
                    "pipelineStatisticsQuery",
                    0, nullptr,
                    0, nullptr,
                    static_cast<uint32_t>(std::size(featureStructTypes)), featureStructTypes,
                    blocks::pipelineStatisticsQuery::featureDesc,
                    0, nullptr,
                    blocks::pipelineStatisticsQuery::propertyDesc,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    blocks::pipelineStatisticsQuery::chainerDesc,
                    0, nullptr,
                },
            };
            static const uint32_t variantCount = static_cast<uint32_t>(std::size(variants));
        } // namespace primitivesGeneratedQuery_pipelineStatisticsQuery_

        namespace swBresenhamLines_hwBresenhamLines_ {
            static const VpVariantDesc variants[] = {
                {
                    "swBresenhamLines",
                    0, nullptr,
                    static_cast<uint32_t>(std::size(blocks::swBresenhamLines::deviceExtensions)), blocks::swBresenhamLines::deviceExtensions,
                    static_cast<uint32_t>(std::size(featureStructTypes)), featureStructTypes,
                    blocks::swBresenhamLines::featureDesc,
                    0, nullptr,
                    blocks::swBresenhamLines::propertyDesc,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    blocks::swBresenhamLines::chainerDesc,
                    0, nullptr,
                },
                {
                    "hwBresenhamLines",
                    0, nullptr,
                    static_cast<uint32_t>(std::size(blocks::hwBresenhamLines::deviceExtensions)), blocks::hwBresenhamLines::deviceExtensions,
                    static_cast<uint32_t>(std::size(featureStructTypes)), featureStructTypes,
                    blocks::hwBresenhamLines::featureDesc,
                    0, nullptr,
                    blocks::hwBresenhamLines::propertyDesc,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    blocks::hwBresenhamLines::chainerDesc,
                    0, nullptr,
                },
            };
            static const uint32_t variantCount = static_cast<uint32_t>(std::size(variants));
        } // namespace swBresenhamLines_hwBresenhamLines_
    } // namespace blocks

    static const VpCapabilitiesDesc capabilities[] = {
        { blocks::MUST::variantCount, blocks::MUST::variants },
        { blocks::primitivesGeneratedQuery_pipelineStatisticsQuery_::variantCount, blocks::primitivesGeneratedQuery_pipelineStatisticsQuery_::variants },
        { blocks::swBresenhamLines_hwBresenhamLines_::variantCount, blocks::swBresenhamLines_hwBresenhamLines_::variants },
    };
    static const uint32_t capabilityCount = static_cast<uint32_t>(std::size(capabilities));

    static const VpProfileProperties profiles[] = {
        {VP_ANDROID_BASELINE_2022_NAME, VP_ANDROID_BASELINE_2022_SPEC_VERSION},
    };
    static const uint32_t profileCount = static_cast<uint32_t>(std::size(profiles));
} // namespace VP_ANDROID_15_MINIMUMS
#endif //VP_ANDROID_15_minimums

#ifdef VP_ANDROID_16_minimums
namespace VP_ANDROID_16_MINIMUMS {
    namespace blocks {
        namespace MUST {
            static const VpVariantDesc variants[] = {
                {
                    "MUST",
                    0, nullptr,
                    static_cast<uint32_t>(std::size(blocks::MUST::deviceExtensions)), blocks::MUST::deviceExtensions,
                    static_cast<uint32_t>(std::size(featureStructTypes)), featureStructTypes,
                    blocks::MUST::featureDesc,
                    static_cast<uint32_t>(std::size(propertyStructTypes)), propertyStructTypes,
                    blocks::MUST::propertyDesc,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    blocks::MUST::chainerDesc,
                    0, nullptr,
                },
            };
            static const uint32_t variantCount = static_cast<uint32_t>(std::size(variants));
        } // namespace MUST

        namespace multisampledToSingleSampled_shaderStencilExport_ {
            static const VpVariantDesc variants[] = {
                {
                    "multisampledToSingleSampled",
                    0, nullptr,
                    static_cast<uint32_t>(std::size(blocks::multisampledToSingleSampled::deviceExtensions)), blocks::multisampledToSingleSampled::deviceExtensions,
                    0, nullptr,
                    blocks::multisampledToSingleSampled::featureDesc,
                    0, nullptr,
                    blocks::multisampledToSingleSampled::propertyDesc,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    blocks::multisampledToSingleSampled::chainerDesc,
                    0, nullptr,
                },
                {
                    "shaderStencilExport",
                    0, nullptr,
                    static_cast<uint32_t>(std::size(blocks::shaderStencilExport::deviceExtensions)), blocks::shaderStencilExport::deviceExtensions,
                    0, nullptr,
                    blocks::shaderStencilExport::featureDesc,
                    0, nullptr,
                    blocks::shaderStencilExport::propertyDesc,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    0, nullptr,
                    blocks::shaderStencilExport::chainerDesc,
                    0, nullptr,
                },
            };
            static const uint32_t variantCount = static_cast<uint32_t>(std::size(variants));
        } // namespace multisampledToSingleSampled_shaderStencilExport_
    } // namespace blocks

    static const VpCapabilitiesDesc capabilities[] = {
        { blocks::MUST::variantCount, blocks::MUST::variants },
        { blocks::multisampledToSingleSampled_shaderStencilExport_::variantCount, blocks::multisampledToSingleSampled_shaderStencilExport_::variants },
    };
    static const uint32_t capabilityCount = static_cast<uint32_t>(std::size(capabilities));

    static const VpProfileProperties profiles[] = {
        {VP_ANDROID_BASELINE_2022_NAME, VP_ANDROID_BASELINE_2022_SPEC_VERSION},
        {VP_ANDROID_15_MINIMUMS_NAME, VP_ANDROID_15_MINIMUMS_SPEC_VERSION},
    };
    static const uint32_t profileCount = static_cast<uint32_t>(std::size(profiles));
} // namespace VP_ANDROID_16_MINIMUMS
#endif //VP_ANDROID_16_minimums

#ifdef VP_ANDROID_baseline_2021
namespace VP_ANDROID_BASELINE_2021 {
    static const VpVariantDesc mergedCapabilities[] = {
        {
        "MERGED",
        static_cast<uint32_t>(std::size(instanceExtensions)), instanceExtensions,
        static_cast<uint32_t>(std::size(deviceExtensions)), deviceExtensions,
        static_cast<uint32_t>(std::size(featureStructTypes)), featureStructTypes,
            featureDesc,
        0, nullptr,
            propertyDesc,
        0, nullptr,
        0, nullptr,
        0, nullptr,
        0, nullptr,
        chainerDesc,
        0, nullptr,
        },
    };

    namespace blocks {
        namespace baseline {
            static const VpVariantDesc variants[] = {
                {
                    "baseline",
                    static_cast<uint32_t>(std::size(blocks::baseline::instanceExtensions)), blocks::baseline::instanceExtensions,
                    static_cast<uint32_t>(std::size(blocks::baseline::deviceExtensions)), blocks::baseline::deviceExtensions,
                    static_cast<uint32_t>(std::size(featureStructTypes)), featureStructTypes,
                    blocks::baseline::featureDesc,
                    static_cast<uint32_t>(std::size(propertyStructTypes)), propertyStructTypes,
                    blocks::baseline::propertyDesc,
                    0, nullptr,
                    0, nullptr,
                    static_cast<uint32_t>(std::size(formatStructTypes)), formatStructTypes,
                    static_cast<uint32_t>(std::size(blocks::baseline::formatDesc)), blocks::baseline::formatDesc,
                    blocks::baseline::chainerDesc,
                    0, nullptr,
                },
            };
            static const uint32_t variantCount = static_cast<uint32_t>(std::size(variants));
        } // namespace baseline
    } // namespace blocks

    static const VpCapabilitiesDesc capabilities[] = {
        { blocks::baseline::variantCount, blocks::baseline::variants },
    };
    static const uint32_t capabilityCount = static_cast<uint32_t>(std::size(capabilities));
} // namespace VP_ANDROID_BASELINE_2021
#endif //VP_ANDROID_baseline_2021

#ifdef VP_ANDROID_baseline_2021_cpu_only
namespace VP_ANDROID_BASELINE_2021_CPU_ONLY {
    static const VpVariantDesc mergedCapabilities[] = {
        {
        "MERGED",
        static_cast<uint32_t>(std::size(instanceExtensions)), instanceExtensions,
        static_cast<uint32_t>(std::size(deviceExtensions)), deviceExtensions,
        static_cast<uint32_t>(std::size(featureStructTypes)), featureStructTypes,
            featureDesc,
        0, nullptr,
            propertyDesc,
        0, nullptr,
        0, nullptr,
        0, nullptr,
        0, nullptr,
        chainerDesc,
        0, nullptr,
        },
    };

    namespace blocks {
        namespace baseline {
            static const VpVariantDesc variants[] = {
                {
                    "baseline",
                    static_cast<uint32_t>(std::size(blocks::baseline::instanceExtensions)), blocks::baseline::instanceExtensions,
                    static_cast<uint32_t>(std::size(blocks::baseline::deviceExtensions)), blocks::baseline::deviceExtensions,
                    static_cast<uint32_t>(std::size(featureStructTypes)), featureStructTypes,
                    blocks::baseline::featureDesc,
                    static_cast<uint32_t>(std::size(propertyStructTypes)), propertyStructTypes,
                    blocks::baseline::propertyDesc,
                    0, nullptr,
                    0, nullptr,
                    static_cast<uint32_t>(std::size(formatStructTypes)), formatStructTypes,
                    static_cast<uint32_t>(std::size(blocks::baseline::formatDesc)), blocks::baseline::formatDesc,
                    blocks::baseline::chainerDesc,
                    0, nullptr,
                },
            };
            static const uint32_t variantCount = static_cast<uint32_t>(std::size(variants));
        } // namespace baseline
    } // namespace blocks

    static const VpCapabilitiesDesc capabilities[] = {
        { blocks::baseline::variantCount, blocks::baseline::variants },
    };
    static const uint32_t capabilityCount = static_cast<uint32_t>(std::size(capabilities));
} // namespace VP_ANDROID_BASELINE_2021_CPU_ONLY
#endif //VP_ANDROID_baseline_2021_cpu_only

#ifdef VP_ANDROID_baseline_2022
namespace VP_ANDROID_BASELINE_2022 {
    static const VpVariantDesc mergedCapabilities[] = {
        {
        "MERGED",
        static_cast<uint32_t>(std::size(instanceExtensions)), instanceExtensions,
        static_cast<uint32_t>(std::size(deviceExtensions)), deviceExtensions,
        static_cast<uint32_t>(std::size(featureStructTypes)), featureStructTypes,
            featureDesc,
        0, nullptr,
            propertyDesc,
        0, nullptr,
        0, nullptr,
        0, nullptr,
        0, nullptr,
        chainerDesc,
        0, nullptr,
        },
    };

    namespace blocks {
        namespace baseline {
            static const VpVariantDesc variants[] = {
                {
                    "baseline",
                    static_cast<uint32_t>(std::size(blocks::baseline::instanceExtensions)), blocks::baseline::instanceExtensions,
                    static_cast<uint32_t>(std::size(blocks::baseline::deviceExtensions)), blocks::baseline::deviceExtensions,
                    static_cast<uint32_t>(std::size(featureStructTypes)), featureStructTypes,
                    blocks::baseline::featureDesc,
                    static_cast<uint32_t>(std::size(propertyStructTypes)), propertyStructTypes,
                    blocks::baseline::propertyDesc,
                    0, nullptr,
                    0, nullptr,
                    static_cast<uint32_t>(std::size(formatStructTypes)), formatStructTypes,
                    static_cast<uint32_t>(std::size(blocks::baseline::formatDesc)), blocks::baseline::formatDesc,
                    blocks::baseline::chainerDesc,
                    0, nullptr,
                },
            };
            static const uint32_t variantCount = static_cast<uint32_t>(std::size(variants));
        } // namespace baseline
    } // namespace blocks

    static const VpCapabilitiesDesc capabilities[] = {
        { blocks::baseline::variantCount, blocks::baseline::variants },
    };
    static const uint32_t capabilityCount = static_cast<uint32_t>(std::size(capabilities));
} // namespace VP_ANDROID_BASELINE_2022
#endif //VP_ANDROID_baseline_2022

static const VpProfileDesc profiles[] = {
#ifdef VP_ANDROID_15_minimums
    VpProfileDesc{
        VpProfileProperties{ VP_ANDROID_15_MINIMUMS_NAME, VP_ANDROID_15_MINIMUMS_SPEC_VERSION },
        VP_ANDROID_15_MINIMUMS_MIN_API_VERSION,
        nullptr,
        VP_ANDROID_15_MINIMUMS::profileCount, VP_ANDROID_15_MINIMUMS::profiles,
        VP_ANDROID_15_MINIMUMS::capabilityCount, VP_ANDROID_15_MINIMUMS::capabilities,
        0, nullptr,
    },
#endif // VP_ANDROID_15_MINIMUMS
#ifdef VP_ANDROID_16_minimums
    VpProfileDesc{
        VpProfileProperties{ VP_ANDROID_16_MINIMUMS_NAME, VP_ANDROID_16_MINIMUMS_SPEC_VERSION },
        VP_ANDROID_16_MINIMUMS_MIN_API_VERSION,
        nullptr,
        VP_ANDROID_16_MINIMUMS::profileCount, VP_ANDROID_16_MINIMUMS::profiles,
        VP_ANDROID_16_MINIMUMS::capabilityCount, VP_ANDROID_16_MINIMUMS::capabilities,
        0, nullptr,
    },
#endif // VP_ANDROID_16_MINIMUMS
#ifdef VP_ANDROID_baseline_2021
    VpProfileDesc{
        VpProfileProperties{ VP_ANDROID_BASELINE_2021_NAME, VP_ANDROID_BASELINE_2021_SPEC_VERSION },
        VP_ANDROID_BASELINE_2021_MIN_API_VERSION,
        VP_ANDROID_BASELINE_2021::mergedCapabilities,
        0, nullptr,
        VP_ANDROID_BASELINE_2021::capabilityCount, VP_ANDROID_BASELINE_2021::capabilities,
        0, nullptr,
    },
#endif // VP_ANDROID_BASELINE_2021
#ifdef VP_ANDROID_baseline_2021_cpu_only
    VpProfileDesc{
        VpProfileProperties{ VP_ANDROID_BASELINE_2021_CPU_ONLY_NAME, VP_ANDROID_BASELINE_2021_CPU_ONLY_SPEC_VERSION },
        VP_ANDROID_BASELINE_2021_CPU_ONLY_MIN_API_VERSION,
        VP_ANDROID_BASELINE_2021_CPU_ONLY::mergedCapabilities,
        0, nullptr,
        VP_ANDROID_BASELINE_2021_CPU_ONLY::capabilityCount, VP_ANDROID_BASELINE_2021_CPU_ONLY::capabilities,
        0, nullptr,
    },
#endif // VP_ANDROID_BASELINE_2021_CPU_ONLY
#ifdef VP_ANDROID_baseline_2022
    VpProfileDesc{
        VpProfileProperties{ VP_ANDROID_BASELINE_2022_NAME, VP_ANDROID_BASELINE_2022_SPEC_VERSION },
        VP_ANDROID_BASELINE_2022_MIN_API_VERSION,
        VP_ANDROID_BASELINE_2022::mergedCapabilities,
        0, nullptr,
        VP_ANDROID_BASELINE_2022::capabilityCount, VP_ANDROID_BASELINE_2022::capabilities,
        0, nullptr,
    },
#endif // VP_ANDROID_BASELINE_2022
};
static const uint32_t profileCount = static_cast<uint32_t>(std::size(profiles));


struct FeaturesChain {
    std::map<VkStructureType, std::size_t> structureSize;

    template<typename T>
    constexpr std::size_t size() const {
        return (sizeof(T) - sizeof(VkBaseOutStructure)) / sizeof(VkBool32);
    }

	// Chain with all Vulkan Features structures
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV physicalDeviceDeviceGeneratedCommandsFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV, nullptr };
    VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV physicalDeviceDeviceGeneratedCommandsComputeFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_COMPUTE_FEATURES_NV, nullptr };
    VkPhysicalDevicePrivateDataFeatures physicalDevicePrivateDataFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, nullptr };
    VkPhysicalDeviceVariablePointersFeatures physicalDeviceVariablePointersFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, nullptr };
    VkPhysicalDeviceMultiviewFeatures physicalDeviceMultiviewFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, nullptr };
    VkPhysicalDevicePresentIdFeaturesKHR physicalDevicePresentIdFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, nullptr };
    VkPhysicalDevicePresentWaitFeaturesKHR physicalDevicePresentWaitFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, nullptr };
    VkPhysicalDevice16BitStorageFeatures physicalDevice16BitStorageFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, nullptr };
    VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures physicalDeviceShaderSubgroupExtendedTypesFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, nullptr };
    VkPhysicalDeviceSamplerYcbcrConversionFeatures physicalDeviceSamplerYcbcrConversionFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, nullptr };
    VkPhysicalDeviceProtectedMemoryFeatures physicalDeviceProtectedMemoryFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, nullptr };
    VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT physicalDeviceBlendOperationAdvancedFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT, nullptr };
    VkPhysicalDeviceMultiDrawFeaturesEXT physicalDeviceMultiDrawFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT, nullptr };
    VkPhysicalDeviceInlineUniformBlockFeatures physicalDeviceInlineUniformBlockFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, nullptr };
    VkPhysicalDeviceMaintenance4Features physicalDeviceMaintenance4Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, nullptr };
    VkPhysicalDeviceMaintenance5Features physicalDeviceMaintenance5Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES, nullptr };
    VkPhysicalDeviceMaintenance6Features physicalDeviceMaintenance6Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES, nullptr };
    VkPhysicalDeviceMaintenance7FeaturesKHR physicalDeviceMaintenance7FeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR, nullptr };
    VkPhysicalDeviceShaderDrawParametersFeatures physicalDeviceShaderDrawParametersFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, nullptr };
    VkPhysicalDeviceShaderFloat16Int8Features physicalDeviceShaderFloat16Int8Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, nullptr };
    VkPhysicalDeviceHostQueryResetFeatures physicalDeviceHostQueryResetFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, nullptr };
    VkPhysicalDeviceGlobalPriorityQueryFeatures physicalDeviceGlobalPriorityQueryFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GLOBAL_PRIORITY_QUERY_FEATURES, nullptr };
    VkPhysicalDeviceDeviceMemoryReportFeaturesEXT physicalDeviceDeviceMemoryReportFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT, nullptr };
    VkPhysicalDeviceDescriptorIndexingFeatures physicalDeviceDescriptorIndexingFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, nullptr };
    VkPhysicalDeviceTimelineSemaphoreFeatures physicalDeviceTimelineSemaphoreFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, nullptr };
    VkPhysicalDevice8BitStorageFeatures physicalDevice8BitStorageFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, nullptr };
    VkPhysicalDeviceConditionalRenderingFeaturesEXT physicalDeviceConditionalRenderingFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT, nullptr };
    VkPhysicalDeviceVulkanMemoryModelFeatures physicalDeviceVulkanMemoryModelFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, nullptr };
    VkPhysicalDeviceShaderAtomicInt64Features physicalDeviceShaderAtomicInt64Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, nullptr };
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT physicalDeviceShaderAtomicFloatFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT, nullptr };
    VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT physicalDeviceShaderAtomicFloat2FeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT, nullptr };
    VkPhysicalDeviceVertexAttributeDivisorFeatures physicalDeviceVertexAttributeDivisorFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES, nullptr };
    VkPhysicalDeviceASTCDecodeFeaturesEXT physicalDeviceASTCDecodeFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ASTC_DECODE_FEATURES_EXT, nullptr };
    VkPhysicalDeviceTransformFeedbackFeaturesEXT physicalDeviceTransformFeedbackFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, nullptr };
    VkPhysicalDeviceRepresentativeFragmentTestFeaturesNV physicalDeviceRepresentativeFragmentTestFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_REPRESENTATIVE_FRAGMENT_TEST_FEATURES_NV, nullptr };
    VkPhysicalDeviceExclusiveScissorFeaturesNV physicalDeviceExclusiveScissorFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXCLUSIVE_SCISSOR_FEATURES_NV, nullptr };
    VkPhysicalDeviceCornerSampledImageFeaturesNV physicalDeviceCornerSampledImageFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CORNER_SAMPLED_IMAGE_FEATURES_NV, nullptr };
    VkPhysicalDeviceShaderImageFootprintFeaturesNV physicalDeviceShaderImageFootprintFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_FOOTPRINT_FEATURES_NV, nullptr };
    VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV physicalDeviceDedicatedAllocationImageAliasingFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEDICATED_ALLOCATION_IMAGE_ALIASING_FEATURES_NV, nullptr };
    VkPhysicalDeviceCopyMemoryIndirectFeaturesNV physicalDeviceCopyMemoryIndirectFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_NV, nullptr };
    VkPhysicalDeviceMemoryDecompressionFeaturesNV physicalDeviceMemoryDecompressionFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_FEATURES_NV, nullptr };
    VkPhysicalDeviceShadingRateImageFeaturesNV physicalDeviceShadingRateImageFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV, nullptr };
    VkPhysicalDeviceInvocationMaskFeaturesHUAWEI physicalDeviceInvocationMaskFeaturesHUAWEI{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INVOCATION_MASK_FEATURES_HUAWEI, nullptr };
    VkPhysicalDeviceMeshShaderFeaturesNV physicalDeviceMeshShaderFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV, nullptr };
    VkPhysicalDeviceMeshShaderFeaturesEXT physicalDeviceMeshShaderFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, nullptr };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR physicalDeviceAccelerationStructureFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, nullptr };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR physicalDeviceRayTracingPipelineFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, nullptr };
    VkPhysicalDeviceRayQueryFeaturesKHR physicalDeviceRayQueryFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, nullptr };
    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR physicalDeviceRayTracingMaintenance1FeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR, nullptr };
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT physicalDeviceFragmentDensityMapFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT, nullptr };
    VkPhysicalDeviceFragmentDensityMap2FeaturesEXT physicalDeviceFragmentDensityMap2FeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_2_FEATURES_EXT, nullptr };
    VkPhysicalDeviceFragmentDensityMapOffsetFeaturesQCOM physicalDeviceFragmentDensityMapOffsetFeaturesQCOM{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_OFFSET_FEATURES_QCOM, nullptr };
    VkPhysicalDeviceScalarBlockLayoutFeatures physicalDeviceScalarBlockLayoutFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, nullptr };
    VkPhysicalDeviceUniformBufferStandardLayoutFeatures physicalDeviceUniformBufferStandardLayoutFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, nullptr };
    VkPhysicalDeviceDepthClipEnableFeaturesEXT physicalDeviceDepthClipEnableFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, nullptr };
    VkPhysicalDeviceMemoryPriorityFeaturesEXT physicalDeviceMemoryPriorityFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT, nullptr };
    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT physicalDevicePageableDeviceLocalMemoryFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT, nullptr };
    VkPhysicalDeviceBufferDeviceAddressFeatures physicalDeviceBufferDeviceAddressFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, nullptr };
    VkPhysicalDeviceBufferDeviceAddressFeaturesEXT physicalDeviceBufferDeviceAddressFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT, nullptr };
    VkPhysicalDeviceImagelessFramebufferFeatures physicalDeviceImagelessFramebufferFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, nullptr };
    VkPhysicalDeviceTextureCompressionASTCHDRFeatures physicalDeviceTextureCompressionASTCHDRFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES, nullptr };
    VkPhysicalDeviceCooperativeMatrixFeaturesNV physicalDeviceCooperativeMatrixFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_NV, nullptr };
    VkPhysicalDeviceYcbcrImageArraysFeaturesEXT physicalDeviceYcbcrImageArraysFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_IMAGE_ARRAYS_FEATURES_EXT, nullptr };
    VkPhysicalDevicePresentBarrierFeaturesNV physicalDevicePresentBarrierFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_BARRIER_FEATURES_NV, nullptr };
    VkPhysicalDevicePerformanceQueryFeaturesKHR physicalDevicePerformanceQueryFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR, nullptr };
    VkPhysicalDeviceCoverageReductionModeFeaturesNV physicalDeviceCoverageReductionModeFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COVERAGE_REDUCTION_MODE_FEATURES_NV, nullptr };
    VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL physicalDeviceShaderIntegerFunctions2FeaturesINTEL{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_FUNCTIONS_2_FEATURES_INTEL, nullptr };
    VkPhysicalDeviceShaderClockFeaturesKHR physicalDeviceShaderClockFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR, nullptr };
    VkPhysicalDeviceIndexTypeUint8Features physicalDeviceIndexTypeUint8Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES, nullptr };
    VkPhysicalDeviceShaderSMBuiltinsFeaturesNV physicalDeviceShaderSMBuiltinsFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_FEATURES_NV, nullptr };
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT physicalDeviceFragmentShaderInterlockFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, nullptr };
    VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures physicalDeviceSeparateDepthStencilLayoutsFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, nullptr };
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT physicalDevicePrimitiveTopologyListRestartFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT, nullptr };
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR physicalDevicePipelineExecutablePropertiesFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, nullptr };
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures physicalDeviceShaderDemoteToHelperInvocationFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, nullptr };
    VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT physicalDeviceTexelBufferAlignmentFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT, nullptr };
    VkPhysicalDeviceSubgroupSizeControlFeatures physicalDeviceSubgroupSizeControlFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, nullptr };
    VkPhysicalDeviceLineRasterizationFeatures physicalDeviceLineRasterizationFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES, nullptr };
    VkPhysicalDevicePipelineCreationCacheControlFeatures physicalDevicePipelineCreationCacheControlFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, nullptr };
    VkPhysicalDeviceVulkan11Features physicalDeviceVulkan11Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, nullptr };
    VkPhysicalDeviceVulkan12Features physicalDeviceVulkan12Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, nullptr };
    VkPhysicalDeviceVulkan13Features physicalDeviceVulkan13Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, nullptr };
    VkPhysicalDeviceVulkan14Features physicalDeviceVulkan14Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, nullptr };
    VkPhysicalDeviceCoherentMemoryFeaturesAMD physicalDeviceCoherentMemoryFeaturesAMD{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD, nullptr };
    VkPhysicalDeviceCustomBorderColorFeaturesEXT physicalDeviceCustomBorderColorFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, nullptr };
    VkPhysicalDeviceBorderColorSwizzleFeaturesEXT physicalDeviceBorderColorSwizzleFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT, nullptr };
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT physicalDeviceExtendedDynamicStateFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, nullptr };
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT physicalDeviceExtendedDynamicState2FeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, nullptr };
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT physicalDeviceExtendedDynamicState3FeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, nullptr };
    VkPhysicalDeviceDiagnosticsConfigFeaturesNV physicalDeviceDiagnosticsConfigFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV, nullptr };
    VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures physicalDeviceZeroInitializeWorkgroupMemoryFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES, nullptr };
    VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR physicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR, nullptr };
    VkPhysicalDeviceRobustness2FeaturesEXT physicalDeviceRobustness2FeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, nullptr };
    VkPhysicalDeviceImageRobustnessFeatures physicalDeviceImageRobustnessFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, nullptr };
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR physicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR, nullptr };
#ifdef VK_ENABLE_BETA_EXTENSIONS
    VkPhysicalDevicePortabilitySubsetFeaturesKHR physicalDevicePortabilitySubsetFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR, nullptr };
#endif
    VkPhysicalDevice4444FormatsFeaturesEXT physicalDevice4444FormatsFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, nullptr };
    VkPhysicalDeviceSubpassShadingFeaturesHUAWEI physicalDeviceSubpassShadingFeaturesHUAWEI{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_SHADING_FEATURES_HUAWEI, nullptr };
    VkPhysicalDeviceClusterCullingShaderFeaturesHUAWEI physicalDeviceClusterCullingShaderFeaturesHUAWEI{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_CULLING_SHADER_FEATURES_HUAWEI, nullptr };
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT physicalDeviceShaderImageAtomicInt64FeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT, nullptr };
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR physicalDeviceFragmentShadingRateFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR, nullptr };
    VkPhysicalDeviceShaderTerminateInvocationFeatures physicalDeviceShaderTerminateInvocationFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES, nullptr };
    VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV physicalDeviceFragmentShadingRateEnumsFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV, nullptr };
    VkPhysicalDeviceImage2DViewOf3DFeaturesEXT physicalDeviceImage2DViewOf3DFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT, nullptr };
    VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT physicalDeviceImageSlicedViewOf3DFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_SLICED_VIEW_OF_3D_FEATURES_EXT, nullptr };
    VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT physicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT, nullptr };
    VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT physicalDeviceLegacyVertexAttributesFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_FEATURES_EXT, nullptr };
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT physicalDeviceMutableDescriptorTypeFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, nullptr };
    VkPhysicalDeviceDepthClipControlFeaturesEXT physicalDeviceDepthClipControlFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, nullptr };
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT physicalDeviceVertexInputDynamicStateFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, nullptr };
    VkPhysicalDeviceExternalMemoryRDMAFeaturesNV physicalDeviceExternalMemoryRDMAFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_RDMA_FEATURES_NV, nullptr };
    VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR physicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_RELAXED_EXTENDED_INSTRUCTION_FEATURES_KHR, nullptr };
    VkPhysicalDeviceColorWriteEnableFeaturesEXT physicalDeviceColorWriteEnableFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT, nullptr };
    VkPhysicalDeviceSynchronization2Features physicalDeviceSynchronization2Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, nullptr };
    VkPhysicalDeviceHostImageCopyFeatures physicalDeviceHostImageCopyFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES, nullptr };
    VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT physicalDevicePrimitivesGeneratedQueryFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT, nullptr };
    VkPhysicalDeviceLegacyDitheringFeaturesEXT physicalDeviceLegacyDitheringFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_DITHERING_FEATURES_EXT, nullptr };
    VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT physicalDeviceMultisampledRenderToSingleSampledFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_FEATURES_EXT, nullptr };
    VkPhysicalDevicePipelineProtectedAccessFeatures physicalDevicePipelineProtectedAccessFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROTECTED_ACCESS_FEATURES, nullptr };
    VkPhysicalDeviceVideoMaintenance1FeaturesKHR physicalDeviceVideoMaintenance1FeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR, nullptr };
    VkPhysicalDeviceInheritedViewportScissorFeaturesNV physicalDeviceInheritedViewportScissorFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INHERITED_VIEWPORT_SCISSOR_FEATURES_NV, nullptr };
    VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT physicalDeviceYcbcr2Plane444FormatsFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_2_PLANE_444_FORMATS_FEATURES_EXT, nullptr };
    VkPhysicalDeviceProvokingVertexFeaturesEXT physicalDeviceProvokingVertexFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, nullptr };
    VkPhysicalDeviceDescriptorBufferFeaturesEXT physicalDeviceDescriptorBufferFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, nullptr };
    VkPhysicalDeviceShaderIntegerDotProductFeatures physicalDeviceShaderIntegerDotProductFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, nullptr };
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR physicalDeviceFragmentShaderBarycentricFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR, nullptr };
    VkPhysicalDeviceRayTracingMotionBlurFeaturesNV physicalDeviceRayTracingMotionBlurFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MOTION_BLUR_FEATURES_NV, nullptr };
    VkPhysicalDeviceRayTracingValidationFeaturesNV physicalDeviceRayTracingValidationFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_VALIDATION_FEATURES_NV, nullptr };
    VkPhysicalDeviceRGBA10X6FormatsFeaturesEXT physicalDeviceRGBA10X6FormatsFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RGBA10X6_FORMATS_FEATURES_EXT, nullptr };
    VkPhysicalDeviceDynamicRenderingFeatures physicalDeviceDynamicRenderingFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, nullptr };
    VkPhysicalDeviceImageViewMinLodFeaturesEXT physicalDeviceImageViewMinLodFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT, nullptr };
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT physicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT, nullptr };
    VkPhysicalDeviceLinearColorAttachmentFeaturesNV physicalDeviceLinearColorAttachmentFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINEAR_COLOR_ATTACHMENT_FEATURES_NV, nullptr };
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT physicalDeviceGraphicsPipelineLibraryFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, nullptr };
    VkPhysicalDevicePipelineBinaryFeaturesKHR physicalDevicePipelineBinaryFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, nullptr };
    VkPhysicalDeviceDescriptorSetHostMappingFeaturesVALVE physicalDeviceDescriptorSetHostMappingFeaturesVALVE{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_SET_HOST_MAPPING_FEATURES_VALVE, nullptr };
    VkPhysicalDeviceNestedCommandBufferFeaturesEXT physicalDeviceNestedCommandBufferFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NESTED_COMMAND_BUFFER_FEATURES_EXT, nullptr };
    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT physicalDeviceShaderModuleIdentifierFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT, nullptr };
    VkPhysicalDeviceImageCompressionControlFeaturesEXT physicalDeviceImageCompressionControlFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT, nullptr };
    VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT physicalDeviceImageCompressionControlSwapchainFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN_FEATURES_EXT, nullptr };
    VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT physicalDeviceSubpassMergeFeedbackFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_MERGE_FEEDBACK_FEATURES_EXT, nullptr };
    VkPhysicalDeviceOpacityMicromapFeaturesEXT physicalDeviceOpacityMicromapFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT, nullptr };
#ifdef VK_ENABLE_BETA_EXTENSIONS
    VkPhysicalDeviceDisplacementMicromapFeaturesNV physicalDeviceDisplacementMicromapFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DISPLACEMENT_MICROMAP_FEATURES_NV, nullptr };
#endif
    VkPhysicalDevicePipelinePropertiesFeaturesEXT physicalDevicePipelinePropertiesFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROPERTIES_FEATURES_EXT, nullptr };
    VkPhysicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD physicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EARLY_AND_LATE_FRAGMENT_TESTS_FEATURES_AMD, nullptr };
    VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT physicalDeviceNonSeamlessCubeMapFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT, nullptr };
    VkPhysicalDevicePipelineRobustnessFeatures physicalDevicePipelineRobustnessFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES, nullptr };
    VkPhysicalDeviceImageProcessingFeaturesQCOM physicalDeviceImageProcessingFeaturesQCOM{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM, nullptr };
    VkPhysicalDeviceTilePropertiesFeaturesQCOM physicalDeviceTilePropertiesFeaturesQCOM{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_PROPERTIES_FEATURES_QCOM, nullptr };
    VkPhysicalDeviceAmigoProfilingFeaturesSEC physicalDeviceAmigoProfilingFeaturesSEC{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_AMIGO_PROFILING_FEATURES_SEC, nullptr };
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT physicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT, nullptr };
    VkPhysicalDeviceDepthClampZeroOneFeaturesEXT physicalDeviceDepthClampZeroOneFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_ZERO_ONE_FEATURES_EXT, nullptr };
    VkPhysicalDeviceAddressBindingReportFeaturesEXT physicalDeviceAddressBindingReportFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT, nullptr };
    VkPhysicalDeviceOpticalFlowFeaturesNV physicalDeviceOpticalFlowFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV, nullptr };
    VkPhysicalDeviceFaultFeaturesEXT physicalDeviceFaultFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, nullptr };
    VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT physicalDevicePipelineLibraryGroupHandlesFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_LIBRARY_GROUP_HANDLES_FEATURES_EXT, nullptr };
    VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM physicalDeviceShaderCoreBuiltinsFeaturesARM{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_BUILTINS_FEATURES_ARM, nullptr };
    VkPhysicalDeviceFrameBoundaryFeaturesEXT physicalDeviceFrameBoundaryFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAME_BOUNDARY_FEATURES_EXT, nullptr };
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT physicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT, nullptr };
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT physicalDeviceSwapchainMaintenance1FeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT, nullptr };
    VkPhysicalDeviceDepthBiasControlFeaturesEXT physicalDeviceDepthBiasControlFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT, nullptr };
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV physicalDeviceRayTracingInvocationReorderFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV, nullptr };
    VkPhysicalDeviceExtendedSparseAddressSpaceFeaturesNV physicalDeviceExtendedSparseAddressSpaceFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_SPARSE_ADDRESS_SPACE_FEATURES_NV, nullptr };
    VkPhysicalDeviceMultiviewPerViewViewportsFeaturesQCOM physicalDeviceMultiviewPerViewViewportsFeaturesQCOM{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PER_VIEW_VIEWPORTS_FEATURES_QCOM, nullptr };
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR physicalDeviceRayTracingPositionFetchFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR, nullptr };
    VkPhysicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM physicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PER_VIEW_RENDER_AREAS_FEATURES_QCOM, nullptr };
    VkPhysicalDeviceShaderObjectFeaturesEXT physicalDeviceShaderObjectFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, nullptr };
    VkPhysicalDeviceShaderTileImageFeaturesEXT physicalDeviceShaderTileImageFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_FEATURES_EXT, nullptr };
#ifdef VK_USE_PLATFORM_SCREEN_QNX
    VkPhysicalDeviceExternalMemoryScreenBufferFeaturesQNX physicalDeviceExternalMemoryScreenBufferFeaturesQNX{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_SCREEN_BUFFER_FEATURES_QNX, nullptr };
#endif
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR physicalDeviceCooperativeMatrixFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, nullptr };
#ifdef VK_ENABLE_BETA_EXTENSIONS
    VkPhysicalDeviceShaderEnqueueFeaturesAMDX physicalDeviceShaderEnqueueFeaturesAMDX{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ENQUEUE_FEATURES_AMDX, nullptr };
#endif
    VkPhysicalDeviceAntiLagFeaturesAMD physicalDeviceAntiLagFeaturesAMD{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD, nullptr };
    VkPhysicalDeviceCubicClampFeaturesQCOM physicalDeviceCubicClampFeaturesQCOM{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUBIC_CLAMP_FEATURES_QCOM, nullptr };
    VkPhysicalDeviceYcbcrDegammaFeaturesQCOM physicalDeviceYcbcrDegammaFeaturesQCOM{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_DEGAMMA_FEATURES_QCOM, nullptr };
    VkPhysicalDeviceCubicWeightsFeaturesQCOM physicalDeviceCubicWeightsFeaturesQCOM{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUBIC_WEIGHTS_FEATURES_QCOM, nullptr };
    VkPhysicalDeviceImageProcessing2FeaturesQCOM physicalDeviceImageProcessing2FeaturesQCOM{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_2_FEATURES_QCOM, nullptr };
    VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV physicalDeviceDescriptorPoolOverallocationFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_POOL_OVERALLOCATION_FEATURES_NV, nullptr };
    VkPhysicalDevicePerStageDescriptorSetFeaturesNV physicalDevicePerStageDescriptorSetFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV, nullptr };
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    VkPhysicalDeviceExternalFormatResolveFeaturesANDROID physicalDeviceExternalFormatResolveFeaturesANDROID{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FORMAT_RESOLVE_FEATURES_ANDROID, nullptr };
#endif
    VkPhysicalDeviceCudaKernelLaunchFeaturesNV physicalDeviceCudaKernelLaunchFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUDA_KERNEL_LAUNCH_FEATURES_NV, nullptr };
    VkPhysicalDeviceSchedulingControlsFeaturesARM physicalDeviceSchedulingControlsFeaturesARM{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCHEDULING_CONTROLS_FEATURES_ARM, nullptr };
    VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG physicalDeviceRelaxedLineRasterizationFeaturesIMG{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG, nullptr };
    VkPhysicalDeviceRenderPassStripedFeaturesARM physicalDeviceRenderPassStripedFeaturesARM{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RENDER_PASS_STRIPED_FEATURES_ARM, nullptr };
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR physicalDeviceShaderMaximalReconvergenceFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR, nullptr };
    VkPhysicalDeviceShaderSubgroupRotateFeatures physicalDeviceShaderSubgroupRotateFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_ROTATE_FEATURES, nullptr };
    VkPhysicalDeviceShaderExpectAssumeFeatures physicalDeviceShaderExpectAssumeFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EXPECT_ASSUME_FEATURES, nullptr };
    VkPhysicalDeviceShaderFloatControls2Features physicalDeviceShaderFloatControls2Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES, nullptr };
    VkPhysicalDeviceDynamicRenderingLocalReadFeatures physicalDeviceDynamicRenderingLocalReadFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES, nullptr };
    VkPhysicalDeviceShaderQuadControlFeaturesKHR physicalDeviceShaderQuadControlFeaturesKHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR, nullptr };
    VkPhysicalDeviceShaderAtomicFloat16VectorFeaturesNV physicalDeviceShaderAtomicFloat16VectorFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT16_VECTOR_FEATURES_NV, nullptr };
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT physicalDeviceMapMemoryPlacedFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT, nullptr };
    VkPhysicalDeviceRawAccessChainsFeaturesNV physicalDeviceRawAccessChainsFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAW_ACCESS_CHAINS_FEATURES_NV, nullptr };
    VkPhysicalDeviceCommandBufferInheritanceFeaturesNV physicalDeviceCommandBufferInheritanceFeaturesNV{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMMAND_BUFFER_INHERITANCE_FEATURES_NV, nullptr };
    VkPhysicalDeviceImageAlignmentControlFeaturesMESA physicalDeviceImageAlignmentControlFeaturesMESA{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ALIGNMENT_CONTROL_FEATURES_MESA, nullptr };
    VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT physicalDeviceShaderReplicatedCompositesFeaturesEXT{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_REPLICATED_COMPOSITES_FEATURES_EXT, nullptr };
    VkPhysicalDeviceFeatures2KHR physicalDeviceFeatures2KHR{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR, nullptr };

    FeaturesChain() {
        // Initializing all feature structures, number of Features (VkBool32) per structure.
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV, size<VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_COMPUTE_FEATURES_NV, size<VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, size<VkPhysicalDevicePrivateDataFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, size<VkPhysicalDeviceVariablePointersFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, size<VkPhysicalDeviceMultiviewFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, size<VkPhysicalDevicePresentIdFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, size<VkPhysicalDevicePresentWaitFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, size<VkPhysicalDevice16BitStorageFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, size<VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, size<VkPhysicalDeviceSamplerYcbcrConversionFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, size<VkPhysicalDeviceProtectedMemoryFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT, size<VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT, size<VkPhysicalDeviceMultiDrawFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, size<VkPhysicalDeviceInlineUniformBlockFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, size<VkPhysicalDeviceMaintenance4Features>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES, size<VkPhysicalDeviceMaintenance5Features>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES, size<VkPhysicalDeviceMaintenance6Features>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR, size<VkPhysicalDeviceMaintenance7FeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, size<VkPhysicalDeviceShaderDrawParametersFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, size<VkPhysicalDeviceShaderFloat16Int8Features>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, size<VkPhysicalDeviceHostQueryResetFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GLOBAL_PRIORITY_QUERY_FEATURES, size<VkPhysicalDeviceGlobalPriorityQueryFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT, size<VkPhysicalDeviceDeviceMemoryReportFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, size<VkPhysicalDeviceDescriptorIndexingFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, size<VkPhysicalDeviceTimelineSemaphoreFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, size<VkPhysicalDevice8BitStorageFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT, size<VkPhysicalDeviceConditionalRenderingFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, size<VkPhysicalDeviceVulkanMemoryModelFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, size<VkPhysicalDeviceShaderAtomicInt64Features>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT, size<VkPhysicalDeviceShaderAtomicFloatFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT, size<VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES, size<VkPhysicalDeviceVertexAttributeDivisorFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ASTC_DECODE_FEATURES_EXT, size<VkPhysicalDeviceASTCDecodeFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, size<VkPhysicalDeviceTransformFeedbackFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_REPRESENTATIVE_FRAGMENT_TEST_FEATURES_NV, size<VkPhysicalDeviceRepresentativeFragmentTestFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXCLUSIVE_SCISSOR_FEATURES_NV, size<VkPhysicalDeviceExclusiveScissorFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CORNER_SAMPLED_IMAGE_FEATURES_NV, size<VkPhysicalDeviceCornerSampledImageFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_FOOTPRINT_FEATURES_NV, size<VkPhysicalDeviceShaderImageFootprintFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEDICATED_ALLOCATION_IMAGE_ALIASING_FEATURES_NV, size<VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_NV, size<VkPhysicalDeviceCopyMemoryIndirectFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_FEATURES_NV, size<VkPhysicalDeviceMemoryDecompressionFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV, size<VkPhysicalDeviceShadingRateImageFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INVOCATION_MASK_FEATURES_HUAWEI, size<VkPhysicalDeviceInvocationMaskFeaturesHUAWEI>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV, size<VkPhysicalDeviceMeshShaderFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, size<VkPhysicalDeviceMeshShaderFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, size<VkPhysicalDeviceAccelerationStructureFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, size<VkPhysicalDeviceRayTracingPipelineFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, size<VkPhysicalDeviceRayQueryFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR, size<VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT, size<VkPhysicalDeviceFragmentDensityMapFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_2_FEATURES_EXT, size<VkPhysicalDeviceFragmentDensityMap2FeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_OFFSET_FEATURES_QCOM, size<VkPhysicalDeviceFragmentDensityMapOffsetFeaturesQCOM>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, size<VkPhysicalDeviceScalarBlockLayoutFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, size<VkPhysicalDeviceUniformBufferStandardLayoutFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, size<VkPhysicalDeviceDepthClipEnableFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT, size<VkPhysicalDeviceMemoryPriorityFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT, size<VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, size<VkPhysicalDeviceBufferDeviceAddressFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT, size<VkPhysicalDeviceBufferDeviceAddressFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, size<VkPhysicalDeviceImagelessFramebufferFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES, size<VkPhysicalDeviceTextureCompressionASTCHDRFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_NV, size<VkPhysicalDeviceCooperativeMatrixFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_IMAGE_ARRAYS_FEATURES_EXT, size<VkPhysicalDeviceYcbcrImageArraysFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_BARRIER_FEATURES_NV, size<VkPhysicalDevicePresentBarrierFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR, size<VkPhysicalDevicePerformanceQueryFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COVERAGE_REDUCTION_MODE_FEATURES_NV, size<VkPhysicalDeviceCoverageReductionModeFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_FUNCTIONS_2_FEATURES_INTEL, size<VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR, size<VkPhysicalDeviceShaderClockFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES, size<VkPhysicalDeviceIndexTypeUint8Features>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_FEATURES_NV, size<VkPhysicalDeviceShaderSMBuiltinsFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, size<VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, size<VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT, size<VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, size<VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, size<VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT, size<VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, size<VkPhysicalDeviceSubgroupSizeControlFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES, size<VkPhysicalDeviceLineRasterizationFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, size<VkPhysicalDevicePipelineCreationCacheControlFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, size<VkPhysicalDeviceVulkan11Features>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, size<VkPhysicalDeviceVulkan12Features>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, size<VkPhysicalDeviceVulkan13Features>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, size<VkPhysicalDeviceVulkan14Features>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD, size<VkPhysicalDeviceCoherentMemoryFeaturesAMD>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, size<VkPhysicalDeviceCustomBorderColorFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT, size<VkPhysicalDeviceBorderColorSwizzleFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, size<VkPhysicalDeviceExtendedDynamicStateFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, size<VkPhysicalDeviceExtendedDynamicState2FeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, size<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV, size<VkPhysicalDeviceDiagnosticsConfigFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES, size<VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR, size<VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, size<VkPhysicalDeviceRobustness2FeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, size<VkPhysicalDeviceImageRobustnessFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR, size<VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR>() });
#ifdef VK_ENABLE_BETA_EXTENSIONS
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR, size<VkPhysicalDevicePortabilitySubsetFeaturesKHR>() });
#endif
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, size<VkPhysicalDevice4444FormatsFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_SHADING_FEATURES_HUAWEI, size<VkPhysicalDeviceSubpassShadingFeaturesHUAWEI>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_CULLING_SHADER_FEATURES_HUAWEI, size<VkPhysicalDeviceClusterCullingShaderFeaturesHUAWEI>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT, size<VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR, size<VkPhysicalDeviceFragmentShadingRateFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES, size<VkPhysicalDeviceShaderTerminateInvocationFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV, size<VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT, size<VkPhysicalDeviceImage2DViewOf3DFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_SLICED_VIEW_OF_3D_FEATURES_EXT, size<VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT, size<VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_FEATURES_EXT, size<VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, size<VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, size<VkPhysicalDeviceDepthClipControlFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, size<VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_RDMA_FEATURES_NV, size<VkPhysicalDeviceExternalMemoryRDMAFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_RELAXED_EXTENDED_INSTRUCTION_FEATURES_KHR, size<VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT, size<VkPhysicalDeviceColorWriteEnableFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, size<VkPhysicalDeviceSynchronization2Features>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES, size<VkPhysicalDeviceHostImageCopyFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT, size<VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_DITHERING_FEATURES_EXT, size<VkPhysicalDeviceLegacyDitheringFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_FEATURES_EXT, size<VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROTECTED_ACCESS_FEATURES, size<VkPhysicalDevicePipelineProtectedAccessFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR, size<VkPhysicalDeviceVideoMaintenance1FeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INHERITED_VIEWPORT_SCISSOR_FEATURES_NV, size<VkPhysicalDeviceInheritedViewportScissorFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_2_PLANE_444_FORMATS_FEATURES_EXT, size<VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, size<VkPhysicalDeviceProvokingVertexFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, size<VkPhysicalDeviceDescriptorBufferFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, size<VkPhysicalDeviceShaderIntegerDotProductFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR, size<VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MOTION_BLUR_FEATURES_NV, size<VkPhysicalDeviceRayTracingMotionBlurFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_VALIDATION_FEATURES_NV, size<VkPhysicalDeviceRayTracingValidationFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RGBA10X6_FORMATS_FEATURES_EXT, size<VkPhysicalDeviceRGBA10X6FormatsFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, size<VkPhysicalDeviceDynamicRenderingFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT, size<VkPhysicalDeviceImageViewMinLodFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT, size<VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINEAR_COLOR_ATTACHMENT_FEATURES_NV, size<VkPhysicalDeviceLinearColorAttachmentFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, size<VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, size<VkPhysicalDevicePipelineBinaryFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_SET_HOST_MAPPING_FEATURES_VALVE, size<VkPhysicalDeviceDescriptorSetHostMappingFeaturesVALVE>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NESTED_COMMAND_BUFFER_FEATURES_EXT, size<VkPhysicalDeviceNestedCommandBufferFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT, size<VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT, size<VkPhysicalDeviceImageCompressionControlFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN_FEATURES_EXT, size<VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_MERGE_FEEDBACK_FEATURES_EXT, size<VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT, size<VkPhysicalDeviceOpacityMicromapFeaturesEXT>() });
#ifdef VK_ENABLE_BETA_EXTENSIONS
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DISPLACEMENT_MICROMAP_FEATURES_NV, size<VkPhysicalDeviceDisplacementMicromapFeaturesNV>() });
#endif
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROPERTIES_FEATURES_EXT, size<VkPhysicalDevicePipelinePropertiesFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EARLY_AND_LATE_FRAGMENT_TESTS_FEATURES_AMD, size<VkPhysicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT, size<VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES, size<VkPhysicalDevicePipelineRobustnessFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM, size<VkPhysicalDeviceImageProcessingFeaturesQCOM>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_PROPERTIES_FEATURES_QCOM, size<VkPhysicalDeviceTilePropertiesFeaturesQCOM>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_AMIGO_PROFILING_FEATURES_SEC, size<VkPhysicalDeviceAmigoProfilingFeaturesSEC>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT, size<VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_ZERO_ONE_FEATURES_EXT, size<VkPhysicalDeviceDepthClampZeroOneFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT, size<VkPhysicalDeviceAddressBindingReportFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV, size<VkPhysicalDeviceOpticalFlowFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, size<VkPhysicalDeviceFaultFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_LIBRARY_GROUP_HANDLES_FEATURES_EXT, size<VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_BUILTINS_FEATURES_ARM, size<VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAME_BOUNDARY_FEATURES_EXT, size<VkPhysicalDeviceFrameBoundaryFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT, size<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT, size<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT, size<VkPhysicalDeviceDepthBiasControlFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV, size<VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_SPARSE_ADDRESS_SPACE_FEATURES_NV, size<VkPhysicalDeviceExtendedSparseAddressSpaceFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PER_VIEW_VIEWPORTS_FEATURES_QCOM, size<VkPhysicalDeviceMultiviewPerViewViewportsFeaturesQCOM>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR, size<VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PER_VIEW_RENDER_AREAS_FEATURES_QCOM, size<VkPhysicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, size<VkPhysicalDeviceShaderObjectFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_FEATURES_EXT, size<VkPhysicalDeviceShaderTileImageFeaturesEXT>() });
#ifdef VK_USE_PLATFORM_SCREEN_QNX
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_SCREEN_BUFFER_FEATURES_QNX, size<VkPhysicalDeviceExternalMemoryScreenBufferFeaturesQNX>() });
#endif
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, size<VkPhysicalDeviceCooperativeMatrixFeaturesKHR>() });
#ifdef VK_ENABLE_BETA_EXTENSIONS
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ENQUEUE_FEATURES_AMDX, size<VkPhysicalDeviceShaderEnqueueFeaturesAMDX>() });
#endif
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD, size<VkPhysicalDeviceAntiLagFeaturesAMD>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUBIC_CLAMP_FEATURES_QCOM, size<VkPhysicalDeviceCubicClampFeaturesQCOM>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_DEGAMMA_FEATURES_QCOM, size<VkPhysicalDeviceYcbcrDegammaFeaturesQCOM>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUBIC_WEIGHTS_FEATURES_QCOM, size<VkPhysicalDeviceCubicWeightsFeaturesQCOM>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_2_FEATURES_QCOM, size<VkPhysicalDeviceImageProcessing2FeaturesQCOM>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_POOL_OVERALLOCATION_FEATURES_NV, size<VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV, size<VkPhysicalDevicePerStageDescriptorSetFeaturesNV>() });
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FORMAT_RESOLVE_FEATURES_ANDROID, size<VkPhysicalDeviceExternalFormatResolveFeaturesANDROID>() });
#endif
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUDA_KERNEL_LAUNCH_FEATURES_NV, size<VkPhysicalDeviceCudaKernelLaunchFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCHEDULING_CONTROLS_FEATURES_ARM, size<VkPhysicalDeviceSchedulingControlsFeaturesARM>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG, size<VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RENDER_PASS_STRIPED_FEATURES_ARM, size<VkPhysicalDeviceRenderPassStripedFeaturesARM>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR, size<VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_ROTATE_FEATURES, size<VkPhysicalDeviceShaderSubgroupRotateFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EXPECT_ASSUME_FEATURES, size<VkPhysicalDeviceShaderExpectAssumeFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES, size<VkPhysicalDeviceShaderFloatControls2Features>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES, size<VkPhysicalDeviceDynamicRenderingLocalReadFeatures>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR, size<VkPhysicalDeviceShaderQuadControlFeaturesKHR>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT16_VECTOR_FEATURES_NV, size<VkPhysicalDeviceShaderAtomicFloat16VectorFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT, size<VkPhysicalDeviceMapMemoryPlacedFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAW_ACCESS_CHAINS_FEATURES_NV, size<VkPhysicalDeviceRawAccessChainsFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMMAND_BUFFER_INHERITANCE_FEATURES_NV, size<VkPhysicalDeviceCommandBufferInheritanceFeaturesNV>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ALIGNMENT_CONTROL_FEATURES_MESA, size<VkPhysicalDeviceImageAlignmentControlFeaturesMESA>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_REPLICATED_COMPOSITES_FEATURES_EXT, size<VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT>() });
        this->structureSize.insert({ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR, size<VkPhysicalDeviceFeatures2KHR>() });

        //Initializing the full list of available structure features
        void* pNext = nullptr;
        physicalDeviceDeviceGeneratedCommandsFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceDeviceGeneratedCommandsFeaturesNV;
        physicalDeviceDeviceGeneratedCommandsComputeFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceDeviceGeneratedCommandsComputeFeaturesNV;
        physicalDevicePrivateDataFeatures.pNext = pNext;
        pNext = &physicalDevicePrivateDataFeatures;
        physicalDeviceVariablePointersFeatures.pNext = pNext;
        pNext = &physicalDeviceVariablePointersFeatures;
        physicalDeviceMultiviewFeatures.pNext = pNext;
        pNext = &physicalDeviceMultiviewFeatures;
        physicalDevicePresentIdFeaturesKHR.pNext = pNext;
        pNext = &physicalDevicePresentIdFeaturesKHR;
        physicalDevicePresentWaitFeaturesKHR.pNext = pNext;
        pNext = &physicalDevicePresentWaitFeaturesKHR;
        physicalDevice16BitStorageFeatures.pNext = pNext;
        pNext = &physicalDevice16BitStorageFeatures;
        physicalDeviceShaderSubgroupExtendedTypesFeatures.pNext = pNext;
        pNext = &physicalDeviceShaderSubgroupExtendedTypesFeatures;
        physicalDeviceSamplerYcbcrConversionFeatures.pNext = pNext;
        pNext = &physicalDeviceSamplerYcbcrConversionFeatures;
        physicalDeviceProtectedMemoryFeatures.pNext = pNext;
        pNext = &physicalDeviceProtectedMemoryFeatures;
        physicalDeviceBlendOperationAdvancedFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceBlendOperationAdvancedFeaturesEXT;
        physicalDeviceMultiDrawFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceMultiDrawFeaturesEXT;
        physicalDeviceInlineUniformBlockFeatures.pNext = pNext;
        pNext = &physicalDeviceInlineUniformBlockFeatures;
        physicalDeviceMaintenance4Features.pNext = pNext;
        pNext = &physicalDeviceMaintenance4Features;
        physicalDeviceMaintenance5Features.pNext = pNext;
        pNext = &physicalDeviceMaintenance5Features;
        physicalDeviceMaintenance6Features.pNext = pNext;
        pNext = &physicalDeviceMaintenance6Features;
        physicalDeviceMaintenance7FeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceMaintenance7FeaturesKHR;
        physicalDeviceShaderDrawParametersFeatures.pNext = pNext;
        pNext = &physicalDeviceShaderDrawParametersFeatures;
        physicalDeviceShaderFloat16Int8Features.pNext = pNext;
        pNext = &physicalDeviceShaderFloat16Int8Features;
        physicalDeviceHostQueryResetFeatures.pNext = pNext;
        pNext = &physicalDeviceHostQueryResetFeatures;
        physicalDeviceGlobalPriorityQueryFeatures.pNext = pNext;
        pNext = &physicalDeviceGlobalPriorityQueryFeatures;
        physicalDeviceDeviceMemoryReportFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceDeviceMemoryReportFeaturesEXT;
        physicalDeviceDescriptorIndexingFeatures.pNext = pNext;
        pNext = &physicalDeviceDescriptorIndexingFeatures;
        physicalDeviceTimelineSemaphoreFeatures.pNext = pNext;
        pNext = &physicalDeviceTimelineSemaphoreFeatures;
        physicalDevice8BitStorageFeatures.pNext = pNext;
        pNext = &physicalDevice8BitStorageFeatures;
        physicalDeviceConditionalRenderingFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceConditionalRenderingFeaturesEXT;
        physicalDeviceVulkanMemoryModelFeatures.pNext = pNext;
        pNext = &physicalDeviceVulkanMemoryModelFeatures;
        physicalDeviceShaderAtomicInt64Features.pNext = pNext;
        pNext = &physicalDeviceShaderAtomicInt64Features;
        physicalDeviceShaderAtomicFloatFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceShaderAtomicFloatFeaturesEXT;
        physicalDeviceShaderAtomicFloat2FeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceShaderAtomicFloat2FeaturesEXT;
        physicalDeviceVertexAttributeDivisorFeatures.pNext = pNext;
        pNext = &physicalDeviceVertexAttributeDivisorFeatures;
        physicalDeviceASTCDecodeFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceASTCDecodeFeaturesEXT;
        physicalDeviceTransformFeedbackFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceTransformFeedbackFeaturesEXT;
        physicalDeviceRepresentativeFragmentTestFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceRepresentativeFragmentTestFeaturesNV;
        physicalDeviceExclusiveScissorFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceExclusiveScissorFeaturesNV;
        physicalDeviceCornerSampledImageFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceCornerSampledImageFeaturesNV;
        physicalDeviceShaderImageFootprintFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceShaderImageFootprintFeaturesNV;
        physicalDeviceDedicatedAllocationImageAliasingFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceDedicatedAllocationImageAliasingFeaturesNV;
        physicalDeviceCopyMemoryIndirectFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceCopyMemoryIndirectFeaturesNV;
        physicalDeviceMemoryDecompressionFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceMemoryDecompressionFeaturesNV;
        physicalDeviceShadingRateImageFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceShadingRateImageFeaturesNV;
        physicalDeviceInvocationMaskFeaturesHUAWEI.pNext = pNext;
        pNext = &physicalDeviceInvocationMaskFeaturesHUAWEI;
        physicalDeviceMeshShaderFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceMeshShaderFeaturesNV;
        physicalDeviceMeshShaderFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceMeshShaderFeaturesEXT;
        physicalDeviceAccelerationStructureFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceAccelerationStructureFeaturesKHR;
        physicalDeviceRayTracingPipelineFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceRayTracingPipelineFeaturesKHR;
        physicalDeviceRayQueryFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceRayQueryFeaturesKHR;
        physicalDeviceRayTracingMaintenance1FeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceRayTracingMaintenance1FeaturesKHR;
        physicalDeviceFragmentDensityMapFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceFragmentDensityMapFeaturesEXT;
        physicalDeviceFragmentDensityMap2FeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceFragmentDensityMap2FeaturesEXT;
        physicalDeviceFragmentDensityMapOffsetFeaturesQCOM.pNext = pNext;
        pNext = &physicalDeviceFragmentDensityMapOffsetFeaturesQCOM;
        physicalDeviceScalarBlockLayoutFeatures.pNext = pNext;
        pNext = &physicalDeviceScalarBlockLayoutFeatures;
        physicalDeviceUniformBufferStandardLayoutFeatures.pNext = pNext;
        pNext = &physicalDeviceUniformBufferStandardLayoutFeatures;
        physicalDeviceDepthClipEnableFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceDepthClipEnableFeaturesEXT;
        physicalDeviceMemoryPriorityFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceMemoryPriorityFeaturesEXT;
        physicalDevicePageableDeviceLocalMemoryFeaturesEXT.pNext = pNext;
        pNext = &physicalDevicePageableDeviceLocalMemoryFeaturesEXT;
        physicalDeviceBufferDeviceAddressFeatures.pNext = pNext;
        pNext = &physicalDeviceBufferDeviceAddressFeatures;
        physicalDeviceBufferDeviceAddressFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceBufferDeviceAddressFeaturesEXT;
        physicalDeviceImagelessFramebufferFeatures.pNext = pNext;
        pNext = &physicalDeviceImagelessFramebufferFeatures;
        physicalDeviceTextureCompressionASTCHDRFeatures.pNext = pNext;
        pNext = &physicalDeviceTextureCompressionASTCHDRFeatures;
        physicalDeviceCooperativeMatrixFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceCooperativeMatrixFeaturesNV;
        physicalDeviceYcbcrImageArraysFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceYcbcrImageArraysFeaturesEXT;
        physicalDevicePresentBarrierFeaturesNV.pNext = pNext;
        pNext = &physicalDevicePresentBarrierFeaturesNV;
        physicalDevicePerformanceQueryFeaturesKHR.pNext = pNext;
        pNext = &physicalDevicePerformanceQueryFeaturesKHR;
        physicalDeviceCoverageReductionModeFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceCoverageReductionModeFeaturesNV;
        physicalDeviceShaderIntegerFunctions2FeaturesINTEL.pNext = pNext;
        pNext = &physicalDeviceShaderIntegerFunctions2FeaturesINTEL;
        physicalDeviceShaderClockFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceShaderClockFeaturesKHR;
        physicalDeviceIndexTypeUint8Features.pNext = pNext;
        pNext = &physicalDeviceIndexTypeUint8Features;
        physicalDeviceShaderSMBuiltinsFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceShaderSMBuiltinsFeaturesNV;
        physicalDeviceFragmentShaderInterlockFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceFragmentShaderInterlockFeaturesEXT;
        physicalDeviceSeparateDepthStencilLayoutsFeatures.pNext = pNext;
        pNext = &physicalDeviceSeparateDepthStencilLayoutsFeatures;
        physicalDevicePrimitiveTopologyListRestartFeaturesEXT.pNext = pNext;
        pNext = &physicalDevicePrimitiveTopologyListRestartFeaturesEXT;
        physicalDevicePipelineExecutablePropertiesFeaturesKHR.pNext = pNext;
        pNext = &physicalDevicePipelineExecutablePropertiesFeaturesKHR;
        physicalDeviceShaderDemoteToHelperInvocationFeatures.pNext = pNext;
        pNext = &physicalDeviceShaderDemoteToHelperInvocationFeatures;
        physicalDeviceTexelBufferAlignmentFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceTexelBufferAlignmentFeaturesEXT;
        physicalDeviceSubgroupSizeControlFeatures.pNext = pNext;
        pNext = &physicalDeviceSubgroupSizeControlFeatures;
        physicalDeviceLineRasterizationFeatures.pNext = pNext;
        pNext = &physicalDeviceLineRasterizationFeatures;
        physicalDevicePipelineCreationCacheControlFeatures.pNext = pNext;
        pNext = &physicalDevicePipelineCreationCacheControlFeatures;
        physicalDeviceVulkan11Features.pNext = pNext;
        pNext = &physicalDeviceVulkan11Features;
        physicalDeviceVulkan12Features.pNext = pNext;
        pNext = &physicalDeviceVulkan12Features;
        physicalDeviceVulkan13Features.pNext = pNext;
        pNext = &physicalDeviceVulkan13Features;
        physicalDeviceVulkan14Features.pNext = pNext;
        pNext = &physicalDeviceVulkan14Features;
        physicalDeviceCoherentMemoryFeaturesAMD.pNext = pNext;
        pNext = &physicalDeviceCoherentMemoryFeaturesAMD;
        physicalDeviceCustomBorderColorFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceCustomBorderColorFeaturesEXT;
        physicalDeviceBorderColorSwizzleFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceBorderColorSwizzleFeaturesEXT;
        physicalDeviceExtendedDynamicStateFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceExtendedDynamicStateFeaturesEXT;
        physicalDeviceExtendedDynamicState2FeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceExtendedDynamicState2FeaturesEXT;
        physicalDeviceExtendedDynamicState3FeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceExtendedDynamicState3FeaturesEXT;
        physicalDeviceDiagnosticsConfigFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceDiagnosticsConfigFeaturesNV;
        physicalDeviceZeroInitializeWorkgroupMemoryFeatures.pNext = pNext;
        pNext = &physicalDeviceZeroInitializeWorkgroupMemoryFeatures;
        physicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR;
        physicalDeviceRobustness2FeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceRobustness2FeaturesEXT;
        physicalDeviceImageRobustnessFeatures.pNext = pNext;
        pNext = &physicalDeviceImageRobustnessFeatures;
        physicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR;
#ifdef VK_ENABLE_BETA_EXTENSIONS
        physicalDevicePortabilitySubsetFeaturesKHR.pNext = pNext;
        pNext = &physicalDevicePortabilitySubsetFeaturesKHR;
#endif
        physicalDevice4444FormatsFeaturesEXT.pNext = pNext;
        pNext = &physicalDevice4444FormatsFeaturesEXT;
        physicalDeviceSubpassShadingFeaturesHUAWEI.pNext = pNext;
        pNext = &physicalDeviceSubpassShadingFeaturesHUAWEI;
        physicalDeviceClusterCullingShaderFeaturesHUAWEI.pNext = pNext;
        pNext = &physicalDeviceClusterCullingShaderFeaturesHUAWEI;
        physicalDeviceShaderImageAtomicInt64FeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceShaderImageAtomicInt64FeaturesEXT;
        physicalDeviceFragmentShadingRateFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceFragmentShadingRateFeaturesKHR;
        physicalDeviceShaderTerminateInvocationFeatures.pNext = pNext;
        pNext = &physicalDeviceShaderTerminateInvocationFeatures;
        physicalDeviceFragmentShadingRateEnumsFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceFragmentShadingRateEnumsFeaturesNV;
        physicalDeviceImage2DViewOf3DFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceImage2DViewOf3DFeaturesEXT;
        physicalDeviceImageSlicedViewOf3DFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceImageSlicedViewOf3DFeaturesEXT;
        physicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT;
        physicalDeviceLegacyVertexAttributesFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceLegacyVertexAttributesFeaturesEXT;
        physicalDeviceMutableDescriptorTypeFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceMutableDescriptorTypeFeaturesEXT;
        physicalDeviceDepthClipControlFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceDepthClipControlFeaturesEXT;
        physicalDeviceVertexInputDynamicStateFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceVertexInputDynamicStateFeaturesEXT;
        physicalDeviceExternalMemoryRDMAFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceExternalMemoryRDMAFeaturesNV;
        physicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR;
        physicalDeviceColorWriteEnableFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceColorWriteEnableFeaturesEXT;
        physicalDeviceSynchronization2Features.pNext = pNext;
        pNext = &physicalDeviceSynchronization2Features;
        physicalDeviceHostImageCopyFeatures.pNext = pNext;
        pNext = &physicalDeviceHostImageCopyFeatures;
        physicalDevicePrimitivesGeneratedQueryFeaturesEXT.pNext = pNext;
        pNext = &physicalDevicePrimitivesGeneratedQueryFeaturesEXT;
        physicalDeviceLegacyDitheringFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceLegacyDitheringFeaturesEXT;
        physicalDeviceMultisampledRenderToSingleSampledFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceMultisampledRenderToSingleSampledFeaturesEXT;
        physicalDevicePipelineProtectedAccessFeatures.pNext = pNext;
        pNext = &physicalDevicePipelineProtectedAccessFeatures;
        physicalDeviceVideoMaintenance1FeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceVideoMaintenance1FeaturesKHR;
        physicalDeviceInheritedViewportScissorFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceInheritedViewportScissorFeaturesNV;
        physicalDeviceYcbcr2Plane444FormatsFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceYcbcr2Plane444FormatsFeaturesEXT;
        physicalDeviceProvokingVertexFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceProvokingVertexFeaturesEXT;
        physicalDeviceDescriptorBufferFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceDescriptorBufferFeaturesEXT;
        physicalDeviceShaderIntegerDotProductFeatures.pNext = pNext;
        pNext = &physicalDeviceShaderIntegerDotProductFeatures;
        physicalDeviceFragmentShaderBarycentricFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceFragmentShaderBarycentricFeaturesKHR;
        physicalDeviceRayTracingMotionBlurFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceRayTracingMotionBlurFeaturesNV;
        physicalDeviceRayTracingValidationFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceRayTracingValidationFeaturesNV;
        physicalDeviceRGBA10X6FormatsFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceRGBA10X6FormatsFeaturesEXT;
        physicalDeviceDynamicRenderingFeatures.pNext = pNext;
        pNext = &physicalDeviceDynamicRenderingFeatures;
        physicalDeviceImageViewMinLodFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceImageViewMinLodFeaturesEXT;
        physicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT;
        physicalDeviceLinearColorAttachmentFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceLinearColorAttachmentFeaturesNV;
        physicalDeviceGraphicsPipelineLibraryFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceGraphicsPipelineLibraryFeaturesEXT;
        physicalDevicePipelineBinaryFeaturesKHR.pNext = pNext;
        pNext = &physicalDevicePipelineBinaryFeaturesKHR;
        physicalDeviceDescriptorSetHostMappingFeaturesVALVE.pNext = pNext;
        pNext = &physicalDeviceDescriptorSetHostMappingFeaturesVALVE;
        physicalDeviceNestedCommandBufferFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceNestedCommandBufferFeaturesEXT;
        physicalDeviceShaderModuleIdentifierFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceShaderModuleIdentifierFeaturesEXT;
        physicalDeviceImageCompressionControlFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceImageCompressionControlFeaturesEXT;
        physicalDeviceImageCompressionControlSwapchainFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceImageCompressionControlSwapchainFeaturesEXT;
        physicalDeviceSubpassMergeFeedbackFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceSubpassMergeFeedbackFeaturesEXT;
        physicalDeviceOpacityMicromapFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceOpacityMicromapFeaturesEXT;
#ifdef VK_ENABLE_BETA_EXTENSIONS
        physicalDeviceDisplacementMicromapFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceDisplacementMicromapFeaturesNV;
#endif
        physicalDevicePipelinePropertiesFeaturesEXT.pNext = pNext;
        pNext = &physicalDevicePipelinePropertiesFeaturesEXT;
        physicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD.pNext = pNext;
        pNext = &physicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD;
        physicalDeviceNonSeamlessCubeMapFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceNonSeamlessCubeMapFeaturesEXT;
        physicalDevicePipelineRobustnessFeatures.pNext = pNext;
        pNext = &physicalDevicePipelineRobustnessFeatures;
        physicalDeviceImageProcessingFeaturesQCOM.pNext = pNext;
        pNext = &physicalDeviceImageProcessingFeaturesQCOM;
        physicalDeviceTilePropertiesFeaturesQCOM.pNext = pNext;
        pNext = &physicalDeviceTilePropertiesFeaturesQCOM;
        physicalDeviceAmigoProfilingFeaturesSEC.pNext = pNext;
        pNext = &physicalDeviceAmigoProfilingFeaturesSEC;
        physicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT;
        physicalDeviceDepthClampZeroOneFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceDepthClampZeroOneFeaturesEXT;
        physicalDeviceAddressBindingReportFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceAddressBindingReportFeaturesEXT;
        physicalDeviceOpticalFlowFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceOpticalFlowFeaturesNV;
        physicalDeviceFaultFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceFaultFeaturesEXT;
        physicalDevicePipelineLibraryGroupHandlesFeaturesEXT.pNext = pNext;
        pNext = &physicalDevicePipelineLibraryGroupHandlesFeaturesEXT;
        physicalDeviceShaderCoreBuiltinsFeaturesARM.pNext = pNext;
        pNext = &physicalDeviceShaderCoreBuiltinsFeaturesARM;
        physicalDeviceFrameBoundaryFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceFrameBoundaryFeaturesEXT;
        physicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT;
        physicalDeviceSwapchainMaintenance1FeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceSwapchainMaintenance1FeaturesEXT;
        physicalDeviceDepthBiasControlFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceDepthBiasControlFeaturesEXT;
        physicalDeviceRayTracingInvocationReorderFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceRayTracingInvocationReorderFeaturesNV;
        physicalDeviceExtendedSparseAddressSpaceFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceExtendedSparseAddressSpaceFeaturesNV;
        physicalDeviceMultiviewPerViewViewportsFeaturesQCOM.pNext = pNext;
        pNext = &physicalDeviceMultiviewPerViewViewportsFeaturesQCOM;
        physicalDeviceRayTracingPositionFetchFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceRayTracingPositionFetchFeaturesKHR;
        physicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM.pNext = pNext;
        pNext = &physicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM;
        physicalDeviceShaderObjectFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceShaderObjectFeaturesEXT;
        physicalDeviceShaderTileImageFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceShaderTileImageFeaturesEXT;
#ifdef VK_USE_PLATFORM_SCREEN_QNX
        physicalDeviceExternalMemoryScreenBufferFeaturesQNX.pNext = pNext;
        pNext = &physicalDeviceExternalMemoryScreenBufferFeaturesQNX;
#endif
        physicalDeviceCooperativeMatrixFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceCooperativeMatrixFeaturesKHR;
#ifdef VK_ENABLE_BETA_EXTENSIONS
        physicalDeviceShaderEnqueueFeaturesAMDX.pNext = pNext;
        pNext = &physicalDeviceShaderEnqueueFeaturesAMDX;
#endif
        physicalDeviceAntiLagFeaturesAMD.pNext = pNext;
        pNext = &physicalDeviceAntiLagFeaturesAMD;
        physicalDeviceCubicClampFeaturesQCOM.pNext = pNext;
        pNext = &physicalDeviceCubicClampFeaturesQCOM;
        physicalDeviceYcbcrDegammaFeaturesQCOM.pNext = pNext;
        pNext = &physicalDeviceYcbcrDegammaFeaturesQCOM;
        physicalDeviceCubicWeightsFeaturesQCOM.pNext = pNext;
        pNext = &physicalDeviceCubicWeightsFeaturesQCOM;
        physicalDeviceImageProcessing2FeaturesQCOM.pNext = pNext;
        pNext = &physicalDeviceImageProcessing2FeaturesQCOM;
        physicalDeviceDescriptorPoolOverallocationFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceDescriptorPoolOverallocationFeaturesNV;
        physicalDevicePerStageDescriptorSetFeaturesNV.pNext = pNext;
        pNext = &physicalDevicePerStageDescriptorSetFeaturesNV;
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        physicalDeviceExternalFormatResolveFeaturesANDROID.pNext = pNext;
        pNext = &physicalDeviceExternalFormatResolveFeaturesANDROID;
#endif
        physicalDeviceCudaKernelLaunchFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceCudaKernelLaunchFeaturesNV;
        physicalDeviceSchedulingControlsFeaturesARM.pNext = pNext;
        pNext = &physicalDeviceSchedulingControlsFeaturesARM;
        physicalDeviceRelaxedLineRasterizationFeaturesIMG.pNext = pNext;
        pNext = &physicalDeviceRelaxedLineRasterizationFeaturesIMG;
        physicalDeviceRenderPassStripedFeaturesARM.pNext = pNext;
        pNext = &physicalDeviceRenderPassStripedFeaturesARM;
        physicalDeviceShaderMaximalReconvergenceFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceShaderMaximalReconvergenceFeaturesKHR;
        physicalDeviceShaderSubgroupRotateFeatures.pNext = pNext;
        pNext = &physicalDeviceShaderSubgroupRotateFeatures;
        physicalDeviceShaderExpectAssumeFeatures.pNext = pNext;
        pNext = &physicalDeviceShaderExpectAssumeFeatures;
        physicalDeviceShaderFloatControls2Features.pNext = pNext;
        pNext = &physicalDeviceShaderFloatControls2Features;
        physicalDeviceDynamicRenderingLocalReadFeatures.pNext = pNext;
        pNext = &physicalDeviceDynamicRenderingLocalReadFeatures;
        physicalDeviceShaderQuadControlFeaturesKHR.pNext = pNext;
        pNext = &physicalDeviceShaderQuadControlFeaturesKHR;
        physicalDeviceShaderAtomicFloat16VectorFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceShaderAtomicFloat16VectorFeaturesNV;
        physicalDeviceMapMemoryPlacedFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceMapMemoryPlacedFeaturesEXT;
        physicalDeviceRawAccessChainsFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceRawAccessChainsFeaturesNV;
        physicalDeviceCommandBufferInheritanceFeaturesNV.pNext = pNext;
        pNext = &physicalDeviceCommandBufferInheritanceFeaturesNV;
        physicalDeviceImageAlignmentControlFeaturesMESA.pNext = pNext;
        pNext = &physicalDeviceImageAlignmentControlFeaturesMESA;
        physicalDeviceShaderReplicatedCompositesFeaturesEXT.pNext = pNext;
        pNext = &physicalDeviceShaderReplicatedCompositesFeaturesEXT;
        physicalDeviceFeatures2KHR.pNext = pNext;

    }


    VkPhysicalDeviceFeatures2KHR requiredFeaturesChain{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR, nullptr};
    VkBaseOutStructure* current = nullptr;

    void ApplyRobustness(const VpDeviceCreateInfo* pCreateInfo) {
#ifdef VK_VERSION_1_1
        VkPhysicalDeviceFeatures2KHR* pFeatures2 = static_cast<VkPhysicalDeviceFeatures2KHR*>(
            vpGetStructure(&this->requiredFeaturesChain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR));
        if (pFeatures2 != nullptr && (pCreateInfo->flags & VP_DEVICE_CREATE_DISABLE_ROBUST_BUFFER_ACCESS_BIT)) {
            pFeatures2->features.robustBufferAccess = VK_FALSE;
        }
#endif

#ifdef VK_EXT_robustness2
        VkPhysicalDeviceRobustness2FeaturesEXT* pRobustness2FeaturesEXT = static_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(
            vpGetStructure(&this->requiredFeaturesChain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT));
        if (pRobustness2FeaturesEXT != nullptr) {
            if (pCreateInfo->flags & VP_DEVICE_CREATE_DISABLE_ROBUST_BUFFER_ACCESS_BIT) {
                pRobustness2FeaturesEXT->robustBufferAccess2 = VK_FALSE;
            }
            if (pCreateInfo->flags & VP_DEVICE_CREATE_DISABLE_ROBUST_IMAGE_ACCESS_BIT) {
                pRobustness2FeaturesEXT->robustImageAccess2 = VK_FALSE;
            }
        }
#endif
#ifdef VK_EXT_image_robustness
        VkPhysicalDeviceImageRobustnessFeaturesEXT* pImageRobustnessFeaturesEXT =
            static_cast<VkPhysicalDeviceImageRobustnessFeaturesEXT*>(vpGetStructure(
                &this->requiredFeaturesChain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT));
        if (pImageRobustnessFeaturesEXT != nullptr && (pCreateInfo->flags & VP_DEVICE_CREATE_DISABLE_ROBUST_IMAGE_ACCESS_BIT)) {
            pImageRobustnessFeaturesEXT->robustImageAccess = VK_FALSE;
        }
#endif
#ifdef VK_VERSION_1_3
        VkPhysicalDeviceVulkan13Features* pVulkan13Features = static_cast<VkPhysicalDeviceVulkan13Features*>(
            vpGetStructure(&this->requiredFeaturesChain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES));
        if (pVulkan13Features != nullptr && (pCreateInfo->flags & VP_DEVICE_CREATE_DISABLE_ROBUST_IMAGE_ACCESS_BIT)) {
            pVulkan13Features->robustImageAccess = VK_FALSE;
        }
#endif
    }

    void ApplyFeatures(const VpDeviceCreateInfo* pCreateInfo) {
        const std::size_t offset = sizeof(VkBaseOutStructure);
        const VkBaseOutStructure* q = reinterpret_cast<const VkBaseOutStructure*>(pCreateInfo->pCreateInfo->pNext);
        while (q) {
            const std::size_t count = this->structureSize[q->sType];
            for (std::size_t index = 0; index < count; ++index) {
                const VkBaseOutStructure* pInputStruct = reinterpret_cast<const VkBaseOutStructure*>(q);
                VkBaseOutStructure* pOutputStruct = reinterpret_cast<VkBaseOutStructure*>(detail::vpGetStructure(&this->requiredFeaturesChain, q->sType));
                const uint8_t* pInputData = reinterpret_cast<const uint8_t*>(pInputStruct) + offset;
                uint8_t* pOutputData = reinterpret_cast<uint8_t*>(pOutputStruct) + offset;
                const VkBool32* input = reinterpret_cast<const VkBool32*>(pInputData);
                VkBool32* output = reinterpret_cast<VkBool32*>(pOutputData);

                output[index] = (output[index] == VK_TRUE || input[index] == VK_TRUE) ? VK_TRUE : VK_FALSE;
            }
            q = q->pNext;
        }

        this->ApplyRobustness(pCreateInfo);
    }

    void PushBack(VkBaseOutStructure* found) {
        VkBaseOutStructure* last = reinterpret_cast<VkBaseOutStructure*>(&requiredFeaturesChain);
        while (last->pNext != nullptr) {
            last = last->pNext;
        }
        last->pNext = found;
    }

    void Build(const std::vector<VkStructureType>& requiredList) {
        for (std::size_t i = 0, n = requiredList.size(); i < n; ++i) {
            const VkStructureType sType = requiredList[i];
            if (sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR) {
                continue;
            }

            VkBaseOutStructure* found = vpExtractStructure(&physicalDeviceFeatures2KHR, sType);
            if (found == nullptr) {
                continue;
            }

            PushBack(found);
        }
    }
}; // struct FeaturesChain

VPAPI_ATTR const VpProfileDesc* vpGetProfileDesc(const char profileName[VP_MAX_PROFILE_NAME_SIZE]) {
    for (uint32_t profileIndex = 0; profileIndex < profileCount; ++profileIndex) {
        if (strncmp(profiles[profileIndex].props.profileName, profileName, VP_MAX_PROFILE_NAME_SIZE) == 0) {
            return &profiles[profileIndex];
        }
    }
    return nullptr;
}

VPAPI_ATTR std::vector<VpProfileProperties> GatherProfiles(const VpProfileProperties& profile, const char* pBlockName = nullptr) {
    std::vector<VpProfileProperties> gatheredProfiles;

    if (pBlockName == nullptr) {
        const detail::VpProfileDesc* profileDesc = detail::vpGetProfileDesc(profile.profileName);
        if (profileDesc != nullptr) {
            for (uint32_t profileIndex = 0; profileIndex < profileDesc->requiredProfileCount; ++profileIndex) {
                gatheredProfiles.push_back(profileDesc->pRequiredProfiles[profileIndex]);
            }
        }
    }

    gatheredProfiles.push_back(profile);

    return gatheredProfiles;
}

VPAPI_ATTR bool vpCheckVersion(uint32_t actual, uint32_t expected) {
    uint32_t actualMajor = VK_API_VERSION_MAJOR(actual);
    uint32_t actualMinor = VK_API_VERSION_MINOR(actual);
    uint32_t expectedMajor = VK_API_VERSION_MAJOR(expected);
    uint32_t expectedMinor = VK_API_VERSION_MINOR(expected);
    return actualMajor > expectedMajor || (actualMajor == expectedMajor && actualMinor >= expectedMinor);
}

VPAPI_ATTR bool HasExtension(const std::vector<VkExtensionProperties>& list, const VkExtensionProperties& element) {
    for (std::size_t i = 0, n = list.size(); i < n; ++i) {
        if (strcmp(list[i].extensionName, element.extensionName) == 0) {
            return true;
        }
    }

    return false;
}

VPAPI_ATTR bool CheckExtension(const VkExtensionProperties* supportedProperties, size_t supportedSize, const char *requestedExtension) {
    bool found = false;
    for (size_t i = 0, n = supportedSize; i < n; ++i) {
        if (strcmp(supportedProperties[i].extensionName, requestedExtension) == 0) {
            found = true;
            break;
            // Drivers don't actually update their spec version, so we cannot rely on this
            // if (supportedProperties[i].specVersion >= expectedVersion) found = true;
        }
    }
    return found;
}

VPAPI_ATTR bool CheckExtension(const std::vector<const char*>& extensions, const char* extension) {
    for (const char* c : extensions) {
        if (strcmp(c, extension) == 0) {
            return true;
        }
    }
    return false;
}

VPAPI_ATTR void GetExtensions(uint32_t extensionCount, const VkExtensionProperties *pExtensions, std::vector<const char *> &extensions) {
    for (uint32_t ext_index = 0; ext_index < extensionCount; ++ext_index) {
        if (CheckExtension(extensions, pExtensions[ext_index].extensionName)) {
            continue;
        }
        extensions.push_back(pExtensions[ext_index].extensionName);
    }
}

VPAPI_ATTR std::vector<VpBlockProperties> GatherBlocks(
    uint32_t enabledFullProfileCount, const VpProfileProperties* pEnabledFullProfiles,
    uint32_t enabledProfileBlockCount, const VpBlockProperties* pEnabledProfileBlocks) {
    std::vector<VpBlockProperties> results;

    for (std::size_t profile_index = 0; profile_index < enabledFullProfileCount; ++profile_index) {
        const std::vector<VpProfileProperties>& gathered_profiles = GatherProfiles(pEnabledFullProfiles[profile_index]);

        for (std::size_t gathered_index = 0; gathered_index < gathered_profiles.size(); ++gathered_index) {
            VpBlockProperties block{gathered_profiles[gathered_index], 0, ""};
            results.push_back(block);
        }
    }

    for (std::size_t block_index = 0; block_index < enabledProfileBlockCount; ++block_index) {
        results.push_back(pEnabledProfileBlocks[block_index]);
    }

    return results;
}

VPAPI_ATTR VkResult vpGetInstanceProfileSupportSingleProfile(
    uint32_t                                    api_version,
    const std::vector<VkExtensionProperties>&   supported_extensions,
    const VpProfileProperties*                  pProfile,
    VkBool32*                                   pSupported,
    std::vector<VpBlockProperties>&             supportedBlocks,
    std::vector<VpBlockProperties>&             unsupportedBlocks) {
    assert(pProfile != nullptr);

    const detail::VpProfileDesc* pProfileDesc = vpGetProfileDesc(pProfile->profileName);
    if (pProfileDesc == nullptr) {
        *pSupported = VK_FALSE;
        return VK_ERROR_UNKNOWN;
    }

    VpBlockProperties block{*pProfile, api_version};

    if (pProfileDesc->props.specVersion < pProfile->specVersion) {
        *pSupported = VK_FALSE;
        unsupportedBlocks.push_back(block);
    }

    // Required API version is built in root profile, not need to check dependent profile API versions
    if (api_version != 0) {
        if (!vpCheckVersion(api_version, pProfileDesc->minApiVersion)) {

            *pSupported = VK_FALSE;
            unsupportedBlocks.push_back(block);
        }
    }

    for (uint32_t capability_index = 0; capability_index < pProfileDesc->requiredCapabilityCount; ++capability_index) {
        const VpCapabilitiesDesc& capabilities_desc = pProfileDesc->pRequiredCapabilities[capability_index];

        VkBool32 supported_capabilities = VK_FALSE;
        for (uint32_t variant_index = 0; variant_index < capabilities_desc.variantCount; ++variant_index) {
            const VpVariantDesc& variant_desc = capabilities_desc.pVariants[variant_index];

            VkBool32 supported_variant = VK_TRUE;
            for (uint32_t i = 0; i < variant_desc.instanceExtensionCount; ++i) {
                if (!detail::CheckExtension(supported_extensions.data(), supported_extensions.size(),
                                              variant_desc.pInstanceExtensions[i].extensionName)) {
                    supported_variant = VK_FALSE;
                    memcpy(block.blockName, variant_desc.blockName, VP_MAX_PROFILE_NAME_SIZE * sizeof(char));
                    unsupportedBlocks.push_back(block);
                }
            }

            if (supported_variant == VK_TRUE) {
                supported_capabilities = VK_TRUE;
                memcpy(block.blockName, variant_desc.blockName, VP_MAX_PROFILE_NAME_SIZE * sizeof(char));
                supportedBlocks.push_back(block);
            }
        }

        if (supported_capabilities == VK_FALSE) {
            *pSupported = VK_FALSE;
            return VK_SUCCESS;
        }
    }

    return VK_SUCCESS;
}

enum structure_type {
    STRUCTURE_FEATURE = 0,
    STRUCTURE_PROPERTY,
    STRUCTURE_QUEUE_FAMILY,
    STRUCTURE_FORMAT
};

VPAPI_ATTR VkResult vpGetProfileStructureTypes(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    structure_type                              type,
    uint32_t*                                   pStructureTypeCount,
    VkStructureType*                            pStructureTypes) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    VkResult result = pBlockName == nullptr ? VK_SUCCESS : VK_INCOMPLETE;

    std::vector<VkStructureType> results;

    const std::vector<VpProfileProperties>& gathered_profiles = detail::GatherProfiles(*pProfile);

    for (std::size_t profile_index = 0, profile_count = gathered_profiles.size(); profile_index < profile_count; ++profile_index) {
        const detail::VpProfileDesc* profile_desc = detail::vpGetProfileDesc(gathered_profiles[profile_index].profileName);
        if (profile_desc == nullptr) return VK_ERROR_UNKNOWN;

        for (uint32_t capability_index = 0; capability_index < profile_desc->requiredCapabilityCount; ++capability_index) {
            const detail::VpCapabilitiesDesc& cap_desc = profile_desc->pRequiredCapabilities[capability_index];

            for (uint32_t variant_index = 0; variant_index < cap_desc.variantCount; ++variant_index) {
                const detail::VpVariantDesc& variant = cap_desc.pVariants[variant_index];
                if (pBlockName != nullptr) {
                    if (strcmp(variant.blockName, pBlockName) != 0) {
                        continue;
                    }
                    result = VK_SUCCESS;
                }

                uint32_t count = 0;
                const VkStructureType* data = nullptr;

                switch (type) {
                    default:
                    case STRUCTURE_FEATURE:
                        count = variant.featureStructTypeCount;
                        data = variant.pFeatureStructTypes;
                        break;
                    case STRUCTURE_PROPERTY:
                        count = variant.propertyStructTypeCount;
                        data = variant.pPropertyStructTypes;
                        break;
                    case STRUCTURE_QUEUE_FAMILY:
                        count = variant.queueFamilyStructTypeCount;
                        data = variant.pQueueFamilyStructTypes;
                        break;
                    case STRUCTURE_FORMAT:
                        count = variant.formatStructTypeCount;
                        data = variant.pFormatStructTypes;
                        break;
                }

                for (uint32_t type_index = 0; type_index < count; ++type_index) {
                    const VkStructureType dataType = data[type_index];
                    if (std::find(results.begin(), results.end(), dataType) == std::end(results)) {
                        results.push_back(dataType);
                    }
                }
            }
        }
    }

    const uint32_t count = static_cast<uint32_t>(results.size());
    std::sort(results.begin(), results.end());

    if (pStructureTypes == nullptr) {
        *pStructureTypeCount = count;
    } else {
        if (*pStructureTypeCount < count) {
            result = VK_INCOMPLETE;
        } else {
            *pStructureTypeCount = count;
        }

        if (*pStructureTypeCount > 0) {
            memcpy(pStructureTypes, &results[0], *pStructureTypeCount * sizeof(VkStructureType));
        }
    }

    return result;
}

enum ExtensionType {
    EXTENSION_INSTANCE,
    EXTENSION_DEVICE,
};

VPAPI_ATTR VkResult vpGetProfileExtensionProperties(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    ExtensionType                               type,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    VkResult result = pBlockName == nullptr ? VK_SUCCESS : VK_INCOMPLETE;

    std::vector<VkExtensionProperties> results;

    const std::vector<VpProfileProperties>& gathered_profiles = detail::GatherProfiles(*pProfile, pBlockName);

    for (std::size_t profile_index = 0, profile_count = gathered_profiles.size(); profile_index < profile_count; ++profile_index) {
        const detail::VpProfileDesc* profile_desc = detail::vpGetProfileDesc(gathered_profiles[profile_index].profileName);
        if (profile_desc == nullptr) {
            return VK_ERROR_UNKNOWN;
        }

        for (uint32_t capability_index = 0; capability_index < profile_desc->requiredCapabilityCount; ++capability_index) {
            const detail::VpCapabilitiesDesc& cap_desc = profile_desc->pRequiredCapabilities[capability_index];

            for (uint32_t variant_index = 0; variant_index < cap_desc.variantCount; ++variant_index) {
                const detail::VpVariantDesc& variant = cap_desc.pVariants[variant_index];
                if (pBlockName != nullptr) {
                    if (strcmp(variant.blockName, pBlockName) != 0) {
                        continue;
                    }
                    result = VK_SUCCESS;
                }

                switch (type) {
                    default:
                    case EXTENSION_INSTANCE:
                        for (uint32_t ext_index = 0; ext_index < variant.instanceExtensionCount; ++ext_index) {
                            if (detail::HasExtension(results, variant.pInstanceExtensions[ext_index])) {
                                continue;
                            }
                            results.push_back(variant.pInstanceExtensions[ext_index]);
                        }
                        break;
                    case EXTENSION_DEVICE:
                        for (uint32_t ext_index = 0; ext_index < variant.deviceExtensionCount; ++ext_index) {
                            if (detail::HasExtension(results, variant.pDeviceExtensions[ext_index])) {
                                continue;
                            }
                            results.push_back(variant.pDeviceExtensions[ext_index]);
                        }
                        break;
                }
            }
        }
    }

    const uint32_t count = static_cast<uint32_t>(results.size());

    if (pProperties == nullptr) {
        *pPropertyCount = count;
    } else {
        if (*pPropertyCount < count) {
            result = VK_INCOMPLETE;
        } else {
            *pPropertyCount = count;
        }
        if (*pPropertyCount > 0) {
            memcpy(pProperties, &results[0], *pPropertyCount * sizeof(VkExtensionProperties));
        }
    }

    return result;
}

VPAPI_ATTR VkResult vpGetProfileVideoProfileDesc(
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t                                    videoProfileIndex,
    const detail::VpVideoProfileDesc**          ppVideoProfileDesc) {
    VkResult result = pBlockName == nullptr ? VK_SUCCESS : VK_INCOMPLETE;

    uint32_t curr_base_video_profile_index = 0;

    const std::vector<VpProfileProperties>& gathered_profiles = detail::GatherProfiles(*pProfile);

    for (std::size_t profile_index = 0, profile_count = gathered_profiles.size(); profile_index < profile_count; ++profile_index) {
        const detail::VpProfileDesc* profile_desc = detail::vpGetProfileDesc(gathered_profiles[profile_index].profileName);
        if (profile_desc == nullptr) return VK_ERROR_UNKNOWN;

        for (uint32_t capability_index = 0; capability_index < profile_desc->requiredCapabilityCount; ++capability_index) {
            const detail::VpCapabilitiesDesc& cap_desc = profile_desc->pRequiredCapabilities[capability_index];

            for (uint32_t variant_index = 0; variant_index < cap_desc.variantCount; ++variant_index) {
                const detail::VpVariantDesc& variant = cap_desc.pVariants[variant_index];
                if (pBlockName != nullptr) {
                    if (strcmp(variant.blockName, pBlockName) != 0) {
                        continue;
                    }
                    result = VK_SUCCESS;
                }

                if (videoProfileIndex < curr_base_video_profile_index + variant.videoProfileCount) {
                    *ppVideoProfileDesc = &variant.pVideoProfiles[videoProfileIndex - curr_base_video_profile_index];
                    return result;
                } else {
                    curr_base_video_profile_index += variant.videoProfileCount;
                }
            }
        }
    }

    *ppVideoProfileDesc = nullptr;
    return VK_ERROR_UNKNOWN;
}

} // namespace detail

struct VpCapabilities_T : public VpVulkanFunctions {
    bool singleton = false;
    uint32_t apiVersion = VK_API_VERSION_1_0;

    static VpCapabilities_T& Get() {
        static VpCapabilities_T instance;
        VpCapabilitiesCreateInfo createInfo{};
        createInfo.flags = VP_PROFILE_CREATE_STATIC_BIT;
        instance.init(&createInfo);
        instance.singleton = true;
        return instance;
    }

    VpCapabilities_T() {
        this->GetInstanceProcAddr = nullptr;
        this->GetDeviceProcAddr = nullptr;
        this->EnumerateInstanceVersion = nullptr;
        this->EnumerateInstanceExtensionProperties = nullptr;
        this->EnumerateDeviceExtensionProperties = nullptr;
        this->GetPhysicalDeviceFeatures2 = nullptr;
        this->GetPhysicalDeviceProperties2 = nullptr;
        this->GetPhysicalDeviceFormatProperties2 = nullptr;
        this->GetPhysicalDeviceQueueFamilyProperties2 = nullptr;
        this->CreateInstance = nullptr;
        this->CreateDevice = nullptr;
    }

    VkResult init(const VpCapabilitiesCreateInfo* pCreateInfo) {
        assert(pCreateInfo != nullptr);

        return ImportVulkanFunctions(pCreateInfo);
    }

    VkResult ImportVulkanFunctions(const VpCapabilitiesCreateInfo* pCreateInfo) {
        if (pCreateInfo->flags & VP_PROFILE_CREATE_STATIC_BIT) {
            ImportVulkanFunctions_Static();
        }

        if (pCreateInfo->pVulkanFunctions != nullptr) {
            ImportVulkanFunctions_Custom((VpVulkanFunctions*)pCreateInfo->pVulkanFunctions);
        }
/*
        if (pCreateInfo->flags & VP_PROFILE_CREATE_DYNAMIC_BIT) {
            ImportVulkanFunctions_Dynamic();
        }
*/
        return ValidateVulkanFunctions();
    }

    void ImportVulkanFunctions_Static() {
        // Vulkan 1.1
        this->GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)vkGetInstanceProcAddr;
        this->GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)vkGetDeviceProcAddr;

        this->EnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)vkEnumerateInstanceVersion;
        this->EnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)vkEnumerateInstanceExtensionProperties;
        this->EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)vkEnumerateDeviceExtensionProperties;

        this->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)vkGetPhysicalDeviceFeatures2;
        this->GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)vkGetPhysicalDeviceProperties2;
        this->GetPhysicalDeviceFormatProperties2 = (PFN_vkGetPhysicalDeviceFormatProperties2)vkGetPhysicalDeviceFormatProperties2;
        this->GetPhysicalDeviceQueueFamilyProperties2 = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)vkGetPhysicalDeviceQueueFamilyProperties2;

        this->CreateInstance = (PFN_vkCreateInstance)vkCreateInstance;
        this->CreateDevice = (PFN_vkCreateDevice)vkCreateDevice;
    }

    void ImportVulkanFunctions_Custom(VpVulkanFunctions* pFunctions) {
    #define VP_COPY_IF_NOT_NULL(funcName)         if(pFunctions->funcName != nullptr) this->funcName = pFunctions->funcName;

        VP_COPY_IF_NOT_NULL(GetInstanceProcAddr);
        VP_COPY_IF_NOT_NULL(GetDeviceProcAddr);

        VP_COPY_IF_NOT_NULL(EnumerateInstanceVersion);
        VP_COPY_IF_NOT_NULL(EnumerateInstanceExtensionProperties);
        VP_COPY_IF_NOT_NULL(EnumerateDeviceExtensionProperties);

        VP_COPY_IF_NOT_NULL(GetPhysicalDeviceFeatures2);
        VP_COPY_IF_NOT_NULL(GetPhysicalDeviceProperties2);
        VP_COPY_IF_NOT_NULL(GetPhysicalDeviceFormatProperties2);
        VP_COPY_IF_NOT_NULL(GetPhysicalDeviceQueueFamilyProperties2);

        VP_COPY_IF_NOT_NULL(CreateInstance);
        VP_COPY_IF_NOT_NULL(CreateDevice);
    #undef VP_COPY_IF_NOT_NULL
    }
/*
    VkResult ImportVulkanFunctions_Dynamic() {
        // To use VP_PROFILE_CREATE_DYNAMIC_BIT you have to pass VpVulkanFunctions::vkGetInstanceProcAddr and vkGetDeviceProcAddr as VpCapabilitiesCreateInfo::pVulkanFunctions. Other members can be null.
        if (this->GetInstanceProcAddr == nullptr || this->GetDeviceProcAddr == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

    #define VP_FETCH_INSTANCE_FUNC(memberName, functionNameString)         if(this->memberName == nullptr)            this->memberName = (PFN_vk##memberName)this->GetInstanceProcAddr(m_hInstance, functionNameString);
    #define VP_FETCH_DEVICE_FUNC(memberName, functionNameString)         if(this->memberName == nullptr)             this->memberName = (PFN_vk##memberName)this->GetDeviceProcAddr(m_hDevice, functionNameString);

        VP_FETCH_INSTANCE_FUNC(GetInstanceProcAddr, "vkGetInstanceProcAddr");
        VP_FETCH_DEVICE_FUNC(GetDeviceProcAddr, "vkGetDeviceProcAddr");

        VP_FETCH_INSTANCE_FUNC(EnumerateInstanceVersion, "vkEnumerateInstanceVersion");
        VP_FETCH_INSTANCE_FUNC(EnumerateInstanceExtensionProperties, "vkEnumerateInstanceExtensionProperties");
        VP_FETCH_DEVICE_FUNC(EnumerateDeviceExtensionProperties, "vkEnumerateDeviceExtensionProperties");

        VP_FETCH_DEVICE_FUNC(GetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2");
        VP_FETCH_DEVICE_FUNC(GetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2");
        VP_FETCH_DEVICE_FUNC(GetPhysicalDeviceFormatProperties2, "vkGetPhysicalDeviceFormatProperties2");
        VP_FETCH_DEVICE_FUNC(GetPhysicalDeviceQueueFamilyProperties2, "vkGetPhysicalDeviceQueueFamilyProperties2");

        VP_FETCH_INSTANCE_FUNC(CreateInstance, "vkCreateInstance");
        VP_FETCH_DEVICE_FUNC(CreateDevice, "vkCreateDevice");
    #undef VP_FETCH_DEVICE_FUNC
    #undef VP_FETCH_INSTANCE_FUNC
    }
*/
    VkResult ValidateVulkanFunctions() {
        if (this->GetInstanceProcAddr == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (this->GetDeviceProcAddr == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (this->EnumerateInstanceVersion == nullptr && apiVersion >= VK_API_VERSION_1_1) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (this->EnumerateInstanceExtensionProperties == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (this->EnumerateDeviceExtensionProperties == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (this->GetPhysicalDeviceFeatures2 == nullptr) {
            return apiVersion >= VK_API_VERSION_1_1 ? VK_ERROR_INITIALIZATION_FAILED : VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        if (this->GetPhysicalDeviceProperties2 == nullptr) {
            return apiVersion >= VK_API_VERSION_1_1 ? VK_ERROR_INITIALIZATION_FAILED : VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        if (this->GetPhysicalDeviceFormatProperties2 == nullptr) {
            return apiVersion >= VK_API_VERSION_1_1 ? VK_ERROR_INITIALIZATION_FAILED : VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        if (this->GetPhysicalDeviceQueueFamilyProperties2 == nullptr) {
            return apiVersion >= VK_API_VERSION_1_1 ? VK_ERROR_INITIALIZATION_FAILED : VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        if (this->CreateInstance == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (this->CreateDevice == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return VK_SUCCESS;
    }
};

VPAPI_ATTR VkResult vpCreateCapabilities(
    const VpCapabilitiesCreateInfo*             pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VpCapabilities*                             pCapabilities) {
    (void)pAllocator;

    VpCapabilities_T* capabilities = new VpCapabilities_T();
    VkResult result = capabilities->init(pCreateInfo);
    *pCapabilities = capabilities;

    return result;
}

/// Destroys allocator object.
VPAPI_ATTR void vpDestroyCapabilities(
    VpCapabilities                              capabilities,
    const VkAllocationCallbacks*                pAllocator) {
    (void)pAllocator;
    
    delete capabilities;
}

VPAPI_ATTR VkResult vpGetProfiles(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    uint32_t*                                   pPropertyCount,
    VpProfileProperties*                        pProperties) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    VkResult result = VK_SUCCESS;

    if (pProperties == nullptr) {
        *pPropertyCount = detail::profileCount;
    } else {
        if (*pPropertyCount < detail::profileCount) {
            result = VK_INCOMPLETE;
        } else {
            *pPropertyCount = detail::profileCount;
        }
        for (uint32_t property_index = 0; property_index < *pPropertyCount; ++property_index) {
            pProperties[property_index] = detail::profiles[property_index].props;
        }
    }
    return result;
}

VPAPI_ATTR VkResult vpGetProfileRequiredProfiles(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    uint32_t*                                   pPropertyCount,
    VpProfileProperties*                        pProperties) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    VkResult result = VK_SUCCESS;

    const detail::VpProfileDesc* desc = detail::vpGetProfileDesc(pProfile->profileName);
    if (desc == nullptr) {
        return VK_ERROR_UNKNOWN;
    }

    if (pProperties == nullptr) {
        *pPropertyCount = desc->requiredProfileCount;
    } else {
        if (*pPropertyCount < desc->requiredProfileCount) {
            result = VK_INCOMPLETE;
        } else {
            *pPropertyCount = desc->requiredProfileCount;
        }
        for (uint32_t property_index = 0; property_index < *pPropertyCount; ++property_index) {
            pProperties[property_index] = desc->pRequiredProfiles[property_index];
        }
    }
    return result;
}

VPAPI_ATTR uint32_t vpGetProfileAPIVersion(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    const std::vector<VpProfileProperties>& gathered_profiles = detail::GatherProfiles(*pProfile, nullptr);

    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;

    for (std::size_t profile_index = 0, profile_count = gathered_profiles.size(); profile_index < profile_count; ++profile_index) {
        const detail::VpProfileDesc* desc = detail::vpGetProfileDesc(gathered_profiles[profile_index].profileName);
        if (desc == nullptr) {
            return 0;
        }

        major = std::max<uint32_t>(major, VK_API_VERSION_MAJOR(desc->minApiVersion));
        minor = std::max<uint32_t>(minor, VK_API_VERSION_MINOR(desc->minApiVersion));
        patch = std::max<uint32_t>(patch, VK_API_VERSION_PATCH(desc->minApiVersion));
    }

    return VK_MAKE_API_VERSION(0, major, minor, patch);
}

VPAPI_ATTR VkResult vpGetProfileFallbacks(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    uint32_t*                                   pPropertyCount,
    VpProfileProperties*                        pProperties) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    VkResult result = VK_SUCCESS;

    const detail::VpProfileDesc* desc = detail::vpGetProfileDesc(pProfile->profileName);
    if (desc == nullptr) {
        return VK_ERROR_UNKNOWN;
    }

    if (pProperties == nullptr) {
        *pPropertyCount = desc->fallbackCount;
    } else {
        if (*pPropertyCount < desc->fallbackCount) {
            result = VK_INCOMPLETE;
        } else {
            *pPropertyCount = desc->fallbackCount;
        }
        for (uint32_t i = 0; i < *pPropertyCount; ++i) {
            pProperties[i] = desc->pFallbacks[i];
        }
    }
    return result;
}

VPAPI_ATTR VkResult vpHasMultipleVariantsProfile(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    VkBool32*                                   pHasMultipleVariants) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    const std::vector<VpProfileProperties>& gathered_profiles = detail::GatherProfiles(*pProfile, nullptr);

    for (std::size_t profile_index = 0, profile_count = gathered_profiles.size(); profile_index < profile_count; ++profile_index) {
        const detail::VpProfileDesc* desc = detail::vpGetProfileDesc(gathered_profiles[profile_index].profileName);
        if (desc == nullptr) {
            return VK_ERROR_UNKNOWN;
        }

        for (uint32_t caps_index = 0, caps_count = desc->requiredCapabilityCount; caps_index < caps_count; ++caps_index) {
            if (desc->pRequiredCapabilities[caps_index].variantCount > 1) {
                *pHasMultipleVariants = VK_TRUE;
                return VK_SUCCESS;
            }
        }
    }

    *pHasMultipleVariants = VK_FALSE;
    return VK_SUCCESS;
}

VPAPI_ATTR VkResult vpGetInstanceProfileVariantsSupport(
#ifdef VP_USE_OBJECT
    VpCapabilities                      capabilities,
#endif//VP_USE_OBJECT
    const char*                         pLayerName,
    const VpProfileProperties*          pProfile,
    VkBool32*                           pSupported,
    uint32_t*                           pPropertyCount,
    VpBlockProperties*                  pProperties) {
#ifdef VP_USE_OBJECT
    const VpCapabilities_T& vp = capabilities == nullptr ? VpCapabilities_T::Get() : *capabilities;
#else
    const VpCapabilities_T& vp = VpCapabilities_T::Get();
#endif//VP_USE_OBJECT

    VkResult result = VK_SUCCESS;

    uint32_t api_version = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion pfnEnumerateInstanceVersion = vp.singleton ?
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion") : vp.EnumerateInstanceVersion;
    if (pfnEnumerateInstanceVersion != nullptr) {
        result = pfnEnumerateInstanceVersion(&api_version);
        if (result != VK_SUCCESS) {
            *pSupported = VK_FALSE;
            return result;
        } /* else {
        } */
    }

    uint32_t supported_instance_extension_count = 0;
    result = vp.EnumerateInstanceExtensionProperties(pLayerName, &supported_instance_extension_count, nullptr);
    if (result != VK_SUCCESS) {
        *pSupported = VK_FALSE;
        return result;
    }
    std::vector<VkExtensionProperties> supported_instance_extensions;
    if (supported_instance_extension_count > 0) {
        supported_instance_extensions.resize(supported_instance_extension_count);
    }
    result = vp.EnumerateInstanceExtensionProperties(pLayerName, &supported_instance_extension_count, supported_instance_extensions.data());
    if (result != VK_SUCCESS) {
        *pSupported = VK_FALSE;
        return result;
    }

    VkBool32 supported = VK_TRUE;

    // We require VK_KHR_get_physical_device_properties2 if we are on Vulkan 1.0
    if (api_version < VK_API_VERSION_1_1) {
        bool foundGPDP2 = false;
        for (size_t ext_index = 0, ext_count = supported_instance_extensions.size(); ext_index < ext_count; ++ext_index) {
            if (strcmp(supported_instance_extensions[ext_index].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) {
                foundGPDP2 = true;
                break;
            }
        }
        if (!foundGPDP2) {
            supported = VK_FALSE;
        }
    }

    const detail::VpProfileDesc* pProfileDesc = detail::vpGetProfileDesc(pProfile->profileName);
    if (pProfileDesc == nullptr) return VK_ERROR_UNKNOWN;

    std::vector<VpBlockProperties> supported_blocks;
    std::vector<VpBlockProperties> unsupported_blocks;

    result = detail::vpGetInstanceProfileSupportSingleProfile(api_version, supported_instance_extensions, pProfile, &supported, supported_blocks, unsupported_blocks);
    if (result != VK_SUCCESS) {
        *pSupported = supported;
        return result;
    }

    for (std::size_t required_profile_index = 0; required_profile_index < pProfileDesc->requiredProfileCount; ++required_profile_index) {
        result = detail::vpGetInstanceProfileSupportSingleProfile(0, supported_instance_extensions, &pProfileDesc->pRequiredProfiles[required_profile_index], &supported, supported_blocks, unsupported_blocks);
        if (result != VK_SUCCESS) {
            *pSupported = supported;
            return result;
        }
    }

    const std::vector<VpBlockProperties>& blocks = supported ? supported_blocks : unsupported_blocks;

    if (pProperties == nullptr) {
        *pPropertyCount = static_cast<uint32_t>(blocks.size());
    } else {
        if (*pPropertyCount < static_cast<uint32_t>(blocks.size())) {
            result = VK_INCOMPLETE;
        } else {
            *pPropertyCount = static_cast<uint32_t>(blocks.size());
        }
        for (uint32_t block_index = 0, block_count = static_cast<uint32_t>(blocks.size()); block_index < block_count; ++block_index) {
            pProperties[block_index] = blocks[block_index];
        }
    }

    *pSupported = supported;
    return result;
}

VPAPI_ATTR VkResult vpGetInstanceProfileSupport(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const char*                                 pLayerName,
    const VpProfileProperties*                  pProfile,
    VkBool32*                                   pSupported) {
    uint32_t count = 0;

    return vpGetInstanceProfileVariantsSupport(
#ifdef VP_USE_OBJECT
        capabilities,
#endif//VP_USE_OBJECT
        pLayerName, pProfile, pSupported, &count, nullptr);
}

VPAPI_ATTR VkResult vpCreateInstance(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkInstance*                                 pInstance) {
#ifdef VP_USE_OBJECT
    const VpCapabilities_T& vp = capabilities == nullptr ? VpCapabilities_T::Get() : *capabilities;
#else
    const VpCapabilities_T& vp = VpCapabilities_T::Get();
#endif//VP_USE_OBJECT

    if (pCreateInfo == nullptr || pInstance == nullptr) {
        return vp.CreateInstance(pCreateInfo == nullptr ? nullptr : pCreateInfo->pCreateInfo, pAllocator, pInstance);
    }

    const std::vector<VpBlockProperties>& blocks = detail::GatherBlocks(
        pCreateInfo->enabledFullProfileCount, pCreateInfo->pEnabledFullProfiles,
        pCreateInfo->enabledProfileBlockCount, pCreateInfo->pEnabledProfileBlocks);

    std::vector<const char*> extensions;
    for (std::uint32_t ext_index = 0, ext_count = pCreateInfo->pCreateInfo->enabledExtensionCount; ext_index < ext_count; ++ext_index) {
        extensions.push_back(pCreateInfo->pCreateInfo->ppEnabledExtensionNames[ext_index]);
    }

    for (std::size_t block_index = 0, block_count = blocks.size(); block_index < block_count; ++block_index) {
        const detail::VpProfileDesc* profile_desc = detail::vpGetProfileDesc(blocks[block_index].profiles.profileName);
        if (profile_desc == nullptr) {
            return VK_ERROR_UNKNOWN;
        }

        for (std::size_t caps_index = 0, caps_count = profile_desc->requiredCapabilityCount; caps_index < caps_count; ++caps_index) {
            const detail::VpCapabilitiesDesc* caps_desc = &profile_desc->pRequiredCapabilities[caps_index];

            for (std::size_t variant_index = 0, variant_count = caps_desc->variantCount; variant_index < variant_count; ++variant_index) {
                const detail::VpVariantDesc* variant = &caps_desc->pVariants[variant_index];

                if (strcmp(blocks[block_index].blockName, "") != 0) {
                    if (strcmp(variant->blockName, blocks[block_index].blockName) != 0) {
                        continue;
                    }
                }

                detail::GetExtensions(variant->instanceExtensionCount, variant->pInstanceExtensions, extensions);
            }
        }
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    if (pCreateInfo->pCreateInfo->pApplicationInfo != nullptr) {
        appInfo = *pCreateInfo->pCreateInfo->pApplicationInfo;
    } else if (!blocks.empty()) {
        appInfo.apiVersion = vpGetProfileAPIVersion(
#ifdef VP_USE_OBJECT
            capabilities,
#endif//VP_USE_OBJECT
            &blocks[0].profiles);
    }

    VkInstanceCreateInfo createInfo = *pCreateInfo->pCreateInfo;
    createInfo.pApplicationInfo = &appInfo;

    // Need to include VK_KHR_get_physical_device_properties2 if we are on Vulkan 1.0
    if (createInfo.pApplicationInfo->apiVersion < VK_API_VERSION_1_1) {
        bool foundGPDP2 = false;
        for (size_t ext_index = 0, ext_count = extensions.size(); ext_index < ext_count; ++ext_index) {
            if (strcmp(extensions[ext_index], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) {
                foundGPDP2 = true;
                break;
            }
        }
        if (!foundGPDP2) {
            extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        }
    }

#ifdef __APPLE__
    bool has_portability_ext = false;
    for (std::size_t ext_index = 0, ext_count = extensions.size(); ext_index < ext_count; ++ext_index) {
        if (strcmp(extensions[ext_index], VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            has_portability_ext = true;
            break;
        }
    }

    if (!has_portability_ext) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }

    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    if (!extensions.empty()) {
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
    }

    return vp.CreateInstance(&createInfo, pAllocator, pInstance);
}

VPAPI_ATTR VkResult vpGetPhysicalDeviceProfileVariantsSupport(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    VkInstance instance,
    VkPhysicalDevice physicalDevice,
    const VpProfileProperties *pProfile,
    VkBool32 *pSupported,
    uint32_t *pPropertyCount,
    VpBlockProperties* pProperties) {
#ifdef VP_USE_OBJECT
    const VpCapabilities_T& vp = capabilities == nullptr ? VpCapabilities_T::Get() : *capabilities;
#else
    const VpCapabilities_T& vp = VpCapabilities_T::Get();
#endif//VP_USE_OBJECT

    VkResult result = VK_SUCCESS;

    uint32_t supported_device_extension_count = 0;
    result = vp.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &supported_device_extension_count, nullptr);
    if (result != VK_SUCCESS) {
        return result;
    }
    std::vector<VkExtensionProperties> supported_device_extensions;
    if (supported_device_extension_count > 0) {
        supported_device_extensions.resize(supported_device_extension_count);
    }
    result = vp.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &supported_device_extension_count, supported_device_extensions.data());
    if (result != VK_SUCCESS) {
        return result;
    }

    // Workaround old loader bug where count could be smaller on the second call to vkEnumerateDeviceExtensionProperties
    if (supported_device_extension_count > 0) {
        supported_device_extensions.resize(supported_device_extension_count);
    }

    {
        const detail::VpProfileDesc* pProfileDesc = detail::vpGetProfileDesc(pProfile->profileName);
        if (pProfileDesc == nullptr) {
            return VK_ERROR_UNKNOWN;
        }
    }

    struct GPDP2EntryPoints {
        PFN_vkGetPhysicalDeviceFeatures2KHR                 pfnGetPhysicalDeviceFeatures2;
        PFN_vkGetPhysicalDeviceProperties2KHR               pfnGetPhysicalDeviceProperties2;
        PFN_vkGetPhysicalDeviceFormatProperties2KHR         pfnGetPhysicalDeviceFormatProperties2;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR    pfnGetPhysicalDeviceQueueFamilyProperties2;
    };

#ifdef VK_KHR_video_queue
    struct VideoInfo {
        PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR         pfnGetPhysicalDeviceVideoCapabilitiesKHR;
        PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR     pfnGetPhysicalDeviceVideoFormatPropertiesKHR;
        const detail::VpVideoProfileDesc*                   pProfileDesc;
        VkVideoProfileInfoKHR                               profileInfo;
        VkPhysicalDeviceVideoFormatInfoKHR                  formatInfo;
        bool                                                supportedProfile;
        uint32_t                                            matchingProfiles;
    };
#endif  // VK_KHR_video_queue

    std::vector<VpBlockProperties> supported_blocks;
    std::vector<VpBlockProperties> unsupported_blocks;

    struct UserData {
        VkPhysicalDevice physicalDevice;
        std::vector<VpBlockProperties>& supported_blocks;
        std::vector<VpBlockProperties>& unsupported_blocks;
        const detail::VpVariantDesc* variant;
        GPDP2EntryPoints gpdp2;
#ifdef VK_KHR_video_queue
        VideoInfo video;
#endif  // VK_KHR_video_queue
        uint32_t index;
        detail::PFN_vpStructChainerCb pfnCb;
        bool supported;
    } userData{physicalDevice, supported_blocks, unsupported_blocks};

    if (!vp.singleton) {
        userData.gpdp2.pfnGetPhysicalDeviceFeatures2 = vp.GetPhysicalDeviceFeatures2;
        userData.gpdp2.pfnGetPhysicalDeviceProperties2 = vp.GetPhysicalDeviceProperties2;
        userData.gpdp2.pfnGetPhysicalDeviceFormatProperties2 = vp.GetPhysicalDeviceFormatProperties2;
        userData.gpdp2.pfnGetPhysicalDeviceQueueFamilyProperties2 = vp.GetPhysicalDeviceQueueFamilyProperties2;
    }

    // Attempt to load core versions of the GPDP2 entry points
    if (userData.gpdp2.pfnGetPhysicalDeviceFeatures2 == nullptr) {
        userData.gpdp2.pfnGetPhysicalDeviceFeatures2 =
            (PFN_vkGetPhysicalDeviceFeatures2KHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2");
        userData.gpdp2.pfnGetPhysicalDeviceProperties2 =
            (PFN_vkGetPhysicalDeviceProperties2KHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2");
        userData.gpdp2.pfnGetPhysicalDeviceFormatProperties2 =
            (PFN_vkGetPhysicalDeviceFormatProperties2KHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFormatProperties2");
        userData.gpdp2.pfnGetPhysicalDeviceQueueFamilyProperties2 =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties2");
    }

    // If not successful, try to load KHR variant
    if (userData.gpdp2.pfnGetPhysicalDeviceFeatures2 == nullptr) {
        userData.gpdp2.pfnGetPhysicalDeviceFeatures2 =
            (PFN_vkGetPhysicalDeviceFeatures2KHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR");
        userData.gpdp2.pfnGetPhysicalDeviceProperties2 =
            (PFN_vkGetPhysicalDeviceProperties2KHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR");
        userData.gpdp2.pfnGetPhysicalDeviceFormatProperties2 =
            (PFN_vkGetPhysicalDeviceFormatProperties2KHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFormatProperties2KHR");
        userData.gpdp2.pfnGetPhysicalDeviceQueueFamilyProperties2 =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties2KHR");
    }

    if (userData.gpdp2.pfnGetPhysicalDeviceFeatures2 == nullptr ||
        userData.gpdp2.pfnGetPhysicalDeviceProperties2 == nullptr ||
        userData.gpdp2.pfnGetPhysicalDeviceFormatProperties2 == nullptr ||
        userData.gpdp2.pfnGetPhysicalDeviceQueueFamilyProperties2 == nullptr) {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

#ifdef VK_KHR_video_queue
    PFN_vkGetInstanceProcAddr gipa = vp.singleton ? vkGetInstanceProcAddr : vp.GetInstanceProcAddr;
    userData.video.pfnGetPhysicalDeviceVideoCapabilitiesKHR =
        (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)gipa(instance, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
    userData.video.pfnGetPhysicalDeviceVideoFormatPropertiesKHR =
        (PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR)gipa(instance, "vkGetPhysicalDeviceVideoFormatPropertiesKHR");
#endif  // VK_KHR_video_queue

    bool supported = true;

    const std::vector<VpProfileProperties>& gathered_profiles = detail::GatherProfiles(*pProfile);

    for (std::size_t profile_index = 0, profile_count = gathered_profiles.size(); profile_index < profile_count; ++profile_index) {
        const char* profile_name = gathered_profiles[profile_index].profileName;

        const detail::VpProfileDesc* profile_desc = detail::vpGetProfileDesc(profile_name);
        if (profile_desc == nullptr) {
            return VK_ERROR_UNKNOWN;
        }

        bool supported_profile = true;

        if (profile_desc->props.specVersion < gathered_profiles[profile_index].specVersion) {
            supported_profile = false;
        }

        VpBlockProperties block{gathered_profiles[profile_index], profile_desc->minApiVersion};

        {
            VkPhysicalDeviceProperties2KHR properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR };
            userData.gpdp2.pfnGetPhysicalDeviceProperties2(physicalDevice, &properties2);
            if (!detail::vpCheckVersion(properties2.properties.apiVersion, profile_desc->minApiVersion)) {
                supported_profile = false;
            }
        }

        for (uint32_t required_capability_index = 0; required_capability_index < profile_desc->requiredCapabilityCount; ++required_capability_index) {
            const detail::VpCapabilitiesDesc* required_capabilities = &profile_desc->pRequiredCapabilities[required_capability_index];

            bool supported_block = false;

            for (uint32_t variant_index = 0; variant_index < required_capabilities->variantCount; ++variant_index) {
                const detail::VpVariantDesc& variant_desc = required_capabilities->pVariants[variant_index];

                bool supported_variant = true;

                for (uint32_t ext_index = 0; ext_index < variant_desc.deviceExtensionCount; ++ext_index) {
                    const char *requested_extension = variant_desc.pDeviceExtensions[ext_index].extensionName;
                    if (!detail::CheckExtension(supported_device_extensions.data(), supported_device_extensions.size(), requested_extension)) {
                        supported_variant = false;
                    }
                }

                userData.variant = &variant_desc;

                VkPhysicalDeviceFeatures2KHR features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR };
                userData.variant->chainers.pfnFeature(
                    static_cast<VkBaseOutStructure*>(static_cast<void*>(&features)), &userData,
                    [](VkBaseOutStructure* p, void* pUser) {
                        UserData* pUserData = static_cast<UserData*>(pUser);
                        pUserData->gpdp2.pfnGetPhysicalDeviceFeatures2(
                            pUserData->physicalDevice,
                            static_cast<VkPhysicalDeviceFeatures2KHR*>(static_cast<void*>(p)));

                        pUserData->supported = true;
                        while (p != nullptr) {
                            if (!pUserData->variant->feature.pfnComparator(p)) {
                                pUserData->supported = false;
                            }
                            p = p->pNext;
                        }
                    }
                );
                if (!userData.supported) {
                    supported_variant = false;
                }

                VkPhysicalDeviceProperties2KHR device_properties2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR };
                userData.variant->chainers.pfnProperty(
                    static_cast<VkBaseOutStructure*>(static_cast<void*>(&device_properties2)), &userData,
                    [](VkBaseOutStructure* p, void* pUser) {
                        UserData* pUserData = static_cast<UserData*>(pUser);
                        pUserData->gpdp2.pfnGetPhysicalDeviceProperties2(
                            pUserData->physicalDevice,
                            static_cast<VkPhysicalDeviceProperties2KHR*>(static_cast<void*>(p)));

                        pUserData->supported = true;
                        while (p != nullptr) {
                            if (!pUserData->variant->property.pfnComparator(p)) {
                                pUserData->supported = false;
                            }
                            p = p->pNext;
                        }
                    }
                );
                if (!userData.supported) {
                    supported_variant = false;
                }

                if (userData.variant->queueFamilyCount > 0) {
                    uint32_t queue_family_count = 0;
                    userData.gpdp2.pfnGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, &queue_family_count, nullptr);
                    std::vector<VkQueueFamilyProperties2KHR> queueFamilyProps(queue_family_count, { VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2_KHR });
                    userData.variant->chainers.pfnQueueFamily(
                        queue_family_count, static_cast<VkBaseOutStructure*>(static_cast<void*>(queueFamilyProps.data())), &userData,
                        [](uint32_t queue_family_count, VkBaseOutStructure* pBaseArray, void* pUser) {
                            UserData* pUserData = static_cast<UserData*>(pUser);
                            VkQueueFamilyProperties2KHR* pArray = static_cast<VkQueueFamilyProperties2KHR*>(static_cast<void*>(pBaseArray));
                            pUserData->gpdp2.pfnGetPhysicalDeviceQueueFamilyProperties2(pUserData->physicalDevice, &queue_family_count, pArray);
                            pUserData->supported = true;
                            for (uint32_t profile_qf_idx = 0; profile_qf_idx < pUserData->variant->queueFamilyCount; ++profile_qf_idx) {
                                bool found_matching = false;
                                for (uint32_t queue_family_index = 0; queue_family_index < queue_family_count; ++queue_family_index) {
                                    bool this_matches = true;
                                    VkBaseOutStructure* p = static_cast<VkBaseOutStructure*>(static_cast<void*>(&pArray[queue_family_index]));
                                    while (p != nullptr) {
                                        if (!pUserData->variant->pQueueFamilies[profile_qf_idx].pfnComparator(p)) {
                                            this_matches = false;
                                        }
                                        p = p->pNext;
                                    }
                                    if (this_matches) {
                                        found_matching = true;
                                        break;
                                    }
                                }
                                if (!found_matching) {
                                    pUserData->supported = false;
                                    break;
                                }
                            }
                        }
                    );
                    if (!userData.supported) {
                        supported_variant = false;
                    }
                }

                for (uint32_t format_index = 0; format_index < userData.variant->formatCount && supported_variant; ++format_index) {
                    userData.index = format_index;
                    VkFormatProperties2KHR format_properties2{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR };
                    userData.variant->chainers.pfnFormat(
                        static_cast<VkBaseOutStructure*>(static_cast<void*>(&format_properties2)), &userData,
                        [](VkBaseOutStructure* p, void* pUser) {
                            UserData* pUserData = static_cast<UserData*>(pUser);
                            pUserData->gpdp2.pfnGetPhysicalDeviceFormatProperties2(
                                pUserData->physicalDevice,
                                pUserData->variant->pFormats[pUserData->index].format,
                                static_cast<VkFormatProperties2KHR*>(static_cast<void*>(p)));
                            pUserData->supported = true;
                            while (p != nullptr) {
                                if (!pUserData->variant->pFormats[pUserData->index].pfnComparator(p)) {
                                    pUserData->supported = false;
                                }
                                p = p->pNext;
                            }
                        }
                    );
                    if (!userData.supported) {
                        supported_variant = false;
                    }
                }

#ifdef VK_KHR_video_queue
                if (userData.variant->videoProfileCount > 0) {
                    VkVideoProfileListInfoKHR profile_list{ VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR };
                    profile_list.profileCount = 1;
                    profile_list.pProfiles = &userData.video.profileInfo;
                    userData.video.formatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
                    userData.video.formatInfo.pNext = &profile_list;

                    if (userData.video.pfnGetPhysicalDeviceVideoCapabilitiesKHR != nullptr &&
                        userData.video.pfnGetPhysicalDeviceVideoFormatPropertiesKHR != nullptr) {
                        for (uint32_t video_profile_index = 0; video_profile_index < userData.variant->videoProfileCount; ++video_profile_index) {
                            userData.video.profileInfo = VkVideoProfileInfoKHR{ VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR };
                            userData.video.pProfileDesc = &userData.variant->pVideoProfiles[video_profile_index];
                            userData.supported = true;
                            userData.video.matchingProfiles = 0;

                            detail::vpForEachMatchingVideoProfiles(&userData.video.profileInfo, &userData,
                                [](VkBaseOutStructure* p, void* pUser) {
                                    UserData* pUserData = static_cast<UserData*>(pUser);
                                    while (p != nullptr) {
                                        if (!pUserData->video.pProfileDesc->info.pfnComparator(p)) {
                                            return;
                                        }
                                        p = p->pNext;
                                    }

                                    pUserData->video.supportedProfile = true;

                                    VkVideoCapabilitiesKHR capabilities{ VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR };
                                    pUserData->video.pProfileDesc->chainers.pfnCapability(
                                        static_cast<VkBaseOutStructure*>(static_cast<void*>(&capabilities)), pUserData,
                                        [](VkBaseOutStructure* p, void* pUser) {
                                            UserData* pUserData = static_cast<UserData*>(pUser);
                                            VkResult result = pUserData->video.pfnGetPhysicalDeviceVideoCapabilitiesKHR(
                                                pUserData->physicalDevice,
                                                &pUserData->video.profileInfo,
                                                static_cast<VkVideoCapabilitiesKHR*>(static_cast<void*>(p)));
                                            if (result != VK_SUCCESS) {
                                                pUserData->video.supportedProfile = false;
                                                return;
                                            }
                                            while (p != nullptr) {
                                                if (!pUserData->video.pProfileDesc->capability.pfnComparator(p)) {
                                                    pUserData->supported = false;
                                                }
                                                p = p->pNext;
                                            }
                                        }
                                    );

                                    if (pUserData->video.supportedProfile) {
                                        pUserData->video.matchingProfiles++;
                                    } else {
                                        return;
                                    }

                                    std::vector<VkVideoFormatPropertiesKHR> format_props;
                                    for (uint32_t format_index = 0; format_index < pUserData->video.pProfileDesc->formatCount; ++format_index) {
                                        pUserData->index = format_index;
                                        {
                                            VkVideoFormatPropertiesKHR tmp_props{ VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR };
                                            pUserData->video.pProfileDesc->pFormats[format_index].pfnFiller(static_cast<VkBaseOutStructure*>(static_cast<void*>(&tmp_props)));
                                            pUserData->video.formatInfo.imageUsage = tmp_props.imageUsageFlags;
                                        }

                                        uint32_t format_count = 0;
                                        pUserData->video.pfnGetPhysicalDeviceVideoFormatPropertiesKHR(pUserData->physicalDevice, &pUserData->video.formatInfo, &format_count, nullptr);
                                        format_props.resize(format_count, { VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR });
                                        pUserData->video.pProfileDesc->chainers.pfnFormat(
                                            format_count, static_cast<VkBaseOutStructure*>(static_cast<void*>(format_props.data())), pUserData,
                                            [](uint32_t format_count, VkBaseOutStructure* pBaseArray, void* pUser) {
                                                UserData* pUserData = static_cast<UserData*>(pUser);
                                                VkVideoFormatPropertiesKHR* pArray = static_cast<VkVideoFormatPropertiesKHR*>(static_cast<void*>(pBaseArray));
                                                pUserData->video.pfnGetPhysicalDeviceVideoFormatPropertiesKHR(pUserData->physicalDevice, &pUserData->video.formatInfo, &format_count, pArray);
                                                bool found_matching = false;
                                                for (uint32_t i = 0; i < format_count; ++i) {
                                                    bool this_matches = true;
                                                    VkBaseOutStructure* p = static_cast<VkBaseOutStructure*>(static_cast<void*>(&pArray[i]));
                                                    while (p != nullptr) {
                                                        if (!pUserData->video.pProfileDesc->pFormats[pUserData->index].pfnComparator(p)) {
                                                            this_matches = false;
                                                        }
                                                        p = p->pNext;
                                                    }
                                                    if (this_matches) {
                                                        found_matching = true;
                                                        break;
                                                    }
                                                }
                                                if (!found_matching) {
                                                    pUserData->supported = false;
                                                }
                                            }
                                        );
                                    }
                                }
                            );
                            if (!userData.supported || userData.video.matchingProfiles == 0) {
                                supported_variant = false;
                            }
                        }
                    } else {
                        supported_variant = false;
                    }
                }
#endif  // VK_KHR_video_queue

                memcpy(block.blockName, variant_desc.blockName, VP_MAX_PROFILE_NAME_SIZE * sizeof(char));
                if (supported_variant) {
                    supported_blocks.push_back(block);
                    supported_block = true;
                    break;
                } else {
                    unsupported_blocks.push_back(block);
                }
            }

            if (!supported_block) {
                supported_profile = false;
            }
        }

        if (!supported_profile) {
            supported = false;
        }
    }

    const std::vector<VpBlockProperties>& blocks = supported ? supported_blocks : unsupported_blocks;

    if (pProperties == nullptr) {
        *pPropertyCount = static_cast<uint32_t>(blocks.size());
    } else {
        if (*pPropertyCount < static_cast<uint32_t>(blocks.size())) {
            result = VK_INCOMPLETE;
        } else {
            *pPropertyCount = static_cast<uint32_t>(blocks.size());
        }
        for (uint32_t i = 0, n = static_cast<uint32_t>(blocks.size()); i < n; ++i) {
            pProperties[i] = blocks[i];
        }
    }

    *pSupported = supported ? VK_TRUE : VK_FALSE;
    return VK_SUCCESS;
}

VPAPI_ATTR VkResult vpGetPhysicalDeviceProfileSupport(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    VkInstance                                  instance,
    VkPhysicalDevice                            physicalDevice,
    const VpProfileProperties*                  pProfile,
    VkBool32 *pSupported) {
    uint32_t count = 0;

    return vpGetPhysicalDeviceProfileVariantsSupport(
#ifdef VP_USE_OBJECT
        capabilities,
#endif//VP_USE_OBJECT
        instance, physicalDevice, pProfile, pSupported, &count, nullptr);
}

VPAPI_ATTR VkResult vpCreateDevice(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    VkPhysicalDevice                            physicalDevice,
    const VpDeviceCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDevice*                                   pDevice) {
#ifdef VP_USE_OBJECT
    const VpCapabilities_T& vp = capabilities == nullptr ? VpCapabilities_T::Get() : *capabilities;
#else
    const VpCapabilities_T& vp = VpCapabilities_T::Get();
#endif//VP_USE_OBJECT

    if (physicalDevice == VK_NULL_HANDLE || pCreateInfo == nullptr || pDevice == nullptr) {
        return vp.CreateDevice(physicalDevice, pCreateInfo == nullptr ? nullptr : pCreateInfo->pCreateInfo, pAllocator, pDevice);
    }

    const std::vector<VpBlockProperties>& blocks = detail::GatherBlocks(
        pCreateInfo->enabledFullProfileCount, pCreateInfo->pEnabledFullProfiles,
        pCreateInfo->enabledProfileBlockCount, pCreateInfo->pEnabledProfileBlocks);

    std::unique_ptr<detail::FeaturesChain> chain = std::make_unique<detail::FeaturesChain>();
    std::vector<VkStructureType> structureTypes;

    std::vector<const char*> extensions;
    for (std::uint32_t ext_index = 0, ext_count = pCreateInfo->pCreateInfo->enabledExtensionCount; ext_index < ext_count; ++ext_index) {
        extensions.push_back(pCreateInfo->pCreateInfo->ppEnabledExtensionNames[ext_index]);
    }

    for (std::size_t block_index = 0, block_count = blocks.size(); block_index < block_count; ++block_index) {
        const detail::VpProfileDesc* pProfileDesc = detail::vpGetProfileDesc(blocks[block_index].profiles.profileName);
        if (pProfileDesc == nullptr) return VK_ERROR_UNKNOWN;

        for (std::size_t caps_index = 0, caps_count = pProfileDesc->requiredCapabilityCount; caps_index < caps_count; ++caps_index) {
            const detail::VpCapabilitiesDesc* pCapsDesc = &pProfileDesc->pRequiredCapabilities[caps_index];

            for (std::size_t variant_index = 0, variant_count = pCapsDesc->variantCount; variant_index < variant_count; ++variant_index) {
                const detail::VpVariantDesc* variant = &pCapsDesc->pVariants[variant_index];

                if (strcmp(blocks[block_index].blockName, "") != 0) {
                    if (strcmp(variant->blockName, blocks[block_index].blockName) != 0) {
                        continue;
                    }
                }

                for (uint32_t type_index = 0; type_index < variant->featureStructTypeCount; ++type_index) {
                    const VkStructureType type = variant->pFeatureStructTypes[type_index];
                    if (std::find(structureTypes.begin(), structureTypes.end(), type) == std::end(structureTypes)) {
                        structureTypes.push_back(type);
                    }
                }

                detail::GetExtensions(variant->deviceExtensionCount, variant->pDeviceExtensions, extensions);
            }
        }
    }

    VkBaseOutStructure* pNext = static_cast<VkBaseOutStructure*>(const_cast<void*>(pCreateInfo->pCreateInfo->pNext));
    detail::GatherStructureTypes(structureTypes, pNext);

    chain->Build(structureTypes);

    VkPhysicalDeviceFeatures2KHR* pFeatures = &chain->requiredFeaturesChain;
    if (pCreateInfo->pCreateInfo->pEnabledFeatures) {
        pFeatures->features = *pCreateInfo->pCreateInfo->pEnabledFeatures;
    }

    for (std::size_t block_index = 0, block_count = blocks.size(); block_index < block_count; ++block_index) {
        const detail::VpProfileDesc* pProfileDesc = detail::vpGetProfileDesc(blocks[block_index].profiles.profileName);
        if (pProfileDesc == nullptr) {
            return VK_ERROR_UNKNOWN;
        }

        for (std::size_t caps_index = 0, caps_count = pProfileDesc->requiredCapabilityCount; caps_index < caps_count; ++caps_index) {
            const detail::VpCapabilitiesDesc* pCapsDesc = &pProfileDesc->pRequiredCapabilities[caps_index];

            for (std::size_t variant_index = 0, variant_count = pCapsDesc->variantCount; variant_index < variant_count; ++variant_index) {
                const detail::VpVariantDesc* variant = &pCapsDesc->pVariants[variant_index];

                VkBaseOutStructure* base_ptr = reinterpret_cast<VkBaseOutStructure*>(pFeatures);
                if (variant->feature.pfnFiller != nullptr) {
                    while (base_ptr != nullptr) {
                        variant->feature.pfnFiller(base_ptr);
                        base_ptr = base_ptr->pNext;
                    }
                }
            }
        }
    }

    chain->ApplyFeatures(pCreateInfo);

    if (pCreateInfo->flags & VP_DEVICE_CREATE_DISABLE_ROBUST_BUFFER_ACCESS_BIT) {
        pFeatures->features.robustBufferAccess = VK_FALSE;
    }

    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = &chain->requiredFeaturesChain;
    createInfo.queueCreateInfoCount = pCreateInfo->pCreateInfo->queueCreateInfoCount;
    createInfo.pQueueCreateInfos = pCreateInfo->pCreateInfo->pQueueCreateInfos;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    return vp.CreateDevice(physicalDevice, &createInfo, pAllocator, pDevice);
}

VPAPI_ATTR VkResult vpGetProfileInstanceExtensionProperties(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties) {
    return detail::vpGetProfileExtensionProperties(
#ifdef VP_USE_OBJECT
        capabilities,
#endif//VP_USE_OBJECT
        pProfile, pBlockName, detail::EXTENSION_INSTANCE, pPropertyCount, pProperties);
}

VPAPI_ATTR VkResult vpGetProfileDeviceExtensionProperties(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties) {
    return detail::vpGetProfileExtensionProperties(
#ifdef VP_USE_OBJECT
        capabilities,
#endif//VP_USE_OBJECT
        pProfile, pBlockName, detail::EXTENSION_DEVICE, pPropertyCount, pProperties);
}

VPAPI_ATTR VkResult vpGetProfileFeatures(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    void*                                       pNext) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    VkResult result = pBlockName == nullptr ? VK_SUCCESS : VK_INCOMPLETE;

    const std::vector<VpProfileProperties>& gathered_profiles = detail::GatherProfiles(*pProfile);

    for (std::size_t profile_index = 0, profile_count = gathered_profiles.size(); profile_index < profile_count; ++profile_index) {
        const detail::VpProfileDesc* profile_desc = detail::vpGetProfileDesc(gathered_profiles[profile_index].profileName);
        if (profile_desc == nullptr) return VK_ERROR_UNKNOWN;

        for (uint32_t capability_index = 0; capability_index < profile_desc->requiredCapabilityCount; ++capability_index) {
            const detail::VpCapabilitiesDesc& cap_desc = profile_desc->pRequiredCapabilities[capability_index];

            for (uint32_t variant_index = 0; variant_index < cap_desc.variantCount; ++variant_index) {
                const detail::VpVariantDesc& variant = cap_desc.pVariants[variant_index];
                if (pBlockName != nullptr) {
                    if (strcmp(variant.blockName, pBlockName) != 0) {
                        continue;
                    }
                    result = VK_SUCCESS;
                }

                if (variant.feature.pfnFiller == nullptr) continue;

                VkBaseOutStructure* p = static_cast<VkBaseOutStructure*>(pNext);
                while (p != nullptr) {
                    variant.feature.pfnFiller(p);
                    p = p->pNext;
                }
            }
        }
    }

    return result;
}

VPAPI_ATTR VkResult vpGetProfileProperties(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    void*                                       pNext) {
    VkResult result = pBlockName == nullptr ? VK_SUCCESS : VK_INCOMPLETE;

    VkBool32 multiple_variants = VK_FALSE;
    if (vpHasMultipleVariantsProfile(
#ifdef VP_USE_OBJECT
        capabilities,
#endif//VP_USE_OBJECT
        pProfile,
        &multiple_variants) == VK_ERROR_UNKNOWN) {
        return VK_ERROR_UNKNOWN;
    }
    if (multiple_variants == VK_TRUE && pBlockName == nullptr) {
        return VK_ERROR_UNKNOWN;
    }

    const std::vector<VpProfileProperties>& gathered_profiles = detail::GatherProfiles(*pProfile);

    for (std::size_t profile_index = 0, profile_count = gathered_profiles.size(); profile_index < profile_count; ++profile_index) {
        const detail::VpProfileDesc* profile_desc = detail::vpGetProfileDesc(gathered_profiles[profile_index].profileName);
        if (profile_desc == nullptr) return VK_ERROR_UNKNOWN;

        for (uint32_t capability_index = 0; capability_index < profile_desc->requiredCapabilityCount; ++capability_index) {
            const detail::VpCapabilitiesDesc& cap_desc = profile_desc->pRequiredCapabilities[capability_index];

            for (uint32_t variant_index = 0; variant_index < cap_desc.variantCount; ++variant_index) {
                const detail::VpVariantDesc& variant = cap_desc.pVariants[variant_index];
                if (pBlockName != nullptr) {
                    if (strcmp(variant.blockName, pBlockName) != 0) {
                        continue;
                    }
                    result = VK_SUCCESS;
                }

                if (variant.property.pfnFiller == nullptr) continue;

                VkBaseOutStructure* p = static_cast<VkBaseOutStructure*>(pNext);
                while (p != nullptr) {
                    variant.property.pfnFiller(p);
                    p = p->pNext;
                }
            }
        }
    }

    return result;
}

VPAPI_ATTR VkResult vpGetProfileQueueFamilyProperties(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t*                                   pPropertyCount,
    VkQueueFamilyProperties2KHR*                pProperties) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    if (pPropertyCount == nullptr) return VK_ERROR_UNKNOWN;

    VkResult result = pBlockName == nullptr ? VK_SUCCESS : VK_INCOMPLETE;

    const std::vector<VpProfileProperties>& gathered_profiles = detail::GatherProfiles(*pProfile);

    uint32_t total_queue_family_count = 0;

    for (std::size_t profile_index = 0, profile_count = gathered_profiles.size(); profile_index < profile_count; ++profile_index) {
        const detail::VpProfileDesc* profile_desc = detail::vpGetProfileDesc(gathered_profiles[profile_index].profileName);
        if (profile_desc == nullptr) return VK_ERROR_UNKNOWN;

        for (uint32_t capability_index = 0; capability_index < profile_desc->requiredCapabilityCount; ++capability_index) {
            const detail::VpCapabilitiesDesc& cap_desc = profile_desc->pRequiredCapabilities[capability_index];

            for (uint32_t variant_index = 0; variant_index < cap_desc.variantCount; ++variant_index) {
                const detail::VpVariantDesc& variant = cap_desc.pVariants[variant_index];
                if (pBlockName != nullptr) {
                    if (strcmp(variant.blockName, pBlockName) != 0) {
                        continue;
                    }
                    result = VK_SUCCESS;
                }

                if (pProperties != nullptr) {
                    for (uint32_t i = 0; i < variant.queueFamilyCount; ++i) {
                        if (total_queue_family_count < *pPropertyCount) {
                            if (variant.pQueueFamilies[i].pfnFiller == nullptr) continue;

                            VkBaseOutStructure* p = reinterpret_cast<VkBaseOutStructure*>(pProperties);
                            while (p != nullptr) {
                                variant.pQueueFamilies[i].pfnFiller(p);
                                p = p->pNext;
                            }

                            total_queue_family_count++;
                            pProperties++;
                        } else {
                            result = VK_INCOMPLETE;
                            break;
                        }
                    }
                } else {
                    total_queue_family_count += variant.queueFamilyCount;
                }
            }
        }
    }

    *pPropertyCount = total_queue_family_count;
    return result;
}

VPAPI_ATTR VkResult vpGetProfileFormats(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t*                                   pFormatCount,
    VkFormat*                                   pFormats) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    VkResult result = pBlockName == nullptr ? VK_SUCCESS : VK_INCOMPLETE;

    std::vector<VkFormat> results;

    const std::vector<VpProfileProperties>& gathered_profiles = detail::GatherProfiles(*pProfile);

    for (std::size_t profile_index = 0, profile_count = gathered_profiles.size(); profile_index < profile_count; ++profile_index) {
        const detail::VpProfileDesc* profile_desc = detail::vpGetProfileDesc(gathered_profiles[profile_index].profileName);
        if (profile_desc == nullptr) return VK_ERROR_UNKNOWN;

        for (uint32_t capability_index = 0; capability_index < profile_desc->requiredCapabilityCount; ++capability_index) {
            const detail::VpCapabilitiesDesc& cap_desc = profile_desc->pRequiredCapabilities[capability_index];

            for (uint32_t variant_index = 0; variant_index < cap_desc.variantCount; ++variant_index) {
                const detail::VpVariantDesc& variant = cap_desc.pVariants[variant_index];
                if (pBlockName != nullptr) {
                    if (strcmp(variant.blockName, pBlockName) != 0) {
                        continue;
                    }
                    result = VK_SUCCESS;
                }

                for (uint32_t format_index = 0; format_index < variant.formatCount; ++format_index) {
                    if (std::find(results.begin(), results.end(), variant.pFormats[format_index].format) == std::end(results)) {
                        results.push_back(variant.pFormats[format_index].format);
                    }
                }
            }
        }
    }

    const uint32_t count = static_cast<uint32_t>(results.size());

    if (pFormats == nullptr) {
        *pFormatCount = count;
    } else {
        if (*pFormatCount < count) {
            result = VK_INCOMPLETE;
        } else {
            *pFormatCount = count;
        }

        if (*pFormatCount > 0) {
            memcpy(pFormats, &results[0], *pFormatCount * sizeof(VkFormat));
        }
    }
    return result;
}

VPAPI_ATTR VkResult vpGetProfileFormatProperties(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    VkFormat                                    format,
    void*                                       pNext) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    VkResult result = pBlockName == nullptr ? VK_SUCCESS : VK_INCOMPLETE;

    const std::vector<VpProfileProperties>& gathered_profiles = detail::GatherProfiles(*pProfile);

    for (std::size_t profile_index = 0, profile_count = gathered_profiles.size(); profile_index < profile_count; ++profile_index) {
        const char* profile_name = gathered_profiles[profile_index].profileName;

        const detail::VpProfileDesc* pProfileDesc = detail::vpGetProfileDesc(profile_name);
        if (pProfileDesc == nullptr) return VK_ERROR_UNKNOWN;

        for (uint32_t required_capability_index = 0; required_capability_index < pProfileDesc->requiredCapabilityCount;
                ++required_capability_index) {
            const detail::VpCapabilitiesDesc& required_capabilities = pProfileDesc->pRequiredCapabilities[required_capability_index];

            for (uint32_t required_variant_index = 0; required_variant_index < required_capabilities.variantCount; ++required_variant_index) {
                const detail::VpVariantDesc& variant = required_capabilities.pVariants[required_variant_index];
                if (pBlockName != nullptr) {
                    if (strcmp(variant.blockName, pBlockName) != 0) {
                        continue;
                    }
                    result = VK_SUCCESS;
                }

                for (uint32_t format_index = 0; format_index < variant.formatCount; ++format_index) {
                    if (variant.pFormats[format_index].format != format) {
                        continue;
                    }

                    VkBaseOutStructure* base_ptr = static_cast<VkBaseOutStructure*>(static_cast<void*>(pNext));
                    while (base_ptr != nullptr) {
                        variant.pFormats[format_index].pfnFiller(base_ptr);
                        base_ptr = base_ptr->pNext;
                    }
#if defined(VK_VERSION_1_3) || defined(VK_KHR_format_feature_flags2)
                    VkFormatProperties2KHR* fp2 = static_cast<VkFormatProperties2KHR*>(
                        detail::vpGetStructure(pNext, VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR));
                    VkFormatProperties3KHR* fp3 = static_cast<VkFormatProperties3KHR*>(
                        detail::vpGetStructure(pNext, VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR));
                    if (fp3 != nullptr) {
                        VkFormatProperties2KHR fp{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR };
                        variant.pFormats[format_index].pfnFiller(static_cast<VkBaseOutStructure*>(static_cast<void*>(&fp)));
                        fp3->linearTilingFeatures |= static_cast<VkFormatFeatureFlags2KHR>(fp3->linearTilingFeatures | fp.formatProperties.linearTilingFeatures);
                        fp3->optimalTilingFeatures |= static_cast<VkFormatFeatureFlags2KHR>(fp3->optimalTilingFeatures | fp.formatProperties.optimalTilingFeatures);
                        fp3->bufferFeatures |= static_cast<VkFormatFeatureFlags2KHR>(fp3->bufferFeatures | fp.formatProperties.bufferFeatures);
                    }
                    if (fp2 != nullptr) {
                        VkFormatProperties3KHR fp{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR };
                        variant.pFormats[format_index].pfnFiller(static_cast<VkBaseOutStructure*>(static_cast<void*>(&fp)));
                        fp2->formatProperties.linearTilingFeatures |= static_cast<VkFormatFeatureFlags>(fp2->formatProperties.linearTilingFeatures | fp.linearTilingFeatures);
                        fp2->formatProperties.optimalTilingFeatures |= static_cast<VkFormatFeatureFlags>(fp2->formatProperties.optimalTilingFeatures | fp.optimalTilingFeatures);
                        fp2->formatProperties.bufferFeatures |= static_cast<VkFormatFeatureFlags>(fp2->formatProperties.bufferFeatures | fp.bufferFeatures);
                    }
#endif
                }
            }
        }
    }

    return result;
}

VPAPI_ATTR VkResult vpGetProfileFeatureStructureTypes(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t*                                   pStructureTypeCount,
    VkStructureType*                            pStructureTypes) {
    return detail::vpGetProfileStructureTypes(
#ifdef VP_USE_OBJECT
        capabilities,
#endif//VP_USE_OBJECT
        pProfile, pBlockName, detail::STRUCTURE_FEATURE, pStructureTypeCount, pStructureTypes);
}

VPAPI_ATTR VkResult vpGetProfilePropertyStructureTypes(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t*                                   pStructureTypeCount,
    VkStructureType*                            pStructureTypes) {
    return detail::vpGetProfileStructureTypes(
#ifdef VP_USE_OBJECT
        capabilities,
#endif//VP_USE_OBJECT
        pProfile, pBlockName, detail::STRUCTURE_PROPERTY, pStructureTypeCount, pStructureTypes);
}

VPAPI_ATTR VkResult vpGetProfileQueueFamilyStructureTypes(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t*                                   pStructureTypeCount,
    VkStructureType*                            pStructureTypes) {
    return detail::vpGetProfileStructureTypes(
#ifdef VP_USE_OBJECT
        capabilities,
#endif//VP_USE_OBJECT
        pProfile, pBlockName, detail::STRUCTURE_QUEUE_FAMILY, pStructureTypeCount, pStructureTypes);
}

VPAPI_ATTR VkResult vpGetProfileFormatStructureTypes(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t*                                   pStructureTypeCount,
    VkStructureType*                            pStructureTypes) {
    return detail::vpGetProfileStructureTypes(
#ifdef VP_USE_OBJECT
        capabilities,
#endif//VP_USE_OBJECT
        pProfile, pBlockName, detail::STRUCTURE_FORMAT, pStructureTypeCount, pStructureTypes);
}

#ifdef VK_KHR_video_queue
// Query the list of video profiles specified by the profile
VPAPI_ATTR VkResult vpGetProfileVideoProfiles(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t*                                   pVideoProfileCount,
    VpVideoProfileProperties*                   pVideoProfiles) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT
    if (pVideoProfileCount == nullptr) return VK_ERROR_UNKNOWN;

    VkResult result = pBlockName == nullptr ? VK_SUCCESS : VK_INCOMPLETE;

    uint32_t total_video_profile_count = 0;

    const std::vector<VpProfileProperties>& gathered_profiles = detail::GatherProfiles(*pProfile);

    for (std::size_t profile_index = 0, profile_count = gathered_profiles.size(); profile_index < profile_count; ++profile_index) {
        const detail::VpProfileDesc* profile_desc = detail::vpGetProfileDesc(gathered_profiles[profile_index].profileName);
        if (profile_desc == nullptr) return VK_ERROR_UNKNOWN;

        for (uint32_t capability_index = 0; capability_index < profile_desc->requiredCapabilityCount; ++capability_index) {
            const detail::VpCapabilitiesDesc& cap_desc = profile_desc->pRequiredCapabilities[capability_index];

            for (uint32_t variant_index = 0; variant_index < cap_desc.variantCount; ++variant_index) {
                const detail::VpVariantDesc& variant = cap_desc.pVariants[variant_index];
                if (pBlockName != nullptr) {
                    if (strcmp(variant.blockName, pBlockName) != 0) {
                        continue;
                    }
                    result = VK_SUCCESS;
                }

                if (pVideoProfiles != nullptr) {
                    for (uint32_t i = 0; i < variant.videoProfileCount; ++i) {
                        if (total_video_profile_count < *pVideoProfileCount) {
                            *pVideoProfiles = variant.pVideoProfiles[i].properties;
                            total_video_profile_count++;
                            pVideoProfiles++;
                        } else {
                            result = VK_INCOMPLETE;
                            break;
                        }
                    }
                } else {
                    total_video_profile_count += variant.videoProfileCount;
                }
            }
        }
    }

    *pVideoProfileCount = total_video_profile_count;
    return result;
}

VPAPI_ATTR VkResult vpGetProfileVideoProfileInfo(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t                                    videoProfileIndex,
    VkVideoProfileInfoKHR*                      pVideoProfileInfo) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    const detail::VpVideoProfileDesc* pVideoProfileDesc = nullptr;
    VkResult result = detail::vpGetProfileVideoProfileDesc(pProfile, pBlockName, videoProfileIndex, &pVideoProfileDesc);

    if (pVideoProfileDesc != nullptr) {
        VkBaseOutStructure* p = reinterpret_cast<VkBaseOutStructure*>(pVideoProfileInfo);
        while (p != nullptr) {
            pVideoProfileDesc->info.pfnFiller(p);
            p = p->pNext;
        }
    }

    return result;
}

VPAPI_ATTR VkResult vpGetProfileVideoCapabilities(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t                                    videoProfileIndex,
    void*                                       pNext) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    const detail::VpVideoProfileDesc* pVideoProfileDesc = nullptr;
    VkResult result = detail::vpGetProfileVideoProfileDesc(pProfile, pBlockName, videoProfileIndex, &pVideoProfileDesc);

    if (pVideoProfileDesc != nullptr) {
        VkBaseOutStructure* p = reinterpret_cast<VkBaseOutStructure*>(pNext);
        while (p != nullptr) {
            pVideoProfileDesc->capability.pfnFiller(p);
            p = p->pNext;
        }
    }

    return result;
}

VPAPI_ATTR VkResult vpGetProfileVideoFormatProperties(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t                                    videoProfileIndex,
    uint32_t*                                   pPropertyCount,
    VkVideoFormatPropertiesKHR*                 pProperties) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    const detail::VpVideoProfileDesc* pVideoProfileDesc = nullptr;
    VkResult result = detail::vpGetProfileVideoProfileDesc(pProfile, pBlockName, videoProfileIndex, &pVideoProfileDesc);

    uint32_t property_count = 0;
    if (pVideoProfileDesc != nullptr) {
        if (pProperties != nullptr) {
            for (; property_count < pVideoProfileDesc->formatCount; ++property_count) {
                if (property_count < *pPropertyCount) {
                    VkBaseOutStructure* p = reinterpret_cast<VkBaseOutStructure*>(&pProperties[property_count]);
                    while (p != nullptr) {
                        pVideoProfileDesc->pFormats[property_count].pfnFiller(p);
                        p = p->pNext;
                    }
                } else {
                    result = VK_INCOMPLETE;
                    break;
                }
            }
        } else {
            property_count = pVideoProfileDesc->formatCount;
        }
    }

    *pPropertyCount = property_count;
    return result;
}

VPAPI_ATTR VkResult vpGetProfileVideoProfileInfoStructureTypes(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t                                    videoProfileIndex,
    uint32_t*                                   pStructureTypeCount,
    VkStructureType*                            pStructureTypes) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    const detail::VpVideoProfileDesc* pVideoProfileDesc = nullptr;
    VkResult result = detail::vpGetProfileVideoProfileDesc(pProfile, pBlockName, videoProfileIndex, &pVideoProfileDesc);

    if (pVideoProfileDesc != nullptr) {
        if (pStructureTypes != nullptr) {
            if (*pStructureTypeCount < pVideoProfileDesc->infoStructTypeCount) {
                result = VK_INCOMPLETE;
            } else {
                *pStructureTypeCount = pVideoProfileDesc->infoStructTypeCount;
            }
            if (*pStructureTypeCount > 0) {
                memcpy(pStructureTypes, pVideoProfileDesc->pInfoStructTypes, *pStructureTypeCount * sizeof(VkStructureType));
            }
        } else {
            *pStructureTypeCount = pVideoProfileDesc->infoStructTypeCount;
        }
    }

    return result;
}

VPAPI_ATTR VkResult vpGetProfileVideoCapabilityStructureTypes(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t                                    videoProfileIndex,
    uint32_t*                                   pStructureTypeCount,
    VkStructureType*                            pStructureTypes) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    const detail::VpVideoProfileDesc* pVideoProfileDesc = nullptr;
    VkResult result = detail::vpGetProfileVideoProfileDesc(pProfile, pBlockName, videoProfileIndex, &pVideoProfileDesc);

    if (pVideoProfileDesc != nullptr) {
        if (pStructureTypes != nullptr) {
            if (*pStructureTypeCount < pVideoProfileDesc->capabilityStructTypeCount) {
                result = VK_INCOMPLETE;
            } else {
                *pStructureTypeCount = pVideoProfileDesc->capabilityStructTypeCount;
            }
            if (*pStructureTypeCount > 0) {
                memcpy(pStructureTypes, pVideoProfileDesc->pCapabilityStructTypes, *pStructureTypeCount * sizeof(VkStructureType));
            }
        } else {
            *pStructureTypeCount = pVideoProfileDesc->capabilityStructTypeCount;
        }
    }

    return result;
}

VPAPI_ATTR VkResult vpGetProfileVideoFormatStructureTypes(
#ifdef VP_USE_OBJECT
    VpCapabilities                              capabilities,
#endif//VP_USE_OBJECT
    const VpProfileProperties*                  pProfile,
    const char*                                 pBlockName,
    uint32_t                                    videoProfileIndex,
    uint32_t*                                   pStructureTypeCount,
    VkStructureType*                            pStructureTypes) {
#ifdef VP_USE_OBJECT
    (void)capabilities;
#endif//VP_USE_OBJECT

    const detail::VpVideoProfileDesc* pVideoProfileDesc = nullptr;
    VkResult result = detail::vpGetProfileVideoProfileDesc(pProfile, pBlockName, videoProfileIndex, &pVideoProfileDesc);

    if (pVideoProfileDesc != nullptr) {
        if (pStructureTypes != nullptr) {
            if (*pStructureTypeCount < pVideoProfileDesc->formatStructTypeCount) {
                result = VK_INCOMPLETE;
            } else {
                *pStructureTypeCount = pVideoProfileDesc->formatStructTypeCount;
            }
            if (*pStructureTypeCount > 0) {
                memcpy(pStructureTypes, pVideoProfileDesc->pFormatStructTypes, *pStructureTypeCount * sizeof(VkStructureType));
            }
        } else {
            *pStructureTypeCount = pVideoProfileDesc->formatStructTypeCount;
        }
    }

    return result;
}
#endif  // VK_KHR_video_queue
