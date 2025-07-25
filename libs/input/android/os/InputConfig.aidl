/**
 * Copyright (c) 2022, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.os;


/**
 * Input configurations flags used to determine the behavior of input windows.
 * @hide
 */
@Backing(type="int")
enum InputConfig {

    /**
     * The default InputConfig value with no flags set.
     */
    DEFAULT                      = 0,

    /**
     * Does not construct an input channel for this window.  The channel will therefore
     * be incapable of receiving input.
     */
    NO_INPUT_CHANNEL             = 1 << 0,

    /**
     * Indicates that this input window is not visible, and thus will not be considered as
     * an input target and will not obscure other windows.
     */
    NOT_VISIBLE                  = 1 << 1,

    /**
     * Indicates that this input window cannot be a focus target, and this will not
     * receive any input events that can only be directed for the focused window, such
     * as key events.
     */
    NOT_FOCUSABLE                = 1 << 2,

    /**
     * Indicates that this input window cannot receive any events directed at a
     * specific location on the screen, such as touchscreen, mouse, and stylus events.
     * The window will not be considered as a touch target, but can still obscure other
     * windows.
     */
    NOT_TOUCHABLE                = 1 << 3,

    /**
     * This flag is now deprecated and should not be used.
     */
    DEPRECATED_PREVENT_SPLITTING = 1 << 4,

    /**
     * Indicates that this window shows the wallpaper behind it, so all touch events
     * that it receives should also be sent to the wallpaper.
     */
    DUPLICATE_TOUCH_TO_WALLPAPER = 1 << 5,

    /** Indicates that this the wallpaper's input window. */
    IS_WALLPAPER                 = 1 << 6,

    /**
     * Indicates that input events should not be dispatched to this window. When set,
     * input events directed towards this window will simply be dropped, and will not
     * be dispatched to windows behind it.
     */
    PAUSE_DISPATCHING            = 1 << 7,

    /**
     * This flag is set when the window is of a trusted type that is allowed to silently
     * overlay other windows for the purpose of implementing the secure views feature.
     * Trusted overlays, such as IME windows, can partly obscure other windows without causing
     * motion events to be delivered to them with AMOTION_EVENT_FLAG_WINDOW_IS_OBSCURED.
     */
    TRUSTED_OVERLAY              = 1 << 8,

    /**
     * Indicates that this window wants to listen for when there is a touch DOWN event
     * that occurs outside its touchable bounds. When such an event occurs, this window
     * will receive a MotionEvent with ACTION_OUTSIDE.
     */
    WATCH_OUTSIDE_TOUCH          = 1 << 9,

    /**
     * When set, this flag allows touches to leave the current window whenever the finger
     * moves above another window. When this happens, the window that touch has just left
     * (the current window) will receive ACTION_CANCEL, and the window that touch has entered
     * will receive ACTION_DOWN, and the remainder of the touch gesture will only go to the
     * new window. Without this flag, the entire gesture is sent to the current window, even
     * if the touch leaves the window's bounds.
     */
    SLIPPERY                     = 1 << 10,

    /**
     * When this window has focus, does not call user activity for all input events so
     * the application will have to do it itself.
     */
    DISABLE_USER_ACTIVITY        = 1 << 11,

    /**
     * Internal flag used to indicate that input should be dropped on this window.
     */
    DROP_INPUT                   = 1 << 12,

    /**
     * Internal flag used to indicate that input should be dropped on this window if this window
     * is obscured.
     */
    DROP_INPUT_IF_OBSCURED       = 1 << 13,

    /**
     * An input spy window. This window will receive all pointer events within its touchable
     * area, but will not stop events from being sent to other windows below it in z-order.
     * An input event will be dispatched to all spy windows above the top non-spy window at the
     * event's coordinates.
     */
    SPY                          = 1 << 14,

    /**
     * When used with {@link #NOT_TOUCHABLE}, this window will continue to receive events from
     * a stylus device within its touchable region. All other pointer events, such as from a
     * mouse or touchscreen, will be dispatched to the windows behind it.
     *
     * This configuration has no effect when the config {@link #NOT_TOUCHABLE} is not set.
     *
     * It is not valid to set this configuration if {@link #TRUSTED_OVERLAY} is not set.
     */
    INTERCEPTS_STYLUS            = 1 << 15,

    /**
     * The window is a clone of another window. This may be treated differently since there's
     * likely a duplicate window with the same client token, but different bounds.
     */
    CLONE                        = 1 << 16,

    /**
     * If the stylus is currently down *anywhere* on the screen, new touches will not be delivered
     * to the window with this flag. This helps prevent unexpected clicks on some system windows,
     * like StatusBar and TaskBar.
     */
    GLOBAL_STYLUS_BLOCKS_TOUCH   = 1 << 17,

    /**
     * InputConfig used to indicate that this window is privacy sensitive. This may be used to
     * redact input interactions from tracing or screen mirroring.
     *
     * This must be set on windows that use {@link WindowManager.LayoutParams#FLAG_SECURE},
     * but it may also be set without setting FLAG_SECURE. The tracing configuration will
     * determine how these sensitive events are eventually traced.
     */
     SENSITIVE_FOR_PRIVACY       = 1 << 18,
}
