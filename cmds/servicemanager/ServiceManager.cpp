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

#include "ServiceManager.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <android-base/strings.h>
#include <binder/BpBinder.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/Stability.h>
#include <cutils/android_filesystem_config.h>
#include <cutils/multiuser.h>
#include <thread>

#if !defined(VENDORSERVICEMANAGER) && !defined(__ANDROID_RECOVERY__)
#include "perfetto/public/protos/trace/android/android_track_event.pzc.h"
#include "perfetto/public/te_category_macros.h"
#include "perfetto/public/te_macros.h"
#endif // !defined(VENDORSERVICEMANAGER) && !defined(__ANDROID_RECOVERY__)

#ifndef VENDORSERVICEMANAGER
#include <vintf/VintfObject.h>
#ifdef __ANDROID_RECOVERY__
#include <vintf/VintfObjectRecovery.h>
#endif // __ANDROID_RECOVERY__
#include <vintf/constants.h>
#endif  // !VENDORSERVICEMANAGER

#include "NameUtil.h"

using ::android::binder::Status;
using ::android::internal::Stability;

namespace android {

#if defined(VENDORSERVICEMANAGER) || defined(__ANDROID_RECOVERY__)
#define SM_PERFETTO_TRACE_FUNC(...)
#else

PERFETTO_TE_CATEGORIES_DEFINE(PERFETTO_SM_CATEGORIES);

#define SM_PERFETTO_TRACE_FUNC(...) \
    PERFETTO_TE_SCOPED(servicemanager, PERFETTO_TE_SLICE_BEGIN(__func__) __VA_OPT__(, ) __VA_ARGS__)

constexpr uint32_t kProtoServiceName =
        perfetto_protos_AndroidTrackEvent_binder_service_name_field_number;
constexpr uint32_t kProtoInterfaceName =
        perfetto_protos_AndroidTrackEvent_binder_interface_name_field_number;
constexpr uint32_t kProtoApexName = perfetto_protos_AndroidTrackEvent_apex_name_field_number;

#endif // !(defined(VENDORSERVICEMANAGER) || defined(__ANDROID_RECOVERY__))

bool is_multiuser_uid_isolated(uid_t uid) {
    uid_t appid = multiuser_get_app_id(uid);
    return appid >= AID_ISOLATED_START && appid <= AID_ISOLATED_END;
}

#ifndef VENDORSERVICEMANAGER

struct ManifestWithDescription {
    std::shared_ptr<const vintf::HalManifest> manifest;
    const char* description;
};
static std::vector<ManifestWithDescription> GetManifestsWithDescription() {
#ifdef __ANDROID_RECOVERY__
    auto vintfObject = vintf::VintfObjectRecovery::GetInstance();
    if (vintfObject == nullptr) {
        ALOGE("NULL VintfObjectRecovery!");
        return {};
    }
    return {ManifestWithDescription{vintfObject->getRecoveryHalManifest(), "recovery"}};
#else
    auto vintfObject = vintf::VintfObject::GetInstance();
    if (vintfObject == nullptr) {
        ALOGE("NULL VintfObject!");
        return {};
    }
    return {ManifestWithDescription{vintfObject->getDeviceHalManifest(), "device"},
            ManifestWithDescription{vintfObject->getFrameworkHalManifest(), "framework"}};
#endif
}

// func true -> stop search and forEachManifest will return true
static bool forEachManifest(const std::function<bool(const ManifestWithDescription&)>& func) {
    for (const ManifestWithDescription& mwd : GetManifestsWithDescription()) {
        if (mwd.manifest == nullptr) {
            ALOGE("NULL VINTF MANIFEST!: %s", mwd.description);
            // note, we explicitly do not retry here, so that we can detect VINTF
            // or other bugs (b/151696835)
            continue;
        }
        if (func(mwd)) return true;
    }
    return false;
}

static std::string getNativeInstanceName(const vintf::ManifestInstance& instance) {
    return instance.package() + "/" + instance.instance();
}

struct AidlName {
    std::string package;
    std::string iface;
    std::string instance;

    static bool fill(const std::string& name, AidlName* aname, bool logError) {
        size_t firstSlash = name.find('/');
        size_t lastDot = name.rfind('.', firstSlash);
        if (firstSlash == std::string::npos || lastDot == std::string::npos) {
            if (logError) {
                ALOGE("VINTF HALs require names in the format type/instance (e.g. "
                      "some.package.foo.IFoo/default) but got: %s",
                      name.c_str());
            }
            return false;
        }
        aname->package = name.substr(0, lastDot);
        aname->iface = name.substr(lastDot + 1, firstSlash - lastDot - 1);
        aname->instance = name.substr(firstSlash + 1);
        return true;
    }
};

static std::string getAidlInstanceName(const vintf::ManifestInstance& instance) {
    return instance.package() + "." + instance.interface() + "/" + instance.instance();
}

static bool isVintfDeclared(const Access::CallingContext& ctx, const std::string& name) {
    NativeName nname;
    if (NativeName::fill(name, &nname)) {
        bool found = forEachManifest([&](const ManifestWithDescription& mwd) {
            if (mwd.manifest->hasNativeInstance(nname.package, nname.instance)) {
                ALOGI("%s Found %s in %s VINTF manifest.", ctx.toDebugString().c_str(),
                      name.c_str(), mwd.description);
                return true; // break
            }
            return false; // continue
        });
        if (!found) {
            ALOGI("%s Could not find %s in the VINTF manifest.", ctx.toDebugString().c_str(),
                  name.c_str());
        }
        return found;
    }

    AidlName aname;
    if (!AidlName::fill(name, &aname, true)) return false;

    bool found = forEachManifest([&](const ManifestWithDescription& mwd) {
        if (mwd.manifest->hasAidlInstance(aname.package, aname.iface, aname.instance)) {
            ALOGI("%s Found %s in %s VINTF manifest.", ctx.toDebugString().c_str(), name.c_str(),
                  mwd.description);
            return true; // break
        }
        return false;  // continue
    });

    if (!found) {
        std::set<std::string> instances;
        forEachManifest([&](const ManifestWithDescription& mwd) {
            std::set<std::string> res = mwd.manifest->getAidlInstances(aname.package, aname.iface);
            instances.insert(res.begin(), res.end());
            return true;
        });

        std::string available;
        if (instances.empty()) {
            available = "No alternative instances declared in VINTF";
        } else {
            // for logging only. We can't return this information to the client
            // because they may not have permissions to find or list those
            // instances
            available = "VINTF declared instances: " + base::Join(instances, ", ");
        }
        // Although it is tested, explicitly rebuilding qualified name, in case it
        // becomes something unexpected.
        ALOGI("%s Could not find %s.%s/%s in the VINTF manifest. %s.", ctx.toDebugString().c_str(),
              aname.package.c_str(), aname.iface.c_str(), aname.instance.c_str(),
              available.c_str());
    }

    return found;
}

static std::optional<std::string> getVintfUpdatableApex(const std::string& name) {
    NativeName nname;
    if (NativeName::fill(name, &nname)) {
        std::optional<std::string> updatableViaApex;

        forEachManifest([&](const ManifestWithDescription& mwd) {
            bool cont = mwd.manifest->forEachInstance([&](const auto& manifestInstance) {
                if (manifestInstance.format() != vintf::HalFormat::NATIVE) return true;
                if (manifestInstance.package() != nname.package) return true;
                if (manifestInstance.instance() != nname.instance) return true;
                updatableViaApex = manifestInstance.updatableViaApex();
                return false; // break (libvintf uses opposite convention)
            });
            return !cont;
        });

        return updatableViaApex;
    }

    AidlName aname;
    if (!AidlName::fill(name, &aname, true)) return std::nullopt;

    std::optional<std::string> updatableViaApex;

    forEachManifest([&](const ManifestWithDescription& mwd) {
        bool cont = mwd.manifest->forEachInstance([&](const auto& manifestInstance) {
            if (manifestInstance.format() != vintf::HalFormat::AIDL) return true;
            if (manifestInstance.package() != aname.package) return true;
            if (manifestInstance.interface() != aname.iface) return true;
            if (manifestInstance.instance() != aname.instance) return true;
            updatableViaApex = manifestInstance.updatableViaApex();
            return false; // break (libvintf uses opposite convention)
        });
        return !cont;
    });

    return updatableViaApex;
}

static std::vector<std::string> getVintfUpdatableNames(const std::string& apexName) {
    std::vector<std::string> names;

    forEachManifest([&](const ManifestWithDescription& mwd) {
        mwd.manifest->forEachInstance([&](const auto& manifestInstance) {
            if (manifestInstance.updatableViaApex().has_value() &&
                manifestInstance.updatableViaApex().value() == apexName) {
                if (manifestInstance.format() == vintf::HalFormat::NATIVE) {
                    names.push_back(getNativeInstanceName(manifestInstance));
                } else if (manifestInstance.format() == vintf::HalFormat::AIDL) {
                    names.push_back(getAidlInstanceName(manifestInstance));
                }
            }
            return true; // continue (libvintf uses opposite convention)
        });
        return false; // continue
    });

    return names;
}

static std::optional<std::string> getVintfAccessorName(const std::string& name) {
    AidlName aname;
    if (!AidlName::fill(name, &aname, false)) return std::nullopt;

    std::optional<std::string> accessor;
    forEachManifest([&](const ManifestWithDescription& mwd) {
        mwd.manifest->forEachInstance([&](const auto& manifestInstance) {
            if (manifestInstance.format() != vintf::HalFormat::AIDL) return true;
            if (manifestInstance.package() != aname.package) return true;
            if (manifestInstance.interface() != aname.iface) return true;
            if (manifestInstance.instance() != aname.instance) return true;
            accessor = manifestInstance.accessor();
            return false; // break (libvintf uses opposite convention)
        });
        return false; // continue
    });
    return accessor;
}

static std::optional<ConnectionInfo> getVintfConnectionInfo(const std::string& name) {
    AidlName aname;
    if (!AidlName::fill(name, &aname, true)) return std::nullopt;

    std::optional<std::string> ip;
    std::optional<uint64_t> port;
    forEachManifest([&](const ManifestWithDescription& mwd) {
        mwd.manifest->forEachInstance([&](const auto& manifestInstance) {
            if (manifestInstance.format() != vintf::HalFormat::AIDL) return true;
            if (manifestInstance.package() != aname.package) return true;
            if (manifestInstance.interface() != aname.iface) return true;
            if (manifestInstance.instance() != aname.instance) return true;
            ip = manifestInstance.ip();
            port = manifestInstance.port();
            return false; // break (libvintf uses opposite convention)
        });
        return false; // continue
    });

    if (ip.has_value() && port.has_value()) {
        ConnectionInfo info;
        info.ipAddress = *ip;
        info.port = *port;
        return std::make_optional<ConnectionInfo>(info);
    } else {
        return std::nullopt;
    }
}

static std::vector<std::string> getVintfInstances(const std::string& interface) {
    size_t lastDot = interface.rfind('.');
    if (lastDot == std::string::npos) {
        // This might be a package for native instance.
        std::vector<std::string> ret;
        (void)forEachManifest([&](const ManifestWithDescription& mwd) {
            auto instances = mwd.manifest->getNativeInstances(interface);
            ret.insert(ret.end(), instances.begin(), instances.end());
            return false; // continue
        });
        // If found, return it without error log.
        if (!ret.empty()) {
            return ret;
        }

        ALOGE("VINTF interfaces require names in Java package format (e.g. some.package.foo.IFoo) "
              "but got: %s",
              interface.c_str());
        return {};
    }
    const std::string package = interface.substr(0, lastDot);
    const std::string iface = interface.substr(lastDot+1);

    std::vector<std::string> ret;
    (void)forEachManifest([&](const ManifestWithDescription& mwd) {
        auto instances = mwd.manifest->getAidlInstances(package, iface);
        ret.insert(ret.end(), instances.begin(), instances.end());
        return false;  // continue
    });

    return ret;
}

static bool meetsDeclarationRequirements(const Access::CallingContext& ctx,
                                         const sp<IBinder>& binder, const std::string& name) {
    if (!Stability::requiresVintfDeclaration(binder)) {
        return true;
    }

    return isVintfDeclared(ctx, name);
}
#endif  // !VENDORSERVICEMANAGER

ServiceManager::Service::~Service() {
    if (hasClients) {
        // only expected to happen on process death, we don't store the service
        // name this late (it's in the map that holds this service), but if it
        // is happening, we might want to change 'unlinkToDeath' to explicitly
        // clear this bit so that we can abort in other cases, where it would
        // mean inconsistent logic in servicemanager (unexpected and tested, but
        // the original lazy service impl here had that bug).
        ALOGW("A service was removed when there are clients");
    }
}

ServiceManager::ServiceManager(std::unique_ptr<Access>&& access) : mAccess(std::move(access)) {
// TODO(b/151696835): reenable performance hack when we solve bug, since with
//     this hack and other fixes, it is unlikely we will see even an ephemeral
//     failure when the manifest parse fails. The goal is that the manifest will
//     be read incorrectly and cause the process trying to register a HAL to
//     fail. If this is in fact an early boot kernel contention issue, then we
//     will get no failure, and by its absence, be signalled to invest more
//     effort in re-adding this performance hack.
// #ifndef VENDORSERVICEMANAGER
//     // can process these at any times, don't want to delay first VINTF client
//     std::thread([] {
//         vintf::VintfObject::GetDeviceHalManifest();
//         vintf::VintfObject::GetFrameworkHalManifest();
//     }).detach();
// #endif  // !VENDORSERVICEMANAGER
}
ServiceManager::~ServiceManager() {
    // this should only happen in tests

    for (const auto& [name, callbacks] : mNameToRegistrationCallback) {
        CHECK(!callbacks.empty()) << name;
        for (const auto& callback : callbacks) {
            CHECK(callback != nullptr) << name;
        }
    }

    for (const auto& [name, service] : mNameToService) {
        CHECK(service.binder != nullptr) << name;
    }
}

Status ServiceManager::getService(const std::string& name, sp<IBinder>* outBinder) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    *outBinder = tryGetBinder(name, true).service;
    // returns ok regardless of result for legacy reasons
    return Status::ok();
}

Status ServiceManager::getService2(const std::string& name, os::Service* outService) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    *outService = tryGetService(name, true);
    // returns ok regardless of result for legacy reasons
    return Status::ok();
}

Status ServiceManager::checkService(const std::string& name, sp<IBinder>* outBinder) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    *outBinder = tryGetBinder(name, false).service;
    // returns ok regardless of result for legacy reasons
    return Status::ok();
}

Status ServiceManager::checkService2(const std::string& name, os::Service* outService) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    *outService = tryGetService(name, false);
    // returns ok regardless of result for legacy reasons
    return Status::ok();
}

os::Service ServiceManager::tryGetService(const std::string& name, bool startIfNotFound) {
    std::optional<std::string> accessorName;
#ifndef VENDORSERVICEMANAGER
    accessorName = getVintfAccessorName(name);
#endif
    if (accessorName.has_value()) {
        auto ctx = mAccess->getCallingContext();
        if (!mAccess->canFind(ctx, name)) {
            return os::Service::make<os::Service::Tag::accessor>(nullptr);
        }
        return os::Service::make<os::Service::Tag::accessor>(
                tryGetBinder(*accessorName, startIfNotFound).service);
    } else {
        return os::Service::make<os::Service::Tag::serviceWithMetadata>(
                tryGetBinder(name, startIfNotFound));
    }
}

os::ServiceWithMetadata ServiceManager::tryGetBinder(const std::string& name,
                                                     bool startIfNotFound) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    auto ctx = mAccess->getCallingContext();

    sp<IBinder> out;
    Service* service = nullptr;
    if (auto it = mNameToService.find(name); it != mNameToService.end()) {
        service = &(it->second);

        if (!service->allowIsolated && is_multiuser_uid_isolated(ctx.uid)) {
            LOG(WARNING) << "Isolated app with UID " << ctx.uid << " requested '" << name
                         << "', but the service is not allowed for isolated apps.";
            return os::ServiceWithMetadata();
        }
        out = service->binder;
    }

    if (!mAccess->canFind(ctx, name)) {
        return os::ServiceWithMetadata();
    }

    if (!out && startIfNotFound) {
        tryStartService(ctx, name);
    }

    if (out) {
        // Force onClients to get sent, and then make sure the timerfd won't clear it
        // by setting guaranteeClient again. This logic could be simplified by using
        // a time-based guarantee. However, forcing onClients(true) to get sent
        // right here is always going to be important for processes serving multiple
        // lazy interfaces.
        service->guaranteeClient = true;
        CHECK(handleServiceClientCallback(2 /* sm + transaction */, name, false));
        service->guaranteeClient = true;
    }
    os::ServiceWithMetadata serviceWithMetadata = os::ServiceWithMetadata();
    serviceWithMetadata.service = out;
    serviceWithMetadata.isLazyService =
            service ? service->dumpPriority & FLAG_IS_LAZY_SERVICE : false;
    return serviceWithMetadata;
}

bool isValidServiceName(const std::string& name) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    if (name.size() == 0) return false;
    if (name.size() > 127) return false;

    for (char c : name) {
        if (c == '_' || c == '-' || c == '.' || c == '/') continue;
        if (c >= 'a' && c <= 'z') continue;
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= '0' && c <= '9') continue;
        return false;
    }

    return true;
}

Status ServiceManager::addService(const std::string& name, const sp<IBinder>& binder, bool allowIsolated, int32_t dumpPriority) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    auto ctx = mAccess->getCallingContext();

    if (multiuser_get_app_id(ctx.uid) >= AID_APP) {
        return Status::fromExceptionCode(Status::EX_SECURITY, "App UIDs cannot add services.");
    }

    std::optional<std::string> accessorName;
    if (auto status = canAddService(ctx, name, &accessorName); !status.isOk()) {
        return status;
    }

    if (binder == nullptr) {
        return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT, "Null binder.");
    }

    if (!isValidServiceName(name)) {
        ALOGE("%s Invalid service name: %s", ctx.toDebugString().c_str(), name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT, "Invalid service name.");
    }

#ifndef VENDORSERVICEMANAGER
    if (!meetsDeclarationRequirements(ctx, binder, name)) {
        // already logged
        return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT, "VINTF declaration error.");
    }
#endif  // !VENDORSERVICEMANAGER

    if ((dumpPriority & DUMP_FLAG_PRIORITY_ALL) == 0) {
        ALOGW("%s Dump flag priority is not set when adding %s", ctx.toDebugString().c_str(),
              name.c_str());
    }

    // implicitly unlinked when the binder is removed
    if (binder->remoteBinder() != nullptr &&
        binder->linkToDeath(sp<ServiceManager>::fromExisting(this)) != OK) {
        ALOGE("%s Could not linkToDeath when adding %s", ctx.toDebugString().c_str(), name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE, "Couldn't linkToDeath.");
    }

    auto it = mNameToService.find(name);
    bool prevClients = false;
    if (it != mNameToService.end()) {
        const Service& existing = it->second;
        prevClients = existing.hasClients;

        // We could do better than this because if the other service dies, it
        // may not have an entry here. However, this case is unlikely. We are
        // only trying to detect when two different services are accidentally installed.

        if (existing.ctx.uid != ctx.uid) {
            ALOGW("Service '%s' originally registered from UID %u but it is now being registered "
                  "from UID %u. Multiple instances installed?",
                  name.c_str(), existing.ctx.uid, ctx.uid);
        }

        if (existing.ctx.sid != ctx.sid) {
            ALOGW("Service '%s' originally registered from SID %s but it is now being registered "
                  "from SID %s. Multiple instances installed?",
                  name.c_str(), existing.ctx.sid.c_str(), ctx.sid.c_str());
        }

        ALOGI("Service '%s' originally registered from PID %d but it is being registered again "
              "from PID %d. Bad state? Late death notification? Multiple instances installed?",
              name.c_str(), existing.ctx.debugPid, ctx.debugPid);
    }

    // Overwrite the old service if it exists
    mNameToService[name] = Service{
            .binder = binder,
            .allowIsolated = allowIsolated,
            .dumpPriority = dumpPriority,
            .hasClients = prevClients, // see b/279898063, matters if existing callbacks
            .guaranteeClient = false,
            .ctx = ctx,
    };

    if (auto it = mNameToRegistrationCallback.find(name); it != mNameToRegistrationCallback.end()) {
        // If someone is currently waiting on the service, notify the service that
        // we're waiting and flush it to the service.
        mNameToService[name].guaranteeClient = true;
        CHECK(handleServiceClientCallback(2 /* sm + transaction */, name, false));
        mNameToService[name].guaranteeClient = true;

        for (const sp<IServiceCallback>& cb : it->second) {
            // permission checked in registerForNotifications
            cb->onRegistration(name, binder);
        }
    }

    return Status::ok();
}

Status ServiceManager::listServices(int32_t dumpPriority, std::vector<std::string>* outList) {
    SM_PERFETTO_TRACE_FUNC();

    if (!mAccess->canList(mAccess->getCallingContext())) {
        return Status::fromExceptionCode(Status::EX_SECURITY, "SELinux denied.");
    }

    size_t toReserve = 0;
    for (auto const& [name, service] : mNameToService) {
        (void) name;

        if (service.dumpPriority & dumpPriority) ++toReserve;
    }

    CHECK(outList->empty());

    outList->reserve(toReserve);
    for (auto const& [name, service] : mNameToService) {
        (void) service;

        if (service.dumpPriority & dumpPriority) {
            outList->push_back(name);
        }
    }

    return Status::ok();
}

Status ServiceManager::registerForNotifications(
        const std::string& name, const sp<IServiceCallback>& callback) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    auto ctx = mAccess->getCallingContext();

    // TODO(b/338541373): Implement the notification mechanism for services accessed via
    // IAccessor.
    std::optional<std::string> accessorName;
    if (auto status = canFindService(ctx, name, &accessorName); !status.isOk()) {
        return status;
    }

    // note - we could allow isolated apps to get notifications if we
    // keep track of isolated callbacks and non-isolated callbacks, but
    // this is done since isolated apps shouldn't access lazy services
    // so we should be able to use different APIs to keep things simple.
    // Here, we disallow everything, because the service might not be
    // registered yet.
    if (is_multiuser_uid_isolated(ctx.uid)) {
        return Status::fromExceptionCode(Status::EX_SECURITY, "isolated app");
    }

    if (!isValidServiceName(name)) {
        ALOGE("%s Invalid service name: %s", ctx.toDebugString().c_str(), name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT, "Invalid service name.");
    }

    if (callback == nullptr) {
        return Status::fromExceptionCode(Status::EX_NULL_POINTER, "Null callback.");
    }

    if (OK !=
        IInterface::asBinder(callback)->linkToDeath(
                sp<ServiceManager>::fromExisting(this))) {
        ALOGE("%s Could not linkToDeath when adding %s", ctx.toDebugString().c_str(), name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE, "Couldn't link to death.");
    }

    mNameToRegistrationCallback[name].push_back(callback);

    if (auto it = mNameToService.find(name); it != mNameToService.end()) {
        const sp<IBinder>& binder = it->second.binder;

        // never null if an entry exists
        CHECK(binder != nullptr) << name;
        callback->onRegistration(name, binder);
    }

    return Status::ok();
}
Status ServiceManager::unregisterForNotifications(
        const std::string& name, const sp<IServiceCallback>& callback) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    auto ctx = mAccess->getCallingContext();

    std::optional<std::string> accessorName;
    if (auto status = canFindService(ctx, name, &accessorName); !status.isOk()) {
        return status;
    }

    bool found = false;

    auto it = mNameToRegistrationCallback.find(name);
    if (it != mNameToRegistrationCallback.end()) {
        removeRegistrationCallback(IInterface::asBinder(callback), &it, &found);
    }

    if (!found) {
        ALOGE("%s Trying to unregister callback, but none exists %s", ctx.toDebugString().c_str(),
              name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE, "Nothing to unregister.");
    }

    return Status::ok();
}

Status ServiceManager::isDeclared(const std::string& name, bool* outReturn) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    auto ctx = mAccess->getCallingContext();

    std::optional<std::string> accessorName;
    if (auto status = canFindService(ctx, name, &accessorName); !status.isOk()) {
        return status;
    }

    *outReturn = false;

#ifndef VENDORSERVICEMANAGER
    *outReturn = isVintfDeclared(ctx, name);
#endif
    return Status::ok();
}

binder::Status ServiceManager::getDeclaredInstances(const std::string& interface, std::vector<std::string>* outReturn) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoInterfaceName, interface.c_str())));

    auto ctx = mAccess->getCallingContext();

    std::vector<std::string> allInstances;
#ifndef VENDORSERVICEMANAGER
    allInstances = getVintfInstances(interface);
#endif

    outReturn->clear();

    std::optional<std::string> _accessorName;
    for (const std::string& instance : allInstances) {
        if (auto status = canFindService(ctx, interface + "/" + instance, &_accessorName);
            status.isOk()) {
            outReturn->push_back(instance);
        }
    }

    if (outReturn->size() == 0 && allInstances.size() != 0) {
        return Status::fromExceptionCode(Status::EX_SECURITY, "SELinux denied.");
    }

    return Status::ok();
}

Status ServiceManager::updatableViaApex(const std::string& name,
                                        std::optional<std::string>* outReturn) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    auto ctx = mAccess->getCallingContext();

    std::optional<std::string> _accessorName;
    if (auto status = canFindService(ctx, name, &_accessorName); !status.isOk()) {
        return status;
    }

    *outReturn = std::nullopt;

#ifndef VENDORSERVICEMANAGER
    *outReturn = getVintfUpdatableApex(name);
#endif
    return Status::ok();
}

Status ServiceManager::getUpdatableNames([[maybe_unused]] const std::string& apexName,
                                         std::vector<std::string>* outReturn) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoApexName, apexName.c_str())));

    auto ctx = mAccess->getCallingContext();

    std::vector<std::string> apexUpdatableNames;
#ifndef VENDORSERVICEMANAGER
    apexUpdatableNames = getVintfUpdatableNames(apexName);
#endif

    outReturn->clear();

    std::optional<std::string> _accessorName;
    for (const std::string& name : apexUpdatableNames) {
        if (auto status = canFindService(ctx, name, &_accessorName); status.isOk()) {
            outReturn->push_back(name);
        }
    }

    if (outReturn->size() == 0 && apexUpdatableNames.size() != 0) {
        return Status::fromExceptionCode(Status::EX_SECURITY, "SELinux denied.");
    }
    return Status::ok();
}

Status ServiceManager::getConnectionInfo(const std::string& name,
                                         std::optional<ConnectionInfo>* outReturn) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    auto ctx = mAccess->getCallingContext();

    std::optional<std::string> _accessorName;
    if (auto status = canFindService(ctx, name, &_accessorName); !status.isOk()) {
        return status;
    }

    *outReturn = std::nullopt;

#ifndef VENDORSERVICEMANAGER
    *outReturn = getVintfConnectionInfo(name);
#endif
    return Status::ok();
}

void ServiceManager::removeRegistrationCallback(const wp<IBinder>& who,
                                    ServiceCallbackMap::iterator* it,
                                    bool* found) {
    SM_PERFETTO_TRACE_FUNC();

    std::vector<sp<IServiceCallback>>& listeners = (*it)->second;

    for (auto lit = listeners.begin(); lit != listeners.end();) {
        if (IInterface::asBinder(*lit) == who) {
            if(found) *found = true;
            lit = listeners.erase(lit);
        } else {
            ++lit;
        }
    }

    if (listeners.empty()) {
        *it = mNameToRegistrationCallback.erase(*it);
    } else {
        (*it)++;
    }
}

void ServiceManager::binderDied(const wp<IBinder>& who) {
    SM_PERFETTO_TRACE_FUNC();

    for (auto it = mNameToService.begin(); it != mNameToService.end();) {
        if (who == it->second.binder) {
            // TODO: currently, this entry contains the state also
            // associated with mNameToClientCallback. If we allowed
            // other processes to register client callbacks, we
            // would have to preserve hasClients (perhaps moving
            // that state into mNameToClientCallback, which is complicated
            // because those callbacks are associated w/ particular binder
            // objects, though they are indexed by name now, they may
            // need to be indexed by binder at that point).
            it = mNameToService.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = mNameToRegistrationCallback.begin(); it != mNameToRegistrationCallback.end();) {
        removeRegistrationCallback(who, &it, nullptr /*found*/);
    }

    for (auto it = mNameToClientCallback.begin(); it != mNameToClientCallback.end();) {
        removeClientCallback(who, &it);
    }
}

void ServiceManager::tryStartService(const Access::CallingContext& ctx, const std::string& name) {
    ALOGI("%s Since '%s' could not be found trying to start it as a lazy AIDL service. (if it's "
          "not configured to be a lazy service, it may be stuck starting or still starting).",
          ctx.toDebugString().c_str(), name.c_str());

    std::thread([=] {
        if (!base::SetProperty("ctl.interface_start", "aidl/" + name)) {
            ALOGI("%s Tried to start aidl service %s as a lazy service, but was unable to. Usually "
                  "this happens when a service is not installed, but if the service is intended to "
                  "be used as a lazy service, then it may be configured incorrectly.",
                  ctx.toDebugString().c_str(), name.c_str());
        }
    }).detach();
}

Status ServiceManager::registerClientCallback(const std::string& name, const sp<IBinder>& service,
                                              const sp<IClientCallback>& cb) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    if (cb == nullptr) {
        return Status::fromExceptionCode(Status::EX_NULL_POINTER, "Callback null.");
    }

    auto ctx = mAccess->getCallingContext();
    std::optional<std::string> accessorName;
    if (auto status = canAddService(ctx, name, &accessorName); !status.isOk()) {
        return status;
    }

    auto serviceIt = mNameToService.find(name);
    if (serviceIt == mNameToService.end()) {
        ALOGE("%s Could not add callback for nonexistent service: %s", ctx.toDebugString().c_str(),
              name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT, "Service doesn't exist.");
    }

    if (serviceIt->second.ctx.debugPid != IPCThreadState::self()->getCallingPid()) {
        ALOGW("%s Only a server can register for client callbacks (for %s)",
              ctx.toDebugString().c_str(), name.c_str());
        return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
                                         "Only service can register client callback for itself.");
    }

    if (serviceIt->second.binder != service) {
        ALOGW("%s Tried to register client callback for %s but a different service is registered "
              "under this name.",
              ctx.toDebugString().c_str(), name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT, "Service mismatch.");
    }

    if (OK !=
        IInterface::asBinder(cb)->linkToDeath(sp<ServiceManager>::fromExisting(this))) {
        ALOGE("%s Could not linkToDeath when adding client callback for %s",
              ctx.toDebugString().c_str(), name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE, "Couldn't linkToDeath.");
    }

    // WARNING: binderDied makes an assumption about this. If we open up client
    // callbacks to other services, certain race conditions may lead to services
    // getting extra client callback notifications.
    // Make sure all callbacks have been told about a consistent state - b/278038751
    if (serviceIt->second.hasClients) {
        cb->onClients(service, true);
    }

    mNameToClientCallback[name].push_back(cb);

    // Flush updated info to client callbacks (especially if guaranteeClient
    // and !hasClient, see b/285202885). We may or may not have clients at
    // this point, so ignore the return value.
    (void)handleServiceClientCallback(2 /* sm + transaction */, name, false);

    return Status::ok();
}

void ServiceManager::removeClientCallback(const wp<IBinder>& who,
                                          ClientCallbackMap::iterator* it) {
    std::vector<sp<IClientCallback>>& listeners = (*it)->second;

    for (auto lit = listeners.begin(); lit != listeners.end();) {
        if (IInterface::asBinder(*lit) == who) {
            lit = listeners.erase(lit);
        } else {
            ++lit;
        }
    }

    if (listeners.empty()) {
        *it = mNameToClientCallback.erase(*it);
    } else {
        (*it)++;
    }
}

ssize_t ServiceManager::Service::getNodeStrongRefCount() {
    sp<BpBinder> bpBinder = sp<BpBinder>::fromExisting(binder->remoteBinder());
    if (bpBinder == nullptr) return -1;

    return ProcessState::self()->getStrongRefCountForNode(bpBinder);
}

void ServiceManager::handleClientCallbacks() {
    for (const auto& [name, service] : mNameToService) {
        handleServiceClientCallback(1 /* sm has one refcount */, name, true);
    }
}

bool ServiceManager::handleServiceClientCallback(size_t knownClients,
                                                 const std::string& serviceName,
                                                 bool isCalledOnInterval) {
    auto serviceIt = mNameToService.find(serviceName);
    if (serviceIt == mNameToService.end() || mNameToClientCallback.count(serviceName) < 1) {
        return true; // return we do have clients a.k.a. DON'T DO ANYTHING
    }

    Service& service = serviceIt->second;
    ssize_t count = service.getNodeStrongRefCount();

    // binder driver doesn't support this feature, consider we have clients
    if (count == -1) return true;

    bool hasKernelReportedClients = static_cast<size_t>(count) > knownClients;

    if (service.guaranteeClient) {
        if (!service.hasClients && !hasKernelReportedClients) {
            sendClientCallbackNotifications(serviceName, true,
                                            "service is guaranteed to be in use");
        }

        // guarantee is temporary
        service.guaranteeClient = false;
    }

    // Regardless of this situation, we want to give this notification as soon as possible.
    // This way, we have a chance of preventing further thrashing.
    if (hasKernelReportedClients && !service.hasClients) {
        sendClientCallbackNotifications(serviceName, true, "we now have a record of a client");
    }

    // But limit rate of shutting down service.
    if (isCalledOnInterval) {
        if (!hasKernelReportedClients && service.hasClients) {
            sendClientCallbackNotifications(serviceName, false,
                                            "we now have no record of a client");
        }
    }

    // May be different than 'hasKernelReportedClients'. We intentionally delay
    // information about clients going away to reduce thrashing.
    return service.hasClients;
}

void ServiceManager::sendClientCallbackNotifications(const std::string& serviceName,
                                                     bool hasClients, const char* context) {
    auto serviceIt = mNameToService.find(serviceName);
    if (serviceIt == mNameToService.end()) {
        ALOGW("sendClientCallbackNotifications could not find service %s when %s",
              serviceName.c_str(), context);
        return;
    }
    Service& service = serviceIt->second;

    CHECK_NE(hasClients, service.hasClients) << context;

    ALOGI("Notifying %s they %s (previously: %s) have clients when %s", serviceName.c_str(),
          hasClients ? "do" : "don't", service.hasClients ? "do" : "don't", context);

    auto ccIt = mNameToClientCallback.find(serviceName);
    CHECK(ccIt != mNameToClientCallback.end())
            << "sendClientCallbackNotifications could not find callbacks for service when "
            << context;

    for (const auto& callback : ccIt->second) {
        callback->onClients(service.binder, hasClients);
    }

    service.hasClients = hasClients;
}

Status ServiceManager::tryUnregisterService(const std::string& name, const sp<IBinder>& binder) {
    SM_PERFETTO_TRACE_FUNC(PERFETTO_TE_PROTO_FIELDS(
            PERFETTO_TE_PROTO_FIELD_CSTR(kProtoServiceName, name.c_str())));

    if (binder == nullptr) {
        return Status::fromExceptionCode(Status::EX_NULL_POINTER, "Null service.");
    }

    auto ctx = mAccess->getCallingContext();
    std::optional<std::string> accessorName;
    if (auto status = canAddService(ctx, name, &accessorName); !status.isOk()) {
        return status;
    }

    auto serviceIt = mNameToService.find(name);
    if (serviceIt == mNameToService.end()) {
        ALOGW("%s Tried to unregister %s, but that service wasn't registered to begin with.",
              ctx.toDebugString().c_str(), name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE, "Service not registered.");
    }

    if (serviceIt->second.ctx.debugPid != IPCThreadState::self()->getCallingPid()) {
        ALOGW("%s Only a server can unregister itself (for %s)", ctx.toDebugString().c_str(),
              name.c_str());
        return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
                                         "Service can only unregister itself.");
    }

    sp<IBinder> storedBinder = serviceIt->second.binder;

    if (binder != storedBinder) {
        ALOGW("%s Tried to unregister %s, but a different service is registered under this name.",
              ctx.toDebugString().c_str(), name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE,
                                         "Different service registered under this name.");
    }

    // important because we don't have timer-based guarantees, we don't want to clear
    // this
    if (serviceIt->second.guaranteeClient) {
        ALOGI("%s Tried to unregister %s, but there is about to be a client.",
              ctx.toDebugString().c_str(), name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE,
                                         "Can't unregister, pending client.");
    }

    // - kernel driver will hold onto one refcount (during this transaction)
    // - servicemanager has a refcount (guaranteed by this transaction)
    constexpr size_t kKnownClients = 2;

    if (handleServiceClientCallback(kKnownClients, name, false)) {
        ALOGI("%s Tried to unregister %s, but there are clients.", ctx.toDebugString().c_str(),
              name.c_str());

        // Since we had a failed registration attempt, and the HIDL implementation of
        // delaying service shutdown for multiple periods wasn't ported here... this may
        // help reduce thrashing, but we should be able to remove it.
        serviceIt->second.guaranteeClient = true;

        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE,
                                         "Can't unregister, known client.");
    }

    ALOGI("%s Unregistering %s", ctx.toDebugString().c_str(), name.c_str());
    mNameToService.erase(name);

    return Status::ok();
}

Status ServiceManager::canAddService(const Access::CallingContext& ctx, const std::string& name,
                                     std::optional<std::string>* accessor) {
    if (!mAccess->canAdd(ctx, name)) {
        return Status::fromExceptionCode(Status::EX_SECURITY, "SELinux denied for service.");
    }
#ifndef VENDORSERVICEMANAGER
    *accessor = getVintfAccessorName(name);
#endif
    if (accessor->has_value()) {
        if (!mAccess->canAdd(ctx, accessor->value())) {
            return Status::fromExceptionCode(Status::EX_SECURITY,
                                             "SELinux denied for the accessor of the service.");
        }
    }
    return Status::ok();
}

Status ServiceManager::canFindService(const Access::CallingContext& ctx, const std::string& name,
                                      std::optional<std::string>* accessor) {
    if (!mAccess->canFind(ctx, name)) {
        return Status::fromExceptionCode(Status::EX_SECURITY, "SELinux denied for service.");
    }
#ifndef VENDORSERVICEMANAGER
    *accessor = getVintfAccessorName(name);
#endif
    if (accessor->has_value()) {
        if (!mAccess->canFind(ctx, accessor->value())) {
            return Status::fromExceptionCode(Status::EX_SECURITY,
                                             "SELinux denied for the accessor of the service.");
        }
    }
    return Status::ok();
}

Status ServiceManager::getServiceDebugInfo(std::vector<ServiceDebugInfo>* outReturn) {
    SM_PERFETTO_TRACE_FUNC();
    if (!mAccess->canList(mAccess->getCallingContext())) {
        return Status::fromExceptionCode(Status::EX_SECURITY, "SELinux denied.");
    }

    outReturn->reserve(mNameToService.size());
    for (auto const& [name, service] : mNameToService) {
        ServiceDebugInfo info;
        info.name = name;
        info.debugPid = service.ctx.debugPid;

        outReturn->push_back(std::move(info));
    }

    return Status::ok();
}

void ServiceManager::clear() {
    mNameToService.clear();
    mNameToRegistrationCallback.clear();
    mNameToClientCallback.clear();
}

}  // namespace android
