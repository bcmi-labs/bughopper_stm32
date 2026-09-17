/*
 * Copyright 2025 Google LLC
 *
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart/uart_bridge.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(cdc_acm_bridge, LOG_LEVEL_INF);

struct usbd_context *usbd_init_device(struct usbd_context *ctx, usbd_msg_cb_t msg_cb, const char *const blocklist[]);

static const char *const blocklist_dual_cdc[] = {
	"dfu_dfu",
};

static const char *const blocklist_std[] = {
	"dfu_dfu",
	"cdc_acm_1",
};

USBD_DEVICE_DEFINE(normal_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_SAMPLE_USBD_VID, CONFIG_SAMPLE_USBD_PID);

USBD_DEVICE_DEFINE(dual_cdc_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_SAMPLE_USBD_VID, CONFIG_SAMPLE_USBD_PID);

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

static void led_set(uint8_t state) {
	static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
	gpio_pin_set_dt(&led, state);
}

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
#define CMD_I2C_READ 			0x04
#define CMD_I2C_WRITE 			0x05
#define CMD_USER_LED	 		0x06
#define CMD_REBOOT_BL	 		0xD1

K_MSGQ_DEFINE(i2c_msgq, 256, 2, 1);

static int get_report_cb(const struct device *dev, const uint8_t type, const uint8_t id, const uint16_t len,
			 uint8_t *const buf) {

	return 0;
}

const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

static const struct gpio_dt_spec pullup_gpios[DT_PROP_LEN(DT_PATH(zephyr_user), i2c_pullup_gpios)] = {
	DT_FOREACH_PROP_ELEM_SEP(
	DT_PATH(zephyr_user), i2c_pullup_gpios, GPIO_DT_SPEC_GET_BY_IDX, (, ))
};

static int set_report_cb(const struct device *dev, const uint8_t type, const uint8_t id, const uint16_t len,
			 const uint8_t *const buf) {

	uint8_t cmd = buf[0];

	// Only valid for I2C read and write commands
	uint8_t address = buf[1];
	uint8_t length = buf[2];
	uint8_t tmp[length];

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
	case CMD_I2C_MODE:
		// set PA5 as OUTPUT_HIGH
		// enable device i2c1
		gpio_pin_configure_dt(&pullup_gpios[0], GPIO_OUTPUT_HIGH);
		device_init(i2c_dev);
		break;
	case CMD_SECONDARY_UART_MODE:
		// set PA5 as INPUT
		gpio_pin_configure_dt(&pullup_gpios[0], GPIO_INPUT);
		// enable device usart2, cdc_acm_uart1 and uart_bridges[1]
		device_init(DEVICE_DT_GET(DT_NODELABEL(usart2)));
		device_init(uart_bridges[1]);
		// restart USB stack
		usbd_disable(&normal_usbd);
		usbd_shutdown(&normal_usbd);
		struct usbd_context *dual_cdc_ctx = usbd_init_device(&dual_cdc_usbd, sample_msg_cb, blocklist_dual_cdc);
		usbd_enable(&dual_cdc_usbd);
		break;
	case CMD_I2C_READ:
		// execute a read on specified address, populate get_report buf with the read data
		i2c_read(i2c_dev, &tmp[1], length, address);
		tmp[0]= length;
		k_msgq_put(&i2c_msgq, tmp, K_NO_WAIT);
		break;
	case CMD_I2C_WRITE:
		// execute a write on specified address
		i2c_write(i2c_dev, &buf[3], length, address);
		break;
	case CMD_USER_LED:
		led_set(buf[1]);
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

	usbd_init_device(&normal_usbd, sample_msg_cb, blocklist_std);

	if (!usbd_can_detect_vbus(&normal_usbd)) {
		err = usbd_enable(&normal_usbd);
		if (err) {
			LOG_ERR("Failed to enable device support");
			return err;
		}
	}

	LOG_INF("USB device support enabled");

	while (true) {
		UDC_STATIC_BUF_DEFINE(report, 256);
		k_msgq_get(&i2c_msgq, &report, K_FOREVER);
		hid_device_submit_report(hid_dev, report[0], &report[1]);
	}

	return 0;
}

