/*
 *   Lowlevel hardware access for the
 *   Razer Lachesis mouse
 *
 *   Important notice:
 *   This hardware driver is based on reverse engineering, only.
 *
 *   Copyright (C) 2008-2011 Michael Buesch <m@bues.ch>
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 */

#include "hw_lachesis.h"
#include "razer_private.h"
#include "buttonmapping.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>


enum lachesis_type {
	LACHESIS_CLASSIC,	/* Lachesis Classic */
	LACHESIS_5600,		/* Lachesis 5600DPI */
};

enum { /* LED IDs */
	LACHESIS_LED_SCROLL = 0,
	LACHESIS_LED_LOGO,
	LACHESIS_NR_LEDS,
};

enum { /* Misc constants */
	LACHESIS_NR_PROFILES		= 5,
	LACHESIS_NR_DPIMAPPINGS		= 5,
	LACHESIS_NR_AXES		= 3,
	LACHESIS_SERIAL_MAX_LEN		= 32,
	LACHESIS_PROFNAME_MAX_LEN	= 20,
};

/* The wire protocol data structures... */

enum lachesis_phys_button {
	/* Physical button IDs */
	LACHESIS_PHYSBUT_LEFT = 0x01,	/* Left button */
	LACHESIS_PHYSBUT_RIGHT,		/* Right button */
	LACHESIS_PHYSBUT_MIDDLE,	/* Middle button */
	LACHESIS_PHYSBUT_LFRONT,	/* Left side, front button */
	LACHESIS_PHYSBUT_LREAR,		/* Left side, rear button */
	LACHESIS_PHYSBUT_RFRONT,	/* Right side, front button */
	LACHESIS_PHYSBUT_RREAR,		/* Right side, rear button */
	LACHESIS_PHYSBUT_TFRONT,	/* Top side, front button */
	LACHESIS_PHYSBUT_TREAR,		/* Top side, rear button */
	LACHESIS_PHYSBUT_SCROLLUP,	/* Scroll wheel up */
	LACHESIS_PHYSBUT_SCROLLDWN,	/* Scroll wheel down */

	NR_LACHESIS_PHYSBUT = 11,	/* Number of physical buttons */
};

struct lachesis_profcfg_cmd {
	le16_t packetlength;
	le16_t magic;
	uint8_t profile;
	uint8_t _padding0;
	uint8_t dpisel;
	uint8_t freq;
	uint8_t _padding1;
	uint8_t buttonmap[35 * NR_LACHESIS_PHYSBUT];
	le16_t checksum;
} _packed;
#define LACHESIS_PROFCFG_MAGIC		cpu_to_le16(0x0002)

struct lachesis_one_dpimapping {
	uint8_t magic;
	uint8_t dpival0;
	uint8_t dpival1;
} _packed;
#define LACHESIS_DPIMAPPING_MAGIC	0x01

struct lachesis_dpimap_cmd {
	struct lachesis_one_dpimapping mappings[5];
	uint8_t _padding[81];
} _packed;

struct lachesis5k6_request {
	uint8_t magic;
	uint8_t flags;
	uint8_t rw;
	uint8_t command;
	uint8_t request;
	uint8_t _padding[3];
	uint8_t payload[80];
	le16_t checksum;
} _packed;

#define LACHESIS5K6_REQ_MAGIC		0x01
#define LACHESIS5K6_REQ_FLG_TRANSOK	0x02
#define LACHESIS5K6_REQ_READ		0x01
#define LACHESIS5K6_REQ_WRITE		0x00

struct lachesis5k6_request_devinfo {
	uint8_t serial[LACHESIS_SERIAL_MAX_LEN];
	uint8_t fwver[2];
} _packed;

struct lachesis5k6_request_globconfig {
	uint8_t profile;
	uint8_t freq;
	uint8_t dpisel;
	uint8_t dpival0;
	uint8_t dpival1;
} _packed;

struct lachesis5k6_request_profname {
	uint8_t profile;
	union {
		uint8_t name[LACHESIS_PROFNAME_MAX_LEN * 2]; /* UTF-16-LE */
		le16_t name_le16[LACHESIS_PROFNAME_MAX_LEN];
	} _packed;
} _packed;

struct lachesis5k6_one_dpimapping {
	uint8_t dpival0;
	uint8_t dpival1;
} _packed;

struct lachesis5k6_led_color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t padding;
} _packed;

#define LACHESIS5K6_LED_COLOR_PADDING	0xFF

struct lachesis5k6_request_hwconfig {
	uint8_t profile;
	uint8_t leds;
	uint8_t dpisel;
	uint8_t nr_dpimappings;
	struct lachesis5k6_one_dpimapping dpimappings[LACHESIS_NR_DPIMAPPINGS];
	uint8_t _padding[6];
	uint8_t buttonmap[4 * NR_LACHESIS_PHYSBUT];
	struct lachesis5k6_led_color scroll_color;
	struct lachesis5k6_led_color logo_color;
} _packed;

struct lachesis_buttons {
	struct razer_buttonmapping mapping[NR_LACHESIS_PHYSBUT];
};

struct lachesis_prof_name {
	razer_utf16_t name[LACHESIS_PROFNAME_MAX_LEN + 1];
};

/* Context data structure */
struct lachesis_private {
	struct razer_mouse *m;

	/* The lachesis hardware revision type */
	enum lachesis_type type;

	uint16_t fw_version;
	char serial[LACHESIS_SERIAL_MAX_LEN + 1];

	/* The currently set LED states.
	 * For Classic, only profile 0 is valid. */
	enum razer_led_state led_states[LACHESIS_NR_PROFILES][LACHESIS_NR_LEDS];
	/* LED colors (5600 only) */
	struct razer_rgb_color led_colors[LACHESIS_NR_PROFILES][LACHESIS_NR_LEDS];

	/* The active profile. */
	struct razer_mouse_profile *cur_profile;
	/* Profile configuration (one per profile). */
	struct razer_mouse_profile profiles[LACHESIS_NR_PROFILES];
	/* Profile names (5600 only) */
	struct lachesis_prof_name profile_names[LACHESIS_NR_PROFILES];

	/* Supported mouse axes */
	struct razer_axis axes[LACHESIS_NR_AXES];

	/* The active DPI mapping; per profile. */
	struct razer_mouse_dpimapping *cur_dpimapping[LACHESIS_NR_PROFILES];
	/* The possible DPI mappings. For the Lachesis Classic, only
	 * the first profile is used. */
	struct razer_mouse_dpimapping dpimappings[LACHESIS_NR_PROFILES][LACHESIS_NR_DPIMAPPINGS];

	/* The active scan frequency. Per profile on Classic.
	 * On the 5600, only the first entry is used. */
	enum razer_mouse_freq cur_freq[LACHESIS_NR_PROFILES];

	/* The active button mapping; per profile. */
	struct lachesis_buttons buttons[LACHESIS_NR_PROFILES];

	bool commit_pending;
};


/* A list of physical buttons on the device. */
static struct razer_button lachesis_physical_buttons[] = {
	{ .id = LACHESIS_PHYSBUT_LEFT,		.name = "Leftclick",		},
	{ .id = LACHESIS_PHYSBUT_RIGHT,		.name = "Rightclick",		},
	{ .id = LACHESIS_PHYSBUT_MIDDLE,	.name = "Middleclick",		},
	{ .id = LACHESIS_PHYSBUT_LFRONT,	.name = "Leftside front",	},
	{ .id = LACHESIS_PHYSBUT_LREAR,		.name = "Leftside rear",	},
	{ .id = LACHESIS_PHYSBUT_RFRONT,	.name = "Rightside front",	},
	{ .id = LACHESIS_PHYSBUT_RREAR,		.name = "Rightside rear",	},
	{ .id = LACHESIS_PHYSBUT_TFRONT,	.name = "Top front",		},
	{ .id = LACHESIS_PHYSBUT_TREAR,		.name = "Top rear",		},
	{ .id = LACHESIS_PHYSBUT_SCROLLUP,	.name = "Scroll up",		},
	{ .id = LACHESIS_PHYSBUT_SCROLLDWN,	.name = "Scroll down",		},
};

/* A list of possible button functions. */
static struct razer_button_function lachesis_button_functions[] = {
	BUTTONFUNC_LEFT,
	BUTTONFUNC_RIGHT,
	BUTTONFUNC_MIDDLE,
	BUTTONFUNC_PROFDOWN,
	BUTTONFUNC_PROFUP,
	BUTTONFUNC_DPIUP,
	BUTTONFUNC_DPIDOWN,
	BUTTONFUNC_DPI1,
	BUTTONFUNC_DPI2,
	BUTTONFUNC_DPI3,
	BUTTONFUNC_DPI4,
	BUTTONFUNC_DPI5,
	BUTTONFUNC_WIN5,
	BUTTONFUNC_WIN4,
	BUTTONFUNC_SCROLLUP,
	BUTTONFUNC_SCROLLDWN,
};

/*XXX: lachesis-CLASSIC notes
 *
 *	read commands:
 *
 *	CLEAR_FEATURE
 *		0x10:	Read DPI map
 *		0x06:	read fw version
 *		0x05:	LEDs
 *		0x09:	cur profile
 *		0x03:	profile config
 *		0x02:	?busy flag?
 *
 *	write commands:
 *	SET_CONFIG
 *		0x0F:	? Data=0x01. Executed on startup
 *
 * TODO: react to profile/dpi/whatever changes via hw buttons. need to poll?
 */

/*XXX: lachesis-5600 notes
 *
 *	get global config:	0x05 / 0x01 (or 0x00)
 *	set global config:	0x05 / 0x05
 *	get prof name:		0x22 / 0x01 / payload[0]=profnr
 *	set prof name:		0x22 / 0x29 / payload = profnr + utf16le
 *	get hwconfig:		0x06 / 0x01 / payload[0]=profnr
 *	set hwconfig:		0x06 / 0x48 / payload = profnr + confdata
 *
 *	LEDs bit0 -> scroll
 *	LEDs bit1 -> logo
 *	LEDs bit2 -> always 1?
*/

static le16_t lachesis5k6_checksum(const struct lachesis5k6_request *req)
{
	uint16_t checksum;

	checksum = razer_xor8_checksum((uint8_t *)req + 2,
				       sizeof(*req) - 2);
	if (!(req->flags & LACHESIS5K6_REQ_FLG_TRANSOK))
		checksum |= 0x100;

	return cpu_to_le16(checksum);
}

static int lachesis_usb_write(struct lachesis_private *priv,
			      int request, int command, int index,
			      void *buf, size_t size)
{
	int err;

	err = libusb_control_transfer(
		priv->m->usb_ctx->h,
		LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS |
		LIBUSB_RECIPIENT_INTERFACE,
		request, command, index,
		buf, size,
		RAZER_USB_TIMEOUT);
	if (err != size) {
		razer_error("hw_lachesis: usb_write failed\n");
		return -EIO;
	}

	return 0;
}

static int lachesis5k6_request_send(struct lachesis_private *priv,
				    const struct lachesis5k6_request *_req)
{
	struct lachesis5k6_request req = *_req;

	req.magic = LACHESIS5K6_REQ_MAGIC;
	req.checksum = lachesis5k6_checksum(&req);
//	razer_dump("WR", &req, sizeof(req));

	return lachesis_usb_write(priv, LIBUSB_REQUEST_SET_CONFIGURATION,
				  0x300, 0, &req, sizeof(req));
}

static int lachesis_usb_read(struct lachesis_private *priv,
			     int request, int command, int index,
			     void *buf, size_t size)
{
	int err;

	err = libusb_control_transfer(
		priv->m->usb_ctx->h,
		LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS |
		LIBUSB_RECIPIENT_INTERFACE,
		request, command, index,
		buf, size,
		RAZER_USB_TIMEOUT);
	if (err != size) {
		razer_error("hw_lachesis: usb_read failed\n");
		return -EIO;
	}

	return 0;
}

static int lachesis5k6_request_receive(struct lachesis_private *priv,
				       struct lachesis5k6_request *req,
				       bool do_checksum)
{
	le16_t checksum;
	int err;

	memset(req, 0, sizeof(*req));
	err = lachesis_usb_read(priv, LIBUSB_REQUEST_CLEAR_FEATURE,
				0x300, 0, req, sizeof(*req));
	if (err)
		return err;
//	razer_dump("RD", req, sizeof(*req));
	if (do_checksum) {
		checksum = lachesis5k6_checksum(req);
		if (req->checksum != checksum) {
			razer_error("hw_lachesis: Received request with invalid "
				    "checksum (was 0x%04X, expected 0x%04X)\n",
				    le16_to_cpu(req->checksum),
				    le16_to_cpu(checksum));
			return -EIO;
		}
	}

	return 0;
}

static int lachesis5k6_request_write(struct lachesis_private *priv,
				     uint8_t command, uint8_t request,
				     const void *payload, size_t payload_len)
{
	struct lachesis5k6_request req, nullreq;
	int err;

	if (WARN_ON(payload_len > sizeof(req.payload)))
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.rw = LACHESIS5K6_REQ_WRITE;
	req.command = command;
	req.request = request;
	if (payload)
		memcpy(req.payload, payload, payload_len);
	err = lachesis5k6_request_send(priv, &req);
	if (err)
		return err;
	err = lachesis5k6_request_receive(priv, &req, 0);
	if (err)
		return err;
	memset(&nullreq, 0, sizeof(nullreq));
	err = lachesis5k6_request_send(priv, &nullreq);
	if (err)
		return err;

	if (req.magic != LACHESIS5K6_REQ_MAGIC) {
		razer_error("hw_lachesis: Invalid magic on sent request\n");
		return -EIO;
	}
	if (req.rw != LACHESIS5K6_REQ_WRITE) {
		razer_error("hw_lachesis: Invalid rw flag on sent request\n");
		return -EIO;
	}
	if (req.command != command || req.request != request) {
		razer_error("hw_lachesis: Invalid command on sent request\n");
		return -EIO;
	}

	return 0;
}

static int lachesis5k6_request_read(struct lachesis_private *priv,
				    uint8_t command, uint8_t request,
				    void *payload, size_t payload_len)
{
	struct lachesis5k6_request req, nullreq;
	int err;

	if (WARN_ON(payload_len > sizeof(req.payload)))
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.rw = LACHESIS5K6_REQ_READ;
	req.command = command;
	req.request = request;
	if (payload)
		memcpy(req.payload, payload, payload_len);
	err = lachesis5k6_request_send(priv, &req);
	if (err)
		return err;
	err = lachesis5k6_request_receive(priv, &req, 0);
	if (err)
		return err;
	memset(&nullreq, 0, sizeof(nullreq));
	err = lachesis5k6_request_send(priv, &nullreq);
	if (err)
		return err;
	if (payload)
		memcpy(payload, req.payload, payload_len);

	if (req.magic != LACHESIS5K6_REQ_MAGIC) {
		razer_error("hw_lachesis: Invalid magic on received request\n");
		return -EIO;
	}
	if (!(req.flags & LACHESIS5K6_REQ_FLG_TRANSOK)) {
		razer_error("hw_lachesis: Failed to receive request. (TRANSOK flag)\n");
		return -EIO;
	}
	if (req.rw != LACHESIS5K6_REQ_READ) {
		razer_error("hw_lachesis: Invalid rw flag on received request\n");
		return -EIO;
	}
	if (req.command != command) {
		razer_error("hw_lachesis: Invalid command on received request\n");
		return -EIO;
	}

	return 0;
}

static int lachesis_read_devinfo(struct lachesis_private *priv)
{
	uint8_t buf[2];
	int err;
	struct lachesis5k6_request_devinfo devinfo;

	priv->fw_version = 0;
	memset(priv->serial, 0, sizeof(priv->serial));

	switch (priv->type) {
	case LACHESIS_CLASSIC:
		err = lachesis_usb_read(priv, LIBUSB_REQUEST_CLEAR_FEATURE,
					0x06, 0, buf, sizeof(buf));
		if (err)
			return -EIO;
		priv->fw_version = ((uint16_t)(buf[0]) << 8) | buf[1];
		break;
	case LACHESIS_5600:
		memset(&devinfo, 0, sizeof(devinfo));
		err = lachesis5k6_request_read(priv, 2, 1,
					       &devinfo, sizeof(devinfo));
		if (err)
			return -EIO;
		priv->fw_version = ((uint16_t)(devinfo.fwver[0]) << 8) |
				   devinfo.fwver[1];
		memcpy(priv->serial, devinfo.serial, LACHESIS_SERIAL_MAX_LEN);
		break;
	}

	return 0;
}

static int lachesis_commit_classic(struct lachesis_private *priv)
{
	unsigned int i;
	int err;
	char value, status;
	struct lachesis_profcfg_cmd profcfg;
	struct lachesis_dpimap_cmd dpimap;

	/* Commit the profile configuration. */
	for (i = 0; i < LACHESIS_NR_PROFILES; i++) {
		memset(&profcfg, 0, sizeof(profcfg));
		profcfg.packetlength = cpu_to_le16(sizeof(profcfg));
		profcfg.magic = LACHESIS_PROFCFG_MAGIC;
		profcfg.profile = i + 1;
		profcfg.dpisel = (priv->cur_dpimapping[i]->nr % 10) + 1;
		switch (priv->cur_freq[i]) {
		default:
		case RAZER_MOUSE_FREQ_1000HZ:
			profcfg.freq = 1;
			break;
		case RAZER_MOUSE_FREQ_500HZ:
			profcfg.freq = 2;
			break;
		case RAZER_MOUSE_FREQ_125HZ:
			profcfg.freq = 3;
			break;
		}
		err = razer_create_buttonmap(profcfg.buttonmap, sizeof(profcfg.buttonmap),
					     priv->buttons[i].mapping,
					     ARRAY_SIZE(priv->buttons[i].mapping), 33);
		if (err)
			return err;
		profcfg.checksum = razer_xor16_checksum(&profcfg,
				sizeof(profcfg) - sizeof(profcfg.checksum));
		err = lachesis_usb_write(priv, LIBUSB_REQUEST_SET_CONFIGURATION,
					 0x01, 0, &profcfg, sizeof(profcfg));
		if (err)
			return err;
		err = lachesis_usb_read(priv, LIBUSB_REQUEST_CLEAR_FEATURE,
					0x02, 0, &status, sizeof(status));
		if (err || status != 1) {
			razer_error("hw_lachesis: Failed to commit profile\n");
			return err;
		}
	}

	/* Commit LED states. */
	value = 0;
	if (priv->led_states[0][LACHESIS_LED_LOGO])
		value |= 0x01;
	if (priv->led_states[0][LACHESIS_LED_SCROLL])
		value |= 0x02;
	err = lachesis_usb_write(priv, LIBUSB_REQUEST_SET_CONFIGURATION,
				 0x04, 0, &value, sizeof(value));
	if (err)
		return err;

	/* Commit the active profile selection. */
	value = priv->cur_profile->nr + 1;
	err = lachesis_usb_write(priv, LIBUSB_REQUEST_SET_CONFIGURATION,
				 0x08, 0, &value, sizeof(value));
	if (err)
		return err;

	/* Commit the DPI map. */
	memset(&dpimap, 0, sizeof(dpimap));
	for (i = 0; i < LACHESIS_NR_DPIMAPPINGS; i++) {
		dpimap.mappings[i].magic = LACHESIS_DPIMAPPING_MAGIC;
		dpimap.mappings[i].dpival0 = (priv->dpimappings[0][i].res[RAZER_DIM_0] / 125) - 1;
		dpimap.mappings[i].dpival1 = dpimap.mappings[i].dpival0;
	}
	err = lachesis_usb_write(priv, LIBUSB_REQUEST_SET_CONFIGURATION,
				 0x12, 0, &dpimap, sizeof(dpimap));
	if (err)
		return err;

	return 0;
}

static int lachesis_commit_5600(struct lachesis_private *priv)
{
	struct lachesis5k6_request_profname profname;
	struct lachesis5k6_request_globconfig globconfig;
	struct lachesis5k6_request_hwconfig hwconfig;
	int err;
	unsigned int i, j;

	/* Commit profile configs */
	for (i = 0; i < LACHESIS_NR_PROFILES; i++) {
		memset(&hwconfig, 0, sizeof(hwconfig));
		hwconfig.profile = i + 1;
		hwconfig.leds = 0x04;
		if (priv->led_states[i][LACHESIS_LED_SCROLL])
			hwconfig.leds |= 0x01;
		if (priv->led_states[i][LACHESIS_LED_LOGO])
			hwconfig.leds |= 0x02;
		hwconfig.dpisel = (priv->cur_dpimapping[i]->nr % 10) + 1;
		hwconfig.nr_dpimappings = LACHESIS_NR_DPIMAPPINGS;
		for (j = 0; j < LACHESIS_NR_DPIMAPPINGS; j++) {
			hwconfig.dpimappings[j].dpival0 = ((priv->dpimappings[i][j].res[RAZER_DIM_X] / 100) - 1) * 4;
			hwconfig.dpimappings[j].dpival1 = ((priv->dpimappings[i][j].res[RAZER_DIM_Y] / 100) - 1) * 4;
		}
		err = razer_create_buttonmap(hwconfig.buttonmap, sizeof(hwconfig.buttonmap),
					     priv->buttons[i].mapping,
					     ARRAY_SIZE(priv->buttons[i].mapping), 2);
		if (err)
			return err;
		hwconfig.scroll_color.padding = LACHESIS5K6_LED_COLOR_PADDING;
		hwconfig.scroll_color.r = priv->led_colors[i][LACHESIS_LED_SCROLL].r;
		hwconfig.scroll_color.g = priv->led_colors[i][LACHESIS_LED_SCROLL].g;
		hwconfig.scroll_color.b = priv->led_colors[i][LACHESIS_LED_SCROLL].b;
		hwconfig.logo_color.padding = LACHESIS5K6_LED_COLOR_PADDING;
		hwconfig.logo_color.r = priv->led_colors[i][LACHESIS_LED_LOGO].r;
		hwconfig.logo_color.g = priv->led_colors[i][LACHESIS_LED_LOGO].g;
		hwconfig.logo_color.b = priv->led_colors[i][LACHESIS_LED_LOGO].b;
		err = lachesis5k6_request_write(priv, 6, 0x48,
						&hwconfig, sizeof(hwconfig));
		if (err)
			return err;
	}

	/* Commit profile names */
	for (i = 0; i < LACHESIS_NR_PROFILES; i++) {
		memset(&profname, 0, sizeof(profname));
		profname.profile = i + 1;
		for (j = 0; j < LACHESIS_PROFNAME_MAX_LEN; j++) {
			le16_t c = cpu_to_le16(priv->profile_names[i].name[j]);
			profname.name_le16[j] = c;
		}
		err = lachesis5k6_request_write(priv, 0x22, 0x29,
						&profname, sizeof(profname));
		if (err)
			return err;
	}

	/* Commit global config */
	memset(&globconfig, 0, sizeof(globconfig));
	globconfig.profile = priv->cur_profile->nr + 1;
	switch (priv->cur_freq[0]) {
	default:
	case RAZER_MOUSE_FREQ_1000HZ:
		globconfig.freq = 1;
		break;
	case RAZER_MOUSE_FREQ_500HZ:
		globconfig.freq = 2;
		break;
	case RAZER_MOUSE_FREQ_125HZ:
		globconfig.freq = 8;
		break;
	}
	globconfig.dpisel = (priv->cur_dpimapping[priv->cur_profile->nr]->nr % 10) + 1;
	globconfig.dpival0 = ((priv->cur_dpimapping[priv->cur_profile->nr]->res[RAZER_DIM_X] / 100) - 1) * 4;
	globconfig.dpival1 = ((priv->cur_dpimapping[priv->cur_profile->nr]->res[RAZER_DIM_Y] / 100) - 1) * 4;
	err = lachesis5k6_request_write(priv, 5, 5,
					&globconfig, sizeof(globconfig));
	if (err)
		return err;

	return 0;
}

static int lachesis_do_commit(struct lachesis_private *priv)
{
	switch (priv->type) {
	case LACHESIS_CLASSIC:
		return lachesis_commit_classic(priv);
	case LACHESIS_5600:
		return lachesis_commit_5600(priv);
	}

	return -ENODEV;
}

static int lachesis_read_config_classic(struct lachesis_private *priv)
{
	int err;
	unsigned char value;
	unsigned int i;
	struct lachesis_dpimap_cmd dpimap;
	struct lachesis_profcfg_cmd profcfg;

	value = 0x01;
	err = lachesis_usb_write(priv, LIBUSB_REQUEST_SET_CONFIGURATION,
				 0x0F, 0, &value, sizeof(value));
	if (err)
		return err;

	/* Get the current profile number */
	err = lachesis_usb_read(priv, LIBUSB_REQUEST_CLEAR_FEATURE,
				0x09, 0, &value, sizeof(value));
	if (err)
		return err;
	if (value < 1 || value > LACHESIS_NR_PROFILES) {
		razer_error("hw_lachesis: Got invalid profile number\n");
		return -EIO;
	}
	priv->cur_profile = &priv->profiles[value - 1];

	/* Get the profile configuration */
	for (i = 0; i < LACHESIS_NR_PROFILES; i++) {
		/* Change to the profile */
		value = i + 1;
		err = lachesis_usb_write(priv, LIBUSB_REQUEST_SET_CONFIGURATION,
					 0x08, 0, &value, sizeof(value));
		if (err)
			return err;
		/* And read the profile config */
		err = lachesis_usb_read(priv, LIBUSB_REQUEST_CLEAR_FEATURE,
					0x03, 1, &profcfg, sizeof(profcfg));
		if (err)
			return err;
		if (profcfg.dpisel < 1 || profcfg.dpisel > LACHESIS_NR_DPIMAPPINGS) {
			razer_error("hw_lachesis: Got invalid DPI selection\n");
			return -EIO;
		}
		razer_debug("hw_lachesis: Got profile config %d "
			"(magic 0x%04X, prof %u, freq %u, dpisel %u)\n",
			i + 1, profcfg.magic, profcfg.profile, profcfg.freq, profcfg.dpisel);
		priv->cur_dpimapping[i] = &priv->dpimappings[0][profcfg.dpisel - 1];
		switch (profcfg.freq) {
		case 1:
			priv->cur_freq[i] = RAZER_MOUSE_FREQ_1000HZ;
			break;
		case 2:
			priv->cur_freq[i] = RAZER_MOUSE_FREQ_500HZ;
			break;
		case 3:
			priv->cur_freq[i] = RAZER_MOUSE_FREQ_125HZ;
			break;
		default:
			razer_error("hw_lachesis: "
				"Read invalid frequency value from device (%u)\n",
				profcfg.freq);
			return -EINVAL;
		}
		err = razer_parse_buttonmap(profcfg.buttonmap, sizeof(profcfg.buttonmap),
					    priv->buttons[i].mapping,
					    ARRAY_SIZE(priv->buttons[i].mapping), 33);
		if (err)
			return err;
	}
	/* Select original profile */
	value = priv->cur_profile->nr + 1;
	err = lachesis_usb_write(priv, LIBUSB_REQUEST_SET_CONFIGURATION,
				 0x08, 0, &value, sizeof(value));
	if (err)
		return err;

	/* Get the LED states */
	err = lachesis_usb_read(priv, LIBUSB_REQUEST_CLEAR_FEATURE,
				0x05, 0, &value, sizeof(value));
	if (err)
		return err;
	priv->led_states[0][LACHESIS_LED_LOGO] = !!(value & 0x01);
	priv->led_states[0][LACHESIS_LED_SCROLL] = !!(value & 0x02);

	/* Get the DPI map */
	err = lachesis_usb_read(priv, LIBUSB_REQUEST_CLEAR_FEATURE,
				0x10, 0, &dpimap, sizeof(dpimap));
	if (err)
		return err;
	for (i = 0; i < LACHESIS_NR_DPIMAPPINGS; i++)
		priv->dpimappings[0][i].res[RAZER_DIM_0] = (dpimap.mappings[i].dpival0 + 1) * 125;

	return 0;
}

static int lachesis_read_config_5600(struct lachesis_private *priv)
{
	unsigned int i, j;
	int err;
	struct lachesis5k6_request_profname profname;
	struct lachesis5k6_request_globconfig globconfig;
	struct lachesis5k6_request_hwconfig hwconfig;
	enum razer_mouse_res res_x, res_y;

	/* Get global config */
	memset(&globconfig, 0, sizeof(globconfig));
	err = lachesis5k6_request_read(priv, 5, 1,
				       &globconfig, sizeof(globconfig));
	if (err)
		return err;
	if (globconfig.profile < 1 || globconfig.profile > LACHESIS_NR_PROFILES) {
		razer_error("hw_lachesis: Got invalid profile number\n");
		return -EIO;
	}
	priv->cur_profile = &priv->profiles[globconfig.profile - 1];
	switch (globconfig.freq) {
	case 1:
		priv->cur_freq[0] = RAZER_MOUSE_FREQ_1000HZ;
		break;
	case 2:
		priv->cur_freq[0] = RAZER_MOUSE_FREQ_500HZ;
		break;
	case 8:
		priv->cur_freq[0] = RAZER_MOUSE_FREQ_125HZ;
		break;
	default:
		razer_error("hw_lachesis: "
			"Read invalid frequency value from device (%u)\n",
			globconfig.freq);
		return -EIO;
	}

	/* Get the profile names */
	for (i = 0; i < LACHESIS_NR_PROFILES; i++) {
		memset(&profname, 0, sizeof(profname));
		profname.profile = i + 1;
		err = lachesis5k6_request_read(priv, 0x22, 1,
					       &profname, sizeof(profname));
		if (err)
			return err;
		memset(&priv->profile_names[i], 0, sizeof(priv->profile_names[i]));
		for (j = 0; j < LACHESIS_PROFNAME_MAX_LEN; j++) {
			priv->profile_names[i].name[j] = profname.name[j * 2 + 0];
			priv->profile_names[i].name[j] |= (uint16_t)profname.name[j * 2 + 1] << 8;
		}
	}

	/* Get the profile configs */
	for (i = 0; i < LACHESIS_NR_PROFILES; i++) {
		memset(&hwconfig, 0, sizeof(hwconfig));
		hwconfig.profile = i + 1;
		err = lachesis5k6_request_read(priv, 6, 1,
					       &hwconfig, sizeof(hwconfig));
		if (err)
			return err;
		if (hwconfig.profile != i + 1) {
			razer_error("hw_lachesis: Failed to read hw config\n");
			return -EIO;
		}
		priv->led_states[i][LACHESIS_LED_SCROLL] = !!(hwconfig.leds & 0x01);
		priv->led_states[i][LACHESIS_LED_LOGO] = !!(hwconfig.leds & 0x02);
		if (hwconfig.dpisel < 1 || hwconfig.dpisel > LACHESIS_NR_DPIMAPPINGS ||
		    hwconfig.dpisel > hwconfig.nr_dpimappings) {
			razer_error("hw_lachesis: Got invalid DPI selection: %u\n",
				    hwconfig.dpisel);
			return -EIO;
		}
		priv->cur_dpimapping[i] = &priv->dpimappings[i][hwconfig.dpisel - 1];

		if (hwconfig.nr_dpimappings < 1 ||
		    hwconfig.nr_dpimappings > LACHESIS_NR_DPIMAPPINGS) {
			razer_error("hw_lachesis: Got invalid nr_dpimappings: %u\n",
				    hwconfig.nr_dpimappings);
			return -EIO;
		}
		for (j = 0; j < LACHESIS_NR_DPIMAPPINGS; j++) {
			if (j + 1 > hwconfig.nr_dpimappings) {
				res_x = RAZER_MOUSE_RES_5600DPI;
				res_y = res_x;
			} else {
				res_x = ((hwconfig.dpimappings[j].dpival0 / 4) + 1) * 100;
				res_y = ((hwconfig.dpimappings[j].dpival1 / 4) + 1) * 100;
			}
			priv->dpimappings[i][j].res[RAZER_DIM_X] = res_x;
			priv->dpimappings[i][j].res[RAZER_DIM_Y] = res_y;
		}
		err = razer_parse_buttonmap(hwconfig.buttonmap, sizeof(hwconfig.buttonmap),
					    priv->buttons[i].mapping,
					    ARRAY_SIZE(priv->buttons[i].mapping), 2);
		if (err)
			return err;
		priv->led_colors[i][LACHESIS_LED_SCROLL].r = hwconfig.scroll_color.r;
		priv->led_colors[i][LACHESIS_LED_SCROLL].g = hwconfig.scroll_color.g;
		priv->led_colors[i][LACHESIS_LED_SCROLL].b = hwconfig.scroll_color.b;
		priv->led_colors[i][LACHESIS_LED_SCROLL].valid = 1;
		priv->led_colors[i][LACHESIS_LED_LOGO].r = hwconfig.logo_color.r;
		priv->led_colors[i][LACHESIS_LED_LOGO].g = hwconfig.logo_color.g;
		priv->led_colors[i][LACHESIS_LED_LOGO].b = hwconfig.logo_color.b;
		priv->led_colors[i][LACHESIS_LED_LOGO].valid = 1;
	}

	return 0;
}

static int lachesis_read_config_from_hw(struct lachesis_private *priv)
{
	switch (priv->type) {
	case LACHESIS_CLASSIC:
		return lachesis_read_config_classic(priv);
	case LACHESIS_5600:
		return lachesis_read_config_5600(priv);
	}

	return -ENODEV;
}

static int lachesis_get_fw_version(struct razer_mouse *m)
{
	struct lachesis_private *priv = m->drv_data;

	return priv->fw_version;
}

static int lachesis_commit(struct razer_mouse *m, int force)
{
	struct lachesis_private *priv = m->drv_data;
	int err = 0;

	if (!m->claim_count)
		return -EBUSY;
	if (priv->commit_pending || force) {
		err = lachesis_do_commit(priv);
		if (!err)
			priv->commit_pending = 0;
	}

	return err;
}

static const razer_utf16_t * lachesis_profile_get_name(struct razer_mouse_profile *p)
{
	struct razer_mouse *m = p->mouse;
	struct lachesis_private *priv = m->drv_data;

	if (p->nr >= LACHESIS_NR_PROFILES)
		return NULL;

	return priv->profile_names[p->nr].name;
}

static int lachesis_profile_set_name(struct razer_mouse_profile *p,
				     const razer_utf16_t *new_name)
{
	struct razer_mouse *m = p->mouse;
	struct lachesis_private *priv = m->drv_data;
	int err;

	if (p->nr >= LACHESIS_NR_PROFILES)
		return -EINVAL;

	if (!m->claim_count)
		return -EBUSY;

	err = razer_utf16_cpy(priv->profile_names[p->nr].name,
			      new_name, LACHESIS_PROFNAME_MAX_LEN);
	priv->commit_pending = 1;

	return err;
}

static int lachesis_led_toggle(struct razer_led *led,
			       enum razer_led_state new_state)
{
	struct razer_mouse_profile *p = led->u.mouse_prof;
	struct razer_mouse *m = p->mouse;
	struct lachesis_private *priv = m->drv_data;

	if (led->id >= LACHESIS_NR_LEDS)
		return -EINVAL;
	if ((new_state != RAZER_LED_OFF) &&
	    (new_state != RAZER_LED_ON))
		return -EINVAL;
	if (priv->type == LACHESIS_CLASSIC &&
	    p->nr != 0)
		return -EINVAL;
	if (p->nr >= LACHESIS_NR_PROFILES)
		return -EINVAL;

	if (!priv->m->claim_count)
		return -EBUSY;

	priv->led_states[p->nr][led->id] = new_state;
	priv->commit_pending = 1;

	return 0;
}

static int lachesis_led_change_color(struct razer_led *led,
				     const struct razer_rgb_color *new_color)
{
	struct razer_mouse_profile *p = led->u.mouse_prof;
	struct razer_mouse *m = p->mouse;
	struct lachesis_private *priv = m->drv_data;

	if (WARN_ON(priv->type == LACHESIS_CLASSIC))
		return -ENODEV;
	if (led->id >= LACHESIS_NR_LEDS)
		return -EINVAL;
	if (p->nr >= LACHESIS_NR_PROFILES)
		return -EINVAL;

	if (!priv->m->claim_count)
		return -EBUSY;

	priv->led_colors[p->nr][led->id] = *new_color;
	priv->commit_pending = 1;

	return 0;
}

static int lachesis_get_leds(struct razer_mouse *m,
			     unsigned int profile_nr,
			     struct razer_led **leds_list)
{
	struct lachesis_private *priv = m->drv_data;
	struct razer_led *scroll, *logo;

	if (WARN_ON(priv->type == LACHESIS_CLASSIC &&
		    profile_nr != 0))
		return -EINVAL;
	if (profile_nr >= LACHESIS_NR_PROFILES)
		return -EINVAL;

	scroll = zalloc(sizeof(struct razer_led));
	if (!scroll)
		return -ENOMEM;
	logo = zalloc(sizeof(struct razer_led));
	if (!logo) {
		free(scroll);
		return -ENOMEM;
	}

	scroll->name = "Scrollwheel";
	scroll->id = LACHESIS_LED_SCROLL;
	scroll->state = priv->led_states[profile_nr][LACHESIS_LED_SCROLL];
	scroll->toggle_state = lachesis_led_toggle;
	if (priv->type != LACHESIS_CLASSIC) {
		scroll->color = priv->led_colors[profile_nr][LACHESIS_LED_SCROLL];
		scroll->change_color = lachesis_led_change_color;
	}
	scroll->u.mouse_prof = &priv->profiles[profile_nr];

	logo->name = "GlowingLogo";
	logo->id = LACHESIS_LED_LOGO;
	logo->state = priv->led_states[profile_nr][LACHESIS_LED_LOGO];
	logo->toggle_state = lachesis_led_toggle;
	if (priv->type != LACHESIS_CLASSIC) {
		logo->color = priv->led_colors[profile_nr][LACHESIS_LED_LOGO];
		logo->change_color = lachesis_led_change_color;
	}
	logo->u.mouse_prof = &priv->profiles[profile_nr];

	/* Link the list */
	*leds_list = scroll;
	scroll->next = logo;
	logo->next = NULL;

	return LACHESIS_NR_LEDS;
}

static int lachesis_profile_get_leds(struct razer_mouse_profile *p,
				     struct razer_led **leds_list)
{
	return lachesis_get_leds(p->mouse, p->nr, leds_list);
}

static int lachesis_global_get_leds(struct razer_mouse *m,
				    struct razer_led **leds_list)
{
	return lachesis_get_leds(m, 0, leds_list);
}

static int lachesis_supported_axes(struct razer_mouse *m,
				   struct razer_axis **axes_list)
{
	struct lachesis_private *priv = m->drv_data;

	*axes_list = priv->axes;

	return ARRAY_SIZE(priv->axes);
}

static int lachesis_supported_freqs(struct razer_mouse *m,
				    enum razer_mouse_freq **freq_list)
{
	enum razer_mouse_freq *list;
	const int count = 3;

	list = malloc(sizeof(*list) * count);
	if (!list)
		return -ENOMEM;

	list[0] = RAZER_MOUSE_FREQ_1000HZ;
	list[1] = RAZER_MOUSE_FREQ_500HZ;
	list[2] = RAZER_MOUSE_FREQ_125HZ;

	*freq_list = list;

	return count;
}

static enum razer_mouse_freq lachesis_get_freq(struct razer_mouse *m,
					       unsigned int profile_nr)
{
	struct lachesis_private *priv = m->drv_data;

	if (WARN_ON(priv->type != LACHESIS_CLASSIC &&
		    profile_nr != 0))
		return -EINVAL;
	if (profile_nr >= ARRAY_SIZE(priv->cur_freq))
		return -EINVAL;

	return priv->cur_freq[profile_nr];
}

static enum razer_mouse_freq lachesis_profile_get_freq(struct razer_mouse_profile *p)
{
	return lachesis_get_freq(p->mouse, p->nr);
}

static enum razer_mouse_freq lachesis_global_get_freq(struct razer_mouse *m)
{
	return lachesis_get_freq(m, 0);
}

static int lachesis_set_freq(struct razer_mouse *m,
			     unsigned int profile_nr,
			     enum razer_mouse_freq freq)
{
	struct lachesis_private *priv = m->drv_data;

	if (!priv->m->claim_count)
		return -EBUSY;
	if (WARN_ON(priv->type != LACHESIS_CLASSIC &&
		    profile_nr != 0))
		return -EINVAL;
	if (profile_nr >= ARRAY_SIZE(priv->cur_freq))
		return -EINVAL;

	priv->cur_freq[profile_nr] = freq;
	priv->commit_pending = 1;

	return 0;
}

static int lachesis_profile_set_freq(struct razer_mouse_profile *p,
				     enum razer_mouse_freq freq)
{
	return lachesis_set_freq(p->mouse, p->nr, freq);
}

static int lachesis_global_set_freq(struct razer_mouse *m,
				    enum razer_mouse_freq freq)
{
	return lachesis_set_freq(m, 0, freq);
}

static int lachesis_supported_resolutions(struct razer_mouse *m,
					  enum razer_mouse_res **res_list)
{
	struct lachesis_private *priv = m->drv_data;
	enum razer_mouse_res *list;
	unsigned int i, count = 0, res, step = 0;

	switch (priv->type) {
	case LACHESIS_CLASSIC:
		count = 32;
		step = RAZER_MOUSE_RES_125DPI;
		break;
	case LACHESIS_5600:
		count = 56;
		step = RAZER_MOUSE_RES_100DPI;
		break;
	}

	list = malloc(sizeof(*list) * count);
	if (!list)
		return -ENOMEM;

	res = step;
	for (i = 0; i < count; i++) {
		list[i] = res;
		res += step;
	}

	*res_list = list;

	return count;
}

static struct razer_mouse_profile * lachesis_get_profiles(struct razer_mouse *m)
{
	struct lachesis_private *priv = m->drv_data;

	return &priv->profiles[0];
}

static struct razer_mouse_profile * lachesis_get_active_profile(struct razer_mouse *m)
{
	struct lachesis_private *priv = m->drv_data;

	return priv->cur_profile;
}

static int lachesis_set_active_profile(struct razer_mouse *m,
				       struct razer_mouse_profile *p)
{
	struct lachesis_private *priv = m->drv_data;

	if (!priv->m->claim_count)
		return -EBUSY;

	priv->cur_profile = p;
	priv->commit_pending = 1;

	return 0;
}

static int lachesis_supported_dpimappings(struct razer_mouse *m,
					  struct razer_mouse_dpimapping **res_ptr)
{
	struct lachesis_private *priv = m->drv_data;

	switch (priv->type) {
	case LACHESIS_CLASSIC:
		*res_ptr = &priv->dpimappings[0][0];
		return LACHESIS_NR_DPIMAPPINGS;
	case LACHESIS_5600:
		*res_ptr = &priv->dpimappings[0][0];
		return LACHESIS_NR_PROFILES * LACHESIS_NR_DPIMAPPINGS;
	}

	return -ENODEV;
}

static struct razer_mouse_dpimapping * lachesis_get_dpimapping(struct razer_mouse_profile *p,
							       struct razer_axis *axis)
{
	struct lachesis_private *priv = p->mouse->drv_data;

	if (p->nr >= ARRAY_SIZE(priv->cur_dpimapping))
		return NULL;

	return priv->cur_dpimapping[p->nr];
}

static int lachesis_set_dpimapping(struct razer_mouse_profile *p,
				   struct razer_axis *axis,
				   struct razer_mouse_dpimapping *d)
{
	struct lachesis_private *priv = p->mouse->drv_data;
	razer_id_mask_t idmask;

	if (!priv->m->claim_count)
		return -EBUSY;
	if (p->nr >= ARRAY_SIZE(priv->cur_dpimapping))
		return -EINVAL;

	razer_id_mask_zero(&idmask);
	if (priv->type != LACHESIS_CLASSIC)
		razer_id_mask_set(&idmask, p->nr);
	if (d->profile_mask != idmask)
		return -EINVAL;

	priv->cur_dpimapping[p->nr] = d;
	priv->commit_pending = 1;

	return 0;
}

static int lachesis_dpimapping_modify(struct razer_mouse_dpimapping *d,
				      enum razer_dimension dim,
				      enum razer_mouse_res res)
{
	struct lachesis_private *priv = d->mouse->drv_data;

	if ((int)dim < 0 || (int)dim >= ARRAY_SIZE(d->res))
		return -EINVAL;

	if (!priv->m->claim_count)
		return -EBUSY;

	d->res[dim] = res;
	priv->commit_pending = 1;

	return 0;
}

static int lachesis_supported_buttons(struct razer_mouse *m,
				      struct razer_button **res_ptr)
{
	*res_ptr = lachesis_physical_buttons;
	return ARRAY_SIZE(lachesis_physical_buttons);
}

static int lachesis_supported_button_functions(struct razer_mouse *m,
					       struct razer_button_function **res_ptr)
{
	*res_ptr = lachesis_button_functions;
	return ARRAY_SIZE(lachesis_button_functions);
}

static struct razer_button_function * lachesis_get_button_function(struct razer_mouse_profile *p,
								   struct razer_button *b)
{
	struct lachesis_private *priv = p->mouse->drv_data;
	struct lachesis_buttons *buttons;

	if (p->nr > ARRAY_SIZE(priv->buttons))
		return NULL;
	buttons = &priv->buttons[p->nr];

	return razer_get_buttonfunction_by_button(
			buttons->mapping, ARRAY_SIZE(buttons->mapping),
			lachesis_button_functions, ARRAY_SIZE(lachesis_button_functions),
			b);
}

static int lachesis_set_button_function(struct razer_mouse_profile *p,
					struct razer_button *b,
					struct razer_button_function *f)
{
	struct lachesis_private *priv = p->mouse->drv_data;
	struct lachesis_buttons *buttons;
	struct razer_buttonmapping *mapping;

	if (!priv->m->claim_count)
		return -EBUSY;
	if (p->nr > ARRAY_SIZE(priv->buttons))
		return -EINVAL;
	buttons = &priv->buttons[p->nr];

	mapping = razer_get_buttonmapping_by_physid(
			buttons->mapping, ARRAY_SIZE(buttons->mapping),
			b->id);
	if (!mapping)
		return -ENODEV;

	mapping->logical = f->id;
	priv->commit_pending = 1;

	return 0;
}

int razer_lachesis_init(struct razer_mouse *m,
			struct libusb_device *usbdev)
{
	struct lachesis_private *priv;
	struct libusb_device_descriptor desc;
	unsigned int i, j, k;
	int err;
	const char *devname = "";

	BUILD_BUG_ON(sizeof(struct lachesis_profcfg_cmd) != 0x18C);
	BUILD_BUG_ON(sizeof(struct lachesis_dpimap_cmd) != 0x60);
	BUILD_BUG_ON(sizeof(struct lachesis5k6_request) != 90);
	BUILD_BUG_ON(sizeof(struct lachesis5k6_request_devinfo) != 34);
	BUILD_BUG_ON(sizeof(struct lachesis5k6_request_globconfig) != 5);
	BUILD_BUG_ON(sizeof(struct lachesis5k6_request_profname) != 41);
	BUILD_BUG_ON(sizeof(struct lachesis5k6_request_hwconfig) != 72);

	err = libusb_get_device_descriptor(usbdev, &desc);
	if (err) {
		razer_error("hw_lachesis: Failed to get device descriptor\n");
		return -EIO;
	}

	priv = zalloc(sizeof(struct lachesis_private));
	if (!priv)
		return -ENOMEM;
	priv->m = m;
	m->drv_data = priv;

	priv->type = LACHESIS_CLASSIC;
	if (desc.idVendor == 0x1532 && desc.idProduct == 0x001E)
		priv->type = LACHESIS_5600;

	err = razer_usb_add_used_interface(m->usb_ctx, 0, 0);
	if (priv->type == LACHESIS_CLASSIC)
		err |= razer_usb_add_used_interface(m->usb_ctx, 1, 0);
	if (err) {
		err = -ENODEV;
		goto err_free;
	}

	for (i = 0; i < LACHESIS_NR_PROFILES; i++) {
		priv->profiles[i].nr = i;
		if (priv->type == LACHESIS_CLASSIC) {
			priv->profiles[i].get_freq = lachesis_profile_get_freq;
			priv->profiles[i].set_freq = lachesis_profile_set_freq;
		} else {
			priv->profiles[i].get_leds = lachesis_profile_get_leds;
			priv->profiles[i].get_name = lachesis_profile_get_name;
			priv->profiles[i].set_name = lachesis_profile_set_name;
		}
		priv->profiles[i].get_dpimapping = lachesis_get_dpimapping;
		priv->profiles[i].set_dpimapping = lachesis_set_dpimapping;
		priv->profiles[i].get_button_function = lachesis_get_button_function;
		priv->profiles[i].set_button_function = lachesis_set_button_function;
		priv->profiles[i].mouse = m;
	}

	razer_init_axes(&priv->axes[0],
			"X", 0,
			"Y", 0,
			"Scroll", 0);

	for (i = 0; i < LACHESIS_NR_PROFILES; i++) {
		for (j = 0; j < LACHESIS_NR_DPIMAPPINGS; j++) {
			priv->dpimappings[i][j].nr = i * 10 + j;
			for (k = 0; k < RAZER_NR_DIMS; k++)
				priv->dpimappings[i][j].res[k] = RAZER_MOUSE_RES_UNKNOWN;
			if (priv->type == LACHESIS_CLASSIC) {
				priv->dpimappings[i][j].dimension_mask = (1 << RAZER_DIM_0);
			} else {
				priv->dpimappings[i][j].dimension_mask = (1 << RAZER_DIM_X) |
									 (1 << RAZER_DIM_Y);
			}
			razer_id_mask_zero(&priv->dpimappings[i][j].profile_mask);
			if (priv->type != LACHESIS_CLASSIC)
				razer_id_mask_set(&priv->dpimappings[i][j].profile_mask, i);
			priv->dpimappings[i][j].change = lachesis_dpimapping_modify;
			priv->dpimappings[i][j].mouse = m;
		}
	}

	err = m->claim(m);
	if (err) {
		razer_error("hw_lachesis: "
			    "Failed to initially claim the device\n");
		goto err_free;
	}
	err = lachesis_read_devinfo(priv);
	if (err) {
		razer_error("hw_lachesis: Failed to get firmware version\n");
		goto err_release;
	}

	err = lachesis_read_config_from_hw(priv);
	if (err) {
		razer_error("hw_lachesis: "
			    "Failed to read the configuration from hardware\n");
		goto err_release;
	}
	switch (priv->type) {
	case LACHESIS_CLASSIC:
		devname = "Lachesis Classic";
		break;
	case LACHESIS_5600:
		devname = "Lachesis 5600DPI";
		break;
	}
	razer_generic_usb_gen_idstr(usbdev, m->usb_ctx->h, devname, 1,
				    priv->serial, m->idstr);

	m->type = RAZER_MOUSETYPE_LACHESIS;

	m->get_fw_version = lachesis_get_fw_version;
	m->commit = lachesis_commit;
	if (priv->type == LACHESIS_CLASSIC) {
		m->global_get_leds = lachesis_global_get_leds;
	} else {
		m->global_get_freq = lachesis_global_get_freq;
		m->global_set_freq = lachesis_global_set_freq;
	}
	m->nr_profiles = ARRAY_SIZE(priv->profiles);
	m->get_profiles = lachesis_get_profiles;
	m->get_active_profile = lachesis_get_active_profile;
	m->set_active_profile = lachesis_set_active_profile;
	m->supported_axes = lachesis_supported_axes;
	m->supported_resolutions = lachesis_supported_resolutions;
	m->supported_freqs = lachesis_supported_freqs;
	m->supported_dpimappings = lachesis_supported_dpimappings;
	m->supported_buttons = lachesis_supported_buttons;
	m->supported_button_functions = lachesis_supported_button_functions;

	err = lachesis_do_commit(priv);
	if (err) {
		razer_error("hw_lachesis: Failed to commit initial settings\n");
		goto err_release;
	}
	m->release(m);

	return 0;

err_release:
	m->release(m);
err_free:
	free(priv);

	return err;
}

void razer_lachesis_release(struct razer_mouse *m)
{
	struct lachesis_private *priv = m->drv_data;

	free(priv);
}
