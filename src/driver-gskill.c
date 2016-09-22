/*
 * Copyright © 2016 Red Hat, Inc.
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

#include "libratbag-private.h"
#include "libratbag-hidraw.h"

#include <errno.h>
#include <stdbool.h>

#define GSKILL_PROFILE_MAX  5
#define GSKILL_NUM_DPI      5
#define GSKILL_BUTTON_MAX  10

#define GSKILL_MAX_POLLING_RATE 1000

#define GSKILL_MIN_DPI   100
#define GSKILL_MAX_DPI  8200
#define GSKILL_DPI_UNIT   50

/* Commands */
#define GSKILL_GET_CURRENT_PROFILE_NUM 0x3
#define GSKILL_GET_SET_PROFILE         0x5
#define GSKILL_GENERAL_CMD             0xc

#define GSKILL_REPORT_SIZE_PROFILE 644
#define GSKILL_REPORT_SIZE_CMD       9

#define GSKILL_CHECKSUM_OFFSET 3

/* Command status codes */
#define GSKILL_CMD_SUCCESS     0xb0
#define GSKILL_CMD_IN_PROGRESS 0xb1
#define GSKILL_CMD_FAILURE     0xb2
#define GSKILL_CMD_IDLE        0xb3

/* LED groups. DPI is omitted here since it's handled specially */
#define GSKILL_LED_TYPE_LOGO  0
#define GSKILL_LED_TYPE_WHEEL 1
#define GSKILL_LED_TYPE_TAIL  2
#define GSKILL_LED_TYPE_COUNT 3

#define GSKILL_KBD_MOD_CTRL_LEFT   (1 << 0)
#define GSKILL_KBD_MOD_SHIFT_LEFT  (1 << 1)
#define GSKILL_KBD_MOD_ALT_LEFT    (1 << 2)
#define GSKILL_KBD_MOD_SUPER_LEFT  (1 << 3)
#define GSKILL_KBD_MOD_CTRL_RIGHT  (1 << 4)
#define GSKILL_KBD_MOD_SHIFT_RIGHT (1 << 5)
#define GSKILL_KBD_MOD_ALT_RIGHT   (1 << 6)
#define GSKILL_KBD_MOD_SUPER_RIGHT (1 << 7)

struct gskill_raw_dpi_level {
	uint8_t x;
	uint8_t y;
} __attribute__((packed));

struct gskill_led_color {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
} __attribute__((packed));

struct gskill_led_values {
	uint8_t brightness;
	struct gskill_led_color color;
} __attribute__((packed));

enum gskill_led_control_type {
	GSKILL_LED_ALL_OFF         = 0x0,
	GSKILL_LED_ALL_ON          = 0x1,
	GSKILL_LED_BREATHING       = 0x2,
	GSKILL_DPI_LED_RIGHT_CYCLE = 0x3,
	GSKILL_DPI_LED_LEFT_CYCLE  = 0x4
};

struct gskill_background_led_cfg {
	uint8_t brightness;
	struct gskill_led_color dpi[4];
	struct gskill_led_color leds[GSKILL_LED_TYPE_COUNT];
} __attribute__((packed));

struct gskill_dpi_led_group_cfg {
	uint8_t duration_step;
	uint8_t duration_high;
	uint8_t duration_low;
	uint8_t cycle_num;

	struct gskill_led_values steps[12];
};

struct gskill_led_group_cfg {
	enum gskill_led_control_type type :3;
	uint8_t                           :5; /* unused */

	uint8_t duration_step;
	uint8_t duration_high;
	uint8_t duration_low;
	uint8_t cycle_num;

	struct gskill_led_values steps[12];
} __attribute__((packed));

struct gskill_dpi_led_cycle_cfg {
	enum gskill_led_control_type type :3;
	uint8_t                           :5; /* unused */

	/* Don't worry, the low/high flip-flop here is intentional */
	uint8_t duration_low;
	uint8_t duration_high;
	uint8_t cycle_num;

	struct gskill_led_values cycles[12];
} __attribute__((packed));

/*
 * We may occasionally run into codes outside this, however those codes
 * indicate functionalities that aren't too useful for us
 */
enum gskill_button_function_type {
	GSKILL_BUTTON_FUNCTION_WHEEL                = 0x00,
	GSKILL_BUTTON_FUNCTION_MOUSE                = 0x01,
	GSKILL_BUTTON_FUNCTION_KBD                  = 0x02,
	GSKILL_BUTTON_FUNCTION_CONSUMER             = 0x03,
	GSKILL_BUTTON_FUNCTION_MACRO                = 0x06,
	GSKILL_BUTTON_FUNCTION_DPI_UP               = 0x09,
	GSKILL_BUTTON_FUNCTION_DPI_DOWN             = 0x0a,
	GSKILL_BUTTON_FUNCTION_CYCLE_DPI_UP         = 0x0b,
	GSKILL_BUTTON_FUNCTION_CYCLE_DPI_DOWN       = 0x0c,
	GSKILL_BUTTON_FUNCTION_PROFILE_SWITCH       = 0x0d,
	GSKILL_BUTTON_FUNCTION_TEMPORARY_CPI_ADJUST = 0x15,
	GSKILL_BUTTON_FUNCTION_DIRECT_DPI_CHANGE    = 0x16,
	GSKILL_BUTTON_FUNCTION_CYCLE_PROFILE_UP     = 0x18,
	GSKILL_BUTTON_FUNCTION_CYCLE_PROFILE_DOWN   = 0x19,
	GSKILL_BUTTON_FUNCTION_DISABLE              = 0xff
};

struct gskill_button_cfg {
	enum gskill_button_function_type type :8;
	union {
		struct {
			enum {
				GSKILL_WHEEL_SCROLL_UP = 0,
				GSKILL_WHEEL_SCROLL_DOWN = 1,
			} direction :8;
		} wheel;

		struct {
			enum {
				GSKILL_BTN_MASK_LEFT   = (1 << 0),
				GSKILL_BTN_MASK_RIGHT  = (1 << 1),
				GSKILL_BTN_MASK_MIDDLE = (1 << 2),
				GSKILL_BTN_MASK_SIDE   = (1 << 3),
				GSKILL_BTN_MASK_EXTRA  = (1 << 4)
			} button_mask :8;
		} mouse;

		struct {
			uint16_t code;
		} consumer;

		struct {
			uint8_t modifier_mask;
			uint8_t hid_code;
			/*
			 * XXX: Supposedly this is supposed to have additional
			 * parts of the kbd code, however that doesn't seem to
			 * be the case in practice…
			 */
			uint16_t :16;
		} kbd;

		struct {
			uint8_t level;
		} dpi;
	} params;
} __attribute__((packed));

struct gskill_profile_report {
	uint16_t                  :16;
	uint8_t profile_num;
	uint8_t checksum;
	uint8_t polling_rate      :4;
	uint8_t angle_snap_ratio  :4;
	uint8_t liftoff_value     :5;
	bool liftoff_enabled      :1;
	uint16_t                  :10; /* unused */

	uint8_t current_dpi_level :4;
	uint8_t dpi_num           :4;
	struct gskill_raw_dpi_level dpi_levels[GSKILL_NUM_DPI];

	/* LEDs */
	struct gskill_background_led_cfg background_lighting;
	struct gskill_dpi_led_cycle_cfg led_dpi_cycle;
	struct gskill_dpi_led_group_cfg dpi_led;
	struct gskill_led_group_cfg leds[GSKILL_LED_TYPE_COUNT];

	/* Button assignments */
	uint8_t button_function_redirections[8];
	struct gskill_button_cfg btn_cfgs[GSKILL_BUTTON_MAX];

	/* A mystery */
	uint8_t _unused1[27];

	uint16_t name[128];
} __attribute__((packed));

_Static_assert(sizeof(struct gskill_profile_report) == GSKILL_REPORT_SIZE_PROFILE,
	       "Size of gskill_profile_report isn't 644");

struct gskill_profile_data {
	struct gskill_profile_report report;
	uint8_t res_idx_to_dev_idx[GSKILL_NUM_DPI];
};

struct gskill_data {
	struct gskill_profile_data profile_data[GSKILL_PROFILE_MAX];
};

static uint8_t
gskill_calculate_checksum(const uint8_t *buf, size_t len)
{
	uint8_t checksum = 0;
	unsigned i;

	for (i = GSKILL_CHECKSUM_OFFSET + 1; i < len; i++)
		checksum += buf[i];

	checksum = ~checksum + 1;

	return checksum;
}

static int
gskill_general_cmd(struct ratbag_device *device,
		   uint8_t buf[GSKILL_REPORT_SIZE_CMD]) {
	int rc;
	int retries;

	rc = ratbag_hidraw_raw_request(device, GSKILL_GENERAL_CMD, buf,
				       GSKILL_REPORT_SIZE_CMD,
				       HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (rc < 0) {
		log_error(device->ratbag,
			  "Error while sending command to mouse: %d\n", rc);
		return rc;
	}

	for (retries = 0; retries < 10; retries++) {
		/* Wait for the device to be ready */
		msleep(20);

		rc = ratbag_hidraw_raw_request(device, 0, buf,
					       GSKILL_REPORT_SIZE_CMD,
					       HID_FEATURE_REPORT,
					       HID_REQ_GET_REPORT);
		/*
		 * Sometimes the mouse just doesn't send anything when it wants
		 * to tell us it's ready
		 */
		if (rc == 0) {
			continue;
		} else if (rc < GSKILL_REPORT_SIZE_CMD) {
			log_error(device->ratbag,
				  "Error while getting command response from mouse: %d\n",
				  rc);

			return rc;
		}

		/* Check the command status bit */
		switch (buf[1]) {
		case 0: /* sometimes the mouse gets lazy and just returns a
			   blank buffer on success */
		case GSKILL_CMD_SUCCESS:
			return 0;

		case GSKILL_CMD_IN_PROGRESS:
			break;

		case GSKILL_CMD_IDLE:
			log_error(device->ratbag,
				  "Command response indicates idle status? Uh huh.\n");
			return GSKILL_CMD_IDLE;

		case GSKILL_CMD_FAILURE:
			log_error(device->ratbag, "Command failed\n");
			return GSKILL_CMD_FAILURE;

		default:
			log_error(device->ratbag,
				  "Received unknown command status from mouse: 0x%x\n",
				  buf[1]);
			return -1;
		}
	}

	log_error(device->ratbag,
		  "Failed to get command response from mouse after %d tries, giving up\n",
		  retries);
	return -1;
}

static int
gskill_get_active_profile_idx(struct ratbag_device *device)
{
	uint8_t buf[GSKILL_REPORT_SIZE_CMD] = { 0x0c, 0xc4, 0x7, 0x0, 0x1 };
	int rc;

	rc = gskill_general_cmd(device, buf);
	if (rc) {
		log_error(device->ratbag,
			  "Error while getting active profile number from mouse: %d\n",
			  rc);
		return -1;
	}

	return buf[3];
}

static int
gskill_set_active_profile(struct ratbag_device *device, unsigned index)
{
	uint8_t buf[GSKILL_REPORT_SIZE_CMD] = { 0x0c, 0xc4, 0x7, index, 0x0 };
	int rc;

	rc = gskill_general_cmd(device, buf);
	if (rc) {
		log_error(device->ratbag,
			  "Error while changing active profile on mouse: %d\n",
			  rc);
		return -1;
	}

	return 0;
}

static int
gskill_get_profile_count(struct ratbag_device *device)
{
	uint8_t buf[GSKILL_REPORT_SIZE_CMD] = { 0x0c, 0xc4, 0x12, 0x0, 0x1 };
	int rc;

	rc = gskill_general_cmd(device, buf);
	if (rc) {
		log_error(device->ratbag,
			  "Error while getting the number of profiles: %d\n",
			  rc);
		return -1;
	}

	return buf[3];
}

static int
gskill_set_profile_count(struct ratbag_device *device, unsigned int count)
{
	uint8_t buf[GSKILL_REPORT_SIZE_CMD] = { 0x0c, 0xc4, 0x12, count, 0x0 };
	int rc;

	rc = gskill_general_cmd(device, buf);
	if (rc) {
		log_error(device->ratbag,
			  "Error while setting the number of profiles: %d\n",
			  rc);
		return -1;
	}

	return 0;
}

/*
 * This is used for setting the profile index argument on the mouse for both
 * reading and writing profiles
 */
static int
gskill_select_profile(struct ratbag_device *device, unsigned index, bool write)
{
	uint8_t buf[GSKILL_REPORT_SIZE_CMD] = { 0x0c, 0xc4, 0x0c, index, write };
	int rc;

	/* Indicate which profile we want to retrieve */
	rc = ratbag_hidraw_raw_request(device, GSKILL_GENERAL_CMD,
				       buf, sizeof(buf), HID_FEATURE_REPORT,
				       HID_REQ_SET_REPORT);
	if (rc < 0) {
		log_error(device->ratbag,
			  "Error while setting profile number to read/write: %d\n",
			  rc);
		return rc;
	}

	return 0;
}

/*
 * Instructs the mouse to reload the data from a profile we've just written to
 * it.
 */
static int
gskill_reload_profile_data(struct ratbag_device *device)
{
	uint8_t buf[GSKILL_REPORT_SIZE_CMD] = { 0x0c, 0xc4, 0x0 };
	int rc;

	log_debug(device->ratbag, "Asking mouse to reload profile data\n");

	rc = gskill_general_cmd(device, buf);
	if (rc < 0) {
		log_error(device->ratbag,
			  "Failed to get mouse to reload profile data: %d\n",
			  rc);
		return rc;
	}

	return 0;
}

static int
gskill_do_write_profile(struct ratbag_device *device,
			struct gskill_profile_report *report)
{
	uint8_t *buf = (uint8_t*)report;
	int rc;

	report->checksum = gskill_calculate_checksum(buf, sizeof(*report));

	rc = gskill_select_profile(device, report->profile_num, true);
	if (rc < 0)
		return rc;

	/* Wait for the device to be ready */
	msleep(200);

	rc = ratbag_hidraw_raw_request(device, GSKILL_GET_SET_PROFILE,
				       buf, sizeof(*report), HID_FEATURE_REPORT,
				       HID_REQ_SET_REPORT);
	if (rc < 0) {
		log_error(device->ratbag,
			  "Error while writing profile: %d\n", rc);
		return rc;
	}

	rc = gskill_reload_profile_data(device);
	if (rc)
		return rc;

	return 0;
}

static inline void
gskill_read_resolutions(struct ratbag_profile *profile,
			struct gskill_profile_report *raw)
{
	struct gskill_data *drv_data = ratbag_get_drv_data(profile->device);
	struct gskill_profile_data *pdata =
		&drv_data->profile_data[profile->index];
	struct ratbag_resolution *resolution;
	int dpi_x, dpi_y, hz, i;

	log_debug(profile->device->ratbag,
		  "Profile %d: DPI count is %d\n",
		  profile->index, raw->dpi_num);

	hz = GSKILL_MAX_POLLING_RATE / (raw->polling_rate + 1);

	for (i = 0; i < raw->dpi_num; i++) {
		dpi_x = raw->dpi_levels[i].x * GSKILL_DPI_UNIT;
		dpi_y = raw->dpi_levels[i].y * GSKILL_DPI_UNIT;

		resolution = ratbag_resolution_init(profile, i, dpi_x, dpi_y,
						    hz);
		resolution->is_active = (i == raw->current_dpi_level);
		pdata->res_idx_to_dev_idx[i] = i;

		ratbag_resolution_set_cap(resolution,
					  RATBAG_RESOLUTION_CAP_SEPARATE_XY_RESOLUTION);
	}
}

static int
gskill_get_firmware_version(struct ratbag_device *device) {
	uint8_t buf[GSKILL_REPORT_SIZE_CMD] = { 0x0c, 0xc4, 0x08 };
	int rc;

	rc = gskill_general_cmd(device, buf);
	if (rc < 0) {
		log_error(device->ratbag,
			  "Failed to read the firmware version of the mouse: %d\n",
			  rc);
		return rc;
	}

	return buf[4];
}

static int
gskill_probe(struct ratbag_device *device)
{
	struct gskill_data *drv_data = NULL;
	struct ratbag_profile *profile;
	unsigned int active_idx;
	int ret;

	ret = ratbag_open_hidraw(device);
	if (ret)
		return ret;

	drv_data = zalloc(sizeof(*drv_data));
	ratbag_set_drv_data(device, drv_data);

	ret = gskill_get_profile_count(device);
	if (ret < 0)
		goto err;

	/*
	 * TODO: Add proper support for enabling/disabling profiles in
	 * libratbag. For now we workaround this by just setting the profile
	 * count to 5
	 */
	if (ret < GSKILL_PROFILE_MAX) {
		log_info(device->ratbag,
			 "We don't support dynamically enabling/disabling profiles yet, sorry! Setting profile count of mouse to 5\n");
		ret = gskill_set_profile_count(device, GSKILL_PROFILE_MAX);
		if (ret < 0)
			goto err;
	}

	ret = gskill_get_firmware_version(device);
	if (ret < 0)
		goto err;

	log_debug(device->ratbag,
		 "Firmware version: %d\n", ret);

	ratbag_device_init_profiles(device, GSKILL_PROFILE_MAX, GSKILL_NUM_DPI,
				    GSKILL_BUTTON_MAX);

	ratbag_device_set_capability(device, RATBAG_DEVICE_CAP_QUERY_CONFIGURATION);
	ratbag_device_set_capability(device, RATBAG_DEVICE_CAP_RESET_PROFILE);

	ret = gskill_get_active_profile_idx(device);
	if (ret < 0)
		goto err;

	active_idx = ret;
	list_for_each(profile, &device->profiles, link) {
		if (profile->index == active_idx) {
			profile->is_active = true;
			break;
		}
	}

	return 0;

err:
	if (drv_data) {
		ratbag_set_drv_data(device, NULL);
		free(drv_data);
	}

	ratbag_close_hidraw(device);
	return ret;
}

static void
gskill_read_profile(struct ratbag_profile *profile, unsigned int index)
{
	struct ratbag_device *device = profile->device;
	struct gskill_data *drv_data = ratbag_get_drv_data(device);
	struct gskill_profile_data *pdata = &drv_data->profile_data[index];
	struct gskill_profile_report *report = &pdata->report;
	uint8_t checksum;
	int rc, retries;

	/*
	 * There's a couple of situations where after various commands, the
	 * mouse will get confused and send the wrong profile. Keep trying
	 * until we get what we want.
	 *
	 * As well, getting the wrong profile is sometimes a sign from the
	 * mouse we're doing something wrong.
	 */
	for (retries = 0; retries < 3; retries++) {
		rc = gskill_select_profile(device, index, false);
		if (rc < 0)
			return;

		/* Wait for the device to be ready */
		msleep(100);

		rc = ratbag_hidraw_raw_request(device, GSKILL_GET_SET_PROFILE,
					       (uint8_t*)report,
					       sizeof(*report),
					       HID_FEATURE_REPORT,
					       HID_REQ_GET_REPORT);
		if (rc < (signed)sizeof(*report)) {
			log_error(device->ratbag,
				  "Error while requesting profile: %d\n", rc);
			return;
		}

		if (report->profile_num == index)
			break;

		log_debug(device->ratbag,
			  "Mouse send wrong profile %d instead of %d, retrying...\n",
			  profile->index, index);
	}

	checksum = gskill_calculate_checksum((uint8_t*)report, sizeof(*report));
	if (checksum != report->checksum) {
		log_error(device->ratbag,
			  "Warning: profile %d invalid checksum (expected %x, got %x)\n",
			  profile->index, report->checksum, checksum);
	}

	gskill_read_resolutions(profile, report);
}

static int
gskill_write_resolution_dpi(struct ratbag_resolution *resolution,
			    int dpi_x, int dpi_y)
{
	struct ratbag_profile *profile = resolution->profile;
	struct ratbag_device *device = profile->device;
	struct gskill_data *drv_data = ratbag_get_drv_data(device);
	struct gskill_profile_data *pdata =
		&drv_data->profile_data[profile->index];
	struct gskill_profile_report *report = &pdata->report;
	unsigned int res_idx = resolution - profile->resolution.modes;
	int i, rc;

	if ((dpi_x && dpi_y) &&
	    (dpi_x < GSKILL_MIN_DPI || dpi_y < GSKILL_MIN_DPI ||
	     dpi_x > GSKILL_MAX_DPI || dpi_y > GSKILL_MAX_DPI ||
	     dpi_x % GSKILL_DPI_UNIT || dpi_y % GSKILL_DPI_UNIT))
		return -EINVAL;

	report->dpi_num = 0;
	memset(&report->dpi_levels, 0, sizeof(report->dpi_levels));
	memset(&pdata->res_idx_to_dev_idx, 0,
	       sizeof(pdata->res_idx_to_dev_idx));

	/*
	 * These mice start acting strange if we leave holes in the DPI levels.
	 * So only write and map the enabled DPIs, disabled DPIs will just be
	 * lost on exit
	 */
	for (i = 0; i < GSKILL_NUM_DPI; i++) {
		struct ratbag_resolution *res = &profile->resolution.modes[i];
		struct gskill_raw_dpi_level *level =
			&report->dpi_levels[report->dpi_num];

		if (!res->dpi_x || !res->dpi_y)
			continue;

		level->x = res->dpi_x / GSKILL_DPI_UNIT;
		level->y = res->dpi_y / GSKILL_DPI_UNIT;
		pdata->res_idx_to_dev_idx[i] = report->dpi_num;

		log_debug(device->ratbag, "Profile %d res %ld mapped to %d\n",
			  profile->index, res - profile->resolution.modes,
			  report->dpi_num);

		report->dpi_num++;
	}

	rc = gskill_do_write_profile(device, report);
	if (rc < 0)
		return rc;

	/*
	 * The active resolution is now going to be the first resolution on the
	 * device
	 */
	pdata->report.current_dpi_level = 0;
	for (i = 0; i < report->dpi_num; i++) {
		struct ratbag_resolution *res = &profile->resolution.modes[i];
		uint8_t dev_idx = pdata->res_idx_to_dev_idx[i];

		res->is_active = (dev_idx == 0);
	}

	log_debug(device->ratbag, "Profile %d resolution count set to %d\n",
		  profile->index, report->dpi_num);
	log_debug(device->ratbag, "Profile %d resolution %d set to %dx%dHz\n",
		  profile->index, res_idx, dpi_x, dpi_y);

	return 0;
}

static int
gskill_write_profile(struct ratbag_profile *profile)
{
	struct ratbag_device *device = profile->device;
	struct gskill_data *drv_data = ratbag_get_drv_data(device);
	struct gskill_profile_data *pdata =
		&drv_data->profile_data[profile->index];
	struct gskill_profile_report *report = &pdata->report;
	int rc;

	rc = gskill_do_write_profile(device, report);
	if (rc < 0)
		return rc;

	return 0;
}

static int
gskill_reset_profile(struct ratbag_profile *profile)
{
	struct ratbag_device *device = profile->device;
	uint8_t buf[GSKILL_REPORT_SIZE_CMD] = { 0x0c, 0xc4, 0x0a,
		profile->index };
	int rc;

	rc = gskill_general_cmd(device, buf);
	if (rc < 0)
		return rc;

	log_debug(device->ratbag, "reset profile %d to factory defaults\n",
		  profile->index);

	return 0;
}

static void
gskill_remove(struct ratbag_device *device)
{
	ratbag_close_hidraw(device);
	free(ratbag_get_drv_data(device));
}

struct ratbag_driver gskill_driver = {
	.name = "G.Skill Ripjaws MX780",
	.id = "gskill",
	.probe = gskill_probe,
	.remove = gskill_remove,
	.read_profile = gskill_read_profile,
	.write_profile = gskill_write_profile,
	.reset_profile = gskill_reset_profile,
	.set_active_profile = gskill_set_active_profile,
	.write_resolution_dpi = gskill_write_resolution_dpi,
};
