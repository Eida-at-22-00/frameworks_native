/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ANDROID_IAUDIOMANAGER_H
#define ANDROID_IAUDIOMANAGER_H

#include <android/media/IAudioManagerNative.h>
#include <audiomanager/AudioManager.h>
#include <utils/Errors.h>
#include <binder/IInterface.h>
#include <binder/PersistableBundle.h>
#include <hardware/power.h>
#include <system/audio.h>

namespace android {

// ----------------------------------------------------------------------------
// TODO(b/309532236) replace this class with AIDL generated parcelable
class IAudioManager : public IInterface
{
public:
    // These transaction IDs must be kept in sync with the method order from
    // IAudioService.aidl.
    enum {
        GET_NATIVE_INTERFACE                  = IBinder::FIRST_CALL_TRANSACTION,
        TRACK_PLAYER                          = IBinder::FIRST_CALL_TRANSACTION + 1,
        PLAYER_ATTRIBUTES                     = IBinder::FIRST_CALL_TRANSACTION + 2,
        PLAYER_EVENT                          = IBinder::FIRST_CALL_TRANSACTION + 3,
        RELEASE_PLAYER                        = IBinder::FIRST_CALL_TRANSACTION + 4,
        TRACK_RECORDER                        = IBinder::FIRST_CALL_TRANSACTION + 5,
        RECORDER_EVENT                        = IBinder::FIRST_CALL_TRANSACTION + 6,
        RELEASE_RECORDER                      = IBinder::FIRST_CALL_TRANSACTION + 7,
        PLAYER_SESSION_ID                     = IBinder::FIRST_CALL_TRANSACTION + 8,
        PORT_EVENT                            = IBinder::FIRST_CALL_TRANSACTION + 9,
        PERMISSION_UPDATE_BARRIER             = IBinder::FIRST_CALL_TRANSACTION + 10,
    };

    DECLARE_META_INTERFACE(AudioManager)

    virtual sp<media::IAudioManagerNative> getNativeInterface() = 0;

    // The parcels created by these methods must be kept in sync with the
    // corresponding methods from IAudioService.aidl and objects it imports.
    virtual audio_unique_id_t trackPlayer(player_type_t playerType, audio_usage_t usage,
                audio_content_type_t content, const sp<IBinder>& player,
                audio_session_t sessionId) = 0;
    /*oneway*/ virtual status_t playerAttributes(audio_unique_id_t piid, audio_usage_t usage,
                audio_content_type_t content)= 0;
    /*oneway*/ virtual status_t playerEvent(audio_unique_id_t piid, player_state_t event,
                const std::vector<audio_port_handle_t>& eventIds) = 0;
    /*oneway*/ virtual status_t releasePlayer(audio_unique_id_t piid) = 0;
    virtual audio_unique_id_t trackRecorder(const sp<IBinder>& recorder) = 0;
    /*oneway*/ virtual status_t recorderEvent(audio_unique_id_t riid, recorder_state_t event) = 0;
    /*oneway*/ virtual status_t releaseRecorder(audio_unique_id_t riid) = 0;
    /*oneway*/ virtual status_t playerSessionId(audio_unique_id_t piid, audio_session_t sessionId) = 0;
    /*oneway*/ virtual status_t portEvent(audio_port_handle_t portId, player_state_t event,
                const std::unique_ptr<os::PersistableBundle>& extras) = 0;
    virtual status_t permissionUpdateBarrier() = 0;
};

// ----------------------------------------------------------------------------

}; // namespace android

#endif // ANDROID_IAUDIOMANAGER_H
