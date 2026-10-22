// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2010 -2014 Espressif System. */
#include <linux/property.h>
#include "esp_pub.h"
#include "esp_file.h"
#include "esp_init_data.h"

int esp_init_config(struct esp_pub *epub)
{
	u32 crystal;
	int ret;

	memcpy(epub->init_data, esp_init_data, sizeof(epub->init_data));
	ret = device_property_read_u32(epub->dev, "esp,crystal-26M-en",
				       &crystal);
	if (ret == -EINVAL || ret == -ENODATA)
		return 0;
	if (ret)
		return ret;
	if (crystal > 2)
		return -EINVAL;
	epub->init_data[48] = crystal;
	return 0;
}
