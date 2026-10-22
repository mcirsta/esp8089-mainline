# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_ESP8089) += esp8089.o
esp8089-y := lifecycle.o recovery.o power.o txreport.o fw.o sdio_sif_esp.o esp_io.o \
	    esp_file.o esp_main.o esp_sip.o esp_ctrl.o esp_mac80211.o esp_utils.o

obj-$(CONFIG_ESP8089_KUNIT_TEST) += esp8089-test.o
esp8089-test-y := fw-test.o rx-test.o ampdu-test.o lifecycle-test.o ownership-test.o key-test.o bss-test.o
