/*
 * An hwmon driver for the Power-One PFE3000 Power Module
 *
 * Copyright (C)  Brandon Chuang <brandon_chuang@accton.com.tw>
 *
 * Based on ad7414.c
 * Copyright 2006 Stefan Roese <sr at denx.de>, DENX Software Engineering
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/slab.h>

#define DEBUG_MODE 0

#if (DEBUG_MODE == 1)
	#define DEBUG_PRINT(fmt, args...)										 \
		printk (KERN_INFO "%s:%s[%d]: " fmt "\r\n", __FILE__, __FUNCTION__, __LINE__, ##args)
#else
	#define DEBUG_PRINT(fmt, args...)
#endif

#define MAX_FAN_DUTY_CYCLE 100

/* Addresses scanned
 */
static const unsigned short normal_i2c[] = { I2C_CLIENT_END };

/* Each client has this additional data
 */
struct pfe3000_data {
	struct device	   *hwmon_dev;
	struct mutex		update_lock;
	char				valid;			 /* !=0 if registers are valid */
	unsigned long		last_updated;	 /* In jiffies */
	u8	 fan_fault;		/* Register value */
	u8	 over_temp;		/* Register value */
	u8   vout_mode;		/* Register value */
	u16	 status_word;	/* Register value */
	u16	 v_out;			/* Register value */
	u16	 i_out;			/* Register value */
	u16	 p_out;			/* Register value */
	u16	 temp1;			/* Register value */
	u16	 temp2;			/* Register value */
	u16	 temp3;			/* Register value */
	u16	 fan_speed[2];	/* Register value */
	u16	 fan_duty_cycle;/* Register value */
	u8   mfr_model[18];  /* Register value */
};

static ssize_t show_vout(struct device *dev, struct device_attribute *da,
             char *buf);
static ssize_t show_word(struct device *dev, struct device_attribute *da,
			 char *buf);
static ssize_t show_linear(struct device *dev, struct device_attribute *da,
			 char *buf);
static ssize_t show_fan_fault(struct device *dev, struct device_attribute *da,
			 char *buf);
static ssize_t show_over_temp(struct device *dev, struct device_attribute *da,
			 char *buf);
static ssize_t show_ascii(struct device *dev, struct device_attribute *da,
             char *buf);
static struct pfe3000_data *pfe3000_update_device(struct device *dev);
static ssize_t set_fan_duty_cycle(struct device *dev, struct device_attribute *da,
			 const char *buf, size_t count);
static int pfe3000_write_word(struct i2c_client *client, u8 reg, u16 value);

enum pfe3000_sysfs_attributes {
	PSU_POWER_ON = 0,
	PSU_TEMP_FAULT,
	PSU_POWER_GOOD,
	PSU_FAN1_FAULT,
	PSU_FAN2_FAULT,
	PSU_OVER_TEMP,
	PSU_V_OUT,
	PSU_I_OUT,
	PSU_P_OUT,
	PSU_TEMP1_INPUT,
	PSU_TEMP2_INPUT,
	PSU_TEMP3_INPUT,
	PSU_FAN1_SPEED,
	PSU_FAN2_SPEED,
	PSU_FAN_DUTY_CYCLE,
	PSU_MFR_MODEL
};

/* sysfs attributes for hwmon
 */
static SENSOR_DEVICE_ATTR(psu_power_on,	   S_IRUGO, show_word,		NULL, PSU_POWER_ON);
static SENSOR_DEVICE_ATTR(psu_temp_fault,  S_IRUGO, show_word,		NULL, PSU_TEMP_FAULT);
static SENSOR_DEVICE_ATTR(psu_power_good,  S_IRUGO, show_word,		NULL, PSU_POWER_GOOD);
static SENSOR_DEVICE_ATTR(psu_fan1_fault,  S_IRUGO, show_fan_fault, NULL, PSU_FAN1_FAULT);
static SENSOR_DEVICE_ATTR(psu_fan2_fault,  S_IRUGO, show_fan_fault, NULL, PSU_FAN2_FAULT);
static SENSOR_DEVICE_ATTR(psu_over_temp,   S_IRUGO, show_over_temp, NULL, PSU_OVER_TEMP);
static SENSOR_DEVICE_ATTR(psu_v_out,	   S_IRUGO, show_vout,		NULL, PSU_V_OUT);
static SENSOR_DEVICE_ATTR(psu_i_out,	   S_IRUGO, show_linear,	NULL, PSU_I_OUT);
static SENSOR_DEVICE_ATTR(psu_p_out,	   S_IRUGO, show_linear,	NULL, PSU_P_OUT);
static SENSOR_DEVICE_ATTR(psu_temp1_input, S_IRUGO, show_linear,	NULL, PSU_TEMP1_INPUT);
static SENSOR_DEVICE_ATTR(psu_temp2_input, S_IRUGO, show_linear,	NULL, PSU_TEMP2_INPUT);
static SENSOR_DEVICE_ATTR(psu_temp3_input, S_IRUGO, show_linear,	NULL, PSU_TEMP3_INPUT);
static SENSOR_DEVICE_ATTR(psu_fan1_speed_rpm, S_IRUGO, show_linear, NULL, PSU_FAN1_SPEED);
static SENSOR_DEVICE_ATTR(psu_fan2_speed_rpm, S_IRUGO, show_linear, NULL, PSU_FAN2_SPEED);
static SENSOR_DEVICE_ATTR(psu_fan_duty_cycle_percentage, S_IWUSR | S_IRUGO, show_linear, set_fan_duty_cycle, PSU_FAN_DUTY_CYCLE);
static SENSOR_DEVICE_ATTR(psu_mfr_model,      S_IRUGO, show_ascii,  NULL, PSU_MFR_MODEL);

static struct attribute *pfe3000_attributes[] = {
	&sensor_dev_attr_psu_power_on.dev_attr.attr,
	&sensor_dev_attr_psu_temp_fault.dev_attr.attr,
	&sensor_dev_attr_psu_power_good.dev_attr.attr,
	&sensor_dev_attr_psu_fan1_fault.dev_attr.attr,
	&sensor_dev_attr_psu_fan2_fault.dev_attr.attr,
	&sensor_dev_attr_psu_over_temp.dev_attr.attr,
	&sensor_dev_attr_psu_v_out.dev_attr.attr,
	&sensor_dev_attr_psu_i_out.dev_attr.attr,
	&sensor_dev_attr_psu_p_out.dev_attr.attr,
	&sensor_dev_attr_psu_temp1_input.dev_attr.attr,
	&sensor_dev_attr_psu_temp2_input.dev_attr.attr,
	&sensor_dev_attr_psu_temp3_input.dev_attr.attr,
	&sensor_dev_attr_psu_fan1_speed_rpm.dev_attr.attr,
	&sensor_dev_attr_psu_fan2_speed_rpm.dev_attr.attr,
	&sensor_dev_attr_psu_fan_duty_cycle_percentage.dev_attr.attr,
	&sensor_dev_attr_psu_mfr_model.dev_attr.attr,
	NULL
};

static ssize_t show_word(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct pfe3000_data *data = pfe3000_update_device(dev);
	u16 status = 0;

	if (!data->valid) {
		return 0;
	}

	switch (attr->index) {
	case PSU_POWER_ON: /* psu_power_on, low byte bit 6 of status_word, 0=>ON, 1=>OFF */
		status = !(data->status_word & 0x40);
		break;
	case PSU_TEMP_FAULT: /* psu_temp_fault, low byte bit 2 of status_word, 0=>Normal, 1=>temp fault */
		status = !!(data->status_word & 0x4);
		break;
	case PSU_POWER_GOOD: /* psu_power_good, high byte bit 3 of status_word, 0=>OK, 1=>FAIL */
		status = !(data->status_word & 0x800);
		break;
	}

	return sprintf(buf, "%d\n", status);
}

static int two_complement_to_int(u16 data, u8 valid_bit, int mask)
{
	u16	 valid_data	 = data & mask;
	bool is_negative = valid_data >> (valid_bit - 1);

	return is_negative ? (-(((~valid_data) & mask) + 1)) : valid_data;
}

static ssize_t set_fan_duty_cycle(struct device *dev, struct device_attribute *da,
			const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct i2c_client *client = to_i2c_client(dev);
	struct pfe3000_data *data = i2c_get_clientdata(client);
	int nr = (attr->index == PSU_FAN_DUTY_CYCLE) ? 0 : 1;
	long speed;
	int error;

	error = kstrtol(buf, 10, &speed);
	if (error)
		return error;

	if (speed < 0 || speed > MAX_FAN_DUTY_CYCLE)
		return -EINVAL;

	mutex_lock(&data->update_lock);
	data->fan_duty_cycle = speed;
	pfe3000_write_word(client, 0x3B + nr, data->fan_duty_cycle);
	mutex_unlock(&data->update_lock);

	return count;
}

static ssize_t show_vout(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct pfe3000_data *data = pfe3000_update_device(dev);
    int exponent, mantissa;
    int multiplier = 1000;

    exponent = two_complement_to_int(data->vout_mode, 5, 0x1f);
    mantissa = data->v_out;

    return (exponent > 0) ? sprintf(buf, "%d\n", (mantissa << exponent) * multiplier) :
                            sprintf(buf, "%d\n", (mantissa * multiplier) / (1 << -exponent));
}

static ssize_t show_linear(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct pfe3000_data *data = pfe3000_update_device(dev);

	u16 value = 0;
	int exponent, mantissa;
	int multiplier = 1000;

	if (!data->valid) {
		return 0;
	}

	switch (attr->index) {
	case PSU_I_OUT:
		value = data->i_out;
		break;
	case PSU_P_OUT:
		value = data->p_out;
		break;
	case PSU_TEMP1_INPUT:
		value = data->temp1;
		break;
	case PSU_TEMP2_INPUT:
		value = data->temp2;
		break;
	case PSU_TEMP3_INPUT:
		value = data->temp3;
		break;
	case PSU_FAN1_SPEED:
		value = data->fan_speed[0];
		multiplier = 1;
		break;
	case PSU_FAN2_SPEED:
		value = data->fan_speed[1];
		multiplier = 1;
		break;
	case PSU_FAN_DUTY_CYCLE:
		value = data->fan_duty_cycle;
		multiplier = 1;
		break;
	}

	exponent = two_complement_to_int(value >> 11, 5, 0x1f);
	mantissa = two_complement_to_int(value & 0x7ff, 11, 0x7ff);

	return (exponent >= 0) ? sprintf(buf, "%d\n", (mantissa << exponent) * multiplier) :
							 sprintf(buf, "%d\n", (mantissa * multiplier) / (1 << -exponent));
}

static ssize_t show_fan_fault(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct pfe3000_data *data = pfe3000_update_device(dev);

	u8 mask = (attr->index == PSU_FAN1_FAULT) ? BIT(7) : BIT(6);

	if (!data->valid) {
		return 0;
	}

	return sprintf(buf, "%d\n", !!(data->fan_fault & mask));
}

static ssize_t show_over_temp(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct pfe3000_data *data = pfe3000_update_device(dev);

	if (!data->valid) {
		return 0;
	}

	return sprintf(buf, "%d\n", !!(data->over_temp & BIT(7)));
}

static ssize_t show_ascii(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct pfe3000_data *data = pfe3000_update_device(dev);
    u8 *ptr = NULL;

    if (!data->valid) {
        return 0;
    }
	
    switch (attr->index) {
    case PSU_MFR_MODEL: /* psu_mfr_model */
        ptr = data->mfr_model;
        break;
    default:
        return 0;
    }

    return sprintf(buf, "%s\n", ptr);
}

static const struct attribute_group pfe3000_group = {
	.attrs = pfe3000_attributes,
};

static int pfe3000_probe(struct i2c_client *client,
			const struct i2c_device_id *dev_id)
{
	struct pfe3000_data *data;
	int status;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA |
					I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_I2C_BLOCK)) {
		status = -EIO;
		goto exit;
	}

	data = kzalloc(sizeof(struct pfe3000_data), GFP_KERNEL);
	if (!data) {
		status = -ENOMEM;
		goto exit;
	}

	i2c_set_clientdata(client, data);
	mutex_init(&data->update_lock);

	dev_info(&client->dev, "chip found\n");

	/* Register sysfs hooks */
	status = sysfs_create_group(&client->dev.kobj, &pfe3000_group);
	if (status) {
		goto exit_free;
	}

	data->hwmon_dev = hwmon_device_register(&client->dev);
	if (IS_ERR(data->hwmon_dev)) {
		status = PTR_ERR(data->hwmon_dev);
		goto exit_remove;
	}

	dev_info(&client->dev, "%s: psu '%s'\n",
		 dev_name(data->hwmon_dev), client->name);

	return 0;

exit_remove:
	sysfs_remove_group(&client->dev.kobj, &pfe3000_group);
exit_free:
	kfree(data);
exit:

	return status;
}

static int pfe3000_remove(struct i2c_client *client)
{
	struct pfe3000_data *data = i2c_get_clientdata(client);

	hwmon_device_unregister(data->hwmon_dev);
	sysfs_remove_group(&client->dev.kobj, &pfe3000_group);
	kfree(data);

	return 0;
}

static const struct i2c_device_id pfe3000_id[] = {
	{ "pfe3000", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, pfe3000_id);

static struct i2c_driver pfe3000_driver = {
	.class		  = I2C_CLASS_HWMON,
	.driver = {
		.name	 = "pfe3000",
	},
	.probe		= pfe3000_probe,
	.remove		 = pfe3000_remove,
	.id_table = pfe3000_id,
	.address_list = normal_i2c,
};

static int pfe3000_read_byte(struct i2c_client *client, u8 reg)
{
	return i2c_smbus_read_byte_data(client, reg);
}

static int pfe3000_read_word(struct i2c_client *client, u8 reg)
{
	return i2c_smbus_read_word_data(client, reg);
}

static int pfe3000_write_word(struct i2c_client *client, u8 reg, u16 value)
{
	return i2c_smbus_write_word_data(client, reg, value);
}


static int pfe3000_read_block(struct i2c_client *client, u8 command, u8 *data,
              int data_len)
{
    int result = i2c_smbus_read_i2c_block_data(client, command, data_len, data);
    
    if (unlikely(result < 0))
        goto abort;
    if (unlikely(result != data_len)) {
        result = -EIO;
        goto abort;
    }
    
    result = 0;
    
abort:
    return result;
}

struct reg_data_byte {
	u8	 reg;
	u8	*value;
};

struct reg_data_word {
	u8	 reg;
	u16 *value;
};

static struct pfe3000_data *pfe3000_update_device(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct pfe3000_data *data = i2c_get_clientdata(client);

	mutex_lock(&data->update_lock);

	if (time_after(jiffies, data->last_updated + HZ + HZ / 2)
		|| !data->valid) {
		int i, status;
		struct reg_data_byte regs_byte[] = { {0x7d, &data->over_temp},
											 {0x81, &data->fan_fault},
											 {0x20, &data->vout_mode}};
		struct reg_data_word regs_word[] = { {0x79, &data->status_word},
											 {0x8b, &data->v_out},
											 {0x8c, &data->i_out},
											 {0x96, &data->p_out},
											 {0x8d, &data->temp1},
											 {0x8e, &data->temp2},
											 {0x8f, &data->temp3},
											 {0x3b, &data->fan_duty_cycle},
											 {0x90, &(data->fan_speed[0])},
											 {0x91, &(data->fan_speed[1])}};

		dev_dbg(&client->dev, "Starting pfe3000 update\n");
		data->valid = 0;

		/* Read byte data */
		for (i = 0; i < ARRAY_SIZE(regs_byte); i++) {
			status = pfe3000_read_byte(client, regs_byte[i].reg);

			if (status < 0) {
				dev_dbg(&client->dev, "reg %d, err %d\n", regs_byte[i].reg, status);
				goto exit;
			}
			else {
				*(regs_byte[i].value) = status;
			}
		}

		/* Read word data */
		for (i = 0; i < ARRAY_SIZE(regs_word); i++) {
			status = pfe3000_read_word(client, regs_word[i].reg);

			if (status < 0) {
				dev_dbg(&client->dev, "reg %d, err %d\n", regs_word[i].reg, status);
				goto exit;
			}
			else {
				*(regs_word[i].value) = status;
			}
		}

        /* Read mfr_model */
        status = pfe3000_read_block(client, 0x9a, data->mfr_model, 
                                         ARRAY_SIZE(data->mfr_model)-1);
        data->mfr_model[ARRAY_SIZE(data->mfr_model)-1] = '\0';
        
        if (status < 0) {
            dev_dbg(&client->dev, "reg 0x9a, err %d\n", status);
            goto exit;
        }

		data->last_updated = jiffies;
		data->valid = 1;
	}

exit:
	mutex_unlock(&data->update_lock);

	return data;
}

module_i2c_driver(pfe3000_driver);

MODULE_AUTHOR("Brandon Chuang <brandon_chuang@accton.com.tw>");
MODULE_DESCRIPTION("Power-One PFE3000 driver");
MODULE_LICENSE("GPL");


