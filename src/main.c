/*
 * Copyright 2025 Google LLC
 *
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sample_usbd.h>

#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart/uart_bridge.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(cdc_acm_bridge, LOG_LEVEL_INF);

const struct device *const uart_dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);

static struct usbd_context *sample_usbd;

#define DEVICE_DT_GET_COMMA(node_id) DEVICE_DT_GET(node_id),

const struct device *uart_bridges[] = {
	DT_FOREACH_STATUS_OKAY(zephyr_uart_bridge, DEVICE_DT_GET_COMMA)
};

static void sample_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));

	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			if (usbd_enable(ctx)) {
				LOG_ERR("Failed to enable device support");
			}
		}

		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			if (usbd_disable(ctx)) {
				LOG_ERR("Failed to disable device support");
			}
		}
	}

	if (msg->type == USBD_MSG_CDC_ACM_LINE_CODING ||
	    msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE) {
		for (uint8_t i = 0; i < ARRAY_SIZE(uart_bridges); i++) {
			/* update all bridges, non valid combinations are
			 * skipped automatically.
			 */
			uart_bridge_settings_update(msg->dev, uart_bridges[i]);
		}
	}
}


static const struct gpio_dt_spec gpios[DT_PROP_LEN(DT_PATH(zephyr_user), cbus_gpios)] = {
	DT_FOREACH_PROP_ELEM_SEP(
	DT_PATH(zephyr_user), cbus_gpios, GPIO_DT_SPEC_GET_BY_IDX, (, ))
};

__ramfunc static inline void jump_to_rom_bootloader(void)
{
	const struct device *flash_device = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
	if (!device_is_ready(flash_device)) {
		printk("%s: device not ready.\n", flash_device->name);
		return;
	}

	/* erase flash content */
	flash_erase(flash_device, 0, 128);
	sys_reboot(SYS_REBOOT_COLD);
}

#define CMD_GPIO_SET 			0x01
#define CMD_I2C_MODE 			0x02
#define CMD_SECONDARY_UART_MODE	0x03
#define CMD_I2C_TRANSFER 		0x04
#define CMD_REBOOT_BL	 		0xD1

static int get_report_cb(const struct device *dev, const uint8_t type, const uint8_t id, const uint16_t len,
			 uint8_t *const buf) {
    return 0;
}

static int set_report_cb(const struct device *dev, const uint8_t type, const uint8_t id, const uint16_t len,
			 const uint8_t *const buf) {

	uint8_t cmd = buf[0];

	switch (cmd)	{
	case CMD_GPIO_SET:
		for (unsigned int i = 0; i < ARRAY_SIZE(gpios); i++) {
			if ((gpios[i].port == NULL) || ((buf[2] & BIT(i)) == 0)) {
				continue;
			}

			(void)gpio_pin_set_dt(&gpios[i], buf[1] & BIT(i));
		}
		break;
	case CMD_REBOOT_BL:
		jump_to_rom_bootloader();
		break;
	default:
		break;
	}

    return 0;
}

static void output_report(const struct device *dev, const uint16_t len,
			     const uint8_t *const buf)
{
	set_report_cb(dev, HID_REPORT_TYPE_OUTPUT, 0U, len, buf);
}

static struct hid_device_ops ops = {
	.get_report = get_report_cb,
	.set_report = set_report_cb,
	.output_report = output_report,
};

#define HID_USAGE_PAGE16(page, page2)                                                              \
    HID_ITEM(HID_ITEM_TAG_USAGE_PAGE, HID_ITEM_TYPE_GLOBAL, 2), page, page2

#define HID_USAGE_PAGE16_SINGLE(a) HID_USAGE_PAGE16((a & 0xFF), ((a >> 8) & 0xFF))

static const uint8_t hid_report_desc[] = {
    HID_USAGE_PAGE16_SINGLE(0xFF60),
    HID_USAGE(0x61),

    HID_COLLECTION(0x01),

    HID_LOGICAL_MIN8(0x00),
    HID_LOGICAL_MAX16(0xFF, 0x00),
    HID_REPORT_SIZE(0x08),
    HID_REPORT_COUNT(32),

    HID_USAGE(0x01),
    HID_INPUT(0x00),

    HID_USAGE(0x02),
    HID_OUTPUT(0x00),

    HID_END_COLLECTION,
};

int main(void)
{
	int err;

	const struct device *hid_dev;
	hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
	if (!device_is_ready(hid_dev)) {
		LOG_ERR("HID Device is not ready");
		return -EIO;
	}

	err = hid_device_register(hid_dev,
				  hid_report_desc, sizeof(hid_report_desc),
				  &ops);
	if (err != 0) {
		LOG_ERR("Failed to register HID Device, %d", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_USBD_HID_SET_POLLING_PERIOD)) {
		err = hid_device_set_in_polling(hid_dev, 1000);
		if (err) {
			LOG_WRN("Failed to set IN report polling period, %d", err);
		}

		err = hid_device_set_out_polling(hid_dev, 1000);
		if (err != 0 && err != -ENOTSUP) {
			LOG_WRN("Failed to set OUT report polling period, %d", err);
		}
	}

	for (int i = 0; i < ARRAY_SIZE(gpios); i++) {
		if (gpios[i].port == NULL) {
			continue;
		}

		err = gpio_pin_configure_dt(&gpios[i], GPIO_OUTPUT_INACTIVE);
		if (err) {
			LOG_ERR("Failed to configure GPIO pin %u, error %d", gpios[i].pin, err);
			return err;
		}
	}

	sample_usbd = sample_usbd_init_device(sample_msg_cb);
	if (sample_usbd == NULL) {
		LOG_ERR("Failed to initialize USB device");
		return -ENODEV;
	}

	if (!usbd_can_detect_vbus(sample_usbd)) {
		err = usbd_enable(sample_usbd);
		if (err) {
			LOG_ERR("Failed to enable device support");
			return err;
		}
	}

	LOG_INF("USB device support enabled");

	k_sleep(K_FOREVER);

	return 0;
}

