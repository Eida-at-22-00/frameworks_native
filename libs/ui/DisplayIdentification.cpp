/*
 * Copyright (C) 2018 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "DisplayIdentification"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <ftl/concat.h>
#include <ftl/hash.h>
#include <log/log.h>
#include <ui/DisplayIdentification.h>
#include <ui/Size.h>

namespace android {
namespace {

using byte_view = std::span<const uint8_t>;

constexpr size_t kEdidBlockSize = 128;
constexpr size_t kEdidHeaderLength = 5;

constexpr uint16_t kVirtualEdidManufacturerId = 0xffffu;

std::optional<uint8_t> getEdidDescriptorType(const byte_view& view) {
    if (static_cast<size_t>(view.size()) < kEdidHeaderLength || view[0] || view[1] || view[2] ||
        view[4]) {
        return {};
    }

    return view[3];
}

bool isDetailedTimingDescriptor(const byte_view& view) {
    return view[0] != 0 && view[1] != 0;
}

std::string_view parseEdidText(const byte_view& view) {
    std::string_view text(reinterpret_cast<const char*>(view.data()), view.size());
    text = text.substr(0, text.find('\n'));

    if (!std::all_of(text.begin(), text.end(), ::isprint)) {
        ALOGW("Invalid EDID: ASCII text is not printable.");
        return {};
    }

    return text;
}

// Big-endian 16-bit value encodes three 5-bit letters where A is 0b00001.
template <size_t I>
char getPnpLetter(uint16_t id) {
    static_assert(I < 3);
    const char letter = 'A' + (static_cast<uint8_t>(id >> ((2 - I) * 5)) & 0b00011111) - 1;
    return letter < 'A' || letter > 'Z' ? '\0' : letter;
}

DeviceProductInfo buildDeviceProductInfo(const Edid& edid) {
    DeviceProductInfo info;
    info.name.assign(edid.displayName);
    info.productId = std::to_string(edid.productId);
    info.manufacturerPnpId = edid.pnpId;

    constexpr uint8_t kModelYearFlag = 0xff;
    constexpr uint32_t kYearOffset = 1990;

    const auto year = edid.manufactureOrModelYear + kYearOffset;
    if (edid.manufactureWeek == kModelYearFlag) {
        info.manufactureOrModelDate = DeviceProductInfo::ModelYear{.year = year};
    } else if (edid.manufactureWeek == 0) {
        DeviceProductInfo::ManufactureYear date;
        date.year = year;
        info.manufactureOrModelDate = date;
    } else {
        DeviceProductInfo::ManufactureWeekAndYear date;
        date.year = year;
        date.week = edid.manufactureWeek;
        info.manufactureOrModelDate = date;
    }

    if (edid.cea861Block && edid.cea861Block->hdmiVendorDataBlock) {
        const auto& address = edid.cea861Block->hdmiVendorDataBlock->physicalAddress;
        info.relativeAddress = {address.a, address.b, address.c, address.d};
    }
    return info;
}

Cea861ExtensionBlock parseCea861Block(const byte_view& block) {
    Cea861ExtensionBlock cea861Block;

    constexpr size_t kRevisionNumberOffset = 1;
    cea861Block.revisionNumber = block[kRevisionNumberOffset];

    constexpr size_t kDetailedTimingDescriptorsOffset = 2;
    const size_t dtdStart =
            std::min(kEdidBlockSize, static_cast<size_t>(block[kDetailedTimingDescriptorsOffset]));

    // Parse data blocks.
    for (size_t dataBlockOffset = 4; dataBlockOffset < dtdStart;) {
        const uint8_t header = block[dataBlockOffset];
        const uint8_t tag = header >> 5;
        const size_t bodyLength = header & 0b11111;
        constexpr size_t kDataBlockHeaderSize = 1;
        const size_t dataBlockSize = bodyLength + kDataBlockHeaderSize;

        if (static_cast<size_t>(block.size()) < dataBlockOffset + dataBlockSize) {
            ALOGW("Invalid EDID: CEA 861 data block is truncated.");
            break;
        }

        const byte_view dataBlock(block.data() + dataBlockOffset, dataBlockSize);
        constexpr uint8_t kVendorSpecificDataBlockTag = 0x3;

        if (tag == kVendorSpecificDataBlockTag) {
            const uint32_t ieeeRegistrationId = static_cast<uint32_t>(
                    dataBlock[1] | (dataBlock[2] << 8) | (dataBlock[3] << 16));
            constexpr uint32_t kHdmiIeeeRegistrationId = 0xc03;

            if (ieeeRegistrationId == kHdmiIeeeRegistrationId) {
                const uint8_t a = dataBlock[4] >> 4;
                const uint8_t b = dataBlock[4] & 0b1111;
                const uint8_t c = dataBlock[5] >> 4;
                const uint8_t d = dataBlock[5] & 0b1111;
                cea861Block.hdmiVendorDataBlock =
                        HdmiVendorDataBlock{.physicalAddress = HdmiPhysicalAddress{a, b, c, d}};
            } else {
                ALOGV("Ignoring vendor specific data block for vendor with IEEE OUI %x",
                      ieeeRegistrationId);
            }
        } else {
            ALOGV("Ignoring CEA-861 data block with tag %x", tag);
        }
        dataBlockOffset += bodyLength + kDataBlockHeaderSize;
    }

    return cea861Block;
}

} // namespace

bool isEdid(const DisplayIdentificationData& data) {
    const uint8_t kMagic[] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    return data.size() >= sizeof(kMagic) &&
            std::equal(std::begin(kMagic), std::end(kMagic), data.begin());
}

std::optional<Edid> parseEdid(const DisplayIdentificationData& edid) {
    if (edid.size() < kEdidBlockSize) {
        ALOGW("Invalid EDID: structure is truncated.");
        // Attempt parsing even if EDID is malformed.
    } else {
        ALOGW_IF(std::accumulate(edid.begin(), edid.begin() + kEdidBlockSize,
                                 static_cast<uint8_t>(0)),
                 "Invalid EDID: structure does not checksum.");
    }

    constexpr size_t kManufacturerOffset = 8;
    if (edid.size() < kManufacturerOffset + sizeof(uint16_t)) {
        ALOGE("Invalid EDID: manufacturer ID is truncated.");
        return {};
    }

    // Plug and play ID encoded as big-endian 16-bit value.
    const uint16_t manufacturerId =
            static_cast<uint16_t>((edid[kManufacturerOffset] << 8) | edid[kManufacturerOffset + 1]);

    const auto pnpId = getPnpId(manufacturerId);
    if (!pnpId) {
        ALOGE("Invalid EDID: manufacturer ID is not a valid PnP ID.");
        return {};
    }

    constexpr size_t kProductIdOffset = 10;
    if (edid.size() < kProductIdOffset + sizeof(uint16_t)) {
        ALOGE("Invalid EDID: product ID is truncated.");
        return {};
    }
    const uint16_t productId =
            static_cast<uint16_t>(edid[kProductIdOffset] | (edid[kProductIdOffset + 1] << 8));

    //   Bytes 12-15: display serial number, in little-endian (LSB). This field is
    //   optional and its absence is marked by having all bytes set to 0x00.
    //   Values do not represent ASCII characters.
    constexpr size_t kSerialNumberOffset = 12;
    if (edid.size() < kSerialNumberOffset + sizeof(uint32_t)) {
        ALOGE("Invalid EDID: block zero S/N is truncated.");
        return {};
    }
    const uint32_t blockZeroSerialNumber = edid[kSerialNumberOffset] +
            (edid[kSerialNumberOffset + 1] << 8) + (edid[kSerialNumberOffset + 2] << 16) +
            (edid[kSerialNumberOffset + 3] << 24);
    const auto hashedBlockZeroSNOpt = blockZeroSerialNumber == 0
            ? std::nullopt
            : ftl::stable_hash(std::string_view(std::to_string(blockZeroSerialNumber)));

    constexpr size_t kManufactureWeekOffset = 16;
    if (edid.size() < kManufactureWeekOffset + sizeof(uint8_t)) {
        ALOGE("Invalid EDID: manufacture week is truncated.");
        return {};
    }
    const uint8_t manufactureWeek = edid[kManufactureWeekOffset];
    ALOGW_IF(0x37 <= manufactureWeek && manufactureWeek <= 0xfe,
             "Invalid EDID: week of manufacture cannot be in the range [0x37, 0xfe].");

    constexpr size_t kManufactureYearOffset = 17;
    if (edid.size() < kManufactureYearOffset + sizeof(uint8_t)) {
        ALOGE("Invalid EDID: manufacture year is truncated.");
        return {};
    }
    const uint8_t manufactureOrModelYear = edid[kManufactureYearOffset];
    ALOGW_IF(manufactureOrModelYear <= 0xf,
             "Invalid EDID: model year or manufacture year cannot be in the range [0x0, 0xf].");

    constexpr size_t kMaxHorizontalPhysicalSizeOffset = 21;
    constexpr size_t kMaxVerticalPhysicalSizeOffset = 22;
    if (edid.size() < kMaxVerticalPhysicalSizeOffset + sizeof(uint8_t)) {
        ALOGE("Invalid EDID: display's physical size is truncated.");
        return {};
    }
    ui::Size maxPhysicalSizeInCm(edid[kMaxHorizontalPhysicalSizeOffset],
                                 edid[kMaxVerticalPhysicalSizeOffset]);

    constexpr size_t kDescriptorOffset = 54;
    if (edid.size() < kDescriptorOffset) {
        ALOGE("Invalid EDID: descriptors are missing.");
        return {};
    }

    byte_view view(edid.data(), edid.size());
    view = view.subspan(kDescriptorOffset);

    std::string_view displayName;
    std::string_view descriptorBlockSerialNumber;
    std::optional<uint64_t> hashedDescriptorBlockSNOpt = std::nullopt;
    std::string_view asciiText;
    ui::Size preferredDTDPixelSize;
    ui::Size preferredDTDPhysicalSize;

    constexpr size_t kDescriptorCount = 4;
    constexpr size_t kDescriptorLength = 18;

    for (size_t i = 0; i < kDescriptorCount; i++) {
        if (static_cast<size_t>(view.size()) < kDescriptorLength) {
            break;
        }

        if (const auto type = getEdidDescriptorType(view)) {
            byte_view descriptor(view.data(), kDescriptorLength);
            descriptor = descriptor.subspan(kEdidHeaderLength);

            switch (*type) {
                case 0xfc:
                    displayName = parseEdidText(descriptor);
                    break;
                case 0xfe:
                    asciiText = parseEdidText(descriptor);
                    break;
                case 0xff:
                    descriptorBlockSerialNumber = parseEdidText(descriptor);
                    hashedDescriptorBlockSNOpt = descriptorBlockSerialNumber.empty()
                            ? std::nullopt
                            : ftl::stable_hash(descriptorBlockSerialNumber);
                    break;
            }
        } else if (isDetailedTimingDescriptor(view)) {
            static constexpr size_t kHorizontalPhysicalLsbOffset = 12;
            static constexpr size_t kHorizontalPhysicalMsbOffset = 14;
            static constexpr size_t kVerticalPhysicalLsbOffset = 13;
            static constexpr size_t kVerticalPhysicalMsbOffset = 14;
            const uint32_t hSize =
                    static_cast<uint32_t>(view[kHorizontalPhysicalLsbOffset] |
                                          ((view[kHorizontalPhysicalMsbOffset] >> 4) << 8));
            const uint32_t vSize =
                    static_cast<uint32_t>(view[kVerticalPhysicalLsbOffset] |
                                          ((view[kVerticalPhysicalMsbOffset] & 0b1111) << 8));

            static constexpr size_t kHorizontalPixelLsbOffset = 2;
            static constexpr size_t kHorizontalPixelMsbOffset = 4;
            static constexpr size_t kVerticalPixelLsbOffset = 5;
            static constexpr size_t kVerticalPixelMsbOffset = 7;

            const uint8_t hLsb = view[kHorizontalPixelLsbOffset];
            const uint8_t hMsb = view[kHorizontalPixelMsbOffset];
            const int32_t hPixel = hLsb + ((hMsb & 0xF0) << 4);

            const uint8_t vLsb = view[kVerticalPixelLsbOffset];
            const uint8_t vMsb = view[kVerticalPixelMsbOffset];
            const int32_t vPixel = vLsb + ((vMsb & 0xF0) << 4);

            preferredDTDPixelSize.setWidth(hPixel);
            preferredDTDPixelSize.setHeight(vPixel);
            preferredDTDPhysicalSize.setWidth(hSize);
            preferredDTDPhysicalSize.setHeight(vSize);
        }

        view = view.subspan(kDescriptorLength);
    }

    std::string_view modelString = displayName;

    if (modelString.empty()) {
        ALOGW("Invalid EDID: falling back to serial number due to missing display name.");
        modelString = descriptorBlockSerialNumber;
    }
    if (modelString.empty()) {
        ALOGW("Invalid EDID: falling back to ASCII text due to missing serial number.");
        modelString = asciiText;
    }
    if (modelString.empty()) {
        ALOGE("Invalid EDID: display name and fallback descriptors are missing.");
        return {};
    }

    // Hash model string instead of using product code or (integer) serial number, since the latter
    // have been observed to change on some displays with multiple inputs. Use a stable hash instead
    // of std::hash which is only required to be same within a single execution of a program.
    const uint32_t modelHash = static_cast<uint32_t>(*ftl::stable_hash(modelString));

    // Parse extension blocks.
    std::optional<Cea861ExtensionBlock> cea861Block;
    if (edid.size() < kEdidBlockSize) {
        ALOGW("Invalid EDID: block 0 is truncated.");
    } else {
        constexpr size_t kNumExtensionsOffset = 126;
        const size_t numExtensions = edid[kNumExtensionsOffset];
        view = byte_view(edid.data(), edid.size());
        for (size_t blockNumber = 1; blockNumber <= numExtensions; blockNumber++) {
            view = view.subspan(kEdidBlockSize);
            if (static_cast<size_t>(view.size()) < kEdidBlockSize) {
                ALOGW("Invalid EDID: block %zu is truncated.", blockNumber);
                break;
            }

            const byte_view block(view.data(), kEdidBlockSize);
            ALOGW_IF(std::accumulate(block.begin(), block.end(), static_cast<uint8_t>(0)),
                     "Invalid EDID: block %zu does not checksum.", blockNumber);
            const uint8_t tag = block[0];

            constexpr uint8_t kCea861BlockTag = 0x2;
            if (tag == kCea861BlockTag) {
                cea861Block = parseCea861Block(block);
            } else {
                ALOGV("Ignoring block number %zu with tag %x.", blockNumber, tag);
            }
        }
    }

    DetailedTimingDescriptor preferredDetailedTimingDescriptor{
            .pixelSizeCount = preferredDTDPixelSize,
            .physicalSizeInMm = preferredDTDPhysicalSize,
    };

    return Edid{
            .manufacturerId = manufacturerId,
            .productId = productId,
            .hashedBlockZeroSerialNumberOpt = hashedBlockZeroSNOpt,
            .hashedDescriptorBlockSerialNumberOpt = hashedDescriptorBlockSNOpt,
            .pnpId = *pnpId,
            .modelHash = modelHash,
            .displayName = displayName,
            .manufactureOrModelYear = manufactureOrModelYear,
            .manufactureWeek = manufactureWeek,
            .physicalSizeInCm = maxPhysicalSizeInCm,
            .cea861Block = cea861Block,
            .preferredDetailedTimingDescriptor = preferredDetailedTimingDescriptor,
    };
}

std::optional<PnpId> getPnpId(uint16_t manufacturerId) {
    const char a = getPnpLetter<0>(manufacturerId);
    const char b = getPnpLetter<1>(manufacturerId);
    const char c = getPnpLetter<2>(manufacturerId);
    return a && b && c ? std::make_optional(PnpId{a, b, c}) : std::nullopt;
}

std::optional<DisplayIdentificationInfo> parseDisplayIdentificationData(
        uint8_t port, const DisplayIdentificationData& data) {
    if (data.empty()) {
        ALOGI("Display identification data is empty.");
        return {};
    }

    if (!isEdid(data)) {
        ALOGE("Display identification data has unknown format.");
        return {};
    }

    const auto edid = parseEdid(data);
    if (!edid) {
        return {};
    }

    const auto displayId = PhysicalDisplayId::fromEdid(port, edid->manufacturerId, edid->modelHash);
    return DisplayIdentificationInfo{
            .id = displayId,
            .name = std::string(edid->displayName),
            .port = port,
            .deviceProductInfo = buildDeviceProductInfo(*edid),
            .preferredDetailedTimingDescriptor = edid->preferredDetailedTimingDescriptor,
    };
}

PhysicalDisplayId getVirtualDisplayId(uint32_t id) {
    return PhysicalDisplayId::fromEdid(0, kVirtualEdidManufacturerId, id);
}

PhysicalDisplayId generateEdidDisplayId(const Edid& edid) {
    const ftl::Concat displayDetailsString{edid.manufacturerId,
                                           edid.productId,
                                           ftl::truncated<13>(edid.displayName),
                                           edid.manufactureWeek,
                                           edid.manufactureOrModelYear,
                                           edid.physicalSizeInCm.getWidth(),
                                           edid.physicalSizeInCm.getHeight()};

    // String has to be cropped to 64 characters (at most) for ftl::stable_hash.
    // This is fine as the accuracy or completeness of the above fields is not
    // critical for a ID fabrication.
    const std::optional<uint64_t> hashedDisplayDetailsOpt =
            ftl::stable_hash(std::string_view(displayDetailsString.c_str(), 64));

    // Combine the hashes via bit-shifted XORs.
    const uint64_t id = (hashedDisplayDetailsOpt.value_or(0) << 17) ^
            (edid.hashedBlockZeroSerialNumberOpt.value_or(0) >> 11) ^
            (edid.hashedDescriptorBlockSerialNumberOpt.value_or(0) << 23);

    return PhysicalDisplayId::fromValue(id);
}

} // namespace android
