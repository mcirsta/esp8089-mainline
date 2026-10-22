/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ESP8089_FW_H
#define ESP8089_FW_H

#include <linux/types.h>

#define ESP8089_FW_MAX_SEGMENTS 16

struct esp8089_fw_segment {
	u32 addr;
	u32 len;
	const u8 *data;
};

struct esp8089_fw_image {
	u32 entry;
	u8 nsegments;
	struct esp8089_fw_segment segments[ESP8089_FW_MAX_SEGMENTS];
};

/* Validate the complete image before allowing any writes to the device. */
int esp8089_fw_parse(const u8 *data, size_t size,
		     struct esp8089_fw_image *image);

#endif
