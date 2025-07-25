/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include <chrono>
#include <fstream>
#include <thread>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/result-gmock.h>
#include <binder/Binder.h>
#include <binder/BpBinder.h>
#include <binder/Functional.h>
#include <binder/IBinder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/RpcServer.h>
#include <binder/RpcSession.h>
#include <binder/Status.h>
#include <binder/unique_fd.h>
#include <input/BlockingQueue.h>
#include <processgroup/processgroup.h>
#include <utils/Flattenable.h>
#include <utils/SystemClock.h>
#include "binder/IServiceManagerUnitTestHelper.h"

#include <linux/sched.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "../Utils.h"
#include "../binder_module.h"

using namespace android;
using namespace android::binder::impl;
using namespace std::string_literals;
using namespace std::chrono_literals;
using android::base::testing::HasValue;
using android::binder::Status;
using android::binder::unique_fd;
using std::chrono_literals::operator""ms;
using testing::ExplainMatchResult;
using testing::Matcher;
using testing::Not;
using testing::WithParamInterface;

// e.g. EXPECT_THAT(expr, StatusEq(OK)) << "additional message";
MATCHER_P(StatusEq, expected, (negation ? "not " : "") + statusToString(expected)) {
    *result_listener << statusToString(arg);
    return expected == arg;
}

static ::testing::AssertionResult IsPageAligned(void *buf) {
    if (((unsigned long)buf & ((unsigned long)getpagesize() - 1)) == 0)
        return ::testing::AssertionSuccess();
    else
        return ::testing::AssertionFailure() << buf << " is not page aligned";
}

static testing::Environment* binder_env;
static char *binderservername;
static char *binderserversuffix;
static char binderserverarg[] = "--binderserver";

static constexpr int kSchedPolicy = SCHED_RR;
static constexpr int kSchedPriority = 7;
static constexpr int kSchedPriorityMore = 8;
static constexpr int kKernelThreads = 17; // anything different than the default

static String16 binderLibTestServiceName = String16("test.binderLib");

enum BinderLibTestTranscationCode {
    BINDER_LIB_TEST_NOP_TRANSACTION = IBinder::FIRST_CALL_TRANSACTION,
    BINDER_LIB_TEST_REGISTER_SERVER,
    BINDER_LIB_TEST_ADD_SERVER,
    BINDER_LIB_TEST_ADD_POLL_SERVER,
    BINDER_LIB_TEST_USE_CALLING_GUARD_TRANSACTION,
    BINDER_LIB_TEST_CALL_BACK,
    BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF,
    BINDER_LIB_TEST_DELAYED_CALL_BACK,
    BINDER_LIB_TEST_NOP_CALL_BACK,
    BINDER_LIB_TEST_GET_SELF_TRANSACTION,
    BINDER_LIB_TEST_GET_ID_TRANSACTION,
    BINDER_LIB_TEST_INDIRECT_TRANSACTION,
    BINDER_LIB_TEST_SET_ERROR_TRANSACTION,
    BINDER_LIB_TEST_GET_STATUS_TRANSACTION,
    BINDER_LIB_TEST_ADD_STRONG_REF_TRANSACTION,
    BINDER_LIB_TEST_LINK_DEATH_TRANSACTION,
    BINDER_LIB_TEST_WRITE_FILE_TRANSACTION,
    BINDER_LIB_TEST_WRITE_PARCEL_FILE_DESCRIPTOR_TRANSACTION,
    BINDER_LIB_TEST_GET_FILE_DESCRIPTORS_OWNED_TRANSACTION,
    BINDER_LIB_TEST_GET_FILE_DESCRIPTORS_UNOWNED_TRANSACTION,
    BINDER_LIB_TEST_EXIT_TRANSACTION,
    BINDER_LIB_TEST_DELAYED_EXIT_TRANSACTION,
    BINDER_LIB_TEST_GET_PTR_SIZE_TRANSACTION,
    BINDER_LIB_TEST_CREATE_BINDER_TRANSACTION,
    BINDER_LIB_TEST_GET_WORK_SOURCE_TRANSACTION,
    BINDER_LIB_TEST_GET_SCHEDULING_POLICY,
    BINDER_LIB_TEST_NOP_TRANSACTION_WAIT,
    BINDER_LIB_TEST_GETPID,
    BINDER_LIB_TEST_GETUID,
    BINDER_LIB_TEST_LISTEN_FOR_FROZEN_STATE_CHANGE,
    BINDER_LIB_TEST_CONSUME_STATE_CHANGE_EVENTS,
    BINDER_LIB_TEST_ECHO_VECTOR,
    BINDER_LIB_TEST_GET_NON_BLOCKING_FD,
    BINDER_LIB_TEST_REJECT_OBJECTS,
    BINDER_LIB_TEST_CAN_GET_SID,
    BINDER_LIB_TEST_GET_MAX_THREAD_COUNT,
    BINDER_LIB_TEST_SET_MAX_THREAD_COUNT,
    BINDER_LIB_TEST_IS_THREADPOOL_STARTED,
    BINDER_LIB_TEST_LOCK_UNLOCK,
    BINDER_LIB_TEST_PROCESS_LOCK,
    BINDER_LIB_TEST_UNLOCK_AFTER_MS,
    BINDER_LIB_TEST_PROCESS_TEMPORARY_LOCK
};

pid_t start_server_process(int arg2, bool usePoll = false)
{
    int ret;
    pid_t pid;
    status_t status;
    int pipefd[2];
    char stri[16];
    char strpipefd1[16];
    char usepoll[2];
    char *childargv[] = {
        binderservername,
        binderserverarg,
        stri,
        strpipefd1,
        usepoll,
        binderserversuffix,
        nullptr
    };

    ret = pipe(pipefd);
    if (ret < 0)
        return ret;

    snprintf(stri, sizeof(stri), "%d", arg2);
    snprintf(strpipefd1, sizeof(strpipefd1), "%d", pipefd[1]);
    snprintf(usepoll, sizeof(usepoll), "%d", usePoll ? 1 : 0);

    pid = fork();
    if (pid == -1)
        return pid;
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGHUP);
        close(pipefd[0]);
        execv(binderservername, childargv);
        status = -errno;
        write(pipefd[1], &status, sizeof(status));
        fprintf(stderr, "execv failed, %s\n", strerror(errno));
        _exit(EXIT_FAILURE);
    }
    close(pipefd[1]);
    ret = read(pipefd[0], &status, sizeof(status));
    //printf("pipe read returned %d, status %d\n", ret, status);
    close(pipefd[0]);
    if (ret == sizeof(status)) {
        ret = status;
    } else {
        kill(pid, SIGKILL);
        if (ret >= 0) {
            ret = NO_INIT;
        }
    }
    if (ret < 0) {
        wait(nullptr);
        return ret;
    }
    return pid;
}

android::base::Result<int32_t> GetId(sp<IBinder> service) {
    using android::base::Error;
    Parcel data, reply;
    data.markForBinder(service);
    const char *prefix = data.isForRpc() ? "On RPC server, " : "On binder server, ";
    status_t status = service->transact(BINDER_LIB_TEST_GET_ID_TRANSACTION, data, &reply);
    if (status != OK)
        return Error(status) << prefix << "transact(GET_ID): " << statusToString(status);
    int32_t result = 0;
    status = reply.readInt32(&result);
    if (status != OK) return Error(status) << prefix << "readInt32: " << statusToString(status);
    return result;
}

class BinderLibTestEnv : public ::testing::Environment {
    public:
        BinderLibTestEnv() {}
        sp<IBinder> getServer(void) {
            return m_server;
        }

    private:
        virtual void SetUp() {
            m_serverpid = start_server_process(0);
            //printf("m_serverpid %d\n", m_serverpid);
            ASSERT_GT(m_serverpid, 0);

            sp<IServiceManager> sm = defaultServiceManager();
            // disable caching during addService.
            sm->enableAddServiceCache(false);
            //printf("%s: pid %d, get service\n", __func__, m_pid);
            LIBBINDER_IGNORE("-Wdeprecated-declarations")
            m_server = sm->getService(binderLibTestServiceName);
            LIBBINDER_IGNORE_END()
            ASSERT_TRUE(m_server != nullptr);
            //printf("%s: pid %d, get service done\n", __func__, m_pid);
        }
        virtual void TearDown() {
            status_t ret;
            Parcel data, reply;
            int exitStatus;
            pid_t pid;

            //printf("%s: pid %d\n", __func__, m_pid);
            if (m_server != nullptr) {
                ret = m_server->transact(BINDER_LIB_TEST_GET_STATUS_TRANSACTION, data, &reply);
                EXPECT_EQ(0, ret);
                ret = m_server->transact(BINDER_LIB_TEST_EXIT_TRANSACTION, data, &reply, TF_ONE_WAY);
                EXPECT_EQ(0, ret);
            }
            if (m_serverpid > 0) {
                //printf("wait for %d\n", m_pids[i]);
                pid = wait(&exitStatus);
                EXPECT_EQ(m_serverpid, pid);
                EXPECT_TRUE(WIFEXITED(exitStatus));
                EXPECT_EQ(0, WEXITSTATUS(exitStatus));
            }
        }

        pid_t m_serverpid;
        sp<IBinder> m_server;
};

class TestFrozenStateChangeCallback : public IBinder::FrozenStateChangeCallback {
public:
    BlockingQueue<std::pair<const wp<IBinder>, State>> events;

    virtual void onStateChanged(const wp<IBinder>& who, State state) {
        events.push(std::make_pair(who, state));
    }

    void ensureFrozenEventReceived() {
        auto event = events.popWithTimeout(500ms);
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(State::FROZEN, event->second); // isFrozen should be true
        EXPECT_EQ(0u, events.size());
    }

    void ensureUnfrozenEventReceived() {
        auto event = events.popWithTimeout(500ms);
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(State::UNFROZEN, event->second); // isFrozen should be false
        EXPECT_EQ(0u, events.size());
    }

    std::vector<bool> getAllAndClear() {
        std::vector<bool> results;
        while (true) {
            auto event = events.popWithTimeout(0ms);
            if (!event.has_value()) {
                break;
            }
            results.push_back(event->second == State::FROZEN);
        }
        return results;
    }

    sp<IBinder> binder;
};

class BinderLibTest : public ::testing::Test {
    public:
        virtual void SetUp() {
            m_server = static_cast<BinderLibTestEnv *>(binder_env)->getServer();
            IPCThreadState::self()->restoreCallingWorkSource(0);
            sp<IServiceManager> sm = defaultServiceManager();
            // disable caching during addService.
            sm->enableAddServiceCache(false);
        }
        virtual void TearDown() {
        }
    protected:
        sp<IBinder> addServerEtc(int32_t *idPtr, int code)
        {
            int32_t id;
            Parcel data, reply;

            EXPECT_THAT(m_server->transact(code, data, &reply), StatusEq(NO_ERROR));

            sp<IBinder> binder = reply.readStrongBinder();
            EXPECT_NE(nullptr, binder);
            EXPECT_THAT(reply.readInt32(&id), StatusEq(NO_ERROR));
            if (idPtr)
                *idPtr = id;
            return binder;
        }

        sp<IBinder> addServer(int32_t *idPtr = nullptr)
        {
            return addServerEtc(idPtr, BINDER_LIB_TEST_ADD_SERVER);
        }

        sp<IBinder> addPollServer(int32_t *idPtr = nullptr)
        {
            return addServerEtc(idPtr, BINDER_LIB_TEST_ADD_POLL_SERVER);
        }

        void waitForReadData(int fd, int timeout_ms) {
            int ret;
            pollfd pfd = pollfd();

            pfd.fd = fd;
            pfd.events = POLLIN;
            ret = poll(&pfd, 1, timeout_ms);
            EXPECT_EQ(1, ret);
        }

        bool checkFreezeSupport() {
            std::string path;
            if (!CgroupGetAttributePathForTask("FreezerState", getpid(), &path)) {
                return false;
            }

            std::ifstream freezer_file(path);
            // Pass test on devices where the cgroup v2 freezer is not supported
            if (freezer_file.fail()) {
                return false;
            }
            return IPCThreadState::self()->freeze(getpid(), false, 0) == NO_ERROR;
        }

        bool checkFreezeAndNotificationSupport() {
            if (!checkFreezeSupport()) {
                return false;
            }
            return ProcessState::isDriverFeatureEnabled(
                    ProcessState::DriverFeature::FREEZE_NOTIFICATION);
        }

        bool getBinderPid(int32_t* pid, sp<IBinder> server) {
            Parcel data, replypid;
            if (server->transact(BINDER_LIB_TEST_GETPID, data, &replypid) != NO_ERROR) {
                ALOGE("BINDER_LIB_TEST_GETPID failed");
                return false;
            }
            *pid = replypid.readInt32();
            if (*pid <= 0) {
                ALOGE("pid should be greater than zero");
                return false;
            }
            return true;
        }

        void freezeProcess(int32_t pid) {
            EXPECT_EQ(NO_ERROR, IPCThreadState::self()->freeze(pid, true, 1000));
        }

        void unfreezeProcess(int32_t pid) {
            EXPECT_EQ(NO_ERROR, IPCThreadState::self()->freeze(pid, false, 0));
        }

        void removeCallbackAndValidateNoEvent(sp<IBinder> binder,
                                              sp<TestFrozenStateChangeCallback> callback) {
            EXPECT_THAT(binder->removeFrozenStateChangeCallback(callback), StatusEq(NO_ERROR));
            EXPECT_EQ(0u, callback->events.size());
        }

        sp<IBinder> m_server;
};

class BinderLibTestBundle : public Parcel
{
    public:
        BinderLibTestBundle(void) {}
        explicit BinderLibTestBundle(const Parcel *source) : m_isValid(false) {
            int32_t mark;
            int32_t bundleLen;
            size_t pos;

            if (source->readInt32(&mark))
                return;
            if (mark != MARK_START)
                return;
            if (source->readInt32(&bundleLen))
                return;
            pos = source->dataPosition();
            if (Parcel::appendFrom(source, pos, bundleLen))
                return;
            source->setDataPosition(pos + bundleLen);
            if (source->readInt32(&mark))
                return;
            if (mark != MARK_END)
                return;
            m_isValid = true;
            setDataPosition(0);
        }
        void appendTo(Parcel *dest) {
            dest->writeInt32(MARK_START);
            dest->writeInt32(dataSize());
            dest->appendFrom(this, 0, dataSize());
            dest->writeInt32(MARK_END);
        };
        bool isValid(void) {
            return m_isValid;
        }
    private:
        enum {
            MARK_START  = B_PACK_CHARS('B','T','B','S'),
            MARK_END    = B_PACK_CHARS('B','T','B','E'),
        };
        bool m_isValid;
};

class BinderLibTestEvent
{
    public:
        BinderLibTestEvent(void)
            : m_eventTriggered(false)
        {
            pthread_mutex_init(&m_waitMutex, nullptr);
            pthread_cond_init(&m_waitCond, nullptr);
        }
        int waitEvent(int timeout_s)
        {
            int ret;
            pthread_mutex_lock(&m_waitMutex);
            if (!m_eventTriggered) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += timeout_s;
                pthread_cond_timedwait(&m_waitCond, &m_waitMutex, &ts);
            }
            ret = m_eventTriggered ? NO_ERROR : TIMED_OUT;
            pthread_mutex_unlock(&m_waitMutex);
            return ret;
        }
        pthread_t getTriggeringThread()
        {
            return m_triggeringThread;
        }
    protected:
        void triggerEvent(void) {
            pthread_mutex_lock(&m_waitMutex);
            pthread_cond_signal(&m_waitCond);
            m_eventTriggered = true;
            m_triggeringThread = pthread_self();
            pthread_mutex_unlock(&m_waitMutex);
        };
    private:
        pthread_mutex_t m_waitMutex;
        pthread_cond_t m_waitCond;
        bool m_eventTriggered;
        pthread_t m_triggeringThread;
};

class BinderLibTestCallBack : public BBinder, public BinderLibTestEvent
{
    public:
        BinderLibTestCallBack()
            : m_result(NOT_ENOUGH_DATA)
            , m_prev_end(nullptr)
        {
        }
        status_t getResult(void)
        {
            return m_result;
        }

    private:
        virtual status_t onTransact(uint32_t code,
                                    const Parcel& data, Parcel* reply,
                                    uint32_t flags = 0)
        {
            (void)reply;
            (void)flags;
            switch(code) {
            case BINDER_LIB_TEST_CALL_BACK: {
                status_t status = data.readInt32(&m_result);
                if (status != NO_ERROR) {
                    m_result = status;
                }
                triggerEvent();
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF: {
                sp<IBinder> server;
                int ret;
                const uint8_t *buf = data.data();
                size_t size = data.dataSize();
                if (m_prev_end) {
                    /* 64-bit kernel needs at most 8 bytes to align buffer end */
                    EXPECT_LE((size_t)(buf - m_prev_end), (size_t)8);
                } else {
                    EXPECT_TRUE(IsPageAligned((void *)buf));
                }

                m_prev_end = buf + size + data.objectsCount() * sizeof(binder_size_t);

                if (size > 0) {
                    server = static_cast<BinderLibTestEnv *>(binder_env)->getServer();
                    ret = server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION,
                                           data, reply);
                    EXPECT_EQ(NO_ERROR, ret);
                }
                return NO_ERROR;
            }
            default:
                return UNKNOWN_TRANSACTION;
            }
        }

        status_t m_result;
        const uint8_t *m_prev_end;
};

class TestDeathRecipient : public IBinder::DeathRecipient, public BinderLibTestEvent
{
    private:
        virtual void binderDied(const wp<IBinder>& who) {
            (void)who;
            triggerEvent();
        };
};

ssize_t countFds() {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator{});
}

struct FdLeakDetector {
    int startCount;

    FdLeakDetector() {
        // This log statement is load bearing. We have to log something before
        // counting FDs to make sure the logging system is initialized, otherwise
        // the sockets it opens will look like a leak.
        ALOGW("FdLeakDetector counting FDs.");
        startCount = countFds();
    }
    ~FdLeakDetector() {
        int endCount = countFds();
        if (startCount != endCount) {
            ADD_FAILURE() << "fd count changed (" << startCount << " -> " << endCount
                          << ") fd leak?";
        }
    }
};

TEST_F(BinderLibTest, CannotUseBinderAfterFork) {
    // EXPECT_DEATH works by forking the process
    EXPECT_DEATH({ ProcessState::self(); }, "libbinder ProcessState can not be used after fork");
}

TEST_F(BinderLibTest, AddManagerToManager) {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = IInterface::asBinder(sm);
    EXPECT_EQ(NO_ERROR, sm->addService(String16("binderLibTest-manager"), binder));
}

class LocalRegistrationCallbackImpl : public virtual IServiceManager::LocalRegistrationCallback {
    void onServiceRegistration(const String16&, const sp<IBinder>&) override {}
    virtual ~LocalRegistrationCallbackImpl() {}
};

TEST_F(BinderLibTest, RegisterForNotificationsFailure) {
    auto sm = defaultServiceManager();
    sp<IServiceManager::LocalRegistrationCallback> cb = sp<LocalRegistrationCallbackImpl>::make();

    EXPECT_EQ(BAD_VALUE, sm->registerForNotifications(String16("ValidName"), nullptr));
    EXPECT_EQ(UNKNOWN_ERROR, sm->registerForNotifications(String16("InvalidName!$"), cb));
}

TEST_F(BinderLibTest, UnregisterForNotificationsFailure) {
    auto sm = defaultServiceManager();
    sp<IServiceManager::LocalRegistrationCallback> cb = sp<LocalRegistrationCallbackImpl>::make();

    EXPECT_EQ(OK, sm->registerForNotifications(String16("ValidName"), cb));

    EXPECT_EQ(BAD_VALUE, sm->unregisterForNotifications(String16("ValidName"), nullptr));
    EXPECT_EQ(BAD_VALUE, sm->unregisterForNotifications(String16("AnotherValidName"), cb));
    EXPECT_EQ(BAD_VALUE, sm->unregisterForNotifications(String16("InvalidName!!!"), cb));
}

TEST_F(BinderLibTest, WasParceled) {
    auto binder = sp<BBinder>::make();
    EXPECT_FALSE(binder->wasParceled());
    Parcel data;
    data.writeStrongBinder(binder);
    EXPECT_TRUE(binder->wasParceled());
}

TEST_F(BinderLibTest, NopTransaction) {
    Parcel data, reply;
    EXPECT_THAT(m_server->transact(BINDER_LIB_TEST_NOP_TRANSACTION, data, &reply),
                StatusEq(NO_ERROR));
}

TEST_F(BinderLibTest, NopTransactionOneway) {
    Parcel data, reply;
    EXPECT_THAT(m_server->transact(BINDER_LIB_TEST_NOP_TRANSACTION, data, &reply, TF_ONE_WAY),
                StatusEq(NO_ERROR));
}

TEST_F(BinderLibTest, NopTransactionClear) {
    Parcel data, reply;
    // make sure it accepts the transaction flag
    EXPECT_THAT(m_server->transact(BINDER_LIB_TEST_NOP_TRANSACTION, data, &reply, TF_CLEAR_BUF),
                StatusEq(NO_ERROR));
}

TEST_F(BinderLibTest, Freeze) {
    if (!checkFreezeSupport()) {
        GTEST_SKIP() << "Skipping test for kernels that do not support proceess freezing";
        return;
    }
    Parcel data, reply, replypid;
    EXPECT_THAT(m_server->transact(BINDER_LIB_TEST_GETPID, data, &replypid), StatusEq(NO_ERROR));
    int32_t pid = replypid.readInt32();
    for (int i = 0; i < 10; i++) {
        EXPECT_EQ(NO_ERROR, m_server->transact(BINDER_LIB_TEST_NOP_TRANSACTION_WAIT, data, &reply, TF_ONE_WAY));
    }

    EXPECT_EQ(NO_ERROR, IPCThreadState::self()->freeze(pid, false, 0));
    EXPECT_EQ(-EAGAIN, IPCThreadState::self()->freeze(pid, true, 0));

    // b/268232063 - succeeds ~0.08% of the time
    {
        auto ret = IPCThreadState::self()->freeze(pid, true, 0);
        EXPECT_TRUE(ret == -EAGAIN || ret == OK);
    }

    EXPECT_EQ(NO_ERROR, IPCThreadState::self()->freeze(pid, true, 1000));
    EXPECT_EQ(FAILED_TRANSACTION, m_server->transact(BINDER_LIB_TEST_NOP_TRANSACTION, data, &reply));

    uint32_t sync_received, async_received;

    EXPECT_EQ(NO_ERROR, IPCThreadState::self()->getProcessFreezeInfo(pid, &sync_received,
                &async_received));

    EXPECT_EQ(sync_received, 1u);
    EXPECT_EQ(async_received, 0u);

    EXPECT_EQ(NO_ERROR, IPCThreadState::self()->freeze(pid, false, 0));
    EXPECT_EQ(NO_ERROR, m_server->transact(BINDER_LIB_TEST_NOP_TRANSACTION, data, &reply));
}

TEST_F(BinderLibTest, SetError) {
    int32_t testValue[] = { 0, -123, 123 };
    for (size_t i = 0; i < countof(testValue); i++) {
        Parcel data, reply;
        data.writeInt32(testValue[i]);
        EXPECT_THAT(m_server->transact(BINDER_LIB_TEST_SET_ERROR_TRANSACTION, data, &reply),
                    StatusEq(testValue[i]));
    }
}

TEST_F(BinderLibTest, GetId) {
    EXPECT_THAT(GetId(m_server), HasValue(0));
}

TEST_F(BinderLibTest, PtrSize) {
    int32_t ptrsize;
    Parcel data, reply;
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);
    EXPECT_THAT(server->transact(BINDER_LIB_TEST_GET_PTR_SIZE_TRANSACTION, data, &reply),
                StatusEq(NO_ERROR));
    EXPECT_THAT(reply.readInt32(&ptrsize), StatusEq(NO_ERROR));
    RecordProperty("TestPtrSize", sizeof(void *));
    RecordProperty("ServerPtrSize", sizeof(void *));
}

TEST_F(BinderLibTest, IndirectGetId2)
{
    int32_t id;
    int32_t count;
    Parcel data, reply;
    int32_t serverId[3];

    data.writeInt32(countof(serverId));
    for (size_t i = 0; i < countof(serverId); i++) {
        sp<IBinder> server;
        BinderLibTestBundle datai;

        server = addServer(&serverId[i]);
        ASSERT_TRUE(server != nullptr);
        data.writeStrongBinder(server);
        data.writeInt32(BINDER_LIB_TEST_GET_ID_TRANSACTION);
        datai.appendTo(&data);
    }

    ASSERT_THAT(m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply),
                StatusEq(NO_ERROR));

    ASSERT_THAT(reply.readInt32(&id), StatusEq(NO_ERROR));
    EXPECT_EQ(0, id);

    ASSERT_THAT(reply.readInt32(&count), StatusEq(NO_ERROR));
    EXPECT_EQ(countof(serverId), (size_t)count);

    for (size_t i = 0; i < (size_t)count; i++) {
        BinderLibTestBundle replyi(&reply);
        EXPECT_TRUE(replyi.isValid());
        EXPECT_THAT(replyi.readInt32(&id), StatusEq(NO_ERROR));
        EXPECT_EQ(serverId[i], id);
        EXPECT_EQ(replyi.dataSize(), replyi.dataPosition());
    }

    EXPECT_EQ(reply.dataSize(), reply.dataPosition());
}

TEST_F(BinderLibTest, IndirectGetId3)
{
    int32_t id;
    int32_t count;
    Parcel data, reply;
    int32_t serverId[3];

    data.writeInt32(countof(serverId));
    for (size_t i = 0; i < countof(serverId); i++) {
        sp<IBinder> server;
        BinderLibTestBundle datai;
        BinderLibTestBundle datai2;

        server = addServer(&serverId[i]);
        ASSERT_TRUE(server != nullptr);
        data.writeStrongBinder(server);
        data.writeInt32(BINDER_LIB_TEST_INDIRECT_TRANSACTION);

        datai.writeInt32(1);
        datai.writeStrongBinder(m_server);
        datai.writeInt32(BINDER_LIB_TEST_GET_ID_TRANSACTION);
        datai2.appendTo(&datai);

        datai.appendTo(&data);
    }

    ASSERT_THAT(m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply),
                StatusEq(NO_ERROR));

    ASSERT_THAT(reply.readInt32(&id), StatusEq(NO_ERROR));
    EXPECT_EQ(0, id);

    ASSERT_THAT(reply.readInt32(&count), StatusEq(NO_ERROR));
    EXPECT_EQ(countof(serverId), (size_t)count);

    for (size_t i = 0; i < (size_t)count; i++) {
        int32_t counti;

        BinderLibTestBundle replyi(&reply);
        EXPECT_TRUE(replyi.isValid());
        EXPECT_THAT(replyi.readInt32(&id), StatusEq(NO_ERROR));
        EXPECT_EQ(serverId[i], id);

        ASSERT_THAT(replyi.readInt32(&counti), StatusEq(NO_ERROR));
        EXPECT_EQ(1, counti);

        BinderLibTestBundle replyi2(&replyi);
        EXPECT_TRUE(replyi2.isValid());
        EXPECT_THAT(replyi2.readInt32(&id), StatusEq(NO_ERROR));
        EXPECT_EQ(0, id);
        EXPECT_EQ(replyi2.dataSize(), replyi2.dataPosition());

        EXPECT_EQ(replyi.dataSize(), replyi.dataPosition());
    }

    EXPECT_EQ(reply.dataSize(), reply.dataPosition());
}

TEST_F(BinderLibTest, CallBack)
{
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    data.writeStrongBinder(callBack);
    EXPECT_THAT(m_server->transact(BINDER_LIB_TEST_NOP_CALL_BACK, data, &reply, TF_ONE_WAY),
                StatusEq(NO_ERROR));
    EXPECT_THAT(callBack->waitEvent(5), StatusEq(NO_ERROR));
    EXPECT_THAT(callBack->getResult(), StatusEq(NO_ERROR));
}

TEST_F(BinderLibTest, BinderCallContextGuard) {
    sp<IBinder> binder = addServer();
    Parcel data, reply;
    EXPECT_THAT(binder->transact(BINDER_LIB_TEST_USE_CALLING_GUARD_TRANSACTION, data, &reply),
                StatusEq(DEAD_OBJECT));
}

TEST_F(BinderLibTest, AddServer)
{
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);
}

TEST_F(BinderLibTest, DeathNotificationStrongRef)
{
    sp<IBinder> sbinder;

    sp<TestDeathRecipient> testDeathRecipient = new TestDeathRecipient();

    {
        sp<IBinder> binder = addServer();
        ASSERT_TRUE(binder != nullptr);
        EXPECT_THAT(binder->linkToDeath(testDeathRecipient), StatusEq(NO_ERROR));
        sbinder = binder;
    }
    {
        Parcel data, reply;
        EXPECT_THAT(sbinder->transact(BINDER_LIB_TEST_EXIT_TRANSACTION, data, &reply, TF_ONE_WAY),
                    StatusEq(OK));
    }
    IPCThreadState::self()->flushCommands();
    EXPECT_THAT(testDeathRecipient->waitEvent(5), StatusEq(NO_ERROR));
    EXPECT_THAT(sbinder->unlinkToDeath(testDeathRecipient), StatusEq(DEAD_OBJECT));
}

TEST_F(BinderLibTest, DeathNotificationMultiple)
{
    status_t ret;
    const int clientcount = 2;
    sp<IBinder> target;
    sp<IBinder> linkedclient[clientcount];
    sp<BinderLibTestCallBack> callBack[clientcount];
    sp<IBinder> passiveclient[clientcount];

    target = addServer();
    ASSERT_TRUE(target != nullptr);
    for (int i = 0; i < clientcount; i++) {
        {
            Parcel data, reply;

            linkedclient[i] = addServer();
            ASSERT_TRUE(linkedclient[i] != nullptr);
            callBack[i] = new BinderLibTestCallBack();
            data.writeStrongBinder(target);
            data.writeStrongBinder(callBack[i]);
            EXPECT_THAT(linkedclient[i]->transact(BINDER_LIB_TEST_LINK_DEATH_TRANSACTION, data,
                                                  &reply, TF_ONE_WAY),
                        StatusEq(NO_ERROR));
        }
        {
            Parcel data, reply;

            passiveclient[i] = addServer();
            ASSERT_TRUE(passiveclient[i] != nullptr);
            data.writeStrongBinder(target);
            EXPECT_THAT(passiveclient[i]->transact(BINDER_LIB_TEST_ADD_STRONG_REF_TRANSACTION, data,
                                                   &reply, TF_ONE_WAY),
                        StatusEq(NO_ERROR));
        }
    }
    {
        Parcel data, reply;
        ret = target->transact(BINDER_LIB_TEST_EXIT_TRANSACTION, data, &reply, TF_ONE_WAY);
        EXPECT_EQ(0, ret);
    }

    for (int i = 0; i < clientcount; i++) {
        EXPECT_THAT(callBack[i]->waitEvent(5), StatusEq(NO_ERROR));
        EXPECT_THAT(callBack[i]->getResult(), StatusEq(NO_ERROR));
    }
}

TEST_F(BinderLibTest, DeathNotificationThread)
{
    status_t ret;
    sp<BinderLibTestCallBack> callback;
    sp<IBinder> target = addServer();
    ASSERT_TRUE(target != nullptr);
    sp<IBinder> client = addServer();
    ASSERT_TRUE(client != nullptr);

    sp<TestDeathRecipient> testDeathRecipient = new TestDeathRecipient();

    EXPECT_THAT(target->linkToDeath(testDeathRecipient), StatusEq(NO_ERROR));

    {
        Parcel data, reply;
        ret = target->transact(BINDER_LIB_TEST_EXIT_TRANSACTION, data, &reply, TF_ONE_WAY);
        EXPECT_EQ(0, ret);
    }

    /* Make sure it's dead */
    testDeathRecipient->waitEvent(5);

    /* Now, pass the ref to another process and ask that process to
     * call linkToDeath() on it, and wait for a response. This tests
     * two things:
     * 1) You still get death notifications when calling linkToDeath()
     *    on a ref that is already dead when it was passed to you.
     * 2) That death notifications are not directly pushed to the thread
     *    registering them, but to the threadpool (proc workqueue) instead.
     *
     * 2) is tested because the thread handling BINDER_LIB_TEST_DEATH_TRANSACTION
     * is blocked on a condition variable waiting for the death notification to be
     * called; therefore, that thread is not available for handling proc work.
     * So, if the death notification was pushed to the thread workqueue, the callback
     * would never be called, and the test would timeout and fail.
     *
     * Note that we can't do this part of the test from this thread itself, because
     * the binder driver would only push death notifications to the thread if
     * it is a looper thread, which this thread is not.
     *
     * See b/23525545 for details.
     */
    {
        Parcel data, reply;

        callback = new BinderLibTestCallBack();
        data.writeStrongBinder(target);
        data.writeStrongBinder(callback);
        EXPECT_THAT(client->transact(BINDER_LIB_TEST_LINK_DEATH_TRANSACTION, data, &reply,
                                     TF_ONE_WAY),
                    StatusEq(NO_ERROR));
    }

    EXPECT_THAT(callback->waitEvent(5), StatusEq(NO_ERROR));
    EXPECT_THAT(callback->getResult(), StatusEq(NO_ERROR));
}

TEST_F(BinderLibTest, ReturnErrorIfKernelDoesNotSupportFreezeNotification) {
    if (ProcessState::isDriverFeatureEnabled(ProcessState::DriverFeature::FREEZE_NOTIFICATION)) {
        GTEST_SKIP() << "Skipping test for kernels that support FREEZE_NOTIFICATION";
        return;
    }
    sp<TestFrozenStateChangeCallback> callback = sp<TestFrozenStateChangeCallback>::make();
    sp<IBinder> binder = addServer();
    ASSERT_NE(nullptr, binder);
    ASSERT_EQ(nullptr, binder->localBinder());
    EXPECT_THAT(binder->addFrozenStateChangeCallback(callback), StatusEq(INVALID_OPERATION));
}

TEST_F(BinderLibTest, FrozenStateChangeNotificatiion) {
    if (!checkFreezeAndNotificationSupport()) {
        GTEST_SKIP() << "Skipping test for kernels that do not support FREEZE_NOTIFICATION";
        return;
    }
    sp<TestFrozenStateChangeCallback> callback = sp<TestFrozenStateChangeCallback>::make();
    sp<IBinder> binder = addServer();
    ASSERT_NE(nullptr, binder);
    int32_t pid;
    ASSERT_TRUE(getBinderPid(&pid, binder));

    EXPECT_THAT(binder->addFrozenStateChangeCallback(callback), StatusEq(NO_ERROR));
    // Expect current state (unfrozen) to be delivered immediately.
    callback->ensureUnfrozenEventReceived();
    // Check that the process hasn't died otherwise there's a risk of freezing
    // the wrong process.
    EXPECT_EQ(OK, binder->pingBinder());
    freezeProcess(pid);
    callback->ensureFrozenEventReceived();
    unfreezeProcess(pid);
    callback->ensureUnfrozenEventReceived();
    removeCallbackAndValidateNoEvent(binder, callback);
}

TEST_F(BinderLibTest, AddFrozenCallbackWhenFrozen) {
    if (!checkFreezeAndNotificationSupport()) {
        GTEST_SKIP() << "Skipping test for kernels that do not support FREEZE_NOTIFICATION";
        return;
    }
    sp<TestFrozenStateChangeCallback> callback = sp<TestFrozenStateChangeCallback>::make();
    sp<IBinder> binder = addServer();
    ASSERT_NE(nullptr, binder);
    int32_t pid;
    ASSERT_TRUE(getBinderPid(&pid, binder));

    // Check that the process hasn't died otherwise there's a risk of freezing
    // the wrong process.
    EXPECT_EQ(OK, binder->pingBinder());
    freezeProcess(pid);
    // Add the callback while the target process is frozen.
    EXPECT_THAT(binder->addFrozenStateChangeCallback(callback), StatusEq(NO_ERROR));
    callback->ensureFrozenEventReceived();
    unfreezeProcess(pid);
    callback->ensureUnfrozenEventReceived();
    removeCallbackAndValidateNoEvent(binder, callback);

    // Check that the process hasn't died otherwise there's a risk of freezing
    // the wrong process.
    EXPECT_EQ(OK, binder->pingBinder());
    freezeProcess(pid);
    unfreezeProcess(pid);
    // Make sure no callback happens since the listener has been removed.
    EXPECT_EQ(0u, callback->events.size());
}

TEST_F(BinderLibTest, NoFrozenNotificationAfterCallbackRemoval) {
    if (!checkFreezeAndNotificationSupport()) {
        GTEST_SKIP() << "Skipping test for kernels that do not support FREEZE_NOTIFICATION";
        return;
    }
    sp<TestFrozenStateChangeCallback> callback = sp<TestFrozenStateChangeCallback>::make();
    sp<IBinder> binder = addServer();
    ASSERT_NE(nullptr, binder);
    int32_t pid;
    ASSERT_TRUE(getBinderPid(&pid, binder));

    EXPECT_THAT(binder->addFrozenStateChangeCallback(callback), StatusEq(NO_ERROR));
    callback->ensureUnfrozenEventReceived();
    removeCallbackAndValidateNoEvent(binder, callback);

    // Make sure no callback happens after the listener is removed.
    freezeProcess(pid);
    unfreezeProcess(pid);
    EXPECT_EQ(0u, callback->events.size());
}

TEST_F(BinderLibTest, MultipleFrozenStateChangeCallbacks) {
    if (!checkFreezeAndNotificationSupport()) {
        GTEST_SKIP() << "Skipping test for kernels that do not support FREEZE_NOTIFICATION";
        return;
    }
    sp<TestFrozenStateChangeCallback> callback1 = sp<TestFrozenStateChangeCallback>::make();
    sp<TestFrozenStateChangeCallback> callback2 = sp<TestFrozenStateChangeCallback>::make();
    sp<IBinder> binder = addServer();
    ASSERT_NE(nullptr, binder);
    int32_t pid;
    ASSERT_TRUE(getBinderPid(&pid, binder));

    EXPECT_THAT(binder->addFrozenStateChangeCallback(callback1), StatusEq(NO_ERROR));
    // Expect current state (unfrozen) to be delivered immediately.
    callback1->ensureUnfrozenEventReceived();

    EXPECT_THAT(binder->addFrozenStateChangeCallback(callback2), StatusEq(NO_ERROR));
    // Expect current state (unfrozen) to be delivered immediately.
    callback2->ensureUnfrozenEventReceived();

    freezeProcess(pid);
    callback1->ensureFrozenEventReceived();
    callback2->ensureFrozenEventReceived();

    removeCallbackAndValidateNoEvent(binder, callback1);
    unfreezeProcess(pid);
    EXPECT_EQ(0u, callback1->events.size());
    callback2->ensureUnfrozenEventReceived();
    removeCallbackAndValidateNoEvent(binder, callback2);

    freezeProcess(pid);
    EXPECT_EQ(0u, callback2->events.size());
}

TEST_F(BinderLibTest, RemoveThenAddFrozenStateChangeCallbacks) {
    if (!checkFreezeAndNotificationSupport()) {
        GTEST_SKIP() << "Skipping test for kernels that do not support FREEZE_NOTIFICATION";
        return;
    }
    sp<TestFrozenStateChangeCallback> callback = sp<TestFrozenStateChangeCallback>::make();
    sp<IBinder> binder = addServer();
    ASSERT_NE(nullptr, binder);
    int32_t pid;
    ASSERT_TRUE(getBinderPid(&pid, binder));

    EXPECT_THAT(binder->addFrozenStateChangeCallback(callback), StatusEq(NO_ERROR));
    // Expect current state (unfrozen) to be delivered immediately.
    callback->ensureUnfrozenEventReceived();
    removeCallbackAndValidateNoEvent(binder, callback);

    EXPECT_THAT(binder->addFrozenStateChangeCallback(callback), StatusEq(NO_ERROR));
    callback->ensureUnfrozenEventReceived();
}

TEST_F(BinderLibTest, CoalesceFreezeCallbacksWhenListenerIsFrozen) {
    if (!checkFreezeAndNotificationSupport()) {
        GTEST_SKIP() << "Skipping test for kernels that do not support FREEZE_NOTIFICATION";
        return;
    }
    sp<IBinder> binder = addServer();
    sp<IBinder> listener = addServer();
    ASSERT_NE(nullptr, binder);
    ASSERT_NE(nullptr, listener);
    int32_t pid, listenerPid;
    ASSERT_TRUE(getBinderPid(&pid, binder));
    ASSERT_TRUE(getBinderPid(&listenerPid, listener));

    // Ask the listener process to register for state change callbacks.
    {
        Parcel data, reply;
        data.writeStrongBinder(binder);
        ASSERT_THAT(listener->transact(BINDER_LIB_TEST_LISTEN_FOR_FROZEN_STATE_CHANGE, data,
                                       &reply),
                    StatusEq(NO_ERROR));
    }
    // Freeze the listener process.
    freezeProcess(listenerPid);
    createProcessGroup(getuid(), listenerPid);
    ASSERT_TRUE(SetProcessProfiles(getuid(), listenerPid, {"Frozen"}));
    // Repeatedly flip the target process between frozen and unfrozen states.
    for (int i = 0; i < 1000; i++) {
        usleep(50);
        unfreezeProcess(pid);
        usleep(50);
        freezeProcess(pid);
    }
    // Unfreeze the listener process. Now it should receive the frozen state
    // change notifications.
    ASSERT_TRUE(SetProcessProfiles(getuid(), listenerPid, {"Unfrozen"}));
    unfreezeProcess(listenerPid);
    // Wait for 500ms to give the process enough time to wake up and handle
    // notifications.
    usleep(500 * 1000);
    {
        std::vector<bool> events;
        Parcel data, reply;
        ASSERT_THAT(listener->transact(BINDER_LIB_TEST_CONSUME_STATE_CHANGE_EVENTS, data, &reply),
                    StatusEq(NO_ERROR));
        reply.readBoolVector(&events);
        // There should only be one single state change notifications delievered.
        ASSERT_EQ(1u, events.size());
        EXPECT_TRUE(events[0]);
    }
}

TEST_F(BinderLibTest, PassFile) {
    int ret;
    int pipefd[2];
    uint8_t buf[1] = { 0 };
    uint8_t write_value = 123;

    ret = pipe2(pipefd, O_NONBLOCK);
    ASSERT_EQ(0, ret);

    {
        Parcel data, reply;
        uint8_t writebuf[1] = { write_value };

        EXPECT_THAT(data.writeFileDescriptor(pipefd[1], true), StatusEq(NO_ERROR));

        EXPECT_THAT(data.writeInt32(sizeof(writebuf)), StatusEq(NO_ERROR));

        EXPECT_THAT(data.write(writebuf, sizeof(writebuf)), StatusEq(NO_ERROR));

        EXPECT_THAT(m_server->transact(BINDER_LIB_TEST_WRITE_FILE_TRANSACTION, data, &reply),
                    StatusEq(NO_ERROR));
    }

    ret = read(pipefd[0], buf, sizeof(buf));
    EXPECT_EQ(sizeof(buf), (size_t)ret);
    EXPECT_EQ(write_value, buf[0]);

    waitForReadData(pipefd[0], 5000); /* wait for other proccess to close pipe */

    ret = read(pipefd[0], buf, sizeof(buf));
    EXPECT_EQ(0, ret);

    close(pipefd[0]);
}

TEST_F(BinderLibTest, PassParcelFileDescriptor) {
    const int datasize = 123;
    std::vector<uint8_t> writebuf(datasize);
    for (size_t i = 0; i < writebuf.size(); ++i) {
        writebuf[i] = i;
    }

    unique_fd read_end, write_end;
    {
        int pipefd[2];
        ASSERT_EQ(0, pipe2(pipefd, O_NONBLOCK));
        read_end.reset(pipefd[0]);
        write_end.reset(pipefd[1]);
    }
    {
        Parcel data;
        EXPECT_EQ(NO_ERROR, data.writeDupParcelFileDescriptor(write_end.get()));
        write_end.reset();
        EXPECT_EQ(NO_ERROR, data.writeInt32(datasize));
        EXPECT_EQ(NO_ERROR, data.write(writebuf.data(), datasize));

        Parcel reply;
        EXPECT_EQ(NO_ERROR,
                  m_server->transact(BINDER_LIB_TEST_WRITE_PARCEL_FILE_DESCRIPTOR_TRANSACTION, data,
                                     &reply));
    }
    std::vector<uint8_t> readbuf(datasize);
    EXPECT_EQ(datasize, read(read_end.get(), readbuf.data(), datasize));
    EXPECT_EQ(writebuf, readbuf);

    waitForReadData(read_end.get(), 5000); /* wait for other proccess to close pipe */

    EXPECT_EQ(0, read(read_end.get(), readbuf.data(), datasize));
}

TEST_F(BinderLibTest, RecvOwnedFileDescriptors) {
    FdLeakDetector fd_leak_detector;

    Parcel data;
    Parcel reply;
    EXPECT_EQ(NO_ERROR,
              m_server->transact(BINDER_LIB_TEST_GET_FILE_DESCRIPTORS_OWNED_TRANSACTION, data,
                                 &reply));
    unique_fd a, b;
    EXPECT_EQ(OK, reply.readUniqueFileDescriptor(&a));
    EXPECT_EQ(OK, reply.readUniqueFileDescriptor(&b));
}

// Used to trigger fdsan error (b/239222407).
TEST_F(BinderLibTest, RecvOwnedFileDescriptorsAndWriteInt) {
    GTEST_SKIP() << "triggers fdsan false positive: b/370824489";

    FdLeakDetector fd_leak_detector;

    Parcel data;
    Parcel reply;
    EXPECT_EQ(NO_ERROR,
              m_server->transact(BINDER_LIB_TEST_GET_FILE_DESCRIPTORS_OWNED_TRANSACTION, data,
                                 &reply));
    reply.setDataPosition(reply.dataSize());
    reply.writeInt32(0);
    reply.setDataPosition(0);
    unique_fd a, b;
    EXPECT_EQ(OK, reply.readUniqueFileDescriptor(&a));
    EXPECT_EQ(OK, reply.readUniqueFileDescriptor(&b));
}

// Used to trigger fdsan error (b/239222407).
TEST_F(BinderLibTest, RecvOwnedFileDescriptorsAndTruncate) {
    GTEST_SKIP() << "triggers fdsan false positive: b/370824489";

    FdLeakDetector fd_leak_detector;

    Parcel data;
    Parcel reply;
    EXPECT_EQ(NO_ERROR,
              m_server->transact(BINDER_LIB_TEST_GET_FILE_DESCRIPTORS_OWNED_TRANSACTION, data,
                                 &reply));
    reply.setDataSize(reply.dataSize() - sizeof(flat_binder_object));
    unique_fd a, b;
    EXPECT_EQ(OK, reply.readUniqueFileDescriptor(&a));
    EXPECT_EQ(BAD_TYPE, reply.readUniqueFileDescriptor(&b));
}

TEST_F(BinderLibTest, RecvUnownedFileDescriptors) {
    FdLeakDetector fd_leak_detector;

    Parcel data;
    Parcel reply;
    EXPECT_EQ(NO_ERROR,
              m_server->transact(BINDER_LIB_TEST_GET_FILE_DESCRIPTORS_UNOWNED_TRANSACTION, data,
                                 &reply));
    unique_fd a, b;
    EXPECT_EQ(OK, reply.readUniqueFileDescriptor(&a));
    EXPECT_EQ(OK, reply.readUniqueFileDescriptor(&b));
}

// Used to trigger fdsan error (b/239222407).
TEST_F(BinderLibTest, RecvUnownedFileDescriptorsAndWriteInt) {
    FdLeakDetector fd_leak_detector;

    Parcel data;
    Parcel reply;
    EXPECT_EQ(NO_ERROR,
              m_server->transact(BINDER_LIB_TEST_GET_FILE_DESCRIPTORS_UNOWNED_TRANSACTION, data,
                                 &reply));
    reply.setDataPosition(reply.dataSize());
    reply.writeInt32(0);
    reply.setDataPosition(0);
    unique_fd a, b;
    EXPECT_EQ(OK, reply.readUniqueFileDescriptor(&a));
    EXPECT_EQ(OK, reply.readUniqueFileDescriptor(&b));
}

// Used to trigger fdsan error (b/239222407).
TEST_F(BinderLibTest, RecvUnownedFileDescriptorsAndTruncate) {
    FdLeakDetector fd_leak_detector;

    Parcel data;
    Parcel reply;
    EXPECT_EQ(NO_ERROR,
              m_server->transact(BINDER_LIB_TEST_GET_FILE_DESCRIPTORS_UNOWNED_TRANSACTION, data,
                                 &reply));
    reply.setDataSize(reply.dataSize() - sizeof(flat_binder_object));
    unique_fd a, b;
    EXPECT_EQ(OK, reply.readUniqueFileDescriptor(&a));
    EXPECT_EQ(BAD_TYPE, reply.readUniqueFileDescriptor(&b));
}

TEST_F(BinderLibTest, PromoteLocal) {
    sp<IBinder> strong = new BBinder();
    wp<IBinder> weak = strong;
    sp<IBinder> strong_from_weak = weak.promote();
    EXPECT_TRUE(strong != nullptr);
    EXPECT_EQ(strong, strong_from_weak);
    strong = nullptr;
    strong_from_weak = nullptr;
    strong_from_weak = weak.promote();
    EXPECT_TRUE(strong_from_weak == nullptr);
}

TEST_F(BinderLibTest, LocalGetExtension) {
    sp<BBinder> binder = new BBinder();
    sp<IBinder> ext = new BBinder();
    binder->setExtension(ext);
    EXPECT_EQ(ext, binder->getExtension());
}

TEST_F(BinderLibTest, RemoteGetExtension) {
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);

    sp<IBinder> extension;
    EXPECT_EQ(NO_ERROR, server->getExtension(&extension));
    ASSERT_NE(nullptr, extension.get());

    EXPECT_EQ(NO_ERROR, extension->pingBinder());
}

TEST_F(BinderLibTest, CheckHandleZeroBinderHighBitsZeroCookie) {
    Parcel data, reply;

    EXPECT_THAT(m_server->transact(BINDER_LIB_TEST_GET_SELF_TRANSACTION, data, &reply),
                StatusEq(NO_ERROR));

    const flat_binder_object *fb = reply.readObject(false);
    ASSERT_TRUE(fb != nullptr);
    EXPECT_EQ(BINDER_TYPE_HANDLE, fb->hdr.type);
    EXPECT_EQ(m_server, ProcessState::self()->getStrongProxyForHandle(fb->handle));
    EXPECT_EQ((binder_uintptr_t)0, fb->cookie);
    EXPECT_EQ((uint64_t)0, (uint64_t)fb->binder >> 32);
}

TEST_F(BinderLibTest, FreedBinder) {
    status_t ret;

    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);

    __u32 freedHandle;
    wp<IBinder> keepFreedBinder;
    {
        Parcel data, reply;
        ASSERT_THAT(server->transact(BINDER_LIB_TEST_CREATE_BINDER_TRANSACTION, data, &reply),
                    StatusEq(NO_ERROR));
        struct flat_binder_object *freed = (struct flat_binder_object *)(reply.data());
        freedHandle = freed->handle;
        /* Add a weak ref to the freed binder so the driver does not
         * delete its reference to it - otherwise the transaction
         * fails regardless of whether the driver is fixed.
         */
        keepFreedBinder = reply.readStrongBinder();
    }
    IPCThreadState::self()->flushCommands();
    {
        Parcel data, reply;
        data.writeStrongBinder(server);
        /* Replace original handle with handle to the freed binder */
        struct flat_binder_object *strong = (struct flat_binder_object *)(data.data());
        __u32 oldHandle = strong->handle;
        strong->handle = freedHandle;
        ret = server->transact(BINDER_LIB_TEST_ADD_STRONG_REF_TRANSACTION, data, &reply);
        /* Returns DEAD_OBJECT (-32) if target crashes and
         * FAILED_TRANSACTION if the driver rejects the invalid
         * object.
         */
        EXPECT_EQ((status_t)FAILED_TRANSACTION, ret);
        /* Restore original handle so parcel destructor does not use
         * the wrong handle.
         */
        strong->handle = oldHandle;
    }
}

TEST_F(BinderLibTest, CheckNoHeaderMappedInUser) {
    Parcel data, reply;
    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    for (int i = 0; i < 2; i++) {
        BinderLibTestBundle datai;
        datai.appendFrom(&data, 0, data.dataSize());

        data.freeData();
        data.writeInt32(1);
        data.writeStrongBinder(callBack);
        data.writeInt32(BINDER_LIB_TEST_CALL_BACK_VERIFY_BUF);

        datai.appendTo(&data);
    }
    EXPECT_THAT(m_server->transact(BINDER_LIB_TEST_INDIRECT_TRANSACTION, data, &reply),
                StatusEq(NO_ERROR));
}

TEST_F(BinderLibTest, OnewayQueueing)
{
    Parcel data, data2;

    sp<IBinder> pollServer = addPollServer();

    sp<BinderLibTestCallBack> callBack = new BinderLibTestCallBack();
    data.writeStrongBinder(callBack);
    data.writeInt32(500000); // delay in us before calling back

    sp<BinderLibTestCallBack> callBack2 = new BinderLibTestCallBack();
    data2.writeStrongBinder(callBack2);
    data2.writeInt32(0); // delay in us

    EXPECT_THAT(pollServer->transact(BINDER_LIB_TEST_DELAYED_CALL_BACK, data, nullptr, TF_ONE_WAY),
                StatusEq(NO_ERROR));

    // The delay ensures that this second transaction will end up on the async_todo list
    // (for a single-threaded server)
    EXPECT_THAT(pollServer->transact(BINDER_LIB_TEST_DELAYED_CALL_BACK, data2, nullptr, TF_ONE_WAY),
                StatusEq(NO_ERROR));

    // The server will ensure that the two transactions are handled in the expected order;
    // If the ordering is not as expected, an error will be returned through the callbacks.
    EXPECT_THAT(callBack->waitEvent(2), StatusEq(NO_ERROR));
    EXPECT_THAT(callBack->getResult(), StatusEq(NO_ERROR));

    EXPECT_THAT(callBack2->waitEvent(2), StatusEq(NO_ERROR));
    EXPECT_THAT(callBack2->getResult(), StatusEq(NO_ERROR));
}

TEST_F(BinderLibTest, WorkSourceUnsetByDefault)
{
    status_t ret;
    Parcel data, reply;
    data.writeInterfaceToken(binderLibTestServiceName);
    ret = m_server->transact(BINDER_LIB_TEST_GET_WORK_SOURCE_TRANSACTION, data, &reply);
    EXPECT_EQ(-1, reply.readInt32());
    EXPECT_EQ(NO_ERROR, ret);
}

TEST_F(BinderLibTest, WorkSourceSet)
{
    status_t ret;
    Parcel data, reply;
    IPCThreadState::self()->clearCallingWorkSource();
    int64_t previousWorkSource = IPCThreadState::self()->setCallingWorkSourceUid(100);
    data.writeInterfaceToken(binderLibTestServiceName);
    ret = m_server->transact(BINDER_LIB_TEST_GET_WORK_SOURCE_TRANSACTION, data, &reply);
    EXPECT_EQ(100, reply.readInt32());
    EXPECT_EQ(-1, previousWorkSource);
    EXPECT_EQ(true, IPCThreadState::self()->shouldPropagateWorkSource());
    EXPECT_EQ(NO_ERROR, ret);
}

TEST_F(BinderLibTest, WorkSourceSetWithoutPropagation)
{
    status_t ret;
    Parcel data, reply;

    IPCThreadState::self()->setCallingWorkSourceUidWithoutPropagation(100);
    EXPECT_EQ(false, IPCThreadState::self()->shouldPropagateWorkSource());

    data.writeInterfaceToken(binderLibTestServiceName);
    ret = m_server->transact(BINDER_LIB_TEST_GET_WORK_SOURCE_TRANSACTION, data, &reply);
    EXPECT_EQ(-1, reply.readInt32());
    EXPECT_EQ(false, IPCThreadState::self()->shouldPropagateWorkSource());
    EXPECT_EQ(NO_ERROR, ret);
}

TEST_F(BinderLibTest, WorkSourceCleared)
{
    status_t ret;
    Parcel data, reply;

    IPCThreadState::self()->setCallingWorkSourceUid(100);
    int64_t token = IPCThreadState::self()->clearCallingWorkSource();
    int32_t previousWorkSource = (int32_t)token;
    data.writeInterfaceToken(binderLibTestServiceName);
    ret = m_server->transact(BINDER_LIB_TEST_GET_WORK_SOURCE_TRANSACTION, data, &reply);

    EXPECT_EQ(-1, reply.readInt32());
    EXPECT_EQ(100, previousWorkSource);
    EXPECT_EQ(NO_ERROR, ret);
}

TEST_F(BinderLibTest, WorkSourceRestored)
{
    status_t ret;
    Parcel data, reply;

    IPCThreadState::self()->setCallingWorkSourceUid(100);
    int64_t token = IPCThreadState::self()->clearCallingWorkSource();
    IPCThreadState::self()->restoreCallingWorkSource(token);

    data.writeInterfaceToken(binderLibTestServiceName);
    ret = m_server->transact(BINDER_LIB_TEST_GET_WORK_SOURCE_TRANSACTION, data, &reply);

    EXPECT_EQ(100, reply.readInt32());
    EXPECT_EQ(true, IPCThreadState::self()->shouldPropagateWorkSource());
    EXPECT_EQ(NO_ERROR, ret);
}

TEST_F(BinderLibTest, PropagateFlagSet)
{
    IPCThreadState::self()->clearPropagateWorkSource();
    IPCThreadState::self()->setCallingWorkSourceUid(100);
    EXPECT_EQ(true, IPCThreadState::self()->shouldPropagateWorkSource());
}

TEST_F(BinderLibTest, PropagateFlagCleared)
{
    IPCThreadState::self()->setCallingWorkSourceUid(100);
    IPCThreadState::self()->clearPropagateWorkSource();
    EXPECT_EQ(false, IPCThreadState::self()->shouldPropagateWorkSource());
}

TEST_F(BinderLibTest, PropagateFlagRestored)
{
    int token = IPCThreadState::self()->setCallingWorkSourceUid(100);
    IPCThreadState::self()->restoreCallingWorkSource(token);

    EXPECT_EQ(false, IPCThreadState::self()->shouldPropagateWorkSource());
}

TEST_F(BinderLibTest, WorkSourcePropagatedForAllFollowingBinderCalls)
{
    IPCThreadState::self()->setCallingWorkSourceUid(100);

    Parcel data, reply;
    status_t ret;
    data.writeInterfaceToken(binderLibTestServiceName);
    ret = m_server->transact(BINDER_LIB_TEST_GET_WORK_SOURCE_TRANSACTION, data, &reply);
    EXPECT_EQ(NO_ERROR, ret);

    Parcel data2, reply2;
    status_t ret2;
    data2.writeInterfaceToken(binderLibTestServiceName);
    ret2 = m_server->transact(BINDER_LIB_TEST_GET_WORK_SOURCE_TRANSACTION, data2, &reply2);
    EXPECT_EQ(100, reply2.readInt32());
    EXPECT_EQ(NO_ERROR, ret2);
}

TEST_F(BinderLibTest, SchedPolicySet) {
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);

    Parcel data, reply;
    EXPECT_THAT(server->transact(BINDER_LIB_TEST_GET_SCHEDULING_POLICY, data, &reply),
                StatusEq(NO_ERROR));

    int policy = reply.readInt32();
    int priority = reply.readInt32();

    EXPECT_EQ(kSchedPolicy, policy & (~SCHED_RESET_ON_FORK));
    EXPECT_EQ(kSchedPriority, priority);
}

TEST_F(BinderLibTest, InheritRt) {
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);

    const struct sched_param param {
        .sched_priority = kSchedPriorityMore,
    };
    EXPECT_EQ(0, sched_setscheduler(getpid(), SCHED_RR, &param));

    Parcel data, reply;
    EXPECT_THAT(server->transact(BINDER_LIB_TEST_GET_SCHEDULING_POLICY, data, &reply),
                StatusEq(NO_ERROR));

    int policy = reply.readInt32();
    int priority = reply.readInt32();

    EXPECT_EQ(kSchedPolicy, policy & (~SCHED_RESET_ON_FORK));
    EXPECT_EQ(kSchedPriorityMore, priority);
}

TEST_F(BinderLibTest, VectorSent) {
    Parcel data, reply;
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);

    std::vector<uint64_t> const testValue = { std::numeric_limits<uint64_t>::max(), 0, 200 };
    data.writeUint64Vector(testValue);

    EXPECT_THAT(server->transact(BINDER_LIB_TEST_ECHO_VECTOR, data, &reply), StatusEq(NO_ERROR));
    std::vector<uint64_t> readValue;
    EXPECT_THAT(reply.readUint64Vector(&readValue), StatusEq(OK));
    EXPECT_EQ(readValue, testValue);
}

TEST_F(BinderLibTest, FileDescriptorRemainsNonBlocking) {
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);

    Parcel reply;
    EXPECT_THAT(server->transact(BINDER_LIB_TEST_GET_NON_BLOCKING_FD, {} /*data*/, &reply),
                StatusEq(NO_ERROR));
    unique_fd fd;
    EXPECT_THAT(reply.readUniqueFileDescriptor(&fd), StatusEq(OK));

    const int result = fcntl(fd.get(), F_GETFL);
    ASSERT_NE(result, -1);
    EXPECT_EQ(result & O_NONBLOCK, O_NONBLOCK);
}

// see ProcessState.cpp BINDER_VM_SIZE = 1MB.
// This value is not exposed, but some code in the framework relies on being able to use
// buffers near the cap size.
constexpr size_t kSizeBytesAlmostFull = 950'000;
constexpr size_t kSizeBytesOverFull = 1'050'000;

TEST_F(BinderLibTest, GargantuanVectorSent) {
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);

    for (size_t i = 0; i < 10; i++) {
        // a slight variation in size is used to consider certain possible caching implementations
        const std::vector<uint64_t> testValue((kSizeBytesAlmostFull + i) / sizeof(uint64_t), 42);

        Parcel data, reply;
        data.writeUint64Vector(testValue);
        EXPECT_THAT(server->transact(BINDER_LIB_TEST_ECHO_VECTOR, data, &reply), StatusEq(NO_ERROR))
                << i;
        std::vector<uint64_t> readValue;
        EXPECT_THAT(reply.readUint64Vector(&readValue), StatusEq(OK));
        EXPECT_EQ(readValue, testValue);
    }
}

TEST_F(BinderLibTest, LimitExceededVectorSent) {
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);
    const std::vector<uint64_t> testValue(kSizeBytesOverFull / sizeof(uint64_t), 42);

    Parcel data, reply;
    data.writeUint64Vector(testValue);
    EXPECT_THAT(server->transact(BINDER_LIB_TEST_ECHO_VECTOR, data, &reply),
                StatusEq(FAILED_TRANSACTION));
}

TEST_F(BinderLibTest, BufRejected) {
    Parcel data, reply;
    uint32_t buf;
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);

    binder_buffer_object obj {
        .hdr = { .type = BINDER_TYPE_PTR },
        .flags = 0,
        .buffer = reinterpret_cast<binder_uintptr_t>((void*)&buf),
        .length = 4,
    };
    data.setDataCapacity(1024);
    // Write a bogus object at offset 0 to get an entry in the offset table
    data.writeFileDescriptor(0);
    EXPECT_EQ(data.objectsCount(), 1u);
    uint8_t *parcelData = const_cast<uint8_t*>(data.data());
    // And now, overwrite it with the buffer object
    memcpy(parcelData, &obj, sizeof(obj));
    data.setDataSize(sizeof(obj));

    EXPECT_EQ(data.objectsCount(), 1u);

    // Either the kernel should reject this transaction (if it's correct), but
    // if it's not, the server implementation should return an error if it
    // finds an object in the received Parcel.
    EXPECT_THAT(server->transact(BINDER_LIB_TEST_REJECT_OBJECTS, data, &reply),
                Not(StatusEq(NO_ERROR)));
}

TEST_F(BinderLibTest, WeakRejected) {
    Parcel data, reply;
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);

    auto binder = sp<BBinder>::make();
    wp<BBinder> wpBinder(binder);
    flat_binder_object obj{
            .hdr = {.type = BINDER_TYPE_WEAK_BINDER},
            .flags = 0,
            .binder = reinterpret_cast<uintptr_t>(wpBinder.get_refs()),
            .cookie = reinterpret_cast<uintptr_t>(wpBinder.unsafe_get()),
    };
    data.setDataCapacity(1024);
    // Write a bogus object at offset 0 to get an entry in the offset table
    data.writeFileDescriptor(0);
    EXPECT_EQ(data.objectsCount(), 1u);
    uint8_t *parcelData = const_cast<uint8_t *>(data.data());
    // And now, overwrite it with the weak binder
    memcpy(parcelData, &obj, sizeof(obj));
    data.setDataSize(sizeof(obj));

    // a previous bug caused other objects to be released an extra time, so we
    // test with an object that libbinder will actually try to release
    EXPECT_EQ(OK, data.writeStrongBinder(sp<BBinder>::make()));

    EXPECT_EQ(data.objectsCount(), 2u);

    // send it many times, since previous error was memory corruption, make it
    // more likely that the server crashes
    for (size_t i = 0; i < 100; i++) {
        EXPECT_THAT(server->transact(BINDER_LIB_TEST_REJECT_OBJECTS, data, &reply),
                    StatusEq(BAD_VALUE));
    }

    EXPECT_THAT(server->pingBinder(), StatusEq(NO_ERROR));
}

TEST_F(BinderLibTest, GotSid) {
    sp<IBinder> server = addServer();

    Parcel data;
    EXPECT_THAT(server->transact(BINDER_LIB_TEST_CAN_GET_SID, data, nullptr), StatusEq(OK));
}

struct TooManyFdsFlattenable : Flattenable<TooManyFdsFlattenable> {
    TooManyFdsFlattenable(size_t fdCount) : mFdCount(fdCount) {}

    // Flattenable protocol
    size_t getFlattenedSize() const {
        // Return a valid non-zero size here so we don't get an unintended
        // BAD_VALUE from Parcel::write
        return 16;
    }
    size_t getFdCount() const { return mFdCount; }
    status_t flatten(void *& /*buffer*/, size_t & /*size*/, int *&fds, size_t &count) const {
        for (size_t i = 0; i < count; i++) {
            fds[i] = STDIN_FILENO;
        }
        return NO_ERROR;
    }
    status_t unflatten(void const *& /*buffer*/, size_t & /*size*/, int const *& /*fds*/,
                       size_t & /*count*/) {
        /* This doesn't get called */
        return NO_ERROR;
    }

    size_t mFdCount;
};

TEST_F(BinderLibTest, TooManyFdsFlattenable) {
    rlimit origNofile;
    int ret = getrlimit(RLIMIT_NOFILE, &origNofile);
    ASSERT_EQ(0, ret);

    // Restore the original file limits when the test finishes
    auto guardUnguard = make_scope_guard([&]() { setrlimit(RLIMIT_NOFILE, &origNofile); });

    rlimit testNofile = {1024, 1024};
    ret = setrlimit(RLIMIT_NOFILE, &testNofile);
    ASSERT_EQ(0, ret);

    Parcel parcel;
    // Try to write more file descriptors than supported by the OS
    TooManyFdsFlattenable tooManyFds1(1024);
    EXPECT_THAT(parcel.write(tooManyFds1), StatusEq(-EMFILE));

    // Try to write more file descriptors than the internal limit
    TooManyFdsFlattenable tooManyFds2(1025);
    EXPECT_THAT(parcel.write(tooManyFds2), StatusEq(BAD_VALUE));
}

TEST(ServiceNotifications, Unregister) {
    auto sm = defaultServiceManager();
    sm->enableAddServiceCache(false);
    using LocalRegistrationCallback = IServiceManager::LocalRegistrationCallback;
    class LocalRegistrationCallbackImpl : public virtual LocalRegistrationCallback {
        void onServiceRegistration(const String16 &, const sp<IBinder> &) override {}
        virtual ~LocalRegistrationCallbackImpl() {}
    };
    sp<LocalRegistrationCallback> cb = sp<LocalRegistrationCallbackImpl>::make();

    EXPECT_EQ(sm->registerForNotifications(String16("RogerRafa"), cb), OK);
    EXPECT_EQ(sm->unregisterForNotifications(String16("RogerRafa"), cb), OK);
}

// Make sure all IServiceManager APIs will function without an AIDL service
// manager registered on the device.
TEST(ServiceManagerNoAidlServer, SanityCheck) {
    String16 kServiceName("no_services_exist");
    // This is what clients will see when there is no servicemanager process
    // that registers itself as context object 0.
    // Can't use setDefaultServiceManager() here because these test cases run in
    // the same process and will abort when called twice or before/after
    // defaultServiceManager().
    sp<IServiceManager> sm = getServiceManagerShimFromAidlServiceManagerForTests(nullptr);
    auto status = sm->addService(kServiceName, sp<BBinder>::make());
    // CppBackendShim returns Status::exceptionCode as the status_t
    EXPECT_EQ(status, Status::Exception::EX_UNSUPPORTED_OPERATION) << statusToString(status);
    auto service = sm->checkService(String16("no_services_exist"));
    EXPECT_TRUE(service == nullptr);
    auto list = sm->listServices(android::IServiceManager::DUMP_FLAG_PRIORITY_ALL);
    EXPECT_TRUE(list.isEmpty());
    bool declared = sm->isDeclared(kServiceName);
    EXPECT_FALSE(declared);
    list = sm->getDeclaredInstances(kServiceName);
    EXPECT_TRUE(list.isEmpty());
    auto updatable = sm->updatableViaApex(kServiceName);
    EXPECT_EQ(updatable, std::nullopt);
    list = sm->getUpdatableNames(kServiceName);
    EXPECT_TRUE(list.isEmpty());
    auto conInfo = sm->getConnectionInfo(kServiceName);
    EXPECT_EQ(conInfo, std::nullopt);
    auto cb = sp<LocalRegistrationCallbackImpl>::make();
    status = sm->registerForNotifications(kServiceName, cb);
    EXPECT_EQ(status, UNKNOWN_ERROR) << statusToString(status);
    status = sm->unregisterForNotifications(kServiceName, cb);
    EXPECT_EQ(status, BAD_VALUE) << statusToString(status);
    auto dbgInfos = sm->getServiceDebugInfo();
    EXPECT_TRUE(dbgInfos.empty());
    sm->enableAddServiceCache(true);
}

TEST_F(BinderLibTest, ThreadPoolAvailableThreads) {
    Parcel data, reply;
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);
    EXPECT_THAT(server->transact(BINDER_LIB_TEST_GET_MAX_THREAD_COUNT, data, &reply),
                StatusEq(NO_ERROR));
    int32_t replyi = reply.readInt32();
    // see getThreadPoolMaxTotalThreadCount for why there is a race
    EXPECT_TRUE(replyi == kKernelThreads + 1 || replyi == kKernelThreads + 2) << replyi;

    EXPECT_THAT(server->transact(BINDER_LIB_TEST_PROCESS_LOCK, data, &reply), NO_ERROR);

    /*
     * This will use all threads in the pool but one. There are actually kKernelThreads+2
     * available in the other process (startThreadPool, joinThreadPool, + the kernel-
     * started threads from setThreadPoolMaxThreadCount
     *
     * Adding one more will cause it to deadlock.
     */
    std::vector<std::thread> ts;
    for (size_t i = 0; i < kKernelThreads + 1; i++) {
        ts.push_back(std::thread([&] {
            Parcel local_reply;
            EXPECT_THAT(server->transact(BINDER_LIB_TEST_LOCK_UNLOCK, data, &local_reply),
                        NO_ERROR);
        }));
    }

    // make sure all of the above calls will be queued in parallel. Otherwise, most of
    // the time, the below call will pre-empt them (presumably because we have the
    // scheduler timeslice already + scheduler hint).
    sleep(1);

    data.writeInt32(1000);
    // Give a chance for all threads to be used (kKernelThreads + 1 thread in use)
    EXPECT_THAT(server->transact(BINDER_LIB_TEST_UNLOCK_AFTER_MS, data, &reply), NO_ERROR);

    for (auto &t : ts) {
        t.join();
    }

    EXPECT_THAT(server->transact(BINDER_LIB_TEST_GET_MAX_THREAD_COUNT, data, &reply),
                StatusEq(NO_ERROR));
    replyi = reply.readInt32();
    EXPECT_EQ(replyi, kKernelThreads + 2);
}

TEST_F(BinderLibTest, ThreadPoolStarted) {
    Parcel data, reply;
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);
    EXPECT_THAT(server->transact(BINDER_LIB_TEST_IS_THREADPOOL_STARTED, data, &reply), NO_ERROR);
    EXPECT_TRUE(reply.readBool());
}

TEST_F(BinderLibTest, HangingServices) {
    Parcel data, reply;
    sp<IBinder> server = addServer();
    ASSERT_TRUE(server != nullptr);
    int32_t delay = 1000; // ms
    data.writeInt32(delay);
    // b/266537959 - must take before taking lock, since countdown is started in the remote
    // process there.
    int64_t timeBefore = uptimeMillis();
    EXPECT_THAT(server->transact(BINDER_LIB_TEST_PROCESS_TEMPORARY_LOCK, data, &reply), NO_ERROR);
    std::vector<std::thread> ts;
    for (size_t i = 0; i < kKernelThreads + 1; i++) {
        ts.push_back(std::thread([&] {
            Parcel local_reply;
            EXPECT_THAT(server->transact(BINDER_LIB_TEST_LOCK_UNLOCK, data, &local_reply),
                        NO_ERROR);
        }));
    }

    for (auto &t : ts) {
        t.join();
    }
    int64_t timeAfter = uptimeMillis();

    // deadlock occurred and threads only finished after 1s passed.
    EXPECT_GE(timeAfter, timeBefore + delay);
}

TEST_F(BinderLibTest, BinderProxyCount) {
    Parcel data, reply;
    sp<IBinder> server = addServer();
    ASSERT_NE(server, nullptr);

    uint32_t initialCount = BpBinder::getBinderProxyCount();
    size_t iterations = 100;
    {
        uint32_t count = initialCount;
        std::vector<sp<IBinder> > proxies;
        sp<IBinder> proxy;
        // Create binder proxies and verify the count.
        for (size_t i = 0; i < iterations; i++) {
            ASSERT_THAT(server->transact(BINDER_LIB_TEST_CREATE_BINDER_TRANSACTION, data, &reply),
                        StatusEq(NO_ERROR));
            proxies.push_back(reply.readStrongBinder());
            EXPECT_EQ(BpBinder::getBinderProxyCount(), ++count);
        }
        // Remove every other one and verify the count.
        auto it = proxies.begin();
        for (size_t i = 0; it != proxies.end(); i++) {
            if (i % 2 == 0) {
                it = proxies.erase(it);
                EXPECT_EQ(BpBinder::getBinderProxyCount(), --count);
            }
        }
    }
    EXPECT_EQ(BpBinder::getBinderProxyCount(), initialCount);
}

static constexpr int kBpCountHighWatermark = 20;
static constexpr int kBpCountLowWatermark = 10;
static constexpr int kBpCountWarningWatermark = 15;
static constexpr int kInvalidUid = -1;

TEST_F(BinderLibTest, BinderProxyCountCallback) {
    Parcel data, reply;
    sp<IBinder> server = addServer();
    ASSERT_NE(server, nullptr);

    BpBinder::enableCountByUid();
    EXPECT_THAT(m_server->transact(BINDER_LIB_TEST_GETUID, data, &reply), StatusEq(NO_ERROR));
    int32_t uid = reply.readInt32();
    ASSERT_NE(uid, kInvalidUid);

    uint32_t initialCount = BpBinder::getBinderProxyCount();
    {
        uint32_t count = initialCount;
        BpBinder::setBinderProxyCountWatermarks(kBpCountHighWatermark,
                                                kBpCountLowWatermark,
                                                kBpCountWarningWatermark);
        int limitCallbackUid = kInvalidUid;
        int warningCallbackUid = kInvalidUid;
        BpBinder::setBinderProxyCountEventCallback([&](int uid) { limitCallbackUid = uid; },
                                                   [&](int uid) { warningCallbackUid = uid; });

        std::vector<sp<IBinder> > proxies;
        auto createProxyOnce = [&](int expectedWarningCallbackUid, int expectedLimitCallbackUid) {
            warningCallbackUid = limitCallbackUid = kInvalidUid;
            ASSERT_THAT(server->transact(BINDER_LIB_TEST_CREATE_BINDER_TRANSACTION, data, &reply),
                        StatusEq(NO_ERROR));
            proxies.push_back(reply.readStrongBinder());
            EXPECT_EQ(BpBinder::getBinderProxyCount(), ++count);
            EXPECT_EQ(warningCallbackUid, expectedWarningCallbackUid);
            EXPECT_EQ(limitCallbackUid, expectedLimitCallbackUid);
        };
        auto removeProxyOnce = [&](int expectedWarningCallbackUid, int expectedLimitCallbackUid) {
            warningCallbackUid = limitCallbackUid = kInvalidUid;
            proxies.pop_back();
            EXPECT_EQ(BpBinder::getBinderProxyCount(), --count);
            EXPECT_EQ(warningCallbackUid, expectedWarningCallbackUid);
            EXPECT_EQ(limitCallbackUid, expectedLimitCallbackUid);
        };

        // Test the increment/decrement of the binder proxies.
        for (int i = 1; i <= kBpCountWarningWatermark; i++) {
            createProxyOnce(kInvalidUid, kInvalidUid);
        }
        createProxyOnce(uid, kInvalidUid); // Warning callback should have been triggered.
        for (int i = kBpCountWarningWatermark + 2; i <= kBpCountHighWatermark; i++) {
            createProxyOnce(kInvalidUid, kInvalidUid);
        }
        createProxyOnce(kInvalidUid, uid); // Limit callback should have been triggered.
        createProxyOnce(kInvalidUid, kInvalidUid);
        for (int i = kBpCountHighWatermark + 2; i >= kBpCountHighWatermark; i--) {
            removeProxyOnce(kInvalidUid, kInvalidUid);
        }
        createProxyOnce(kInvalidUid, kInvalidUid);

        // Go down below the low watermark.
        for (int i = kBpCountHighWatermark; i >= kBpCountLowWatermark; i--) {
            removeProxyOnce(kInvalidUid, kInvalidUid);
        }
        for (int i = kBpCountLowWatermark; i <= kBpCountWarningWatermark; i++) {
            createProxyOnce(kInvalidUid, kInvalidUid);
        }
        createProxyOnce(uid, kInvalidUid); // Warning callback should have been triggered.
        for (int i = kBpCountWarningWatermark + 2; i <= kBpCountHighWatermark; i++) {
            createProxyOnce(kInvalidUid, kInvalidUid);
        }
        createProxyOnce(kInvalidUid, uid); // Limit callback should have been triggered.
        createProxyOnce(kInvalidUid, kInvalidUid);
        for (int i = kBpCountHighWatermark + 2; i >= kBpCountHighWatermark; i--) {
            removeProxyOnce(kInvalidUid, kInvalidUid);
        }
        createProxyOnce(kInvalidUid, kInvalidUid);
    }
    EXPECT_EQ(BpBinder::getBinderProxyCount(), initialCount);
}

class BinderLibRpcTestBase : public BinderLibTest {
public:
    void SetUp() override {
        if (!base::GetBoolProperty("ro.debuggable", false)) {
            GTEST_SKIP() << "Binder RPC is only enabled on debuggable builds, skipping test on "
                            "non-debuggable builds.";
        }
        BinderLibTest::SetUp();
    }

    std::tuple<unique_fd, unsigned int> CreateSocket() {
        auto rpcServer = RpcServer::make();
        EXPECT_NE(nullptr, rpcServer);
        if (rpcServer == nullptr) return {};
        unsigned int port;
        if (status_t status = rpcServer->setupInetServer("127.0.0.1", 0, &port); status != OK) {
            ADD_FAILURE() << "setupInetServer failed" << statusToString(status);
            return {};
        }
        return {rpcServer->releaseServer(), port};
    }
};

class BinderLibRpcTest : public BinderLibRpcTestBase {};

// e.g. EXPECT_THAT(expr, Debuggable(StatusEq(...))
// If device is debuggable AND not on user builds, expects matcher.
// Otherwise expects INVALID_OPERATION.
// Debuggable + non user builds is necessary but not sufficient for setRpcClientDebug to work.
static Matcher<status_t> Debuggable(const Matcher<status_t> &matcher) {
    bool isDebuggable = android::base::GetBoolProperty("ro.debuggable", false) &&
            android::base::GetProperty("ro.build.type", "") != "user";
    return isDebuggable ? matcher : StatusEq(INVALID_OPERATION);
}

TEST_F(BinderLibRpcTest, SetRpcClientDebug) {
    auto binder = addServer();
    ASSERT_TRUE(binder != nullptr);
    auto [socket, port] = CreateSocket();
    ASSERT_TRUE(socket.ok());
    EXPECT_THAT(binder->setRpcClientDebug(std::move(socket), sp<BBinder>::make()),
                Debuggable(StatusEq(OK)));
}

// Tests for multiple RpcServer's on the same binder object.
TEST_F(BinderLibRpcTest, SetRpcClientDebugTwice) {
    auto binder = addServer();
    ASSERT_TRUE(binder != nullptr);

    auto [socket1, port1] = CreateSocket();
    ASSERT_TRUE(socket1.ok());
    auto keepAliveBinder1 = sp<BBinder>::make();
    EXPECT_THAT(binder->setRpcClientDebug(std::move(socket1), keepAliveBinder1),
                Debuggable(StatusEq(OK)));

    auto [socket2, port2] = CreateSocket();
    ASSERT_TRUE(socket2.ok());
    auto keepAliveBinder2 = sp<BBinder>::make();
    EXPECT_THAT(binder->setRpcClientDebug(std::move(socket2), keepAliveBinder2),
                Debuggable(StatusEq(OK)));
}

// Negative tests for RPC APIs on IBinder. Call should fail in the same way on both remote and
// local binders.
class BinderLibRpcTestP : public BinderLibRpcTestBase, public WithParamInterface<bool> {
public:
    sp<IBinder> GetService() {
        return GetParam() ? sp<IBinder>(addServer()) : sp<IBinder>(sp<BBinder>::make());
    }
    static std::string ParamToString(const testing::TestParamInfo<ParamType> &info) {
        return info.param ? "remote" : "local";
    }
};

TEST_P(BinderLibRpcTestP, SetRpcClientDebugNoFd) {
    auto binder = GetService();
    ASSERT_TRUE(binder != nullptr);
    EXPECT_THAT(binder->setRpcClientDebug(unique_fd(), sp<BBinder>::make()),
                Debuggable(StatusEq(BAD_VALUE)));
}

TEST_P(BinderLibRpcTestP, SetRpcClientDebugNoKeepAliveBinder) {
    auto binder = GetService();
    ASSERT_TRUE(binder != nullptr);
    auto [socket, port] = CreateSocket();
    ASSERT_TRUE(socket.ok());
    EXPECT_THAT(binder->setRpcClientDebug(std::move(socket), nullptr),
                Debuggable(StatusEq(UNEXPECTED_NULL)));
}
INSTANTIATE_TEST_SUITE_P(BinderLibTest, BinderLibRpcTestP, testing::Bool(),
                         BinderLibRpcTestP::ParamToString);

class BinderLibTestService : public BBinder {
public:
    explicit BinderLibTestService(int32_t id, bool exitOnDestroy = true)
          : m_id(id),
            m_nextServerId(id + 1),
            m_serverStartRequested(false),
            m_callback(nullptr),
            m_exitOnDestroy(exitOnDestroy) {
        pthread_mutex_init(&m_serverWaitMutex, nullptr);
        pthread_cond_init(&m_serverWaitCond, nullptr);
    }
    ~BinderLibTestService() {
        if (m_exitOnDestroy) exit(EXIT_SUCCESS);
    }

    void processPendingCall() {
        if (m_callback != nullptr) {
            Parcel data;
            data.writeInt32(NO_ERROR);
            m_callback->transact(BINDER_LIB_TEST_CALL_BACK, data, nullptr, TF_ONE_WAY);
            m_callback = nullptr;
        }
    }

    virtual status_t onTransact(uint32_t code, const Parcel &data, Parcel *reply,
                                uint32_t flags = 0) {
        // TODO(b/182914638): also checks getCallingUid() for RPC
        if (!data.isForRpc() && getuid() != (uid_t)IPCThreadState::self()->getCallingUid()) {
            return PERMISSION_DENIED;
        }
        switch (code) {
            case BINDER_LIB_TEST_REGISTER_SERVER: {
                sp<IBinder> binder;
                /*id =*/data.readInt32();
                binder = data.readStrongBinder();
                if (binder == nullptr) {
                    return BAD_VALUE;
                }

                if (m_id != 0) return INVALID_OPERATION;

                pthread_mutex_lock(&m_serverWaitMutex);
                if (m_serverStartRequested) {
                    m_serverStartRequested = false;
                    m_serverStarted = binder;
                    pthread_cond_signal(&m_serverWaitCond);
                }
                pthread_mutex_unlock(&m_serverWaitMutex);
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_ADD_POLL_SERVER:
            case BINDER_LIB_TEST_ADD_SERVER: {
                int ret;
                int serverid;

                if (m_id != 0) {
                    return INVALID_OPERATION;
                }
                pthread_mutex_lock(&m_serverWaitMutex);
                if (m_serverStartRequested) {
                    ret = -EBUSY;
                } else {
                    serverid = m_nextServerId++;
                    m_serverStartRequested = true;
                    bool usePoll = code == BINDER_LIB_TEST_ADD_POLL_SERVER;

                    pthread_mutex_unlock(&m_serverWaitMutex);
                    ret = start_server_process(serverid, usePoll);
                    pthread_mutex_lock(&m_serverWaitMutex);
                }
                if (ret > 0) {
                    if (m_serverStartRequested) {
                        struct timespec ts;
                        clock_gettime(CLOCK_REALTIME, &ts);
                        ts.tv_sec += 5;
                        ret = pthread_cond_timedwait(&m_serverWaitCond, &m_serverWaitMutex, &ts);
                    }
                    if (m_serverStartRequested) {
                        m_serverStartRequested = false;
                        ret = -ETIMEDOUT;
                    } else {
                        reply->writeStrongBinder(m_serverStarted);
                        reply->writeInt32(serverid);
                        m_serverStarted = nullptr;
                        ret = NO_ERROR;
                    }
                } else if (ret >= 0) {
                    m_serverStartRequested = false;
                    ret = UNKNOWN_ERROR;
                }
                pthread_mutex_unlock(&m_serverWaitMutex);
                return ret;
            }
            case BINDER_LIB_TEST_USE_CALLING_GUARD_TRANSACTION: {
                IPCThreadState::SpGuard spGuard{
                        .address = __builtin_frame_address(0),
                        .context = "GuardInBinderTransaction",
                };
                const IPCThreadState::SpGuard *origGuard =
                        IPCThreadState::self()->pushGetCallingSpGuard(&spGuard);

                // if the guard works, this should abort
                (void)IPCThreadState::self()->getCallingPid();

                IPCThreadState::self()->restoreGetCallingSpGuard(origGuard);
                return NO_ERROR;
            }

            case BINDER_LIB_TEST_GETPID:
                reply->writeInt32(getpid());
                return NO_ERROR;
            case BINDER_LIB_TEST_GETUID:
                reply->writeInt32(getuid());
                return NO_ERROR;
            case BINDER_LIB_TEST_NOP_TRANSACTION_WAIT:
                usleep(5000);
                [[fallthrough]];
            case BINDER_LIB_TEST_NOP_TRANSACTION:
                // oneway error codes should be ignored
                if (flags & TF_ONE_WAY) {
                    return UNKNOWN_ERROR;
                }
                return NO_ERROR;
            case BINDER_LIB_TEST_DELAYED_CALL_BACK: {
                // Note: this transaction is only designed for use with a
                // poll() server. See comments around epoll_wait().
                if (m_callback != nullptr) {
                    // A callback was already pending; this means that
                    // we received a second call while still processing
                    // the first one. Fail the test.
                    sp<IBinder> callback = data.readStrongBinder();
                    Parcel data2;
                    data2.writeInt32(UNKNOWN_ERROR);

                    callback->transact(BINDER_LIB_TEST_CALL_BACK, data2, nullptr, TF_ONE_WAY);
                } else {
                    m_callback = data.readStrongBinder();
                    int32_t delayUs = data.readInt32();
                    /*
                     * It's necessary that we sleep here, so the next
                     * transaction the caller makes will be queued to
                     * the async queue.
                     */
                    usleep(delayUs);

                    /*
                     * Now when we return, libbinder will tell the kernel
                     * we are done with this transaction, and the kernel
                     * can move the queued transaction to either the
                     * thread todo worklist (for kernels without the fix),
                     * or the proc todo worklist. In case of the former,
                     * the next outbound call will pick up the pending
                     * transaction, which leads to undesired reentrant
                     * behavior. This is caught in the if() branch above.
                     */
                }

                return NO_ERROR;
            }
            case BINDER_LIB_TEST_NOP_CALL_BACK: {
                Parcel data2, reply2;
                sp<IBinder> binder;
                binder = data.readStrongBinder();
                if (binder == nullptr) {
                    return BAD_VALUE;
                }
                data2.writeInt32(NO_ERROR);
                binder->transact(BINDER_LIB_TEST_CALL_BACK, data2, &reply2);
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_GET_SELF_TRANSACTION:
                reply->writeStrongBinder(this);
                return NO_ERROR;
            case BINDER_LIB_TEST_GET_ID_TRANSACTION:
                reply->writeInt32(m_id);
                return NO_ERROR;
            case BINDER_LIB_TEST_INDIRECT_TRANSACTION: {
                int32_t count;
                uint32_t indirect_code;
                sp<IBinder> binder;

                count = data.readInt32();
                reply->writeInt32(m_id);
                reply->writeInt32(count);
                for (int i = 0; i < count; i++) {
                    binder = data.readStrongBinder();
                    if (binder == nullptr) {
                        return BAD_VALUE;
                    }
                    indirect_code = data.readInt32();
                    BinderLibTestBundle data2(&data);
                    if (!data2.isValid()) {
                        return BAD_VALUE;
                    }
                    BinderLibTestBundle reply2;
                    binder->transact(indirect_code, data2, &reply2);
                    reply2.appendTo(reply);
                }
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_SET_ERROR_TRANSACTION:
                reply->setError(data.readInt32());
                return NO_ERROR;
            case BINDER_LIB_TEST_GET_PTR_SIZE_TRANSACTION:
                reply->writeInt32(sizeof(void *));
                return NO_ERROR;
            case BINDER_LIB_TEST_GET_STATUS_TRANSACTION:
                return NO_ERROR;
            case BINDER_LIB_TEST_ADD_STRONG_REF_TRANSACTION:
                m_strongRef = data.readStrongBinder();
                return NO_ERROR;
            case BINDER_LIB_TEST_LINK_DEATH_TRANSACTION: {
                int ret;
                Parcel data2, reply2;
                sp<TestDeathRecipient> testDeathRecipient = new TestDeathRecipient();
                sp<IBinder> target;
                sp<IBinder> callback;

                target = data.readStrongBinder();
                if (target == nullptr) {
                    return BAD_VALUE;
                }
                callback = data.readStrongBinder();
                if (callback == nullptr) {
                    return BAD_VALUE;
                }
                ret = target->linkToDeath(testDeathRecipient);
                if (ret == NO_ERROR) ret = testDeathRecipient->waitEvent(5);
                data2.writeInt32(ret);
                callback->transact(BINDER_LIB_TEST_CALL_BACK, data2, &reply2);
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_WRITE_FILE_TRANSACTION: {
                int ret;
                int32_t size;
                const void *buf;
                int fd;

                fd = data.readFileDescriptor();
                if (fd < 0) {
                    return BAD_VALUE;
                }
                ret = data.readInt32(&size);
                if (ret != NO_ERROR) {
                    return ret;
                }
                buf = data.readInplace(size);
                if (buf == nullptr) {
                    return BAD_VALUE;
                }
                ret = write(fd, buf, size);
                if (ret != size) return UNKNOWN_ERROR;
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_WRITE_PARCEL_FILE_DESCRIPTOR_TRANSACTION: {
                int ret;
                int32_t size;
                const void *buf;
                unique_fd fd;

                ret = data.readUniqueParcelFileDescriptor(&fd);
                if (ret != NO_ERROR) {
                    return ret;
                }
                ret = data.readInt32(&size);
                if (ret != NO_ERROR) {
                    return ret;
                }
                buf = data.readInplace(size);
                if (buf == nullptr) {
                    return BAD_VALUE;
                }
                ret = write(fd.get(), buf, size);
                if (ret != size) return UNKNOWN_ERROR;
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_GET_FILE_DESCRIPTORS_OWNED_TRANSACTION: {
                unique_fd fd1(memfd_create("memfd1", MFD_CLOEXEC));
                if (!fd1.ok()) {
                    PLOGE("memfd_create failed");
                    return UNKNOWN_ERROR;
                }
                unique_fd fd2(memfd_create("memfd2", MFD_CLOEXEC));
                if (!fd2.ok()) {
                    PLOGE("memfd_create failed");
                    return UNKNOWN_ERROR;
                }
                status_t ret;
                ret = reply->writeFileDescriptor(fd1.release(), true);
                if (ret != NO_ERROR) {
                    return ret;
                }
                ret = reply->writeFileDescriptor(fd2.release(), true);
                if (ret != NO_ERROR) {
                    return ret;
                }
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_GET_FILE_DESCRIPTORS_UNOWNED_TRANSACTION: {
                status_t ret;
                ret = reply->writeFileDescriptor(STDOUT_FILENO, false);
                if (ret != NO_ERROR) {
                    return ret;
                }
                ret = reply->writeFileDescriptor(STDERR_FILENO, false);
                if (ret != NO_ERROR) {
                    return ret;
                }
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_DELAYED_EXIT_TRANSACTION:
                alarm(10);
                return NO_ERROR;
            case BINDER_LIB_TEST_EXIT_TRANSACTION:
                while (wait(nullptr) != -1 || errno != ECHILD)
                    ;
                exit(EXIT_SUCCESS);
            case BINDER_LIB_TEST_CREATE_BINDER_TRANSACTION: {
                sp<IBinder> binder = new BBinder();
                reply->writeStrongBinder(binder);
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_GET_WORK_SOURCE_TRANSACTION: {
                data.enforceInterface(binderLibTestServiceName);
                reply->writeInt32(IPCThreadState::self()->getCallingWorkSourceUid());
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_GET_SCHEDULING_POLICY: {
                int policy = 0;
                sched_param param;
                if (0 != pthread_getschedparam(pthread_self(), &policy, &param)) {
                    return UNKNOWN_ERROR;
                }
                reply->writeInt32(policy);
                reply->writeInt32(param.sched_priority);
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_LISTEN_FOR_FROZEN_STATE_CHANGE: {
                sp<IBinder> binder = data.readStrongBinder();
                frozenStateChangeCallback = sp<TestFrozenStateChangeCallback>::make();
                // Hold an strong pointer to the binder object so it doesn't go
                // away.
                frozenStateChangeCallback->binder = binder;
                int ret = binder->addFrozenStateChangeCallback(frozenStateChangeCallback);
                if (ret != NO_ERROR) {
                    return ret;
                }
                auto event = frozenStateChangeCallback->events.popWithTimeout(1000ms);
                if (!event.has_value()) {
                    return NOT_ENOUGH_DATA;
                }
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_CONSUME_STATE_CHANGE_EVENTS: {
                reply->writeBoolVector(frozenStateChangeCallback->getAllAndClear());
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_ECHO_VECTOR: {
                std::vector<uint64_t> vector;
                auto err = data.readUint64Vector(&vector);
                if (err != NO_ERROR) return err;
                reply->writeUint64Vector(vector);
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_GET_NON_BLOCKING_FD: {
                std::array<int, 2> sockets;
                const bool created = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets.data()) == 0;
                if (!created) {
                    ALOGE("Could not create socket pair");
                    return UNKNOWN_ERROR;
                }

                const int result = fcntl(sockets[0], F_SETFL, O_NONBLOCK);
                if (result != 0) {
                    ALOGE("Could not make socket non-blocking: %s", strerror(errno));
                    return UNKNOWN_ERROR;
                }
                unique_fd out(sockets[0]);
                status_t writeResult = reply->writeUniqueFileDescriptor(out);
                if (writeResult != NO_ERROR) {
                    ALOGE("Could not write unique_fd");
                    return writeResult;
                }
                close(sockets[1]); // we don't need the other side of the fd
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_REJECT_OBJECTS: {
                return data.objectsCount() == 0 ? BAD_VALUE : NO_ERROR;
            }
            case BINDER_LIB_TEST_CAN_GET_SID: {
                return IPCThreadState::self()->getCallingSid() == nullptr ? BAD_VALUE : NO_ERROR;
            }
            case BINDER_LIB_TEST_GET_MAX_THREAD_COUNT: {
                reply->writeInt32(ProcessState::self()->getThreadPoolMaxTotalThreadCount());
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_IS_THREADPOOL_STARTED: {
                reply->writeBool(ProcessState::self()->isThreadPoolStarted());
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_PROCESS_LOCK: {
                m_blockMutex.lock();
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_LOCK_UNLOCK: {
                std::lock_guard<std::mutex> _l(m_blockMutex);
                return NO_ERROR;
            }
            case BINDER_LIB_TEST_UNLOCK_AFTER_MS: {
                int32_t ms = data.readInt32();
                return unlockInMs(ms);
            }
            case BINDER_LIB_TEST_PROCESS_TEMPORARY_LOCK: {
                m_blockMutex.lock();
                sp<BinderLibTestService> thisService = this;
                int32_t value = data.readInt32();
                // start local thread to unlock in 1s
                std::thread t([=] { thisService->unlockInMs(value); });
                t.detach();
                return NO_ERROR;
            }
            default:
                return UNKNOWN_TRANSACTION;
        };
    }

    status_t unlockInMs(int32_t ms) {
        usleep(ms * 1000);
        m_blockMutex.unlock();
        return NO_ERROR;
    }

private:
    int32_t m_id;
    int32_t m_nextServerId;
    pthread_mutex_t m_serverWaitMutex;
    pthread_cond_t m_serverWaitCond;
    bool m_serverStartRequested;
    sp<IBinder> m_serverStarted;
    sp<IBinder> m_strongRef;
    sp<IBinder> m_callback;
    bool m_exitOnDestroy;
    std::mutex m_blockMutex;
    sp<TestFrozenStateChangeCallback> frozenStateChangeCallback;
};

int run_server(int index, int readypipefd, bool usePoll)
{
    binderLibTestServiceName += String16(binderserversuffix);

    // Testing to make sure that calls that we are serving can use getCallin*
    // even though we don't here.
    IPCThreadState::SpGuard spGuard{
            .address = __builtin_frame_address(0),
            .context = "main server thread",
    };
    (void)IPCThreadState::self()->pushGetCallingSpGuard(&spGuard);

    status_t ret;
    sp<IServiceManager> sm = defaultServiceManager();
    sm->enableAddServiceCache(false);

    BinderLibTestService* testServicePtr;
    {
        sp<BinderLibTestService> testService = new BinderLibTestService(index);

        testService->setMinSchedulerPolicy(kSchedPolicy, kSchedPriority);

        testService->setInheritRt(true);

        /*
         * Normally would also contain functionality as well, but we are only
         * testing the extension mechanism.
         */
        testService->setExtension(new BBinder());

        // Required for test "BufRejected'
        testService->setRequestingSid(true);

        /*
         * We need this below, but can't hold a sp<> because it prevents the
         * node from being cleaned up automatically. It's safe in this case
         * because of how the tests are written.
         */
        testServicePtr = testService.get();

        if (index == 0) {
            ret = sm->addService(binderLibTestServiceName, testService);
        } else {
            LIBBINDER_IGNORE("-Wdeprecated-declarations")
            sp<IBinder> server = sm->getService(binderLibTestServiceName);
            LIBBINDER_IGNORE_END()
            Parcel data, reply;
            data.writeInt32(index);
            data.writeStrongBinder(testService);

            ret = server->transact(BINDER_LIB_TEST_REGISTER_SERVER, data, &reply);
        }
    }
    write(readypipefd, &ret, sizeof(ret));
    close(readypipefd);
    //printf("%s: ret %d\n", __func__, ret);
    if (ret)
        return 1;
    //printf("%s: joinThreadPool\n", __func__);
    if (usePoll) {
        int fd;
        struct epoll_event ev;
        int epoll_fd;
        IPCThreadState::self()->setupPolling(&fd);
        if (fd < 0) {
            return 1;
        }
        IPCThreadState::self()->flushCommands(); // flush BC_ENTER_LOOPER

        epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd == -1) {
            return 1;
        }

        ev.events = EPOLLIN;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            return 1;
        }

        while (1) {
             /*
              * We simulate a single-threaded process using the binder poll
              * interface; besides handling binder commands, it can also
              * issue outgoing transactions, by storing a callback in
              * m_callback.
              *
              * processPendingCall() will then issue that transaction.
              */
             struct epoll_event events[1];
             int numEvents = epoll_wait(epoll_fd, events, 1, 1000);
             if (numEvents < 0) {
                 if (errno == EINTR) {
                     continue;
                 }
                 return 1;
             }
             if (numEvents > 0) {
                 IPCThreadState::self()->handlePolledCommands();
                 IPCThreadState::self()->flushCommands(); // flush BC_FREE_BUFFER
                 testServicePtr->processPendingCall();
             }
        }
    } else {
        ProcessState::self()->setThreadPoolMaxThreadCount(kKernelThreads);
        ProcessState::self()->startThreadPool();
        IPCThreadState::self()->joinThreadPool();
    }
    //printf("%s: joinThreadPool returned\n", __func__);
    return 1; /* joinThreadPool should not return */
}

int main(int argc, char** argv) {
    if (argc == 4 && !strcmp(argv[1], "--servername")) {
        binderservername = argv[2];
    } else {
        binderservername = argv[0];
    }

    if (argc == 6 && !strcmp(argv[1], binderserverarg)) {
        binderserversuffix = argv[5];
        return run_server(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]) == 1);
    }
    binderserversuffix = new char[16];
    snprintf(binderserversuffix, 16, "%d", getpid());
    binderLibTestServiceName += String16(binderserversuffix);

    ::testing::InitGoogleTest(&argc, argv);
    binder_env = AddGlobalTestEnvironment(new BinderLibTestEnv());
    ProcessState::self()->startThreadPool();
    return RUN_ALL_TESTS();
}
