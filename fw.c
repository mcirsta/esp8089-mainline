// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/export.h>
#include <kunit/visibility.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/align.h>
#include <linux/unaligned.h>

#include "fw.h"

int esp8089_fw_parse(const u8 *data, size_t size,
		     struct esp8089_fw_image *image)
{
	size_t offset = 8, tail;
	u8 checksum = 0xef;
	unsigned int i;

	if (!data || !image || size < 16 || size > SZ_1M)
		return -EINVAL;
	if (data[0] != 0xe9 || !data[1] || data[1] > ESP8089_FW_MAX_SEGMENTS)
		return -EBADMSG;

	image->entry = get_unaligned_le32(data + 4);
	image->nsegments = data[1];
	for (i = 0; i < image->nsegments; i++) {
		struct esp8089_fw_segment *seg = &image->segments[i];
		u32 end;
		size_t j;

		if (size - offset < 8)
			return -EBADMSG;
		seg->addr = get_unaligned_le32(data + offset);
		seg->len = get_unaligned_le32(data + offset + 4);
		offset += 8;
		if (!seg->len || seg->len > size - offset ||
		    check_add_overflow(seg->addr, seg->len, &end))
			return -EBADMSG;
		seg->data = data + offset;
		for (j = 0; j < seg->len; j++)
			checksum ^= seg->data[j];
		offset += seg->len;
	}

	/* The checksum is the final byte of the next 16-byte boundary. */
	tail = ALIGN(offset + 1, 16);
	if (tail != size || checksum != data[tail - 1])
		return -EBADMSG;
	while (offset < tail - 1)
		if (data[offset++])
			return -EBADMSG;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(esp8089_fw_parse);
