/*
 * Copyright (C) 2007 The Android Open Source Project
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

#define LOG_TAG "GraphicBuffer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <ui/GraphicBuffer.h>

#include <cutils/atomic.h>

#include <grallocusage/GrallocUsageConversion.h>
#include <sync/sync.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>
#include <utils/Trace.h>

#include <string>

namespace android {

// ===========================================================================
// Buffer and implementation of ANativeWindowBuffer
// ===========================================================================

static uint64_t getUniqueId() {
    static volatile int32_t nextId = 0;
    uint64_t id = static_cast<uint64_t>(getpid()) << 32;
    id |= static_cast<uint32_t>(android_atomic_inc(&nextId));
    return id;
}

static void resolveLegacyByteLayoutFromPlaneLayout(const std::vector<ui::PlaneLayout>& planeLayouts,
                                                   int32_t* outBytesPerPixel,
                                                   int32_t* outBytesPerStride) {
    if (planeLayouts.empty()) return;
    if (outBytesPerPixel) {
        int32_t bitsPerPixel = planeLayouts.front().sampleIncrementInBits;
        for (const auto& planeLayout : planeLayouts) {
            if (bitsPerPixel != planeLayout.sampleIncrementInBits) {
                bitsPerPixel = -1;
            }
        }
        if (bitsPerPixel >= 0 && bitsPerPixel % 8 == 0) {
            *outBytesPerPixel = bitsPerPixel / 8;
        } else {
            *outBytesPerPixel = -1;
        }
    }
    if (outBytesPerStride) {
        int32_t bytesPerStride = planeLayouts.front().strideInBytes;
        for (const auto& planeLayout : planeLayouts) {
            if (bytesPerStride != planeLayout.strideInBytes) {
                bytesPerStride = -1;
            }
        }
        if (bytesPerStride >= 0) {
            *outBytesPerStride = bytesPerStride;
        } else {
            *outBytesPerStride = -1;
        }
    }
}

sp<GraphicBuffer> GraphicBuffer::from(ANativeWindowBuffer* anwb) {
    return static_cast<GraphicBuffer *>(anwb);
}

GraphicBuffer* GraphicBuffer::fromAHardwareBuffer(AHardwareBuffer* buffer) {
    return reinterpret_cast<GraphicBuffer*>(buffer);
}

GraphicBuffer const* GraphicBuffer::fromAHardwareBuffer(AHardwareBuffer const* buffer) {
    return reinterpret_cast<GraphicBuffer const*>(buffer);
}

AHardwareBuffer* GraphicBuffer::toAHardwareBuffer() {
    return reinterpret_cast<AHardwareBuffer*>(this);
}

AHardwareBuffer const* GraphicBuffer::toAHardwareBuffer() const {
    return reinterpret_cast<AHardwareBuffer const*>(this);
}

GraphicBuffer::GraphicBuffer()
    : BASE(), mOwner(ownData), mBufferMapper(GraphicBufferMapper::get()),
      mInitCheck(NO_ERROR), mId(getUniqueId()), mGenerationNumber(0)
{
    width  =
    height =
    stride =
    format =
    usage_deprecated = 0;
    usage  = 0;
    layerCount = 0;
    handle = nullptr;
    mDependencyMonitor.setToken(std::to_string(mId));
}

// deprecated
GraphicBuffer::GraphicBuffer(uint32_t inWidth, uint32_t inHeight,
        PixelFormat inFormat, uint32_t inUsage, std::string requestorName)
    : GraphicBuffer(inWidth, inHeight, inFormat, 1, static_cast<uint64_t>(inUsage), requestorName)
{
}

GraphicBuffer::GraphicBuffer(uint32_t inWidth, uint32_t inHeight, PixelFormat inFormat,
                             uint32_t inLayerCount, uint64_t inUsage, std::string requestorName)
      : GraphicBuffer() {
    mInitCheck = initWithSize(inWidth, inHeight, inFormat, inLayerCount, inUsage,
                              std::move(requestorName));
}

// deprecated
GraphicBuffer::GraphicBuffer(uint32_t inWidth, uint32_t inHeight,
        PixelFormat inFormat, uint32_t inLayerCount, uint32_t inUsage,
        uint32_t inStride, native_handle_t* inHandle, bool keepOwnership)
    : GraphicBuffer(inHandle, keepOwnership ? TAKE_HANDLE : WRAP_HANDLE,
            inWidth, inHeight, inFormat, inLayerCount, static_cast<uint64_t>(inUsage),
            inStride)
{
}

GraphicBuffer::GraphicBuffer(const native_handle_t* inHandle, HandleWrapMethod method,
                             uint32_t inWidth, uint32_t inHeight, PixelFormat inFormat,
                             uint32_t inLayerCount, uint64_t inUsage, uint32_t inStride)
      : GraphicBuffer() {
    mInitCheck = initWithHandle(inHandle, method, inWidth, inHeight, inFormat, inLayerCount,
                                inUsage, inStride);
}

GraphicBuffer::GraphicBuffer(const GraphicBufferAllocator::AllocationRequest& request)
      : GraphicBuffer() {
    GraphicBufferAllocator& allocator = GraphicBufferAllocator::get();
    auto result = allocator.allocate(request);
    mInitCheck = result.status;
    if (result.status == NO_ERROR) {
        handle = result.handle;
        stride = result.stride;

        mBufferMapper.getTransportSize(handle, &mTransportNumFds, &mTransportNumInts);

        width = static_cast<int>(request.width);
        height = static_cast<int>(request.height);
        format = request.format;
        layerCount = request.layerCount;
        usage = request.usage;
        usage_deprecated = int(usage);
        std::string name = request.requestorName;
        mDependencyMonitor.setToken(name.append(":").append(std::to_string(mId)));
    }
}

GraphicBuffer::~GraphicBuffer()
{
    ATRACE_CALL();
    if (handle) {
        free_handle();
    }
    for (auto& [callback, context] : mDeathCallbacks) {
        callback(context, mId);
    }
}

void GraphicBuffer::free_handle()
{
    if (mOwner == ownHandle) {
        mBufferMapper.freeBuffer(handle);
    } else if (mOwner == ownData) {
        GraphicBufferAllocator& allocator(GraphicBufferAllocator::get());
        allocator.free(handle);
    }
    handle = nullptr;
}

status_t GraphicBuffer::initCheck() const {
    return static_cast<status_t>(mInitCheck);
}

void GraphicBuffer::dumpAllocationsToSystemLog()
{
    GraphicBufferAllocator::dumpToSystemLog();
}

ANativeWindowBuffer* GraphicBuffer::getNativeBuffer() const
{
    return static_cast<ANativeWindowBuffer*>(
            const_cast<GraphicBuffer*>(this));
}

status_t GraphicBuffer::getDataspace(ui::Dataspace* outDataspace) const {
    return mBufferMapper.getDataspace(handle, outDataspace);
}

status_t GraphicBuffer::reallocate(uint32_t inWidth, uint32_t inHeight,
        PixelFormat inFormat, uint32_t inLayerCount, uint64_t inUsage)
{
    if (mOwner != ownData)
        return INVALID_OPERATION;

    if (handle &&
            static_cast<int>(inWidth) == width &&
            static_cast<int>(inHeight) == height &&
            inFormat == format &&
            inLayerCount == layerCount &&
            inUsage == usage)
        return NO_ERROR;

    if (handle) {
        GraphicBufferAllocator& allocator(GraphicBufferAllocator::get());
        allocator.free(handle);
        handle = nullptr;
    }
    return initWithSize(inWidth, inHeight, inFormat, inLayerCount, inUsage, "[Reallocation]");
}

bool GraphicBuffer::needsReallocation(uint32_t inWidth, uint32_t inHeight,
        PixelFormat inFormat, uint32_t inLayerCount, uint64_t inUsage)
{
    if (static_cast<int>(inWidth) != width) return true;
    if (static_cast<int>(inHeight) != height) return true;
    if (inFormat != format) return true;
    if (inLayerCount != layerCount) return true;
    if ((usage & inUsage) != inUsage) return true;
    if ((usage & USAGE_PROTECTED) != (inUsage & USAGE_PROTECTED)) return true;
    return false;
}

status_t GraphicBuffer::initWithSize(uint32_t inWidth, uint32_t inHeight,
        PixelFormat inFormat, uint32_t inLayerCount, uint64_t inUsage,
        std::string requestorName)
{
    GraphicBufferAllocator& allocator = GraphicBufferAllocator::get();
    uint32_t outStride = 0;
    status_t err = allocator.allocate(inWidth, inHeight, inFormat, inLayerCount,
            inUsage, &handle, &outStride, mId,
            std::move(requestorName));
    if (err == NO_ERROR) {
        mBufferMapper.getTransportSize(handle, &mTransportNumFds, &mTransportNumInts);

        width = static_cast<int>(inWidth);
        height = static_cast<int>(inHeight);
        format = inFormat;
        layerCount = inLayerCount;
        usage = inUsage;
        usage_deprecated = int(usage);
        stride = static_cast<int>(outStride);
        mDependencyMonitor.setToken(requestorName.append(":").append(std::to_string(mId)));
    }
    return err;
}

status_t GraphicBuffer::initWithHandle(const native_handle_t* inHandle, HandleWrapMethod method,
                                       uint32_t inWidth, uint32_t inHeight, PixelFormat inFormat,
                                       uint32_t inLayerCount, uint64_t inUsage, uint32_t inStride) {
    ANativeWindowBuffer::width = static_cast<int>(inWidth);
    ANativeWindowBuffer::height = static_cast<int>(inHeight);
    ANativeWindowBuffer::stride = static_cast<int>(inStride);
    ANativeWindowBuffer::format = inFormat;
    ANativeWindowBuffer::usage = inUsage;
    ANativeWindowBuffer::usage_deprecated = int(inUsage);

    ANativeWindowBuffer::layerCount = inLayerCount;

    mOwner = (method == WRAP_HANDLE) ? ownNone : ownHandle;

    if (method == TAKE_UNREGISTERED_HANDLE || method == CLONE_HANDLE) {
        buffer_handle_t importedHandle;
        status_t err = mBufferMapper.importBuffer(inHandle, inWidth, inHeight, inLayerCount,
                                                  inFormat, inUsage, inStride, &importedHandle);
        if (err != NO_ERROR) {
            initWithHandle(nullptr, WRAP_HANDLE, 0, 0, 0, 0, 0, 0);

            return err;
        }

        if (method == TAKE_UNREGISTERED_HANDLE) {
            native_handle_close(inHandle);
            native_handle_delete(const_cast<native_handle_t*>(inHandle));
        }

        inHandle = importedHandle;
        mBufferMapper.getTransportSize(inHandle, &mTransportNumFds, &mTransportNumInts);
    }

    ANativeWindowBuffer::handle = inHandle;

    return NO_ERROR;
}

status_t GraphicBuffer::lock(uint32_t inUsage, void** vaddr, int32_t* outBytesPerPixel,
                             int32_t* outBytesPerStride) {
    const Rect lockBounds(width, height);
    status_t res = lock(inUsage, lockBounds, vaddr, outBytesPerPixel, outBytesPerStride);
    return res;
}

status_t GraphicBuffer::lock(uint32_t inUsage, const Rect& rect, void** vaddr,
                             int32_t* outBytesPerPixel, int32_t* outBytesPerStride) {
    if (rect.left < 0 || rect.right  > width ||
        rect.top  < 0 || rect.bottom > height) {
        ALOGE("locking pixels (%d,%d,%d,%d) outside of buffer (w=%d, h=%d)",
                rect.left, rect.top, rect.right, rect.bottom,
                width, height);
        return BAD_VALUE;
    }

    return lockAsync(inUsage, rect, vaddr, -1, outBytesPerPixel, outBytesPerStride);
}

status_t GraphicBuffer::lockYCbCr(uint32_t inUsage, android_ycbcr* ycbcr)
{
    const Rect lockBounds(width, height);
    status_t res = lockYCbCr(inUsage, lockBounds, ycbcr);
    return res;
}

status_t GraphicBuffer::lockYCbCr(uint32_t inUsage, const Rect& rect,
        android_ycbcr* ycbcr)
{
    if (rect.left < 0 || rect.right  > width ||
        rect.top  < 0 || rect.bottom > height) {
        ALOGE("locking pixels (%d,%d,%d,%d) outside of buffer (w=%d, h=%d)",
                rect.left, rect.top, rect.right, rect.bottom,
                width, height);
        return BAD_VALUE;
    }
    return lockAsyncYCbCr(inUsage, rect, ycbcr, -1);
}

status_t GraphicBuffer::unlock()
{
    return unlockAsync(nullptr);
}

status_t GraphicBuffer::lockAsync(uint32_t inUsage, void** vaddr, int fenceFd,
                                  int32_t* outBytesPerPixel, int32_t* outBytesPerStride) {
    const Rect lockBounds(width, height);
    status_t res =
            lockAsync(inUsage, lockBounds, vaddr, fenceFd, outBytesPerPixel, outBytesPerStride);
    return res;
}

status_t GraphicBuffer::lockAsync(uint32_t inUsage, const Rect& rect, void** vaddr, int fenceFd,
                                  int32_t* outBytesPerPixel, int32_t* outBytesPerStride) {
    return lockAsync(inUsage, inUsage, rect, vaddr, fenceFd, outBytesPerPixel, outBytesPerStride);
}

status_t GraphicBuffer::lockAsync(uint64_t inProducerUsage, uint64_t inConsumerUsage,
                                  const Rect& rect, void** vaddr, int fenceFd,
                                  int32_t* outBytesPerPixel, int32_t* outBytesPerStride) {
    if (rect.left < 0 || rect.right  > width ||
        rect.top  < 0 || rect.bottom > height) {
        ALOGE("locking pixels (%d,%d,%d,%d) outside of buffer (w=%d, h=%d)",
                rect.left, rect.top, rect.right, rect.bottom,
                width, height);
        return BAD_VALUE;
    }

    // Resolve the bpp & bps before doing a lock in case this fails we don't have to worry about
    // doing an unlock
    int32_t legacyBpp = -1, legacyBps = -1;
    if (outBytesPerPixel || outBytesPerStride) {
        const auto mapperVersion = getBufferMapperVersion();
        // For gralloc2 we need to guess at the bpp & bps
        // For gralloc3 the lock() call will return it
        // For gralloc4 & later the PlaneLayout metadata query is vastly superior and we
        // resolve bpp & bps just for compatibility

        // TODO: See if we can't just remove gralloc2 support.
        if (mapperVersion == GraphicBufferMapper::GRALLOC_2) {
            legacyBpp = bytesPerPixel(format);
            if (legacyBpp > 0) {
                legacyBps = stride * legacyBpp;
            } else {
                legacyBpp = -1;
            }
        } else if (mapperVersion >= GraphicBufferMapper::GRALLOC_4) {
            auto planeLayout = getBufferMapper().getPlaneLayouts(handle);
            if (!planeLayout.has_value()) return planeLayout.asStatus();
            resolveLegacyByteLayoutFromPlaneLayout(planeLayout.value(), &legacyBpp, &legacyBps);
        }
    }

    const uint64_t usage = static_cast<uint64_t>(ANDROID_NATIVE_UNSIGNED_CAST(
            android_convertGralloc1To0Usage(inProducerUsage, inConsumerUsage)));

    auto result = getBufferMapper().lock(handle, usage, rect, base::unique_fd{fenceFd});

    if (!result.has_value()) {
        return result.error().asStatus();
    }
    auto value = result.value();
    *vaddr = value.address;

    if (outBytesPerPixel) {
        *outBytesPerPixel = legacyBpp != -1 ? legacyBpp : value.bytesPerPixel;
    }
    if (outBytesPerStride) {
        *outBytesPerStride = legacyBps != -1 ? legacyBps : value.bytesPerStride;
    }
    return OK;
}

status_t GraphicBuffer::lockAsyncYCbCr(uint32_t inUsage, android_ycbcr* ycbcr,
        int fenceFd)
{
    const Rect lockBounds(width, height);
    status_t res = lockAsyncYCbCr(inUsage, lockBounds, ycbcr, fenceFd);
    return res;
}

status_t GraphicBuffer::lockAsyncYCbCr(uint32_t inUsage, const Rect& rect,
        android_ycbcr* ycbcr, int fenceFd)
{
    if (rect.left < 0 || rect.right  > width ||
        rect.top  < 0 || rect.bottom > height) {
        ALOGE("locking pixels (%d,%d,%d,%d) outside of buffer (w=%d, h=%d)",
                rect.left, rect.top, rect.right, rect.bottom,
                width, height);
        return BAD_VALUE;
    }
    auto result = getBufferMapper().lockYCbCr(handle, static_cast<int64_t>(inUsage), rect,
                                              base::unique_fd{fenceFd});
    if (!result.has_value()) {
        return result.error().asStatus();
    }
    *ycbcr = result.value();
    return OK;
}

status_t GraphicBuffer::unlockAsync(int *fenceFd)
{
    return getBufferMapper().unlockAsync(handle, fenceFd);
}

status_t GraphicBuffer::isSupported(uint32_t inWidth, uint32_t inHeight, PixelFormat inFormat,
                                    uint32_t inLayerCount, uint64_t inUsage,
                                    bool* outSupported) const {
    return mBufferMapper.isSupported(inWidth, inHeight, inFormat, inLayerCount, inUsage,
                                     outSupported);
}

size_t GraphicBuffer::getFlattenedSize() const {
    return static_cast<size_t>(13 + (handle ? mTransportNumInts : 0)) * sizeof(int);
}

size_t GraphicBuffer::getFdCount() const {
    return static_cast<size_t>(handle ? mTransportNumFds : 0);
}

status_t GraphicBuffer::flatten(void*& buffer, size_t& size, int*& fds, size_t& count) const {
    size_t sizeNeeded = GraphicBuffer::getFlattenedSize();
    if (size < sizeNeeded) return NO_MEMORY;

    size_t fdCountNeeded = GraphicBuffer::getFdCount();
    if (count < fdCountNeeded) return NO_MEMORY;

    int32_t* buf = static_cast<int32_t*>(buffer);
    buf[0] = 'GB01';
    buf[1] = width;
    buf[2] = height;
    buf[3] = stride;
    buf[4] = format;
    buf[5] = static_cast<int32_t>(layerCount);
    buf[6] = int(usage); // low 32-bits
    buf[7] = static_cast<int32_t>(mId >> 32);
    buf[8] = static_cast<int32_t>(mId & 0xFFFFFFFFull);
    buf[9] = static_cast<int32_t>(mGenerationNumber);
    buf[10] = 0;
    buf[11] = 0;
    buf[12] = int(usage >> 32); // high 32-bits

    if (handle) {
        buf[10] = int32_t(mTransportNumFds);
        buf[11] = int32_t(mTransportNumInts);
        memcpy(fds, handle->data, static_cast<size_t>(mTransportNumFds) * sizeof(int));
        memcpy(buf + 13, handle->data + handle->numFds,
               static_cast<size_t>(mTransportNumInts) * sizeof(int));
    }

    buffer = static_cast<void*>(static_cast<uint8_t*>(buffer) + sizeNeeded);
    size -= sizeNeeded;
    if (handle) {
        fds += mTransportNumFds;
        count -= static_cast<size_t>(mTransportNumFds);
    }
    return NO_ERROR;
}

status_t GraphicBuffer::unflatten(void const*& buffer, size_t& size, int const*& fds,
                                  size_t& count) {
    // Check if size is not smaller than buf[0] is supposed to take.
    if (size < sizeof(int)) {
        return NO_MEMORY;
    }

    int const* buf = static_cast<int const*>(buffer);

    // NOTE: it turns out that some media code generates a flattened GraphicBuffer manually!!!!!
    // see H2BGraphicBufferProducer.cpp
    uint32_t flattenWordCount = 0;
    if (buf[0] == 'GB01') {
        // new version with 64-bits usage bits
        flattenWordCount = 13;
    } else if (buf[0] == 'GBFR') {
        // old version, when usage bits were 32-bits
        flattenWordCount = 12;
    } else {
        return BAD_TYPE;
    }

    if (size < 12 * sizeof(int)) {
        android_errorWriteLog(0x534e4554, "114223584");
        return NO_MEMORY;
    }

    const size_t numFds  = static_cast<size_t>(buf[10]);
    const size_t numInts = static_cast<size_t>(buf[11]);

    // Limit the maxNumber to be relatively small. The number of fds or ints
    // should not come close to this number, and the number itself was simply
    // chosen to be high enough to not cause issues and low enough to prevent
    // overflow problems.
    const size_t maxNumber = 4096;
    if (numFds >= maxNumber || numInts >= (maxNumber - flattenWordCount)) {
        width = height = stride = format = usage_deprecated = 0;
        layerCount = 0;
        usage = 0;
        handle = nullptr;
        ALOGE("unflatten: numFds or numInts is too large: %zd, %zd", numFds, numInts);
        return BAD_VALUE;
    }

    const size_t sizeNeeded = (flattenWordCount + numInts) * sizeof(int);
    if (size < sizeNeeded) return NO_MEMORY;

    size_t fdCountNeeded = numFds;
    if (count < fdCountNeeded) return NO_MEMORY;

    if (handle) {
        // free previous handle if any
        free_handle();
    }

    if (numFds || numInts) {
        width  = buf[1];
        height = buf[2];
        stride = buf[3];
        format = buf[4];
        layerCount = static_cast<uintptr_t>(buf[5]);
        usage_deprecated = buf[6];
        if (flattenWordCount == 13) {
            usage = (uint64_t(buf[12]) << 32) | uint32_t(buf[6]);
        } else {
            usage = uint64_t(ANDROID_NATIVE_UNSIGNED_CAST(usage_deprecated));
        }
        native_handle* h =
                native_handle_create(static_cast<int>(numFds), static_cast<int>(numInts));
        if (!h) {
            width = height = stride = format = usage_deprecated = 0;
            layerCount = 0;
            usage = 0;
            handle = nullptr;
            ALOGE("unflatten: native_handle_create failed");
            return NO_MEMORY;
        }
        memcpy(h->data, fds, numFds * sizeof(int));
        memcpy(h->data + numFds, buf + flattenWordCount, numInts * sizeof(int));
        handle = h;
    } else {
        width = height = stride = format = usage_deprecated = 0;
        layerCount = 0;
        usage = 0;
        handle = nullptr;
    }

    mId = static_cast<uint64_t>(buf[7]) << 32;
    mId |= static_cast<uint32_t>(buf[8]);

    mGenerationNumber = static_cast<uint32_t>(buf[9]);

    mOwner = ownHandle;

    if (handle != nullptr) {
        buffer_handle_t importedHandle;
        status_t err = mBufferMapper.importBuffer(handle, uint32_t(width), uint32_t(height),
                uint32_t(layerCount), format, usage, uint32_t(stride), &importedHandle);
        if (err != NO_ERROR) {
            width = height = stride = format = usage_deprecated = 0;
            layerCount = 0;
            usage = 0;
            native_handle_close(handle);
            native_handle_delete(const_cast<native_handle_t*>(handle));
            handle = nullptr;
            ALOGE("unflatten: registerBuffer failed: %s (%d)", strerror(-err), err);
            return err;
        }

        native_handle_close(handle);
        native_handle_delete(const_cast<native_handle_t*>(handle));
        handle = importedHandle;
        mBufferMapper.getTransportSize(handle, &mTransportNumFds, &mTransportNumInts);
    }

    std::string name;
    status_t err = mBufferMapper.getName(handle, &name);
    if (err != NO_ERROR) {
        name = "<Unknown>";
    }

    mDependencyMonitor.setToken(name.append(":").append(std::to_string(mId)));

    buffer = static_cast<void const*>(static_cast<uint8_t const*>(buffer) + sizeNeeded);
    size -= sizeNeeded;
    fds += numFds;
    count -= numFds;
    return NO_ERROR;
}

void GraphicBuffer::addDeathCallback(GraphicBufferDeathCallback deathCallback, void* context) {
    mDeathCallbacks.emplace_back(deathCallback, context);
}

// ---------------------------------------------------------------------------

}; // namespace android
