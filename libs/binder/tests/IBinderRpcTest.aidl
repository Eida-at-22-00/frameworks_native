/*
 * Copyright (C) 2020 The Android Open Source Project
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

interface IBinderRpcTest {
    oneway void sendString(@utf8InCpp String str);
    @utf8InCpp String doubleString(@utf8InCpp String str);

    // get the port that a client used to connect to this object
    int getClientPort();

    // number of known RPC binders to process, RpcState::countBinders by session
    int[] countBinders();

    // Return a null binder with a non-nullable return type.
    IBinder getNullBinder();

    // Caller sends server, callee pings caller's server and returns error code.
    int pingMe(IBinder binder);
    @nullable IBinder repeatBinder(@nullable IBinder binder);

    void holdBinder(@nullable IBinder binder);
    @nullable IBinder getHeldBinder();

    byte[] repeatBytes(in byte[] bytes);

    // Idea is client creates its own instance of IBinderRpcTest and calls this,
    // and the server calls 'binder' with (calls - 1) passing itself as 'binder',
    // going back and forth until calls = 0
    void nestMe(IBinderRpcTest binder, int calls);

    // should always return the same binder
    IBinder alwaysGiveMeTheSameBinder();

    // Idea is that the server will not hold onto the session, the remote session
    // object must. This is to test lifetimes of binder objects, and consequently, also
    // identity (since by assigning sessions names, we can make sure a section always
    // references the session it was originally opened with).
    IBinderRpcSession openSession(@utf8InCpp String name);

    // Decremented in ~IBinderRpcSession
    int getNumOpenSessions();

    // primitives to test threading behavior
    void lock();
    oneway void unlockInMsAsync(int ms);
    void lockUnlock(); // locks and unlocks a mutex

    // take up binder thread for some time
    void sleepMs(int ms);
    oneway void sleepMsAsync(int ms);

    void doCallback(IBinderRpcCallback callback, boolean isOneway, boolean delayed, @utf8InCpp String value);
    oneway void doCallbackAsync(IBinderRpcCallback callback, boolean isOneway, boolean delayed, @utf8InCpp String value);

    void die(boolean cleanup);
    void scheduleShutdown();

    void useKernelBinderCallingId();

    ParcelFileDescriptor echoAsFile(@utf8InCpp String content);

    ParcelFileDescriptor concatFiles(in List<ParcelFileDescriptor> files);

    // FDs sent via `blockingSendFdOneway` can be received via
    // `blockingRecvFd`. The handler for `blockingSendFdOneway` will block
    // until the next `blockingRecvFd` call.
    //
    // This is useful for carefully controlling how/when oneway transactions
    // get queued.
    oneway void blockingSendFdOneway(in ParcelFileDescriptor fd);
    ParcelFileDescriptor blockingRecvFd();

    // Same as blockingSendFdOneway, but with integers.
    oneway void blockingSendIntOneway(int n);
    int blockingRecvInt();
}
