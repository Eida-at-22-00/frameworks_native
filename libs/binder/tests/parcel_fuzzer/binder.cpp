/*
 * Copyright (C) 2019 The Android Open Source Project
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
#define FUZZ_LOG_TAG "binder"

#include "binder.h"
#include "parcelables/EmptyParcelable.h"
#include "parcelables/GenericDataParcelable.h"
#include "parcelables/SingleDataParcelable.h"
#include "util.h"

#include <android/os/IServiceManager.h>
#include <binder/ParcelableHolder.h>
#include <binder/PersistableBundle.h>
#include <binder/Status.h>
#include <fuzzbinder/random_binder.h>
#include <fuzzbinder/random_fd.h>
#include <utils/Flattenable.h>

#include "../../Utils.h"

using ::android::HexString;
using ::android::status_t;
using ::android::binder::unique_fd;

enum ByteEnum : int8_t {};
enum IntEnum : int32_t {};
enum LongEnum : int64_t {};

class ExampleParcelable : public android::Parcelable {
public:
    status_t writeToParcel(android::Parcel* /*parcel*/) const override {
        FUZZ_LOG() << "should not reach";
        abort();
    }
    status_t readFromParcel(const android::Parcel* parcel) override {
        mExampleExtraField++;
        return parcel->readInt64(&(this->mExampleUsedData));
    }
private:
    int64_t mExampleExtraField = 0;
    int64_t mExampleUsedData = 0;
};

struct ExampleFlattenable : public android::Flattenable<ExampleFlattenable> {
public:
    size_t getFlattenedSize() const { return sizeof(mValue); }
    size_t getFdCount() const { return 0; }
    status_t flatten(void*& /*buffer*/, size_t& /*size*/, int*& /*fds*/, size_t& /*count*/) const {
        FUZZ_LOG() << "should not reach";
        abort();
    }
    status_t unflatten(void const*& buffer, size_t& size, int const*& /*fds*/, size_t& /*count*/) {
        if (size < sizeof(mValue)) {
            return android::NO_MEMORY;
        }
        android::FlattenableUtils::read(buffer, size, mValue);
        return android::OK;
    }
private:
    int32_t mValue = 0xFEEDBEEF;
};

struct ExampleLightFlattenable : public android::LightFlattenablePod<ExampleLightFlattenable> {
    int32_t mValue = 0;
};

struct BigStruct {
    uint8_t data[1337];
};

#define PARCEL_READ_WITH_STATUS(T, FUN)                                  \
    [](const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {   \
        FUZZ_LOG() << "about to read " #T " using " #FUN " with status"; \
        T t{};                                                           \
        status_t status = p.FUN(&t);                                     \
        FUZZ_LOG() << #T " status: " << status /* << " value: " << t*/;  \
    }

#define PARCEL_READ_NO_STATUS(T, FUN)                                       \
    [](const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {      \
        FUZZ_LOG() << "about to read " #T " using " #FUN " with no status"; \
        T t = p.FUN();                                                      \
        (void)t;                                                            \
        FUZZ_LOG() << #T " done " /* << " value: " << t*/;                  \
    }

#define PARCEL_READ_OPT_STATUS(T, FUN) \
    PARCEL_READ_WITH_STATUS(T, FUN), \
    PARCEL_READ_NO_STATUS(T, FUN)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
// clang-format off
std::vector<ParcelRead<::android::Parcel>> BINDER_PARCEL_READ_FUNCTIONS {
    PARCEL_READ_NO_STATUS(size_t, dataSize),
    PARCEL_READ_NO_STATUS(size_t, dataAvail),
    PARCEL_READ_NO_STATUS(size_t, dataPosition),
    PARCEL_READ_NO_STATUS(size_t, dataCapacity),
    PARCEL_READ_NO_STATUS(::android::binder::Status, enforceNoDataAvail),
    [] (const ::android::Parcel& p, FuzzedDataProvider& provider) {
        // aborts on larger values
        size_t pos = provider.ConsumeIntegralInRange<size_t>(0, INT32_MAX);
        FUZZ_LOG() << "about to setDataPosition: " << pos;
        p.setDataPosition(pos);
        FUZZ_LOG() << "setDataPosition done";
    },
    PARCEL_READ_NO_STATUS(size_t, allowFds),
    PARCEL_READ_NO_STATUS(size_t, hasFileDescriptors),
    PARCEL_READ_NO_STATUS(std::vector<android::sp<android::IBinder>>, debugReadAllStrongBinders),
    PARCEL_READ_NO_STATUS(std::vector<int>, debugReadAllFileDescriptors),
    [] (const ::android::Parcel& p, FuzzedDataProvider&) {
        FUZZ_LOG() << "about to markSensitive";
        p.markSensitive();
        FUZZ_LOG() << "markSensitive done";
    },
    [] (const ::android::Parcel& p, FuzzedDataProvider& provider) {
        std::string interface = provider.ConsumeRandomLengthString();
        FUZZ_LOG() << "about to enforceInterface: " << interface;
        bool b = p.enforceInterface(::android::String16(interface.c_str()));
        FUZZ_LOG() << "enforced interface: " << b;
    },
    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to checkInterface";
        android::sp<android::IBinder> aBinder = new android::BBinder();
        bool b = p.checkInterface(aBinder.get());
        FUZZ_LOG() << "checked interface: " << b;
    },
    PARCEL_READ_NO_STATUS(size_t, objectsCount),
    PARCEL_READ_NO_STATUS(status_t, errorCheck),
    [] (const ::android::Parcel& p, FuzzedDataProvider& provider) {
        // Read at least a bit. Unbounded allocation would OOM.
        size_t len = provider.ConsumeIntegralInRange<size_t>(0, 1024);
        FUZZ_LOG() << "about to read void*";
        std::vector<uint8_t> data(len);
        status_t status = p.read(data.data(), len);
        FUZZ_LOG() << "read status: " << status;
    },
    [] (const ::android::Parcel& p, FuzzedDataProvider& provider) {
        size_t len = provider.ConsumeIntegral<size_t>();
        FUZZ_LOG() << "about to readInplace";
        const void* r = p.readInplace(len);
        FUZZ_LOG() << "readInplace done. pointer: " << r << " bytes: " << (r ? HexString(r, len) : "null");
    },
    PARCEL_READ_OPT_STATUS(int32_t, readInt32),
    PARCEL_READ_OPT_STATUS(uint32_t, readUint32),
    PARCEL_READ_OPT_STATUS(int64_t, readInt64),
    PARCEL_READ_OPT_STATUS(uint64_t, readUint64),
    PARCEL_READ_OPT_STATUS(float, readFloat),
    PARCEL_READ_OPT_STATUS(double, readDouble),
    PARCEL_READ_OPT_STATUS(bool, readBool),
    PARCEL_READ_OPT_STATUS(char16_t, readChar),
    PARCEL_READ_OPT_STATUS(int8_t, readByte),

    PARCEL_READ_WITH_STATUS(std::string, readUtf8FromUtf16),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::string>, readUtf8FromUtf16),
    PARCEL_READ_WITH_STATUS(std::optional<std::string>, readUtf8FromUtf16),
    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to read c-str";
        const char* str = p.readCString();
        FUZZ_LOG() << "read c-str: " << (str ? str : "<empty string>");
    },
    PARCEL_READ_OPT_STATUS(android::String8, readString8),
    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to readString8Inplace";
        size_t outLen = 0;
        const char* str = p.readString8Inplace(&outLen);
        std::string bytes = str ? HexString(str, sizeof(char) * (outLen + 1)) : "null";
        FUZZ_LOG() << "readString8Inplace: " << bytes << " size: " << outLen;
    },
    PARCEL_READ_OPT_STATUS(android::String16, readString16),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<android::String16>, readString16),
    PARCEL_READ_WITH_STATUS(std::optional<android::String16>, readString16),
    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to readString16Inplace";
        size_t outLen = 0;
        const char16_t* str = p.readString16Inplace(&outLen);
        std::string bytes = str ? HexString(str, sizeof(char16_t) * (outLen + 1)) : "null";
        FUZZ_LOG() << "readString16Inplace: " << bytes << " size: " << outLen;
    },
    PARCEL_READ_WITH_STATUS(android::sp<android::IBinder>, readStrongBinder),
    PARCEL_READ_WITH_STATUS(android::sp<android::IBinder>, readNullableStrongBinder),

    PARCEL_READ_WITH_STATUS(std::vector<ByteEnum>, readEnumVector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<ByteEnum>>, readEnumVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<ByteEnum>>, readEnumVector),
    PARCEL_READ_WITH_STATUS(std::vector<IntEnum>, readEnumVector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<IntEnum>>, readEnumVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<IntEnum>>, readEnumVector),
    PARCEL_READ_WITH_STATUS(std::vector<LongEnum>, readEnumVector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<LongEnum>>, readEnumVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<LongEnum>>, readEnumVector),

    // only reading one parcelable type for now
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<std::unique_ptr<ExampleParcelable>>>, readParcelableVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<std::optional<ExampleParcelable>>>, readParcelableVector),
    PARCEL_READ_WITH_STATUS(std::vector<ExampleParcelable>, readParcelableVector),
    PARCEL_READ_WITH_STATUS(ExampleParcelable, readParcelable),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<ExampleParcelable>, readParcelable),
    PARCEL_READ_WITH_STATUS(std::optional<ExampleParcelable>, readParcelable),

    // only reading one binder type for now
    PARCEL_READ_WITH_STATUS(android::sp<android::os::IServiceManager>, readStrongBinder),
    PARCEL_READ_WITH_STATUS(android::sp<android::os::IServiceManager>, readNullableStrongBinder),
    PARCEL_READ_WITH_STATUS(std::vector<android::sp<android::os::IServiceManager>>, readStrongBinderVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<android::sp<android::os::IServiceManager>>>, readStrongBinderVector),

    PARCEL_READ_WITH_STATUS(::std::unique_ptr<std::vector<android::sp<android::IBinder>>>, readStrongBinderVector),
    PARCEL_READ_WITH_STATUS(::std::optional<std::vector<android::sp<android::IBinder>>>, readStrongBinderVector),
    PARCEL_READ_WITH_STATUS(std::vector<android::sp<android::IBinder>>, readStrongBinderVector),

    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<int8_t>>, readByteVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<int8_t>>, readByteVector),
    PARCEL_READ_WITH_STATUS(std::vector<int8_t>, readByteVector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<uint8_t>>, readByteVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<uint8_t>>, readByteVector),
    PARCEL_READ_WITH_STATUS(std::vector<uint8_t>, readByteVector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<int32_t>>, readInt32Vector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<int32_t>>, readInt32Vector),
    PARCEL_READ_WITH_STATUS(std::vector<int32_t>, readInt32Vector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<int64_t>>, readInt64Vector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<int64_t>>, readInt64Vector),
    PARCEL_READ_WITH_STATUS(std::vector<int64_t>, readInt64Vector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<uint64_t>>, readUint64Vector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<uint64_t>>, readUint64Vector),
    PARCEL_READ_WITH_STATUS(std::vector<uint64_t>, readUint64Vector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<float>>, readFloatVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<float>>, readFloatVector),
    PARCEL_READ_WITH_STATUS(std::vector<float>, readFloatVector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<double>>, readDoubleVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<double>>, readDoubleVector),
    PARCEL_READ_WITH_STATUS(std::vector<double>, readDoubleVector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<bool>>, readBoolVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<bool>>, readBoolVector),
    PARCEL_READ_WITH_STATUS(std::vector<bool>, readBoolVector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<char16_t>>, readCharVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<char16_t>>, readCharVector),
    PARCEL_READ_WITH_STATUS(std::vector<char16_t>, readCharVector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<std::unique_ptr<android::String16>>>, readString16Vector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<std::optional<android::String16>>>, readString16Vector),
    PARCEL_READ_WITH_STATUS(std::vector<android::String16>, readString16Vector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<std::unique_ptr<std::string>>>, readUtf8VectorFromUtf16Vector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<std::optional<std::string>>>, readUtf8VectorFromUtf16Vector),
    PARCEL_READ_WITH_STATUS(std::vector<std::string>, readUtf8VectorFromUtf16Vector),

#define COMMA ,
    PARCEL_READ_WITH_STATUS(std::array<uint8_t COMMA 3>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::optional<std::array<uint8_t COMMA 3>>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::array<char16_t COMMA 3>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::optional<std::array<char16_t COMMA 3>>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::array<std::string COMMA 3>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::optional<std::array<std::optional<std::string> COMMA 3>>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::array<android::String16 COMMA 3>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::optional<std::array<std::optional<android::String16> COMMA 3>>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::array<android::sp<android::IBinder> COMMA 3>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::optional<std::array<android::sp<android::IBinder> COMMA 3>>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::array<ExampleParcelable COMMA 3>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::optional<std::array<std::optional<ExampleParcelable> COMMA 3>>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::array<ByteEnum COMMA 3>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::optional<std::array<ByteEnum COMMA 3>>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::array<IntEnum COMMA 3>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::optional<std::array<IntEnum COMMA 3>>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::array<LongEnum COMMA 3>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::optional<std::array<LongEnum COMMA 3>>, readFixedArray),
    // nested arrays
    PARCEL_READ_WITH_STATUS(std::array<std::array<uint8_t COMMA 3> COMMA 4>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::optional<std::array<std::array<uint8_t COMMA 3> COMMA 4>>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::array<ExampleParcelable COMMA 3>, readFixedArray),
    PARCEL_READ_WITH_STATUS(std::optional<std::array<std::array<std::optional<ExampleParcelable> COMMA 3> COMMA 4>>, readFixedArray),
#undef COMMA

    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to read flattenable";
        ExampleFlattenable f;
        status_t status = p.read(f);
        FUZZ_LOG() << "read flattenable: " << status;
    },
    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to read lite flattenable";
        ExampleLightFlattenable f;
        status_t status = p.read(f);
        FUZZ_LOG() << "read lite flattenable: " << status;
    },

    PARCEL_READ_WITH_STATUS(std::vector<uint8_t>, resizeOutVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<uint8_t>>, resizeOutVector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<uint8_t>>, resizeOutVector),
    PARCEL_READ_WITH_STATUS(std::vector<BigStruct>, resizeOutVector),
    PARCEL_READ_WITH_STATUS(std::optional<std::vector<BigStruct>>, resizeOutVector),
    PARCEL_READ_WITH_STATUS(std::unique_ptr<std::vector<BigStruct>>, resizeOutVector),

    PARCEL_READ_NO_STATUS(int32_t, readExceptionCode),
    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to readNativeHandle";
        native_handle_t* t = p.readNativeHandle();
        FUZZ_LOG() << "readNativeHandle: " << t;
        if (t != nullptr) {
            FUZZ_LOG() << "about to free readNativeHandle";
            native_handle_close(t);
            native_handle_delete(t);
            FUZZ_LOG() << "readNativeHandle freed";
        }
    },
    PARCEL_READ_NO_STATUS(int, readFileDescriptor),
    PARCEL_READ_NO_STATUS(int, readParcelFileDescriptor),
    PARCEL_READ_WITH_STATUS(unique_fd, readUniqueFileDescriptor),

    PARCEL_READ_WITH_STATUS(std::optional<std::vector<unique_fd>>, readUniqueFileDescriptorVector),
    PARCEL_READ_WITH_STATUS(std::vector<unique_fd>, readUniqueFileDescriptorVector),

    [] (const ::android::Parcel& p, FuzzedDataProvider& provider) {
        size_t len = provider.ConsumeIntegral<size_t>();
        FUZZ_LOG() << "about to readBlob";
        ::android::Parcel::ReadableBlob blob;
        status_t status = p.readBlob(len, &blob);
        FUZZ_LOG() << "readBlob status: " << status;
    },
    [] (const ::android::Parcel& p, FuzzedDataProvider& provider) {
        FUZZ_LOG() << "about to readObject";
        bool nullMetaData = provider.ConsumeBool();
        const void* obj = static_cast<const void*>(p.readObject(nullMetaData));
        FUZZ_LOG() << "readObject: " << obj;
    },
    PARCEL_READ_NO_STATUS(uid_t, readCallingWorkSourceUid),
    PARCEL_READ_NO_STATUS(size_t, getOpenAshmemSize),

    // additional parcelable objects defined in libbinder
    [] (const ::android::Parcel& p, FuzzedDataProvider& provider) {
        using ::android::os::ParcelableHolder;
        using ::android::Parcelable;
        FUZZ_LOG() << "about to read ParcelableHolder using readParcelable with status";
        Parcelable::Stability stability = provider.ConsumeBool()
            ? Parcelable::Stability::STABILITY_LOCAL
            : Parcelable::Stability::STABILITY_VINTF;
        ParcelableHolder t = ParcelableHolder(stability);
        status_t status = p.readParcelable(&t);
        FUZZ_LOG() << "ParcelableHolder status: " << status;
    },
    PARCEL_READ_WITH_STATUS(android::os::PersistableBundle, readParcelable),
    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to call hasFileDescriptorsInRange() with status";
        size_t offset = p.readUint32();
        size_t length = p.readUint32();
        bool result;
        status_t status = p.hasFileDescriptorsInRange(offset, length, &result);
        FUZZ_LOG() << " status: " << status  << " result: " << result;
    },
    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to call hasBinders() with status";
        bool result;
        status_t status = p.hasBinders(&result);
        FUZZ_LOG() << " status: " << status  << " result: " << result;
    },
    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to call hasBindersInRange() with status";
        size_t offset = p.readUint32();
        size_t length = p.readUint32();
        bool result;
        status_t status = p.hasBindersInRange(offset, length, &result);
        FUZZ_LOG() << " status: " << status  << " result: " << result;
    },
    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to call compareDataInRange() with status";
        size_t thisOffset = p.readUint32();
        size_t otherOffset = p.readUint32();
        size_t length = p.readUint32();
        int result;
        status_t status = p.compareDataInRange(thisOffset, p, otherOffset, length, &result);
        FUZZ_LOG() << " status: " << status  << " result: " << result;
    },
    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to call readFromParcel() with status for EmptyParcelable";
        parcelables::EmptyParcelable emptyParcelable{};
        status_t status = emptyParcelable.readFromParcel(&p);
        FUZZ_LOG() << " status: " << status;
    },
    [] (const ::android::Parcel& p , FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to call readFromParcel() with status for SingleDataParcelable";
        parcelables::SingleDataParcelable singleDataParcelable;
        status_t status = singleDataParcelable.readFromParcel(&p);
        FUZZ_LOG() << " status: " << status;
    },
    [] (const ::android::Parcel& p, FuzzedDataProvider& /*provider*/) {
        FUZZ_LOG() << "about to call readFromParcel() with status for GenericDataParcelable";
        parcelables::GenericDataParcelable genericDataParcelable;
        status_t status = genericDataParcelable.readFromParcel(&p);
        FUZZ_LOG() << " status: " << status;
        std::string toString = genericDataParcelable.toString();
        FUZZ_LOG() << " toString() result: " << toString;
    },
};

std::vector<ParcelWrite<::android::Parcel>> BINDER_PARCEL_WRITE_FUNCTIONS {
    [] (::android::Parcel& p, FuzzedDataProvider& provider, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call setDataSize";
        size_t len = provider.ConsumeIntegralInRange<size_t>(0, 1024);
        p.setDataSize(len);
    },
    [] (::android::Parcel& p, FuzzedDataProvider& provider, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call setDataCapacity";
        size_t len = provider.ConsumeIntegralInRange<size_t>(0, 1024);
        p.setDataCapacity(len);
    },
    [] (::android::Parcel& p, FuzzedDataProvider& provider, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call setData";
        size_t len = provider.ConsumeIntegralInRange<size_t>(0, 1024);
        std::vector<uint8_t> bytes = provider.ConsumeBytes<uint8_t>(len);
        p.setData(bytes.data(), bytes.size());
    },
    [] (::android::Parcel& p, FuzzedDataProvider& provider, android::RandomParcelOptions* options) {
        FUZZ_LOG() << "about to call appendFrom";

        std::vector<uint8_t> bytes = provider.ConsumeBytes<uint8_t>(provider.ConsumeIntegralInRange<size_t>(0, 4096));
        ::android::Parcel p2;
        fillRandomParcel(&p2, FuzzedDataProvider(bytes.data(), bytes.size()), options);

        int32_t start = provider.ConsumeIntegral<int32_t>();
        int32_t len = provider.ConsumeIntegral<int32_t>();
        p.appendFrom(&p2, start, len);
    },
    [] (::android::Parcel& p, FuzzedDataProvider& provider, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call pushAllowFds";
        bool val = provider.ConsumeBool();
        p.pushAllowFds(val);
    },
    [] (::android::Parcel& p, FuzzedDataProvider& provider, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call restoreAllowFds";
        bool val = provider.ConsumeBool();
        p.restoreAllowFds(val);
    },
    // markForBinder - covered by fillRandomParcel, aborts if called multiple times
    // markForRpc - covered by fillRandomParcel, aborts if called multiple times
    [] (::android::Parcel& p, FuzzedDataProvider& provider, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call writeInterfaceToken";
        std::string interface = provider.ConsumeRandomLengthString();
        p.writeInterfaceToken(android::String16(interface.c_str()));
    },
    [] (::android::Parcel& p, FuzzedDataProvider& provider, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call setEnforceNoDataAvail";
        p.setEnforceNoDataAvail(provider.ConsumeBool());
    },
    [] (::android::Parcel& p, FuzzedDataProvider& /* provider */, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call setServiceFuzzing";
        p.setServiceFuzzing();
    },
    [] (::android::Parcel& p, FuzzedDataProvider& /* provider */, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call freeData";
        p.freeData();
    },
    [] (::android::Parcel& p, FuzzedDataProvider& provider, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call write";
        size_t len = provider.ConsumeIntegralInRange<size_t>(0, 256);
        std::vector<uint8_t> bytes = provider.ConsumeBytes<uint8_t>(len);
        p.write(bytes.data(), bytes.size());
    },
    // write* - write functions all implemented by calling 'write' itself.
    [] (::android::Parcel& p, FuzzedDataProvider& provider, android::RandomParcelOptions* options) {
        FUZZ_LOG() << "about to call writeStrongBinder";

        // TODO: this logic is somewhat duplicated with random parcel
       android::sp<android::IBinder> binder;
       if (provider.ConsumeBool() && options->extraBinders.size() > 0) {
            binder = options->extraBinders.at(
                    provider.ConsumeIntegralInRange<size_t>(0, options->extraBinders.size() - 1));
        } else {
            binder = android::getRandomBinder(&provider);
            options->extraBinders.push_back(binder);
        }

        p.writeStrongBinder(binder);
    },
    [] (::android::Parcel& p, FuzzedDataProvider& /* provider */, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call writeFileDescriptor (no ownership)";
        p.writeFileDescriptor(STDERR_FILENO, false /* takeOwnership */);
    },
    [] (::android::Parcel& p, FuzzedDataProvider& provider, android::RandomParcelOptions* options) {
        FUZZ_LOG() << "about to call writeFileDescriptor (take ownership)";
        std::vector<unique_fd> fds = android::getRandomFds(&provider);
        if (fds.size() == 0) return;

        p.writeDupFileDescriptor(fds.at(0).get());
        options->extraFds.insert(options->extraFds.end(),
             std::make_move_iterator(fds.begin() + 1),
             std::make_move_iterator(fds.end()));
    },
    // TODO: writeBlob
    // TODO: writeDupImmutableBlobFileDescriptor
    // TODO: writeObject (or make the API private more likely)
    [] (::android::Parcel& p, FuzzedDataProvider& /* provider */, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call writeNoException";
        p.writeNoException();
    },
    [] (::android::Parcel& p, FuzzedDataProvider& provider, android::RandomParcelOptions* /*options*/) {
        FUZZ_LOG() << "about to call replaceCallingWorkSourceUid";
        uid_t uid = provider.ConsumeIntegral<uid_t>();
        p.replaceCallingWorkSourceUid(uid);
    },
};

// clang-format on
#pragma clang diagnostic pop
