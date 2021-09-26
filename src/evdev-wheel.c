/*
 * Copyright © 2010 Intel Corporation
 * Copyright © 2013 Jonas Ådahl
 * Copyright © 2013-2017 Red Hat, Inc.
 * Copyright © 2017 James Ye <jye836@gmail.com>
 * Copyright © 2021 José Expósito
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include "evdev-fallback.h"
#include "util-input-event.h"

void
fallback_wheel_process_relative(struct fallback_dispatch *dispatch,
				struct evdev_device *device,
				struct input_event *e, uint64_t time)
{
	switch (e->code) {
	case REL_WHEEL:
		dispatch->wheel.lo_res.y += e->value;
		if (dispatch->wheel.emulate_hi_res_wheel)
			dispatch->wheel.hi_res.y += e->value * 120;
		dispatch->pending_event |= EVDEV_WHEEL;
		break;
	case REL_HWHEEL:
		dispatch->wheel.lo_res.x += e->value;
		if (dispatch->wheel.emulate_hi_res_wheel)
			dispatch->wheel.hi_res.x += e->value * 120;
		dispatch->pending_event |= EVDEV_WHEEL;
		break;
	case REL_WHEEL_HI_RES:
		dispatch->wheel.hi_res.y += e->value;
		dispatch->wheel.hi_res_event_received = true;
		dispatch->pending_event |= EVDEV_WHEEL;
		break;
	case REL_HWHEEL_HI_RES:
		dispatch->wheel.hi_res.x += e->value;
		dispatch->wheel.hi_res_event_received = true;
		dispatch->pending_event |= EVDEV_WHEEL;
		break;
	}
}

void
fallback_wheel_notify_physical_button(struct fallback_dispatch *dispatch,
				      struct evdev_device *device,
				      uint64_t time,
				      int button,
				      enum libinput_button_state state)
{
	if (button == BTN_MIDDLE)
		dispatch->wheel.is_inhibited = (state == LIBINPUT_BUTTON_STATE_PRESSED);

	/* Lenovo TrackPoint Keyboard II sends its own scroll events when its
	 * trackpoint is moved while the middle button is pressed.
	 * Do not inhibit the scroll events.
	 * https://gitlab.freedesktop.org/libinput/libinput/-/issues/651
	 */
	if (evdev_device_has_model_quirk(device,
					 QUIRK_MODEL_LENOVO_TRACKPOINT_KEYBOARD_2))
		dispatch->wheel.is_inhibited = false;
}

void
fallback_wheel_handle_state(struct fallback_dispatch *dispatch,
			    struct evdev_device *device,
			    uint64_t time)
{
	struct normalized_coords wheel_degrees = { 0.0, 0.0 };
	struct discrete_coords discrete = { 0.0, 0.0 };
	struct wheel_v120 v120 = { 0.0, 0.0 };

	if (!(device->seat_caps & EVDEV_DEVICE_POINTER))
		return;

	if (!dispatch->wheel.emulate_hi_res_wheel &&
	    !dispatch->wheel.hi_res_event_received &&
	    (dispatch->wheel.lo_res.x != 0 || dispatch->wheel.lo_res.y != 0)) {
		evdev_log_bug_kernel(device,
				     "device supports high-resolution scroll but only low-resolution events have been received.\n"
				     "See %s/incorrectly-enabled-hires.html for details\n",
				     HTTP_DOC_LINK);
		dispatch->wheel.emulate_hi_res_wheel = true;
		dispatch->wheel.hi_res.x = dispatch->wheel.lo_res.x * 120;
		dispatch->wheel.hi_res.y = dispatch->wheel.lo_res.y * 120;
	}

	if (dispatch->wheel.is_inhibited) {
		dispatch->wheel.hi_res.x = 0;
		dispatch->wheel.hi_res.y = 0;
		dispatch->wheel.lo_res.x = 0;
		dispatch->wheel.lo_res.y = 0;
		return;
	}

	if (device->model_flags & EVDEV_MODEL_LENOVO_SCROLLPOINT) {
		struct normalized_coords unaccel = { 0.0, 0.0 };

		dispatch->wheel.hi_res.y *= -1;
		fallback_normalize_delta(device, &dispatch->wheel.hi_res, &unaccel);
		evdev_post_scroll(device,
				  time,
				  LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS,
				  &unaccel);
		dispatch->wheel.hi_res.x = 0;
		dispatch->wheel.hi_res.y = 0;

		return;
	}

	if (dispatch->wheel.hi_res.y != 0) {
		int value = dispatch->wheel.hi_res.y;

		v120.y = -1 * value;
		wheel_degrees.y = -1 * value/120.0 * device->scroll.wheel_click_angle.y;
		evdev_notify_axis_wheel(
			device,
			time,
			bit(LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL),
			&wheel_degrees,
			&v120);
		dispatch->wheel.hi_res.y = 0;
	}

	if (dispatch->wheel.lo_res.y != 0) {
		int value = dispatch->wheel.lo_res.y;

		wheel_degrees.y = -1 * value * device->scroll.wheel_click_angle.y;
		discrete.y = -1 * value;
		evdev_notify_axis_legacy_wheel(
			device,
			time,
			bit(LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL),
			&wheel_degrees,
			&discrete);
		dispatch->wheel.lo_res.y = 0;
	}

	if (dispatch->wheel.hi_res.x != 0) {
		int value = dispatch->wheel.hi_res.x;

		v120.x = value;
		wheel_degrees.x = value/120.0 * device->scroll.wheel_click_angle.x;
		evdev_notify_axis_wheel(
			device,
			time,
			bit(LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL),
			&wheel_degrees,
			&v120);
		dispatch->wheel.hi_res.x = 0;
	}

	if (dispatch->wheel.lo_res.x != 0) {
		int value = dispatch->wheel.lo_res.x;

		wheel_degrees.x = value * device->scroll.wheel_click_angle.x;
		discrete.x = value;
		evdev_notify_axis_legacy_wheel(
			device,
			time,
			bit(LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL),
			&wheel_degrees,
			&discrete);
		dispatch->wheel.lo_res.x = 0;
	}
}

void
fallback_init_wheel(struct fallback_dispatch *dispatch,
		    struct evdev_device *device)
{
	/* On kernel < 5.0 we need to emulate high-resolution
	   wheel scroll events */
	if ((libevdev_has_event_code(device->evdev,
				     EV_REL,
				     REL_WHEEL) &&
	     !libevdev_has_event_code(device->evdev,
				      EV_REL,
				      REL_WHEEL_HI_RES)) ||
	    (libevdev_has_event_code(device->evdev,
				     EV_REL,
				     REL_HWHEEL) &&
	     !libevdev_has_event_code(device->evdev,
				      EV_REL,
				      REL_HWHEEL_HI_RES)))
		dispatch->wheel.emulate_hi_res_wheel = true;
}
