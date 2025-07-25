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

#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android/os/BnServiceCallback.h>
#include <binder/Binder.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <cutils/android_filesystem_config.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Access.h"
#include "ServiceManager.h"

using android::Access;
using android::BBinder;
using android::IBinder;
using android::ServiceManager;
using android::sp;
using android::base::EndsWith;
using android::base::GetProperty;
using android::base::StartsWith;
using android::binder::Status;
using android::os::BnServiceCallback;
using android::os::IServiceManager;
using android::os::Service;
using testing::_;
using testing::ElementsAre;
using testing::NiceMock;
using testing::Return;

static sp<IBinder> getBinder() {
    class LinkableBinder : public BBinder {
        android::status_t linkToDeath(const sp<DeathRecipient>&, void*, uint32_t) override {
            // let SM linkToDeath
            return android::OK;
        }
    };

    return sp<LinkableBinder>::make();
}

class MockAccess : public Access {
public:
    MOCK_METHOD0(getCallingContext, CallingContext());
    MOCK_METHOD2(canAdd, bool(const CallingContext&, const std::string& name));
    MOCK_METHOD2(canFind, bool(const CallingContext&, const std::string& name));
    MOCK_METHOD1(canList, bool(const CallingContext&));
};

class MockServiceManager : public ServiceManager {
 public:
    MockServiceManager(std::unique_ptr<Access>&& access) : ServiceManager(std::move(access)) {}
    MOCK_METHOD2(tryStartService, void(const Access::CallingContext&, const std::string& name));
};

static sp<ServiceManager> getPermissiveServiceManager() {
    std::unique_ptr<MockAccess> access = std::make_unique<NiceMock<MockAccess>>();

    ON_CALL(*access, getCallingContext()).WillByDefault(Return(Access::CallingContext{}));
    ON_CALL(*access, canAdd(_, _)).WillByDefault(Return(true));
    ON_CALL(*access, canFind(_, _)).WillByDefault(Return(true));
    ON_CALL(*access, canList(_)).WillByDefault(Return(true));

    sp<ServiceManager> sm = sp<NiceMock<MockServiceManager>>::make(std::move(access));
    return sm;
}

// Determines if test device is a cuttlefish phone device
static bool isCuttlefishPhone() {
    auto device = GetProperty("ro.product.vendor.device", "");
    auto product = GetProperty("ro.product.vendor.name", "");
    return StartsWith(device, "vsoc_") && EndsWith(product, "_phone");
}

TEST(AddService, HappyHappy) {
    auto sm = getPermissiveServiceManager();
    EXPECT_TRUE(sm->addService("foo", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());

    EXPECT_TRUE(sm->addService("lazyfoo", getBinder(), false /*allowIsolated*/,
                               IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT |
                                       IServiceManager::FLAG_IS_LAZY_SERVICE)
                        .isOk());
}

TEST(AddService, EmptyNameDisallowed) {
    auto sm = getPermissiveServiceManager();
    EXPECT_FALSE(sm->addService("", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());
}

TEST(AddService, JustShortEnoughServiceNameHappy) {
    auto sm = getPermissiveServiceManager();
    EXPECT_TRUE(sm->addService(std::string(127, 'a'), getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());
}

TEST(AddService, TooLongNameDisallowed) {
    auto sm = getPermissiveServiceManager();
    EXPECT_FALSE(sm->addService(std::string(128, 'a'), getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());
}

TEST(AddService, WeirdCharactersDisallowed) {
    auto sm = getPermissiveServiceManager();
    EXPECT_FALSE(sm->addService("happy$foo$foo", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());
}

TEST(AddService, AddNullServiceDisallowed) {
    auto sm = getPermissiveServiceManager();
    EXPECT_FALSE(sm->addService("foo", nullptr, false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());
}

TEST(AddService, AddDisallowedFromApp) {
    for (uid_t uid : { AID_APP_START, AID_APP_START + 1, AID_APP_END }) {
        std::unique_ptr<MockAccess> access = std::make_unique<NiceMock<MockAccess>>();
        EXPECT_CALL(*access, getCallingContext()).WillOnce(Return(Access::CallingContext{
            .debugPid = 1337,
            .uid = uid,
        }));
        EXPECT_CALL(*access, canAdd(_, _)).Times(0);
        sp<ServiceManager> sm = sp<NiceMock<MockServiceManager>>::make(std::move(access));

        EXPECT_FALSE(sm->addService("foo", getBinder(), false /*allowIsolated*/,
            IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());
    }

}

TEST(AddService, HappyOverExistingService) {
    auto sm = getPermissiveServiceManager();
    EXPECT_TRUE(sm->addService("foo", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());
    EXPECT_TRUE(sm->addService("foo", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());
}

TEST(AddService, OverwriteExistingService) {
    auto sm = getPermissiveServiceManager();
    sp<IBinder> serviceA = getBinder();
    EXPECT_TRUE(sm->addService("foo", serviceA, false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());

    Service outA;
    EXPECT_TRUE(sm->getService2("foo", &outA).isOk());
    EXPECT_EQ(serviceA, outA.get<Service::Tag::serviceWithMetadata>().service);
    sp<IBinder> outBinderA;
    EXPECT_TRUE(sm->getService("foo", &outBinderA).isOk());
    EXPECT_EQ(serviceA, outBinderA);

    // serviceA should be overwritten by serviceB
    sp<IBinder> serviceB = getBinder();
    EXPECT_TRUE(sm->addService("foo", serviceB, false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());

    Service outB;
    EXPECT_TRUE(sm->getService2("foo", &outB).isOk());
    EXPECT_EQ(serviceB, outB.get<Service::Tag::serviceWithMetadata>().service);
    sp<IBinder> outBinderB;
    EXPECT_TRUE(sm->getService("foo", &outBinderB).isOk());
    EXPECT_EQ(serviceB, outBinderB);
}

TEST(AddService, NoPermissions) {
    std::unique_ptr<MockAccess> access = std::make_unique<NiceMock<MockAccess>>();

    EXPECT_CALL(*access, getCallingContext()).WillOnce(Return(Access::CallingContext{}));
    EXPECT_CALL(*access, canAdd(_, _)).WillOnce(Return(false));

    sp<ServiceManager> sm = sp<NiceMock<MockServiceManager>>::make(std::move(access));

    EXPECT_FALSE(sm->addService("foo", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());
}

TEST(GetService, HappyHappy) {
    auto sm = getPermissiveServiceManager();
    sp<IBinder> service = getBinder();

    EXPECT_TRUE(sm->addService("foo", service, false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());

    Service out;
    EXPECT_TRUE(sm->getService2("foo", &out).isOk());
    EXPECT_EQ(service, out.get<Service::Tag::serviceWithMetadata>().service);
    sp<IBinder> outBinder;
    EXPECT_TRUE(sm->getService("foo", &outBinder).isOk());
    EXPECT_EQ(service, outBinder);

    EXPECT_TRUE(sm->checkService2("foo", &out).isOk());
    EXPECT_EQ(service, out.get<Service::Tag::serviceWithMetadata>().service);
    EXPECT_TRUE(sm->checkService("foo", &outBinder).isOk());
    EXPECT_EQ(service, outBinder);
}

TEST(GetService, NonExistant) {
    auto sm = getPermissiveServiceManager();

    Service out;
    EXPECT_TRUE(sm->getService2("foo", &out).isOk());
    EXPECT_EQ(nullptr, out.get<Service::Tag::serviceWithMetadata>().service);
    sp<IBinder> outBinder;
    EXPECT_TRUE(sm->getService("foo", &outBinder).isOk());
    EXPECT_EQ(nullptr, outBinder);
}

TEST(GetService, NoPermissionsForGettingService) {
    std::unique_ptr<MockAccess> access = std::make_unique<NiceMock<MockAccess>>();

    EXPECT_CALL(*access, getCallingContext()).WillRepeatedly(Return(Access::CallingContext{}));
    EXPECT_CALL(*access, canAdd(_, _)).WillOnce(Return(true));
    EXPECT_CALL(*access, canFind(_, _)).WillRepeatedly(Return(false));

    sp<ServiceManager> sm = sp<NiceMock<MockServiceManager>>::make(std::move(access));

    EXPECT_TRUE(sm->addService("foo", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());

    Service out;
    // returns nullptr but has OK status for legacy compatibility
    EXPECT_TRUE(sm->getService2("foo", &out).isOk());
    EXPECT_EQ(nullptr, out.get<Service::Tag::serviceWithMetadata>().service);
    sp<IBinder> outBinder;
    EXPECT_TRUE(sm->getService("foo", &outBinder).isOk());
    EXPECT_EQ(nullptr, outBinder);
}

TEST(GetService, AllowedFromIsolated) {
    std::unique_ptr<MockAccess> access = std::make_unique<NiceMock<MockAccess>>();

    EXPECT_CALL(*access, getCallingContext())
            // something adds it
            .WillOnce(Return(Access::CallingContext{}))
            // next calls is from isolated app
            .WillOnce(Return(Access::CallingContext{
                    .uid = AID_ISOLATED_START,
            }))
            .WillOnce(Return(Access::CallingContext{
                    .uid = AID_ISOLATED_START,
            }));
    EXPECT_CALL(*access, canAdd(_, _)).WillOnce(Return(true));
    EXPECT_CALL(*access, canFind(_, _)).WillRepeatedly(Return(true));

    sp<ServiceManager> sm = sp<NiceMock<MockServiceManager>>::make(std::move(access));

    sp<IBinder> service = getBinder();
    EXPECT_TRUE(sm->addService("foo", service, true /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());

    Service out;
    EXPECT_TRUE(sm->getService2("foo", &out).isOk());
    EXPECT_EQ(service, out.get<Service::Tag::serviceWithMetadata>().service);
    sp<IBinder> outBinder;
    EXPECT_TRUE(sm->getService("foo", &outBinder).isOk());
    EXPECT_EQ(service, outBinder);
}

TEST(GetService, NotAllowedFromIsolated) {
    std::unique_ptr<MockAccess> access = std::make_unique<NiceMock<MockAccess>>();

    EXPECT_CALL(*access, getCallingContext())
            // something adds it
            .WillOnce(Return(Access::CallingContext{}))
            // next calls is from isolated app
            .WillOnce(Return(Access::CallingContext{
                    .uid = AID_ISOLATED_START,
            }))
            .WillOnce(Return(Access::CallingContext{
                    .uid = AID_ISOLATED_START,
            }));
    EXPECT_CALL(*access, canAdd(_, _)).WillOnce(Return(true));

    // TODO(b/136023468): when security check is first, this should be called first
    // EXPECT_CALL(*access, canFind(_, _)).WillOnce(Return(true));

    sp<ServiceManager> sm = sp<NiceMock<MockServiceManager>>::make(std::move(access));

    EXPECT_TRUE(sm->addService("foo", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());

    Service out;
    // returns nullptr but has OK status for legacy compatibility
    EXPECT_TRUE(sm->getService2("foo", &out).isOk());
    EXPECT_EQ(nullptr, out.get<Service::Tag::serviceWithMetadata>().service);
    sp<IBinder> outBinder;
    EXPECT_TRUE(sm->getService("foo", &outBinder).isOk());
    EXPECT_EQ(nullptr, outBinder);
}

TEST(ListServices, NoPermissions) {
    std::unique_ptr<MockAccess> access = std::make_unique<NiceMock<MockAccess>>();

    EXPECT_CALL(*access, getCallingContext()).WillOnce(Return(Access::CallingContext{}));
    EXPECT_CALL(*access, canList(_)).WillOnce(Return(false));

    sp<ServiceManager> sm = sp<NiceMock<MockServiceManager>>::make(std::move(access));

    std::vector<std::string> out;
    EXPECT_FALSE(sm->listServices(IServiceManager::DUMP_FLAG_PRIORITY_ALL, &out).isOk());
    EXPECT_TRUE(out.empty());
}

TEST(ListServices, AllServices) {
    auto sm = getPermissiveServiceManager();

    EXPECT_TRUE(sm->addService("sd", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());
    EXPECT_TRUE(sm->addService("sc", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_NORMAL).isOk());
    EXPECT_TRUE(sm->addService("sb", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_HIGH).isOk());
    EXPECT_TRUE(sm->addService("sa", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_CRITICAL).isOk());

    std::vector<std::string> out;
    EXPECT_TRUE(sm->listServices(IServiceManager::DUMP_FLAG_PRIORITY_ALL, &out).isOk());

    // all there and in the right order
    EXPECT_THAT(out, ElementsAre("sa", "sb", "sc", "sd"));
}

TEST(ListServices, CriticalServices) {
    auto sm = getPermissiveServiceManager();

    EXPECT_TRUE(sm->addService("sd", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());
    EXPECT_TRUE(sm->addService("sc", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_NORMAL).isOk());
    EXPECT_TRUE(sm->addService("sb", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_HIGH).isOk());
    EXPECT_TRUE(sm->addService("sa", getBinder(), false /*allowIsolated*/,
        IServiceManager::DUMP_FLAG_PRIORITY_CRITICAL).isOk());

    std::vector<std::string> out;
    EXPECT_TRUE(sm->listServices(IServiceManager::DUMP_FLAG_PRIORITY_CRITICAL, &out).isOk());

    // all there and in the right order
    EXPECT_THAT(out, ElementsAre("sa"));
}

TEST(Vintf, UpdatableViaApex) {
    if (!isCuttlefishPhone()) GTEST_SKIP() << "Skipping non-Cuttlefish-phone devices";

    auto sm = getPermissiveServiceManager();
    std::optional<std::string> updatableViaApex;
    EXPECT_TRUE(sm->updatableViaApex("android.hardware.camera.provider.ICameraProvider/internal/0",
                                     &updatableViaApex)
                        .isOk());
    EXPECT_EQ(std::make_optional<std::string>("com.google.emulated.camera.provider.hal"),
              updatableViaApex);
}

TEST(Vintf, UpdatableViaApex_InvalidNameReturnsNullOpt) {
    if (!isCuttlefishPhone()) GTEST_SKIP() << "Skipping non-Cuttlefish-phone devices";

    auto sm = getPermissiveServiceManager();
    std::optional<std::string> updatableViaApex;
    EXPECT_TRUE(sm->updatableViaApex("android.hardware.camera.provider.ICameraProvider",
                                     &updatableViaApex)
                        .isOk()); // missing instance name
    EXPECT_EQ(std::nullopt, updatableViaApex);
}

TEST(Vintf, GetUpdatableNames) {
    if (!isCuttlefishPhone()) GTEST_SKIP() << "Skipping non-Cuttlefish-phone devices";

    auto sm = getPermissiveServiceManager();
    std::vector<std::string> names;
    EXPECT_TRUE(sm->getUpdatableNames("com.google.emulated.camera.provider.hal", &names).isOk());
    EXPECT_EQ(std::vector<
                      std::string>{"android.hardware.camera.provider.ICameraProvider/internal/0"},
              names);
}

TEST(Vintf, GetUpdatableNames_InvalidApexNameReturnsEmpty) {
    if (!isCuttlefishPhone()) GTEST_SKIP() << "Skipping non-Cuttlefish-phone devices";

    auto sm = getPermissiveServiceManager();
    std::vector<std::string> names;
    EXPECT_TRUE(sm->getUpdatableNames("non.existing.apex.name", &names).isOk());
    EXPECT_EQ(std::vector<std::string>{}, names);
}

TEST(Vintf, IsDeclared_native) {
    if (!isCuttlefishPhone()) GTEST_SKIP() << "Skipping non-Cuttlefish-phone devices";

    auto sm = getPermissiveServiceManager();
    bool declared = false;
    EXPECT_TRUE(sm->isDeclared("mapper/minigbm", &declared).isOk());
    EXPECT_TRUE(declared);
}

TEST(Vintf, GetDeclaredInstances_native) {
    if (!isCuttlefishPhone()) GTEST_SKIP() << "Skipping non-Cuttlefish-phone devices";

    auto sm = getPermissiveServiceManager();
    std::vector<std::string> instances;
    EXPECT_TRUE(sm->getDeclaredInstances("mapper", &instances).isOk());
    EXPECT_EQ(std::vector<std::string>{"minigbm"}, instances);
}

class CallbackHistorian : public BnServiceCallback {
    Status onRegistration(const std::string& name, const sp<IBinder>& binder) override {
        registrations.push_back(name);
        binders.push_back(binder);
        return Status::ok();
    }

    android::status_t linkToDeath(const sp<DeathRecipient>&, void*, uint32_t) override {
        // let SM linkToDeath
        return android::OK;
    }

public:
    std::vector<std::string> registrations;
    std::vector<sp<IBinder>> binders;
};

TEST(ServiceNotifications, NoPermissionsRegister) {
    std::unique_ptr<MockAccess> access = std::make_unique<NiceMock<MockAccess>>();

    EXPECT_CALL(*access, getCallingContext()).WillOnce(Return(Access::CallingContext{}));
    EXPECT_CALL(*access, canFind(_,_)).WillOnce(Return(false));

    sp<ServiceManager> sm = sp<ServiceManager>::make(std::move(access));

    sp<CallbackHistorian> cb = sp<CallbackHistorian>::make();

    EXPECT_EQ(sm->registerForNotifications("foofoo", cb).exceptionCode(), Status::EX_SECURITY);
}

TEST(GetService, IsolatedCantRegister) {
    std::unique_ptr<MockAccess> access = std::make_unique<NiceMock<MockAccess>>();

    EXPECT_CALL(*access, getCallingContext())
            .WillOnce(Return(Access::CallingContext{
                    .uid = AID_ISOLATED_START,
            }));
    EXPECT_CALL(*access, canFind(_, _)).WillOnce(Return(true));

    sp<ServiceManager> sm = sp<ServiceManager>::make(std::move(access));

    sp<CallbackHistorian> cb = sp<CallbackHistorian>::make();

    EXPECT_EQ(sm->registerForNotifications("foofoo", cb).exceptionCode(),
        Status::EX_SECURITY);
}

TEST(ServiceNotifications, NoPermissionsUnregister) {
    std::unique_ptr<MockAccess> access = std::make_unique<NiceMock<MockAccess>>();

    EXPECT_CALL(*access, getCallingContext()).WillOnce(Return(Access::CallingContext{}));
    EXPECT_CALL(*access, canFind(_,_)).WillOnce(Return(false));

    sp<ServiceManager> sm = sp<ServiceManager>::make(std::move(access));

    sp<CallbackHistorian> cb = sp<CallbackHistorian>::make();

    // should always hit security error first
    EXPECT_EQ(sm->unregisterForNotifications("foofoo", cb).exceptionCode(),
        Status::EX_SECURITY);
}

TEST(ServiceNotifications, InvalidName) {
    auto sm = getPermissiveServiceManager();

    sp<CallbackHistorian> cb = sp<CallbackHistorian>::make();

    EXPECT_EQ(sm->registerForNotifications("foo@foo", cb).exceptionCode(),
        Status::EX_ILLEGAL_ARGUMENT);
}

TEST(ServiceNotifications, NullCallback) {
    auto sm = getPermissiveServiceManager();

    EXPECT_EQ(sm->registerForNotifications("foofoo", nullptr).exceptionCode(),
        Status::EX_NULL_POINTER);
}

TEST(ServiceNotifications, Unregister) {
    auto sm = getPermissiveServiceManager();

    sp<CallbackHistorian> cb = sp<CallbackHistorian>::make();

    EXPECT_TRUE(sm->registerForNotifications("foofoo", cb).isOk());
    EXPECT_EQ(sm->unregisterForNotifications("foofoo", cb).exceptionCode(), 0);
}

TEST(ServiceNotifications, UnregisterWhenNoRegistrationExists) {
    auto sm = getPermissiveServiceManager();

    sp<CallbackHistorian> cb = sp<CallbackHistorian>::make();

    EXPECT_EQ(sm->unregisterForNotifications("foofoo", cb).exceptionCode(),
        Status::EX_ILLEGAL_STATE);
}

TEST(ServiceNotifications, NoNotification) {
    auto sm = getPermissiveServiceManager();

    sp<CallbackHistorian> cb = sp<CallbackHistorian>::make();

    EXPECT_TRUE(sm->registerForNotifications("foofoo", cb).isOk());
    EXPECT_TRUE(sm->addService("otherservice", getBinder(),
        false /*allowIsolated*/, IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());

    EXPECT_THAT(cb->registrations, ElementsAre());
    EXPECT_THAT(cb->binders, ElementsAre());
}

TEST(ServiceNotifications, GetNotification) {
    auto sm = getPermissiveServiceManager();

    sp<CallbackHistorian> cb = sp<CallbackHistorian>::make();

    sp<IBinder> service = getBinder();

    EXPECT_TRUE(sm->registerForNotifications("asdfasdf", cb).isOk());
    EXPECT_TRUE(sm->addService("asdfasdf", service,
        false /*allowIsolated*/, IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());

    EXPECT_THAT(cb->registrations, ElementsAre("asdfasdf"));
    EXPECT_THAT(cb->binders, ElementsAre(service));
}

TEST(ServiceNotifications, GetNotificationForAlreadyRegisteredService) {
    auto sm = getPermissiveServiceManager();

    sp<CallbackHistorian> cb = sp<CallbackHistorian>::make();

    sp<IBinder> service = getBinder();

    EXPECT_TRUE(sm->addService("asdfasdf", service,
        false /*allowIsolated*/, IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());

    EXPECT_TRUE(sm->registerForNotifications("asdfasdf", cb).isOk());

    EXPECT_THAT(cb->registrations, ElementsAre("asdfasdf"));
    EXPECT_THAT(cb->binders, ElementsAre(service));
}

TEST(ServiceNotifications, GetMultipleNotification) {
    auto sm = getPermissiveServiceManager();

    sp<CallbackHistorian> cb = sp<CallbackHistorian>::make();

    sp<IBinder> binder1 = getBinder();
    sp<IBinder> binder2 = getBinder();

    EXPECT_TRUE(sm->registerForNotifications("asdfasdf", cb).isOk());
    EXPECT_TRUE(sm->addService("asdfasdf", binder1,
        false /*allowIsolated*/, IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());
    EXPECT_TRUE(sm->addService("asdfasdf", binder2,
        false /*allowIsolated*/, IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT).isOk());

    EXPECT_THAT(cb->registrations, ElementsAre("asdfasdf", "asdfasdf"));
    EXPECT_THAT(cb->registrations, ElementsAre("asdfasdf", "asdfasdf"));
}
