// SPDX-License-Identifier: GPL-2.0
/*
 * VGG804834-0TSLWC I2C Touch Screen Driver
 * slash.linux.c@gmail.com
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ratelimit.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/serio.h>
#include <linux/platform_device.h>
#include <asm/unaligned.h>
#include <linux/of_gpio.h>

#define TS_REG_DEVIDE_MODE 0x00
#define TS_REG_TD_STATUS 0x02

#define TS_REG_TOUCH1_YH 0x03
#define TS_REG_TOUCH1_YL 0x04
#define TS_REG_TOUCH1_XH 0x05
#define TS_REG_TOUCH1_XL 0x06

#define TS_REG_TOUCH2_YH 0x09
#define TS_REG_TOUCH2_YL 0x0A
#define TS_REG_TOUCH2_XH 0x0B
#define TS_REG_TOUCH2_XL 0x0C

#define TS_REG_TOUCH3_YH 0x0F
#define TS_REG_TOUCH3_YL 0x10
#define TS_REG_TOUCH3_XH 0x11
#define TS_REG_TOUCH3_XL 0x12

#define TS_REG_TOUCH4_YH 0x15
#define TS_REG_TOUCH4_YL 0x16
#define TS_REG_TOUCH4_XH 0x17
#define TS_REG_TOUCH4_XL 0x18

#define TS_REG_TOUCH5_YH 0x1B
#define TS_REG_TOUCH5_YL 0x1C
#define TS_REG_TOUCH5_XH 0x1D
#define TS_REG_TOUCH5_XL 0x1E

#define TS_NAME_LEN 10

struct ts_reg_addr {
	int reg_devmode;
	int reg_report_rate;
	int reg_gain;
	int reg_offset;
	int reg_offset_x;
	int reg_offset_y;
	int reg_num_x;
	int reg_num_y;
};

struct ts_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct mutex mutex;
	struct gpio_desc *ts_gpiod;
	int max_ts_points;
	int ts_irq;
	char name[TS_NAME_LEN];
};

struct ts_chip_data {
	int  max_ts_points;
};

static int rzg2l_ts_writeread(struct i2c_client *client,
				   u16 wr_len, u8 *wr_buf,
				   u16 rd_len, u8 *rd_buf)
{
	struct i2c_msg wrmsg[2];
	int i = 0;
	int ret;

	if (wr_len) {
		wrmsg[i].addr  = client->addr;
		wrmsg[i].flags = 0;
		wrmsg[i].len = wr_len;
		wrmsg[i].buf = wr_buf;
		i++;
	}

	if (rd_len) {
		wrmsg[i].addr  = client->addr;
		wrmsg[i].flags = I2C_M_RD;
		wrmsg[i].len = rd_len;
		wrmsg[i].buf = rd_buf;
		i++;
	}

	ret = i2c_transfer(client->adapter, wrmsg, i);
	if (ret < 0)
		return ret;
	if (ret != i)
		return -EIO;

	return 0;
}

static irqreturn_t rzg2l_ts_isr(int irq, void *dev_id)
{
	struct ts_data *tsdata = dev_id;
	struct device *dev = &tsdata->client->dev;
	unsigned int abx, aby;
	u8 cmd, down;
	u8 rdbuf[4];
	u8 datalen = 4;
	int err;

	memset(rdbuf, 0, sizeof(rdbuf));

	cmd = TS_REG_TOUCH1_YH;
	err = rzg2l_ts_writeread(tsdata->client, sizeof(cmd),
		&cmd, datalen, rdbuf);
	if (err) {
		dev_err_ratelimited(dev, "Unable to fetch data: %d", err);
		goto out;
	}

	abx = ((rdbuf[2] & 0x0F) << 8) | rdbuf[3];
	aby = ((rdbuf[0] & 0x0F) << 8) | rdbuf[1];

	down = (rdbuf[0] & 0x80) == 0x80;
	if (!down) {
		abx = 0;
		aby = 0;
	}

	input_report_abs(tsdata->input, ABS_X, abx);
	input_report_abs(tsdata->input, ABS_Y, aby);
	input_report_key(tsdata->input, BTN_TOUCH, down);
	input_sync(tsdata->input);

out:
	return IRQ_HANDLED;
}

static int rzg2l_ts_probe(struct i2c_client *client,
					 const struct i2c_device_id *id)
{
	const struct ts_chip_data *chip_data;
	struct ts_data *tsdata;
	struct input_dev *input;
	int error;

	tsdata = devm_kzalloc(&client->dev, sizeof(*tsdata), GFP_KERNEL);
	if (!tsdata) {
		dev_err(&client->dev, "failed to allocate driver data");
		return -ENOMEM;
	}

	chip_data = device_get_match_data(&client->dev);
	if (!chip_data)
		chip_data = (const struct ts_chip_data *)id->driver_data;
	if (!chip_data || !chip_data->max_ts_points) {
		dev_err(&client->dev, "invalid or missing chip data\n");
		return -EINVAL;
	}

	tsdata->max_ts_points = chip_data->max_ts_points;

	input = devm_input_allocate_device(&client->dev);
	if (!input) {
		dev_err(&client->dev, "failed to allocate input device");
		return -ENOMEM;
	}

	mutex_init(&tsdata->mutex);
	tsdata->client = client;
	tsdata->input = input;

	input->id.bustype = BUS_I2C;
	input->dev.parent = &client->dev;

	input->name = "RZG2L Touchscreen";
	input->id.bustype = BUS_I2C;
	input->id.vendor = SERIO_MSC;
	input->id.product = 0;
	input->id.version = 0x0100;
	input->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	input->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);

	input_set_abs_params(input, ABS_X, 0, 2048, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, 2048, 0, 0);

	i2c_set_clientdata(client, tsdata);

	tsdata->ts_gpiod = devm_gpiod_get_optional(&client->dev, "tsirq", GPIOD_IN);
	if (IS_ERR(tsdata->ts_gpiod)) {
		error = PTR_ERR(tsdata->ts_gpiod);
		if (error != -EPROBE_DEFER)
			dev_err(&client->dev, "Fail irq to get tsirq: %d\n", error);
		return error;
	}

	tsdata->ts_irq = gpiod_to_irq(tsdata->ts_gpiod);

	error = devm_request_threaded_irq(&client->dev, tsdata->ts_irq,
					NULL, rzg2l_ts_isr,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					client->name, tsdata);
	if (error) {
		dev_err(&client->dev, "Unable to request touchscreen IRQ");
		return error;
	}

	error = input_register_device(input);
	if (error)
		return error;

	dev_info(&client->dev,
		"RZG2L initialized: IRQ %d", client->irq);

	return 0;
}

static int rzg2l_ts_remove(struct i2c_client *client)
{
	return 0;
}

static const struct ts_chip_data rzg2l_ts_data = {
	.max_ts_points = 5,
};

static const struct i2c_device_id rzg2l_ts_id[] = {
	{ .name = "rzg2l generic ts", .driver_data = (long)&rzg2l_ts_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, rzg2l_ts_id);

static const struct of_device_id rzg2l_ts_of_match[] = {
	{ .compatible = "rzg2l,touchscreen", .data = &rzg2l_ts_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rzg2l_ts_of_match);

static struct i2c_driver rzg2l_ts_driver = {
	.driver = {
		.name = "rzg2l-ts",
		.of_match_table = rzg2l_ts_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = rzg2l_ts_id,
	.probe    = rzg2l_ts_probe,
	.remove   = rzg2l_ts_remove,
};

module_i2c_driver(rzg2l_ts_driver);

MODULE_AUTHOR("Slash Huang <slash.linux.c@gmail.com>");
MODULE_DESCRIPTION("RZG2L I2C Touchscreen Driver");
MODULE_LICENSE("GPL v2");
