/*
 * Copyright (C) 2010 The Android Open Source Project
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

/**
 * @addtogroup Input
 * @{
 */

/**
 * @file input.h
 */

#ifndef _ANDROID_INPUT_H
#define _ANDROID_INPUT_H

#include <sys/cdefs.h>

/******************************************************************
 *
 * IMPORTANT NOTICE:
 *
 *   This file is part of Android's set of stable system headers
 *   exposed by the Android NDK (Native Development Kit).
 *
 *   Third-party source AND binary code relies on the definitions
 *   here to be FROZEN ON ALL UPCOMING PLATFORM RELEASES.
 *
 *   - DO NOT MODIFY ENUMS (EXCEPT IF YOU ADD NEW 32-BIT VALUES)
 *   - DO NOT MODIFY CONSTANTS OR FUNCTIONAL MACROS
 *   - DO NOT CHANGE THE SIGNATURE OF FUNCTIONS IN ANY WAY
 *   - DO NOT CHANGE THE LAYOUT OR SIZE OF STRUCTURES
 */

/*
 * Structures and functions to receive and process input events in
 * native code.
 *
 * NOTE: These functions MUST be implemented by /system/lib/libui.so
 */

#include <stdint.h>
#include <sys/types.h>
#include <android/keycodes.h>
#include <android/looper.h>

#include <jni.h>

// This file may also be built on glibc or on Windows/MacOS libc's, so no-op
// definitions are provided.
#if !defined(__INTRODUCED_IN)
#define __INTRODUCED_IN(__api_level) /* nothing */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Key states (may be returned by queries about the current state of a
 * particular key code, scan code or switch).
 */
enum {
    /** The key state is unknown or the requested key itself is not supported. */
    AKEY_STATE_UNKNOWN = -1,

    /** The key is up. */
    AKEY_STATE_UP = 0,

    /** The key is down. */
    AKEY_STATE_DOWN = 1,

    /** The key is down but is a virtual key press that is being emulated by the system. */
    AKEY_STATE_VIRTUAL = 2
};

/**
 * Meta key / modifier state.
 */
enum {
    /** No meta keys are pressed. */
    AMETA_NONE = 0,

    /** This mask is used to check whether one of the ALT meta keys is pressed. */
    AMETA_ALT_ON = 0x02,

    /** This mask is used to check whether the left ALT meta key is pressed. */
    AMETA_ALT_LEFT_ON = 0x10,

    /** This mask is used to check whether the right ALT meta key is pressed. */
    AMETA_ALT_RIGHT_ON = 0x20,

    /** This mask is used to check whether one of the SHIFT meta keys is pressed. */
    AMETA_SHIFT_ON = 0x01,

    /** This mask is used to check whether the left SHIFT meta key is pressed. */
    AMETA_SHIFT_LEFT_ON = 0x40,

    /** This mask is used to check whether the right SHIFT meta key is pressed. */
    AMETA_SHIFT_RIGHT_ON = 0x80,

    /** This mask is used to check whether the SYM meta key is pressed. */
    AMETA_SYM_ON = 0x04,

    /** This mask is used to check whether the FUNCTION meta key is pressed. */
    AMETA_FUNCTION_ON = 0x08,

    /** This mask is used to check whether one of the CTRL meta keys is pressed. */
    AMETA_CTRL_ON = 0x1000,

    /** This mask is used to check whether the left CTRL meta key is pressed. */
    AMETA_CTRL_LEFT_ON = 0x2000,

    /** This mask is used to check whether the right CTRL meta key is pressed. */
    AMETA_CTRL_RIGHT_ON = 0x4000,

    /** This mask is used to check whether one of the META meta keys is pressed. */
    AMETA_META_ON = 0x10000,

    /** This mask is used to check whether the left META meta key is pressed. */
    AMETA_META_LEFT_ON = 0x20000,

    /** This mask is used to check whether the right META meta key is pressed. */
    AMETA_META_RIGHT_ON = 0x40000,

    /** This mask is used to check whether the CAPS LOCK meta key is on. */
    AMETA_CAPS_LOCK_ON = 0x100000,

    /** This mask is used to check whether the NUM LOCK meta key is on. */
    AMETA_NUM_LOCK_ON = 0x200000,

    /** This mask is used to check whether the SCROLL LOCK meta key is on. */
    AMETA_SCROLL_LOCK_ON = 0x400000,
};

struct AInputEvent;
/**
 * Input events.
 *
 * Input events are opaque structures.  Use the provided accessors functions to
 * read their properties.
 */
typedef struct AInputEvent AInputEvent;

/**
 * Input event types.
 */
enum {
    /** Indicates that the input event is a key event. */
    AINPUT_EVENT_TYPE_KEY = 1,

    /** Indicates that the input event is a motion event. */
    AINPUT_EVENT_TYPE_MOTION = 2,

    /** Focus event */
    AINPUT_EVENT_TYPE_FOCUS = 3,

    /** Capture event */
    AINPUT_EVENT_TYPE_CAPTURE = 4,

    /** Drag event */
    AINPUT_EVENT_TYPE_DRAG = 5,

    /** TouchMode event */
    AINPUT_EVENT_TYPE_TOUCH_MODE = 6,
};

/**
 * Key event actions.
 */
enum {
    /** The key has been pressed down. */
    AKEY_EVENT_ACTION_DOWN = 0,

    /** The key has been released. */
    AKEY_EVENT_ACTION_UP = 1,

    /**
     * Multiple duplicate key events have occurred in a row, or a
     * complex string is being delivered.  The repeat_count property
     * of the key event contains the number of times the given key
     * code should be executed.
     */
    AKEY_EVENT_ACTION_MULTIPLE = 2
};

/**
 * Key event flags.
 */
enum {
    /** This mask is set if the device woke because of this key event. */
    AKEY_EVENT_FLAG_WOKE_HERE = 0x1,

    /** This mask is set if the key event was generated by a software keyboard. */
    AKEY_EVENT_FLAG_SOFT_KEYBOARD = 0x2,

    /** This mask is set if we don't want the key event to cause us to leave touch mode. */
    AKEY_EVENT_FLAG_KEEP_TOUCH_MODE = 0x4,

    /**
     * This mask is set if an event was known to come from a trusted
     * part of the system.  That is, the event is known to come from
     * the user, and could not have been spoofed by a third party
     * component.
     */
    AKEY_EVENT_FLAG_FROM_SYSTEM = 0x8,

    /**
     * This mask is used for compatibility, to identify enter keys that are
     * coming from an IME whose enter key has been auto-labelled "next" or
     * "done".  This allows TextView to dispatch these as normal enter keys
     * for old applications, but still do the appropriate action when
     * receiving them.
     */
    AKEY_EVENT_FLAG_EDITOR_ACTION = 0x10,

    /**
     * When associated with up key events, this indicates that the key press
     * has been canceled.  Typically this is used with virtual touch screen
     * keys, where the user can slide from the virtual key area on to the
     * display: in that case, the application will receive a canceled up
     * event and should not perform the action normally associated with the
     * key.  Note that for this to work, the application can not perform an
     * action for a key until it receives an up or the long press timeout has
     * expired.
     */
    AKEY_EVENT_FLAG_CANCELED = 0x20,

    /**
     * This key event was generated by a virtual (on-screen) hard key area.
     * Typically this is an area of the touchscreen, outside of the regular
     * display, dedicated to "hardware" buttons.
     */
    AKEY_EVENT_FLAG_VIRTUAL_HARD_KEY = 0x40,

    /**
     * This flag is set for the first key repeat that occurs after the
     * long press timeout.
     */
    AKEY_EVENT_FLAG_LONG_PRESS = 0x80,

    /**
     * Set when a key event has #AKEY_EVENT_FLAG_CANCELED set because a long
     * press action was executed while it was down.
     */
    AKEY_EVENT_FLAG_CANCELED_LONG_PRESS = 0x100,

    /**
     * Set for #AKEY_EVENT_ACTION_UP when this event's key code is still being
     * tracked from its initial down.  That is, somebody requested that tracking
     * started on the key down and a long press has not caused
     * the tracking to be canceled.
     */
    AKEY_EVENT_FLAG_TRACKING = 0x200,

    /**
     * Set when a key event has been synthesized to implement default behavior
     * for an event that the application did not handle.
     * Fallback key events are generated by unhandled trackball motions
     * (to emulate a directional keypad) and by certain unhandled key presses
     * that are declared in the key map (such as special function numeric keypad
     * keys when numlock is off).
     */
    AKEY_EVENT_FLAG_FALLBACK = 0x400,
};

/**
 * Bit shift for the action bits holding the pointer index as
 * defined by #AMOTION_EVENT_ACTION_POINTER_INDEX_MASK.
 */
#define AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT 8

/** Motion event actions */
enum {
    /** Bit mask of the parts of the action code that are the action itself. */
    AMOTION_EVENT_ACTION_MASK = 0xff,

    /**
     * Bits in the action code that represent a pointer index, used with
     * #AMOTION_EVENT_ACTION_POINTER_DOWN and AMOTION_EVENT_ACTION_POINTER_UP.  Shifting
     * down by #AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT provides the actual pointer
     * index where the data for the pointer going up or down can be found.
     */
    AMOTION_EVENT_ACTION_POINTER_INDEX_MASK  = 0xff00,

    /** A pressed gesture has started, the motion contains the initial starting location. */
    AMOTION_EVENT_ACTION_DOWN = 0,

    /**
     * A pressed gesture has finished, the motion contains the final release location
     * as well as any intermediate points since the last down or move event.
     */
    AMOTION_EVENT_ACTION_UP = 1,

    /**
     * A change has happened during a press gesture (between #AMOTION_EVENT_ACTION_DOWN and
     * #AMOTION_EVENT_ACTION_UP).  The motion contains the most recent point, as well as
     * any intermediate points since the last down or move event.
     */
    AMOTION_EVENT_ACTION_MOVE = 2,

    /**
     * The current gesture has been aborted.
     * You will not receive any more points in it.  You should treat this as
     * an up event, but not perform any action that you normally would.
     */
    AMOTION_EVENT_ACTION_CANCEL = 3,

    /**
     * A movement has happened outside of the normal bounds of the UI element.
     * This does not provide a full gesture, but only the initial location of the movement/touch.
     */
    AMOTION_EVENT_ACTION_OUTSIDE = 4,

    /**
     * A non-primary pointer has gone down.
     * The bits in #AMOTION_EVENT_ACTION_POINTER_INDEX_MASK indicate which pointer changed.
     */
    AMOTION_EVENT_ACTION_POINTER_DOWN = 5,

    /**
     * A non-primary pointer has gone up.
     * The bits in #AMOTION_EVENT_ACTION_POINTER_INDEX_MASK indicate which pointer changed.
     */
    AMOTION_EVENT_ACTION_POINTER_UP = 6,

    /**
     * A change happened but the pointer is not down (unlike #AMOTION_EVENT_ACTION_MOVE).
     * The motion contains the most recent point, as well as any intermediate points since
     * the last hover move event.
     */
    AMOTION_EVENT_ACTION_HOVER_MOVE = 7,

    /**
     * The motion event contains relative vertical and/or horizontal scroll offsets.
     * Use {@link AMotionEvent_getAxisValue} to retrieve the information from
     * #AMOTION_EVENT_AXIS_VSCROLL and #AMOTION_EVENT_AXIS_HSCROLL.
     * The pointer may or may not be down when this event is dispatched.
     * This action is always delivered to the winder under the pointer, which
     * may not be the window currently touched.
     */
    AMOTION_EVENT_ACTION_SCROLL = 8,

    /** The pointer is not down but has entered the boundaries of a window or view. */
    AMOTION_EVENT_ACTION_HOVER_ENTER = 9,

    /** The pointer is not down but has exited the boundaries of a window or view. */
    AMOTION_EVENT_ACTION_HOVER_EXIT = 10,

    /* One or more buttons have been pressed. */
    AMOTION_EVENT_ACTION_BUTTON_PRESS = 11,

    /* One or more buttons have been released. */
    AMOTION_EVENT_ACTION_BUTTON_RELEASE = 12,
};

/**
 * Motion event flags.
 */
enum {
    /**
     * This flag indicates that the window that received this motion event is partly
     * or wholly obscured by another visible window above it.  This flag is set to true
     * even if the event did not directly pass through the obscured area.
     * A security sensitive application can check this flag to identify situations in which
     * a malicious application may have covered up part of its content for the purpose
     * of misleading the user or hijacking touches.  An appropriate response might be
     * to drop the suspect touches or to take additional precautions to confirm the user's
     * actual intent.
     */
    AMOTION_EVENT_FLAG_WINDOW_IS_OBSCURED = 0x1,
};

/**
 * Motion event edge touch flags.
 */
enum {
    /** No edges intersected. */
    AMOTION_EVENT_EDGE_FLAG_NONE = 0,

    /** Flag indicating the motion event intersected the top edge of the screen. */
    AMOTION_EVENT_EDGE_FLAG_TOP = 0x01,

    /** Flag indicating the motion event intersected the bottom edge of the screen. */
    AMOTION_EVENT_EDGE_FLAG_BOTTOM = 0x02,

    /** Flag indicating the motion event intersected the left edge of the screen. */
    AMOTION_EVENT_EDGE_FLAG_LEFT = 0x04,

    /** Flag indicating the motion event intersected the right edge of the screen. */
    AMOTION_EVENT_EDGE_FLAG_RIGHT = 0x08
};

/**
 * Constants that identify each individual axis of a motion event.
 * @anchor AMOTION_EVENT_AXIS
 */
enum {
    /**
     * Axis constant: X axis of a motion event.
     *
     * - For a touch screen, reports the absolute X screen position of the center of
     * the touch contact area.  The units are display pixels.
     * - For a touch pad, reports the absolute X surface position of the center of the touch
     * contact area. The units are device-dependent.
     * - For a mouse, reports the absolute X screen position of the mouse pointer.
     * The units are display pixels.
     * - For a trackball, reports the relative horizontal displacement of the trackball.
     * The value is normalized to a range from -1.0 (left) to 1.0 (right).
     * - For a joystick, reports the absolute X position of the joystick.
     * The value is normalized to a range from -1.0 (left) to 1.0 (right).
     */
    AMOTION_EVENT_AXIS_X = 0,
    /**
     * Axis constant: Y axis of a motion event.
     *
     * - For a touch screen, reports the absolute Y screen position of the center of
     * the touch contact area.  The units are display pixels.
     * - For a touch pad, reports the absolute Y surface position of the center of the touch
     * contact area. The units are device-dependent.
     * - For a mouse, reports the absolute Y screen position of the mouse pointer.
     * The units are display pixels.
     * - For a trackball, reports the relative vertical displacement of the trackball.
     * The value is normalized to a range from -1.0 (up) to 1.0 (down).
     * - For a joystick, reports the absolute Y position of the joystick.
     * The value is normalized to a range from -1.0 (up or far) to 1.0 (down or near).
     */
    AMOTION_EVENT_AXIS_Y = 1,
    /**
     * Axis constant: Pressure axis of a motion event.
     *
     * - For a touch screen or touch pad, reports the approximate pressure applied to the surface
     * by a finger or other tool.  The value is normalized to a range from
     * 0 (no pressure at all) to 1 (normal pressure), although values higher than 1
     * may be generated depending on the calibration of the input device.
     * - For a trackball, the value is set to 1 if the trackball button is pressed
     * or 0 otherwise.
     * - For a mouse, the value is set to 1 if the primary mouse button is pressed
     * or 0 otherwise.
     */
    AMOTION_EVENT_AXIS_PRESSURE = 2,
    /**
     * Axis constant: Size axis of a motion event.
     *
     * - For a touch screen or touch pad, reports the approximate size of the contact area in
     * relation to the maximum detectable size for the device.  The value is normalized
     * to a range from 0 (smallest detectable size) to 1 (largest detectable size),
     * although it is not a linear scale. This value is of limited use.
     * To obtain calibrated size information, see
     * {@link AMOTION_EVENT_AXIS_TOUCH_MAJOR} or {@link AMOTION_EVENT_AXIS_TOOL_MAJOR}.
     */
    AMOTION_EVENT_AXIS_SIZE = 3,
    /**
     * Axis constant: TouchMajor axis of a motion event.
     *
     * - For a touch screen, reports the length of the major axis of an ellipse that
     * represents the touch area at the point of contact.
     * The units are display pixels.
     * - For a touch pad, reports the length of the major axis of an ellipse that
     * represents the touch area at the point of contact.
     * The units are device-dependent.
     */
    AMOTION_EVENT_AXIS_TOUCH_MAJOR = 4,
    /**
     * Axis constant: TouchMinor axis of a motion event.
     *
     * - For a touch screen, reports the length of the minor axis of an ellipse that
     * represents the touch area at the point of contact.
     * The units are display pixels.
     * - For a touch pad, reports the length of the minor axis of an ellipse that
     * represents the touch area at the point of contact.
     * The units are device-dependent.
     *
     * When the touch is circular, the major and minor axis lengths will be equal to one another.
     */
    AMOTION_EVENT_AXIS_TOUCH_MINOR = 5,
    /**
     * Axis constant: ToolMajor axis of a motion event.
     *
     * - For a touch screen, reports the length of the major axis of an ellipse that
     * represents the size of the approaching finger or tool used to make contact.
     * - For a touch pad, reports the length of the major axis of an ellipse that
     * represents the size of the approaching finger or tool used to make contact.
     * The units are device-dependent.
     *
     * When the touch is circular, the major and minor axis lengths will be equal to one another.
     *
     * The tool size may be larger than the touch size since the tool may not be fully
     * in contact with the touch sensor.
     */
    AMOTION_EVENT_AXIS_TOOL_MAJOR = 6,
    /**
     * Axis constant: ToolMinor axis of a motion event.
     *
     * - For a touch screen, reports the length of the minor axis of an ellipse that
     * represents the size of the approaching finger or tool used to make contact.
     * - For a touch pad, reports the length of the minor axis of an ellipse that
     * represents the size of the approaching finger or tool used to make contact.
     * The units are device-dependent.
     *
     * When the touch is circular, the major and minor axis lengths will be equal to one another.
     *
     * The tool size may be larger than the touch size since the tool may not be fully
     * in contact with the touch sensor.
     */
    AMOTION_EVENT_AXIS_TOOL_MINOR = 7,
    /**
     * Axis constant: Orientation axis of a motion event.
     *
     * - For a touch screen or touch pad, reports the orientation of the finger
     * or tool in radians relative to the vertical plane of the device.
     * An angle of 0 radians indicates that the major axis of contact is oriented
     * upwards, is perfectly circular or is of unknown orientation.  A positive angle
     * indicates that the major axis of contact is oriented to the right.  A negative angle
     * indicates that the major axis of contact is oriented to the left.
     * The full range is from -PI/2 radians (finger pointing fully left) to PI/2 radians
     * (finger pointing fully right).
     * - For a stylus, the orientation indicates the direction in which the stylus
     * is pointing in relation to the vertical axis of the current orientation of the screen.
     * The range is from -PI radians to PI radians, where 0 is pointing up,
     * -PI/2 radians is pointing left, -PI or PI radians is pointing down, and PI/2 radians
     * is pointing right.  See also #AMOTION_EVENT_AXIS_TILT.
     */
    AMOTION_EVENT_AXIS_ORIENTATION = 8,
    /**
     * Axis constant: Vertical Scroll axis of a motion event.
     *
     * - For a mouse, reports the relative movement of the vertical scroll wheel.
     * The value is normalized to a range from -1.0 (down) to 1.0 (up).
     *
     * This axis should be used to scroll views vertically.
     */
    AMOTION_EVENT_AXIS_VSCROLL = 9,
    /**
     * Axis constant: Horizontal Scroll axis of a motion event.
     *
     * - For a mouse, reports the relative movement of the horizontal scroll wheel.
     * The value is normalized to a range from -1.0 (left) to 1.0 (right).
     *
     * This axis should be used to scroll views horizontally.
     */
    AMOTION_EVENT_AXIS_HSCROLL = 10,
    /**
     * Axis constant: Z axis of a motion event.
     *
     * - For a joystick, reports the absolute Z position of the joystick.
     * The value is normalized to a range from -1.0 (high) to 1.0 (low).
     * <em>On game pads with two analog joysticks, this axis is often reinterpreted
     * to report the absolute X position of the second joystick instead.</em>
     */
    AMOTION_EVENT_AXIS_Z = 11,
    /**
     * Axis constant: X Rotation axis of a motion event.
     *
     * - For a joystick, reports the absolute rotation angle about the X axis.
     * The value is normalized to a range from -1.0 (counter-clockwise) to 1.0 (clockwise).
     */
    AMOTION_EVENT_AXIS_RX = 12,
    /**
     * Axis constant: Y Rotation axis of a motion event.
     *
     * - For a joystick, reports the absolute rotation angle about the Y axis.
     * The value is normalized to a range from -1.0 (counter-clockwise) to 1.0 (clockwise).
     */
    AMOTION_EVENT_AXIS_RY = 13,
    /**
     * Axis constant: Z Rotation axis of a motion event.
     *
     * - For a joystick, reports the absolute rotation angle about the Z axis.
     * The value is normalized to a range from -1.0 (counter-clockwise) to 1.0 (clockwise).
     * On game pads with two analog joysticks, this axis is often reinterpreted
     * to report the absolute Y position of the second joystick instead.
     */
    AMOTION_EVENT_AXIS_RZ = 14,
    /**
     * Axis constant: Hat X axis of a motion event.
     *
     * - For a joystick, reports the absolute X position of the directional hat control.
     * The value is normalized to a range from -1.0 (left) to 1.0 (right).
     */
    AMOTION_EVENT_AXIS_HAT_X = 15,
    /**
     * Axis constant: Hat Y axis of a motion event.
     *
     * - For a joystick, reports the absolute Y position of the directional hat control.
     * The value is normalized to a range from -1.0 (up) to 1.0 (down).
     */
    AMOTION_EVENT_AXIS_HAT_Y = 16,
    /**
     * Axis constant: Left Trigger axis of a motion event.
     *
     * - For a joystick, reports the absolute position of the left trigger control.
     * The value is normalized to a range from 0.0 (released) to 1.0 (fully pressed).
     */
    AMOTION_EVENT_AXIS_LTRIGGER = 17,
    /**
     * Axis constant: Right Trigger axis of a motion event.
     *
     * - For a joystick, reports the absolute position of the right trigger control.
     * The value is normalized to a range from 0.0 (released) to 1.0 (fully pressed).
     */
    AMOTION_EVENT_AXIS_RTRIGGER = 18,
    /**
     * Axis constant: Throttle axis of a motion event.
     *
     * - For a joystick, reports the absolute position of the throttle control.
     * The value is normalized to a range from 0.0 (fully open) to 1.0 (fully closed).
     */
    AMOTION_EVENT_AXIS_THROTTLE = 19,
    /**
     * Axis constant: Rudder axis of a motion event.
     *
     * - For a joystick, reports the absolute position of the rudder control.
     * The value is normalized to a range from -1.0 (turn left) to 1.0 (turn right).
     */
    AMOTION_EVENT_AXIS_RUDDER = 20,
    /**
     * Axis constant: Wheel axis of a motion event.
     *
     * - For a joystick, reports the absolute position of the steering wheel control.
     * The value is normalized to a range from -1.0 (turn left) to 1.0 (turn right).
     */
    AMOTION_EVENT_AXIS_WHEEL = 21,
    /**
     * Axis constant: Gas axis of a motion event.
     *
     * - For a joystick, reports the absolute position of the gas (accelerator) control.
     * The value is normalized to a range from 0.0 (no acceleration)
     * to 1.0 (maximum acceleration).
     */
    AMOTION_EVENT_AXIS_GAS = 22,
    /**
     * Axis constant: Brake axis of a motion event.
     *
     * - For a joystick, reports the absolute position of the brake control.
     * The value is normalized to a range from 0.0 (no braking) to 1.0 (maximum braking).
     */
    AMOTION_EVENT_AXIS_BRAKE = 23,
    /**
     * Axis constant: Distance axis of a motion event.
     *
     * - For a stylus, reports the distance of the stylus from the screen.
     * A value of 0.0 indicates direct contact and larger values indicate increasing
     * distance from the surface.
     */
    AMOTION_EVENT_AXIS_DISTANCE = 24,
    /**
     * Axis constant: Tilt axis of a motion event.
     *
     * - For a stylus, reports the tilt angle of the stylus in radians where
     * 0 radians indicates that the stylus is being held perpendicular to the
     * surface, and PI/2 radians indicates that the stylus is being held flat
     * against the surface.
     */
    AMOTION_EVENT_AXIS_TILT = 25,
    /**
     * Axis constant:  Generic scroll axis of a motion event.
     *
     * - This is used for scroll axis motion events that can't be classified as strictly
     *   vertical or horizontal. The movement of a rotating scroller is an example of this.
     */
    AMOTION_EVENT_AXIS_SCROLL = 26,
    /**
     * Axis constant: The movement of x position of a motion event.
     *
     * - For a mouse, reports a difference of x position between the previous position.
     * This is useful when pointer is captured, in that case the mouse pointer doesn't
     * change the location but this axis reports the difference which allows the app
     * to see how the mouse is moved.
     */
    AMOTION_EVENT_AXIS_RELATIVE_X = 27,
    /**
     * Axis constant: The movement of y position of a motion event.
     *
     * Same as #AMOTION_EVENT_AXIS_RELATIVE_X, but for y position.
     */
    AMOTION_EVENT_AXIS_RELATIVE_Y = 28,
    /**
     * Axis constant: Generic 1 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_1 = 32,
    /**
     * Axis constant: Generic 2 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_2 = 33,
    /**
     * Axis constant: Generic 3 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_3 = 34,
    /**
     * Axis constant: Generic 4 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_4 = 35,
    /**
     * Axis constant: Generic 5 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_5 = 36,
    /**
     * Axis constant: Generic 6 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_6 = 37,
    /**
     * Axis constant: Generic 7 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_7 = 38,
    /**
     * Axis constant: Generic 8 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_8 = 39,
    /**
     * Axis constant: Generic 9 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_9 = 40,
    /**
     * Axis constant: Generic 10 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_10 = 41,
    /**
     * Axis constant: Generic 11 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_11 = 42,
    /**
     * Axis constant: Generic 12 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_12 = 43,
    /**
     * Axis constant: Generic 13 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_13 = 44,
    /**
     * Axis constant: Generic 14 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_14 = 45,
    /**
     * Axis constant: Generic 15 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_15 = 46,
    /**
     * Axis constant: Generic 16 axis of a motion event.
     * The interpretation of a generic axis is device-specific.
     */
    AMOTION_EVENT_AXIS_GENERIC_16 = 47,
    /**
     * Axis constant: X gesture offset axis of a motion event.
     *
     * - For a touch pad, reports the distance that a swipe gesture has moved in the X axis, as a
     *   proportion of the touch pad's size. For example, if a touch pad is 1000 units wide, and a
     *   swipe gesture starts at X = 500 then moves to X = 400, this axis would have a value of
     *   -0.1.
     *
     * These values are relative to the state from the last event, not accumulated, so developers
     * should make sure to process this axis value for all batched historical events.
     *
     * This axis is only set on the first pointer in a motion event.
     */
    AMOTION_EVENT_AXIS_GESTURE_X_OFFSET = 48,
    /**
     * Axis constant: Y gesture offset axis of a motion event.
     *
     * The same as {@link AMOTION_EVENT_AXIS_GESTURE_X_OFFSET}, but for the Y axis.
     */
    AMOTION_EVENT_AXIS_GESTURE_Y_OFFSET = 49,
    /**
     * Axis constant: X scroll distance axis of a motion event.
     *
     * - For a touch pad, reports the distance that should be scrolled in the X axis as a result of
     *   the user's two-finger scroll gesture, in display pixels.
     *
     * These values are relative to the state from the last event, not accumulated, so developers
     * should make sure to process this axis value for all batched historical events.
     *
     * This axis is only set on the first pointer in a motion event.
     */
    AMOTION_EVENT_AXIS_GESTURE_SCROLL_X_DISTANCE = 50,
    /**
     * Axis constant: Y scroll distance axis of a motion event.
     *
     * The same as {@link AMOTION_EVENT_AXIS_GESTURE_SCROLL_X_DISTANCE}, but for the Y axis.
     */
    AMOTION_EVENT_AXIS_GESTURE_SCROLL_Y_DISTANCE = 51,
    /**
     * Axis constant: pinch scale factor of a motion event.
     *
     * - For a touch pad, reports the change in distance between the fingers when the user is making
     *   a pinch gesture, as a proportion of that distance when the gesture was last reported. For
     *   example, if the fingers were 50 units apart and are now 52 units apart, the scale factor
     *   would be 1.04.
     *
     * These values are relative to the state from the last event, not accumulated, so developers
     * should make sure to process this axis value for all batched historical events.
     *
     * This axis is only set on the first pointer in a motion event.
     */
    AMOTION_EVENT_AXIS_GESTURE_PINCH_SCALE_FACTOR = 52,

    /**
     * Axis constant: the number of fingers being used in a multi-finger swipe gesture.
     *
     * - For a touch pad, reports the number of fingers being used in a multi-finger swipe gesture
     *   (with CLASSIFICATION_MULTI_FINGER_SWIPE).
     *
     * Since CLASSIFICATION_MULTI_FINGER_SWIPE is a hidden API, so is this axis. It is only set on
     * the first pointer in a motion event.
     */
    AMOTION_EVENT_AXIS_GESTURE_SWIPE_FINGER_COUNT = 53,

    /**
     * Note: This is not an "Axis constant". It does not represent any axis, nor should it be used
     * to represent any axis. It is a constant holding the value of the largest defined axis value,
     * to make some computations (like iterating through all possible axes) cleaner.
     * Please update the value accordingly if you add a new axis.
     */
    AMOTION_EVENT_MAXIMUM_VALID_AXIS_VALUE = AMOTION_EVENT_AXIS_GESTURE_SWIPE_FINGER_COUNT,

    // NOTE: If you add a new axis here you must also add it to several other files.
    //       Refer to frameworks/base/core/java/android/view/MotionEvent.java for the full list.
    //       Update AMOTION_EVENT_MAXIMUM_VALID_AXIS_VALUE accordingly as well.
};

/**
 * Constants that identify buttons that are associated with motion events.
 * Refer to the documentation on the MotionEvent class for descriptions of each button.
 */
enum {
    // LINT.IfChange(AMOTION_EVENT_BUTTON)
    /** primary */
    AMOTION_EVENT_BUTTON_PRIMARY = 1 << 0,
    /** secondary */
    AMOTION_EVENT_BUTTON_SECONDARY = 1 << 1,
    /** tertiary */
    AMOTION_EVENT_BUTTON_TERTIARY = 1 << 2,
    /** back */
    AMOTION_EVENT_BUTTON_BACK = 1 << 3,
    /** forward */
    AMOTION_EVENT_BUTTON_FORWARD = 1 << 4,
    AMOTION_EVENT_BUTTON_STYLUS_PRIMARY = 1 << 5,
    AMOTION_EVENT_BUTTON_STYLUS_SECONDARY = 1 << 6,
    // LINT.ThenChange(/frameworks/native/libs/input/rust/input.rs,/frameworks/native/services/inputflinger/tests/fuzzers/FuzzedInputStream.h)
};

/**
 * Constants that identify tool types.
 * Refer to the documentation on the MotionEvent class for descriptions of each tool type.
 */
enum {
    /** unknown */
    AMOTION_EVENT_TOOL_TYPE_UNKNOWN = 0,
    /** finger */
    AMOTION_EVENT_TOOL_TYPE_FINGER = 1,
    /** stylus */
    AMOTION_EVENT_TOOL_TYPE_STYLUS = 2,
    /** mouse */
    AMOTION_EVENT_TOOL_TYPE_MOUSE = 3,
    /** eraser */
    AMOTION_EVENT_TOOL_TYPE_ERASER = 4,
    /** palm */
    AMOTION_EVENT_TOOL_TYPE_PALM = 5,
};

/**
 * Constants that identify different gesture classification types.
 */
enum AMotionClassification : uint32_t {
    /**
     * Classification constant: None.
     *
     * No additional information is available about the current motion event stream.
     */
    AMOTION_EVENT_CLASSIFICATION_NONE = 0,
    /**
     * Classification constant: Ambiguous gesture.
     *
     * The user's intent with respect to the current event stream is not yet determined. Events
     * starting in #AMOTION_EVENT_CLASSIFICATION_AMBIGUOUS_GESTURE will eventually resolve into
     * either #AMOTION_EVENT_CLASSIFICATION_DEEP_PRESS or #AMOTION_EVENT_CLASSIFICATION_NONE.
     * Gestural actions, such as scrolling, should be inhibited until the classification resolves
     * to another value or the event stream ends.
     */
    AMOTION_EVENT_CLASSIFICATION_AMBIGUOUS_GESTURE = 1,
    /**
     * Classification constant: Deep press.
     *
     * The current event stream represents the user intentionally pressing harder on the screen.
     * This classification type should be used to accelerate the long press behaviour.
     */
    AMOTION_EVENT_CLASSIFICATION_DEEP_PRESS = 2,
    /**
     * Classification constant: touchpad two-finger swipe.
     *
     * The current event stream represents the user swiping with two fingers on a touchpad.
     */
    AMOTION_EVENT_CLASSIFICATION_TWO_FINGER_SWIPE = 3,
    /**
     * Classification constant: multi-finger swipe.
     *
     * The current event stream represents the user swiping with three or more fingers on a
     * touchpad. Unlike two-finger swipes, these are only to be handled by the system UI, which is
     * why they have a separate constant from two-finger swipes.
     */
    AMOTION_EVENT_CLASSIFICATION_MULTI_FINGER_SWIPE = 4,
    /**
     * Classification constant: pinch.
     *
     * The current event stream represents the user pinching with two fingers on a touchpad. The
     * gesture is centered around the current cursor position.
     */
    AMOTION_EVENT_CLASSIFICATION_PINCH = 5,
};

/**
 * Input source masks.
 *
 * Refer to the documentation on android.view.InputDevice for more details about input sources
 * and their correct interpretation.
 */
enum {
    /** mask */
    AINPUT_SOURCE_CLASS_MASK = 0x000000ff,

    /** none */
    AINPUT_SOURCE_CLASS_NONE = 0x00000000,
    /** button */
    AINPUT_SOURCE_CLASS_BUTTON = 0x00000001,
    /** pointer */
    AINPUT_SOURCE_CLASS_POINTER = 0x00000002,
    /** navigation */
    AINPUT_SOURCE_CLASS_NAVIGATION = 0x00000004,
    /** position */
    AINPUT_SOURCE_CLASS_POSITION = 0x00000008,
    /** joystick */
    AINPUT_SOURCE_CLASS_JOYSTICK = 0x00000010,
};

/**
 * Input sources.
 */
enum {
    /** unknown */
    AINPUT_SOURCE_UNKNOWN = 0x00000000,

    /** keyboard */
    AINPUT_SOURCE_KEYBOARD = 0x00000100 | AINPUT_SOURCE_CLASS_BUTTON,
    /** dpad */
    AINPUT_SOURCE_DPAD = 0x00000200 | AINPUT_SOURCE_CLASS_BUTTON,
    /** gamepad */
    AINPUT_SOURCE_GAMEPAD = 0x00000400 | AINPUT_SOURCE_CLASS_BUTTON,
    /** touchscreen */
    AINPUT_SOURCE_TOUCHSCREEN = 0x00001000 | AINPUT_SOURCE_CLASS_POINTER,
    /** mouse */
    AINPUT_SOURCE_MOUSE = 0x00002000 | AINPUT_SOURCE_CLASS_POINTER,
    /** stylus */
    AINPUT_SOURCE_STYLUS = 0x00004000 | AINPUT_SOURCE_CLASS_POINTER,
    /** bluetooth stylus */
    AINPUT_SOURCE_BLUETOOTH_STYLUS = 0x00008000 | AINPUT_SOURCE_STYLUS,
    /** trackball */
    AINPUT_SOURCE_TRACKBALL = 0x00010000 | AINPUT_SOURCE_CLASS_NAVIGATION,
    /** mouse relative */
    AINPUT_SOURCE_MOUSE_RELATIVE = 0x00020000 | AINPUT_SOURCE_CLASS_NAVIGATION,
    /** touchpad */
    AINPUT_SOURCE_TOUCHPAD = 0x00100000 | AINPUT_SOURCE_CLASS_POSITION,
    /** navigation */
    AINPUT_SOURCE_TOUCH_NAVIGATION = 0x00200000 | AINPUT_SOURCE_CLASS_NONE,
    /** joystick */
    AINPUT_SOURCE_JOYSTICK = 0x01000000 | AINPUT_SOURCE_CLASS_JOYSTICK,
    /** HDMI */
    AINPUT_SOURCE_HDMI = 0x02000000 | AINPUT_SOURCE_CLASS_BUTTON,
    /** sensor */
    AINPUT_SOURCE_SENSOR = 0x04000000 | AINPUT_SOURCE_CLASS_NONE,
    /** rotary encoder */
    AINPUT_SOURCE_ROTARY_ENCODER = 0x00400000 | AINPUT_SOURCE_CLASS_NONE,

    /** any */
    AINPUT_SOURCE_ANY = 0xffffff00,
};

/**
 * Keyboard types.
 *
 * Refer to the documentation on android.view.InputDevice for more details.
 */
enum {
    /** none */
    AINPUT_KEYBOARD_TYPE_NONE = 0,
    /** non alphabetic */
    AINPUT_KEYBOARD_TYPE_NON_ALPHABETIC = 1,
    /** alphabetic */
    AINPUT_KEYBOARD_TYPE_ALPHABETIC = 2,
};

/**
 * Constants used to retrieve information about the range of motion for a particular
 * coordinate of a motion event.
 *
 * Refer to the documentation on android.view.InputDevice for more details about input sources
 * and their correct interpretation.
 *
 * @deprecated These constants are deprecated. Use {@link AMOTION_EVENT_AXIS AMOTION_EVENT_AXIS_*}
 * constants instead.
 */
enum {
    /** x */
    AINPUT_MOTION_RANGE_X = AMOTION_EVENT_AXIS_X,
    /** y */
    AINPUT_MOTION_RANGE_Y = AMOTION_EVENT_AXIS_Y,
    /** pressure */
    AINPUT_MOTION_RANGE_PRESSURE = AMOTION_EVENT_AXIS_PRESSURE,
    /** size */
    AINPUT_MOTION_RANGE_SIZE = AMOTION_EVENT_AXIS_SIZE,
    /** touch major */
    AINPUT_MOTION_RANGE_TOUCH_MAJOR = AMOTION_EVENT_AXIS_TOUCH_MAJOR,
    /** touch minor */
    AINPUT_MOTION_RANGE_TOUCH_MINOR = AMOTION_EVENT_AXIS_TOUCH_MINOR,
    /** tool major */
    AINPUT_MOTION_RANGE_TOOL_MAJOR = AMOTION_EVENT_AXIS_TOOL_MAJOR,
    /** tool minor */
    AINPUT_MOTION_RANGE_TOOL_MINOR = AMOTION_EVENT_AXIS_TOOL_MINOR,
    /** orientation */
    AINPUT_MOTION_RANGE_ORIENTATION = AMOTION_EVENT_AXIS_ORIENTATION,
};


/**
 * Input event accessors.
 *
 * Note that most functions can only be used on input events that are of a given type.
 * Calling these functions on input events of other types will yield undefined behavior.
 */

/*** Accessors for all input events. ***/

/** Get the input event type. */
int32_t AInputEvent_getType(const AInputEvent* event);

/** Get the id for the device that an input event came from.
 *
 * Input events can be generated by multiple different input devices.
 * Use the input device id to obtain information about the input
 * device that was responsible for generating a particular event.
 *
 * An input device id of 0 indicates that the event didn't come from a physical device;
 * other numbers are arbitrary and you shouldn't depend on the values.
 * Use the provided input device query API to obtain information about input devices.
 */
int32_t AInputEvent_getDeviceId(const AInputEvent* event);

/** Get the input event source. */
int32_t AInputEvent_getSource(const AInputEvent* event);

/**
 * Releases interface objects created by {@link AKeyEvent_fromJava()}
 * and {@link AMotionEvent_fromJava()}.
 * After returning, the specified {@link AInputEvent}* object becomes invalid and should no longer
 * be used. The underlying Java object remains valid and does not change its state.
 *
 * Available since API level 31.
 */
void AInputEvent_release(const AInputEvent* event) __INTRODUCED_IN(31);

/*** Accessors for key events only. ***/

/** Get the key event action. */
int32_t AKeyEvent_getAction(const AInputEvent* key_event);

/** Get the key event flags. */
int32_t AKeyEvent_getFlags(const AInputEvent* key_event);

/**
 * Get the key code of the key event.
 * This is the physical key that was pressed, not the Unicode character.
 */
int32_t AKeyEvent_getKeyCode(const AInputEvent* key_event);

/**
 * Get the hardware key id of this key event.
 * These values are not reliable and vary from device to device.
 */
int32_t AKeyEvent_getScanCode(const AInputEvent* key_event);

/** Get the meta key state. */
int32_t AKeyEvent_getMetaState(const AInputEvent* key_event);

/**
 * Get the repeat count of the event.
 * For both key up an key down events, this is the number of times the key has
 * repeated with the first down starting at 0 and counting up from there.  For
 * multiple key events, this is the number of down/up pairs that have occurred.
 */
int32_t AKeyEvent_getRepeatCount(const AInputEvent* key_event);

/**
 * Get the time of the most recent key down event, in the
 * java.lang.System.nanoTime() time base.  If this is a down event,
 * this will be the same as eventTime.
 * Note that when chording keys, this value is the down time of the most recently
 * pressed key, which may not be the same physical key of this event.
 */
int64_t AKeyEvent_getDownTime(const AInputEvent* key_event);

/**
 * Get the time this event occurred, in the
 * java.lang.System.nanoTime() time base.
 */
int64_t AKeyEvent_getEventTime(const AInputEvent* key_event);

/**
 * Creates a native {@link AInputEvent}* object that is a copy of the specified Java
 * android.view.KeyEvent. The result may be used with generic and KeyEvent-specific AInputEvent_*
 * functions. The object returned by this function must be disposed using
 * {@link AInputEvent_release()}.
 *
 * Available since API level 31.
 */
const AInputEvent* AKeyEvent_fromJava(JNIEnv* env, jobject keyEvent) __INTRODUCED_IN(31);

/*** Accessors for motion events only. ***/

/** Get the combined motion event action code and pointer index. */
int32_t AMotionEvent_getAction(const AInputEvent* motion_event);

/** Get the motion event flags. */
int32_t AMotionEvent_getFlags(const AInputEvent* motion_event);

/**
 * Get the state of any meta / modifier keys that were in effect when the
 * event was generated.
 */
int32_t AMotionEvent_getMetaState(const AInputEvent* motion_event);

/** Get the button state of all buttons that are pressed. */
int32_t AMotionEvent_getButtonState(const AInputEvent* motion_event);

/**
 * Get a bitfield indicating which edges, if any, were touched by this motion event.
 * For touch events, clients can use this to determine if the user's finger was
 * touching the edge of the display.
 */
int32_t AMotionEvent_getEdgeFlags(const AInputEvent* motion_event);

/**
 * Get the time when the user originally pressed down to start a stream of
 * position events, in the java.lang.System.nanoTime() time base.
 */
int64_t AMotionEvent_getDownTime(const AInputEvent* motion_event);

/**
 * Get the time when this specific event was generated,
 * in the java.lang.System.nanoTime() time base.
 */
int64_t AMotionEvent_getEventTime(const AInputEvent* motion_event);

/**
 * Get the X coordinate offset.
 * For touch events on the screen, this is the delta that was added to the raw
 * screen coordinates to adjust for the absolute position of the containing windows
 * and views.
 */
float AMotionEvent_getXOffset(const AInputEvent* motion_event);

/**
 * Get the Y coordinate offset.
 * For touch events on the screen, this is the delta that was added to the raw
 * screen coordinates to adjust for the absolute position of the containing windows
 * and views.
 */
float AMotionEvent_getYOffset(const AInputEvent* motion_event);

/**
 * Get the precision of the X coordinates being reported.
 * You can multiply this number with an X coordinate sample to find the
 * actual hardware value of the X coordinate.
 */
float AMotionEvent_getXPrecision(const AInputEvent* motion_event);

/**
 * Get the precision of the Y coordinates being reported.
 * You can multiply this number with a Y coordinate sample to find the
 * actual hardware value of the Y coordinate.
 */
float AMotionEvent_getYPrecision(const AInputEvent* motion_event);

/**
 * Get the number of pointers of data contained in this event.
 * Always >= 1.
 */
size_t AMotionEvent_getPointerCount(const AInputEvent* motion_event);

/**
 * Get the pointer identifier associated with a particular pointer
 * data index in this event.  The identifier tells you the actual pointer
 * number associated with the data, accounting for individual pointers
 * going up and down since the start of the current gesture.
 */
int32_t AMotionEvent_getPointerId(const AInputEvent* motion_event, size_t pointer_index);

/**
 * Get the tool type of a pointer for the given pointer index.
 * The tool type indicates the type of tool used to make contact such as a
 * finger or stylus, if known.
 */
int32_t AMotionEvent_getToolType(const AInputEvent* motion_event, size_t pointer_index);

/**
 * Get the original raw X coordinate of this event.
 * For touch events on the screen, this is the original location of the event
 * on the screen, before it had been adjusted for the containing window
 * and views.
 */
float AMotionEvent_getRawX(const AInputEvent* motion_event, size_t pointer_index);

/**
 * Get the original raw X coordinate of this event.
 * For touch events on the screen, this is the original location of the event
 * on the screen, before it had been adjusted for the containing window
 * and views.
 */
float AMotionEvent_getRawY(const AInputEvent* motion_event, size_t pointer_index);

/**
 * Get the current X coordinate of this event for the given pointer index.
 * Whole numbers are pixels; the value may have a fraction for input devices
 * that are sub-pixel precise.
 */
float AMotionEvent_getX(const AInputEvent* motion_event, size_t pointer_index);

/**
 * Get the current Y coordinate of this event for the given pointer index.
 * Whole numbers are pixels; the value may have a fraction for input devices
 * that are sub-pixel precise.
 */
float AMotionEvent_getY(const AInputEvent* motion_event, size_t pointer_index);

/**
 * Get the current pressure of this event for the given pointer index.
 * The pressure generally ranges from 0 (no pressure at all) to 1 (normal pressure),
 * although values higher than 1 may be generated depending on the calibration of
 * the input device.
 */
float AMotionEvent_getPressure(const AInputEvent* motion_event, size_t pointer_index);

/**
 * Get the current scaled value of the approximate size for the given pointer index.
 * This represents some approximation of the area of the screen being
 * pressed; the actual value in pixels corresponding to the
 * touch is normalized with the device specific range of values
 * and scaled to a value between 0 and 1.  The value of size can be used to
 * determine fat touch events.
 */
float AMotionEvent_getSize(const AInputEvent* motion_event, size_t pointer_index);

/**
 * Get the current length of the major axis of an ellipse that describes the touch area
 * at the point of contact for the given pointer index.
 */
float AMotionEvent_getTouchMajor(const AInputEvent* motion_event, size_t pointer_index);

/**
 * Get the current length of the minor axis of an ellipse that describes the touch area
 * at the point of contact for the given pointer index.
 */
float AMotionEvent_getTouchMinor(const AInputEvent* motion_event, size_t pointer_index);

/**
 * Get the current length of the major axis of an ellipse that describes the size
 * of the approaching tool for the given pointer index.
 * The tool area represents the estimated size of the finger or pen that is
 * touching the device independent of its actual touch area at the point of contact.
 */
float AMotionEvent_getToolMajor(const AInputEvent* motion_event, size_t pointer_index);

/**
 * Get the current length of the minor axis of an ellipse that describes the size
 * of the approaching tool for the given pointer index.
 * The tool area represents the estimated size of the finger or pen that is
 * touching the device independent of its actual touch area at the point of contact.
 */
float AMotionEvent_getToolMinor(const AInputEvent* motion_event, size_t pointer_index);

/**
 * Get the current orientation of the touch area and tool area in radians clockwise from
 * vertical for the given pointer index.
 * An angle of 0 degrees indicates that the major axis of contact is oriented
 * upwards, is perfectly circular or is of unknown orientation.  A positive angle
 * indicates that the major axis of contact is oriented to the right.  A negative angle
 * indicates that the major axis of contact is oriented to the left.
 * The full range is from -PI/2 radians (finger pointing fully left) to PI/2 radians
 * (finger pointing fully right).
 */
float AMotionEvent_getOrientation(const AInputEvent* motion_event, size_t pointer_index);

/** Get the value of the request axis for the given pointer index. */
float AMotionEvent_getAxisValue(const AInputEvent* motion_event,
        int32_t axis, size_t pointer_index);

/**
 * Get the number of historical points in this event.  These are movements that
 * have occurred between this event and the previous event.  This only applies
 * to #AMOTION_EVENT_ACTION_MOVE events -- all other actions will have a size of 0.
 * Historical samples are indexed from oldest to newest.
 */
size_t AMotionEvent_getHistorySize(const AInputEvent* motion_event);

/**
 * Get the time that a historical movement occurred between this event and
 * the previous event, in the java.lang.System.nanoTime() time base.
 */
int64_t AMotionEvent_getHistoricalEventTime(const AInputEvent* motion_event,
        size_t history_index);

/**
 * Get the historical raw X coordinate of this event for the given pointer index that
 * occurred between this event and the previous motion event.
 * For touch events on the screen, this is the original location of the event
 * on the screen, before it had been adjusted for the containing window
 * and views.
 * Whole numbers are pixels; the value may have a fraction for input devices
 * that are sub-pixel precise.
 */
float AMotionEvent_getHistoricalRawX(const AInputEvent* motion_event, size_t pointer_index,
        size_t history_index);

/**
 * Get the historical raw Y coordinate of this event for the given pointer index that
 * occurred between this event and the previous motion event.
 * For touch events on the screen, this is the original location of the event
 * on the screen, before it had been adjusted for the containing window
 * and views.
 * Whole numbers are pixels; the value may have a fraction for input devices
 * that are sub-pixel precise.
 */
float AMotionEvent_getHistoricalRawY(const AInputEvent* motion_event, size_t pointer_index,
        size_t history_index);

/**
 * Get the historical X coordinate of this event for the given pointer index that
 * occurred between this event and the previous motion event.
 * Whole numbers are pixels; the value may have a fraction for input devices
 * that are sub-pixel precise.
 */
float AMotionEvent_getHistoricalX(const AInputEvent* motion_event, size_t pointer_index,
        size_t history_index);

/**
 * Get the historical Y coordinate of this event for the given pointer index that
 * occurred between this event and the previous motion event.
 * Whole numbers are pixels; the value may have a fraction for input devices
 * that are sub-pixel precise.
 */
float AMotionEvent_getHistoricalY(const AInputEvent* motion_event, size_t pointer_index,
        size_t history_index);

/**
 * Get the historical pressure of this event for the given pointer index that
 * occurred between this event and the previous motion event.
 * The pressure generally ranges from 0 (no pressure at all) to 1 (normal pressure),
 * although values higher than 1 may be generated depending on the calibration of
 * the input device.
 */
float AMotionEvent_getHistoricalPressure(const AInputEvent* motion_event, size_t pointer_index,
        size_t history_index);

/**
 * Get the current scaled value of the approximate size for the given pointer index that
 * occurred between this event and the previous motion event.
 * This represents some approximation of the area of the screen being
 * pressed; the actual value in pixels corresponding to the
 * touch is normalized with the device specific range of values
 * and scaled to a value between 0 and 1.  The value of size can be used to
 * determine fat touch events.
 */
float AMotionEvent_getHistoricalSize(const AInputEvent* motion_event, size_t pointer_index,
        size_t history_index);

/**
 * Get the historical length of the major axis of an ellipse that describes the touch area
 * at the point of contact for the given pointer index that
 * occurred between this event and the previous motion event.
 */
float AMotionEvent_getHistoricalTouchMajor(const AInputEvent* motion_event, size_t pointer_index,
        size_t history_index);

/**
 * Get the historical length of the minor axis of an ellipse that describes the touch area
 * at the point of contact for the given pointer index that
 * occurred between this event and the previous motion event.
 */
float AMotionEvent_getHistoricalTouchMinor(const AInputEvent* motion_event, size_t pointer_index,
        size_t history_index);

/**
 * Get the historical length of the major axis of an ellipse that describes the size
 * of the approaching tool for the given pointer index that
 * occurred between this event and the previous motion event.
 * The tool area represents the estimated size of the finger or pen that is
 * touching the device independent of its actual touch area at the point of contact.
 */
float AMotionEvent_getHistoricalToolMajor(const AInputEvent* motion_event, size_t pointer_index,
        size_t history_index);

/**
 * Get the historical length of the minor axis of an ellipse that describes the size
 * of the approaching tool for the given pointer index that
 * occurred between this event and the previous motion event.
 * The tool area represents the estimated size of the finger or pen that is
 * touching the device independent of its actual touch area at the point of contact.
 */
float AMotionEvent_getHistoricalToolMinor(const AInputEvent* motion_event, size_t pointer_index,
        size_t history_index);

/**
 * Get the historical orientation of the touch area and tool area in radians clockwise from
 * vertical for the given pointer index that
 * occurred between this event and the previous motion event.
 * An angle of 0 degrees indicates that the major axis of contact is oriented
 * upwards, is perfectly circular or is of unknown orientation.  A positive angle
 * indicates that the major axis of contact is oriented to the right.  A negative angle
 * indicates that the major axis of contact is oriented to the left.
 * The full range is from -PI/2 radians (finger pointing fully left) to PI/2 radians
 * (finger pointing fully right).
 */
float AMotionEvent_getHistoricalOrientation(const AInputEvent* motion_event, size_t pointer_index,
        size_t history_index);

/**
 * Get the historical value of the request axis for the given pointer index
 * that occurred between this event and the previous motion event.
 */
float AMotionEvent_getHistoricalAxisValue(const AInputEvent* motion_event,
        int32_t axis, size_t pointer_index, size_t history_index);

/**
 * Get the action button for the motion event. Returns a valid action button when the
 * event is associated with a button press or button release action. For other actions
 * the return value is undefined.
 *
 * @see #AMOTION_EVENT_BUTTON_PRIMARY
 * @see #AMOTION_EVENT_BUTTON_SECONDARY
 * @see #AMOTION_EVENT_BUTTON_TERTIARY
 * @see #AMOTION_EVENT_BUTTON_BACK
 * @see #AMOTION_EVENT_BUTTON_FORWARD
 * @see #AMOTION_EVENT_BUTTON_STYLUS_PRIMARY
 * @see #AMOTION_EVENT_BUTTON_STYLUS_SECONDARY
 */
int32_t AMotionEvent_getActionButton(const AInputEvent* motion_event)
        __INTRODUCED_IN(__ANDROID_API_T__);

/**
 * Returns the classification for the current gesture.
 * The classification may change as more events become available for the same gesture.
 *
 * @see #AMOTION_EVENT_CLASSIFICATION_NONE
 * @see #AMOTION_EVENT_CLASSIFICATION_AMBIGUOUS_GESTURE
 * @see #AMOTION_EVENT_CLASSIFICATION_DEEP_PRESS
*/
int32_t AMotionEvent_getClassification(const AInputEvent* motion_event)
        __INTRODUCED_IN(__ANDROID_API_T__);

/**
 * Creates a native {@link AInputEvent}* object that is a copy of the specified Java
 * android.view.MotionEvent. The result may be used with generic and MotionEvent-specific
 * AInputEvent_* functions. The object returned by this function must be disposed using
 * {@link AInputEvent_release()}.
 *
 * Available since API level 31.
 */
const AInputEvent* AMotionEvent_fromJava(JNIEnv* env, jobject motionEvent) __INTRODUCED_IN(31);

/**
 * Creates a java android.view.InputEvent object that is a copy of the specified native
 * {@link AInputEvent}.
 *
 * Specified {@link AInputEvent} is require to be a valid {@link MotionEvent} or {@link KeyEvent}
 * object.
 *
 *  Available since API level 35.
 */
jobject AInputEvent_toJava(JNIEnv* env, const AInputEvent* aInputEvent) __INTRODUCED_IN(35);

struct AInputQueue;
/**
 * Input queue
 *
 * An input queue is the facility through which you retrieve input
 * events.
 */
typedef struct AInputQueue AInputQueue;

/**
 * Add this input queue to a looper for processing.  See
 * {@link ALooper_addFd()} for information on the ident, callback, and data params.
 */
void AInputQueue_attachLooper(AInputQueue* queue, ALooper* looper,
        int ident, ALooper_callbackFunc callback, void* data);

/**
 * Remove the input queue from the looper it is currently attached to.
 */
void AInputQueue_detachLooper(AInputQueue* queue);

/**
 * Returns true if there are one or more events available in the
 * input queue.  Returns 1 if the queue has events; 0 if
 * it does not have events; and a negative value if there is an error.
 */
int32_t AInputQueue_hasEvents(AInputQueue* queue);

/**
 * Returns the next available event from the queue.  Returns a negative
 * value if no events are available or an error has occurred.
 */
int32_t AInputQueue_getEvent(AInputQueue* queue, AInputEvent** outEvent);

/**
 * Sends the key for standard pre-dispatching -- that is, possibly deliver
 * it to the current IME to be consumed before the app.  Returns 0 if it
 * was not pre-dispatched, meaning you can process it right now.  If non-zero
 * is returned, you must abandon the current event processing and allow the
 * event to appear again in the event queue (if it does not get consumed during
 * pre-dispatching).
 */
int32_t AInputQueue_preDispatchEvent(AInputQueue* queue, AInputEvent* event);

/**
 * Report that dispatching has finished with the given event.
 * This must be called after receiving an event with {@link AInputQueue_getEvent()}.
 */
void AInputQueue_finishEvent(AInputQueue* queue, AInputEvent* event, int handled);

/**
 * Returns the {@link AInputQueue}* object associated with the supplied Java InputQueue
 * object. The returned native object holds a weak reference to the Java object,
 * and is only valid as long as the Java object has not yet been disposed. You
 * should ensure that there is a strong reference to the Java object and that it
 * has not been disposed before using the returned object.
 *
 * Available since API level 33.
 */
AInputQueue* AInputQueue_fromJava(JNIEnv* env, jobject inputQueue) __INTRODUCED_IN(33);

#ifdef __cplusplus
}
#endif

#endif // _ANDROID_INPUT_H

/** @} */
