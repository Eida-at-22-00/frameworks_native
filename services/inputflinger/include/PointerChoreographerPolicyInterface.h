/*
 * Copyright 2023 The Android Open Source Project
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

#pragma once

#include "PointerControllerInterface.h"

namespace android {

/**
 * The PointerChoreographer policy interface.
 *
 * This is the interface that PointerChoreographer uses to talk to Window Manager and other
 * system components.
 *
 * NOTE: In general, the PointerChoreographer must not interact with the policy while
 * holding any locks.
 */
class PointerChoreographerPolicyInterface {
public:
    virtual ~PointerChoreographerPolicyInterface() = default;

    /**
     * A factory method for PointerController. The PointerController implementation has
     * dependencies on a graphical library - libgui, used to draw icons on the screen - which
     * isn't available for the host. Since we want libinputflinger and its test to be buildable
     * for and runnable on the host, the PointerController implementation must be in a separate
     * library, libinputservice, that has the additional dependencies. The PointerController
     * will be mocked when testing PointerChoreographer.
     *
     * Since this is a factory method used to work around dependencies, it will not interact with
     * other input components and may be called with the PointerChoreographer lock held.
     */
    virtual std::shared_ptr<PointerControllerInterface> createPointerController(
            PointerControllerInterface::ControllerType type) = 0;

    /**
     * Notifies the policy that the default pointer displayId has changed. PointerChoreographer is
     * the single source of truth for all pointers on screen.
     * @param displayId The updated display on which the mouse cursor is shown
     * @param position The new position of the mouse cursor on the logical display
     */
    virtual void notifyPointerDisplayIdChanged(ui::LogicalDisplayId displayId,
                                               const vec2& position) = 0;

    /* Returns true if any InputConnection is currently active. */
    virtual bool isInputMethodConnectionActive() = 0;

    /* Notifies that mouse cursor faded due to typing. */
    virtual void notifyMouseCursorFadedOnTyping() = 0;

    /**
     * Give accessibility a chance to filter motion event by pointer devices.
     * The return values denotes the delta x and y after filtering it.
     *
     * This call happens on the input hot path and it is extremely performance sensitive.
     * This also must not call back into native code.
     */
    virtual std::optional<vec2> filterPointerMotionForAccessibility(
            const vec2& current, const vec2& delta, const ui::LogicalDisplayId& displayId) = 0;
};

} // namespace android
