/*************************************************************
 *       Copyright 2017 Accton Technology Corporation.
 *
 * Licensed under the Eclipse Public License, Version 1.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *        http://www.eclipse.org/legal/epl-v10.html
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the
 * License.
 *
 ************************************************************/

#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/dmi.h>

#define DEBUG_MODE 0

#if (DEBUG_MODE == 1)
	#define DEBUG_PRINT(fmt, args...)										 \
		printk (KERN_INFO "%s:%s[%d]: " fmt "\r\n", __FILE__, __FUNCTION__, __LINE__, ##args)
#else
	#define DEBUG_PRINT(fmt, args...)
#endif

static ssize_t pdu_show_enable(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t pdu_set_enable(struct device *dev, struct device_attribute *da,
			const char *buf, size_t count);
static ssize_t show_pdu_present(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t show_pdu(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t show_psu(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t psu_set_enable(struct device *dev, struct device_attribute *da, const char *buf, size_t count);
static struct omp800_fc_pdu_data *omp800_fc_pdu_update_device(struct device *dev);
extern int omp800_cpld_read(unsigned short cpld_addr, u8 reg);

/* Addresses scanned 
 */
static const unsigned short normal_i2c[] = { I2C_CLIENT_END };

/* Each client has this additional data 
 */
struct omp800_fc_pdu_data {
    struct device      *hwmon_dev;
    struct mutex        update_lock;
	u8					enable;	   		/* Enable or Disable pdu board i2c access */
    char                valid;			/* !=0 if registers are valid */
    unsigned long       last_updated;	/* In jiffies */
	u8					index;			/* PDU index */
	u8					present;		/* PDU present status */
	u8 					status[5];		/* Register values 
										   0: PDU version
										   1: PSU present
										   2: PSU input power
										   3: PSU output power 
										   4: PSU enable */
};

#define PSU_ATTRIBUTES(ID) \
PSU##ID##_PRESENT, \
PSU##ID##_INPUT_POWER, \
PSU##ID##_OUTPUT_POWER, \
PSU##ID##_ENABLE

enum omp800_fc_psu_sysfs_attributes {
	PDU_INDEX,
	PDU_VERSION,
	PDU_PRESENT,
	PDU_ENABLE,
	PSU_ATTRIBUTES(1),
	PSU_ATTRIBUTES(2),
	PSU_ATTRIBUTES(3),
	NUM_OF_PSU_ATTR = PSU1_ENABLE - PSU1_PRESENT + 1,
	PSU_ATTRIBUTE_BEGIN = PSU1_PRESENT,
	PSU_ATTRIBUTE_END 	= PSU3_ENABLE
};

/* sysfs attributes for hwmon 
 */
static SENSOR_DEVICE_ATTR(pdu_enable,     S_IWUSR | S_IRUGO, pdu_show_enable, pdu_set_enable, PDU_ENABLE);
static SENSOR_DEVICE_ATTR(pdu_version,    S_IRUGO, show_pdu, NULL, PDU_VERSION);
static SENSOR_DEVICE_ATTR(pdu_is_present, S_IRUGO, show_pdu_present, NULL, PDU_PRESENT);

/* psu attributes */
#define DECLARE_PSU_SENSOR_DEV_ATTR(id) \
	static SENSOR_DEVICE_ATTR(psu##id##_is_present, S_IRUGO, show_psu, NULL, PSU##id##_PRESENT);\
	static SENSOR_DEVICE_ATTR(psu##id##_input_power_good, S_IRUGO, show_psu, NULL, PSU##id##_INPUT_POWER);\
	static SENSOR_DEVICE_ATTR(psu##id##_output_power_good, S_IRUGO, show_psu, NULL, PSU##id##_OUTPUT_POWER);\
	static SENSOR_DEVICE_ATTR(psu##id##_enable, S_IWUSR | S_IRUGO, show_psu, psu_set_enable, PSU##id##_ENABLE);
#define DECLARE_PSU_ATTR(id)	  &sensor_dev_attr_psu##id##_is_present.dev_attr.attr, \
								  &sensor_dev_attr_psu##id##_input_power_good.dev_attr.attr, \
								  &sensor_dev_attr_psu##id##_output_power_good.dev_attr.attr, \
								  &sensor_dev_attr_psu##id##_enable.dev_attr.attr

/* 3 PSUs for each PDU board */
DECLARE_PSU_SENSOR_DEV_ATTR(1);
DECLARE_PSU_SENSOR_DEV_ATTR(2);
DECLARE_PSU_SENSOR_DEV_ATTR(3);

static struct attribute *omp800_fc_pdu_attributes[] = {
    &sensor_dev_attr_pdu_version.dev_attr.attr,
    &sensor_dev_attr_pdu_is_present.dev_attr.attr,
    &sensor_dev_attr_pdu_enable.dev_attr.attr,
    DECLARE_PSU_ATTR(1),
    DECLARE_PSU_ATTR(2),
    DECLARE_PSU_ATTR(3),
    NULL
};

static ssize_t pdu_show_enable(struct device *dev, struct device_attribute *da, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct omp800_fc_pdu_data *data = i2c_get_clientdata(client);

	return sprintf(buf, "%d\n", data->enable);
}

static ssize_t pdu_set_enable(struct device *dev, struct device_attribute *da,
			const char *buf, size_t count)
{
	int error, value;
	struct i2c_client *client = to_i2c_client(dev);
	struct omp800_fc_pdu_data *data = i2c_get_clientdata(client);

	error = kstrtoint(buf, 10, &value);
	if (error) {
		return error;
	}

	mutex_lock(&data->update_lock);
	data->enable = !!(value);
	mutex_unlock(&data->update_lock);
	
	return count;
}

static ssize_t show_pdu_present(struct device *dev, struct device_attribute *da,
             char *buf)
{
	struct omp800_fc_pdu_data *data = omp800_fc_pdu_update_device(dev);

	if (!data->enable) {
		//DEBUG_PRINT("Pdu board is disabled");
		return sprintf(buf, "0\n");
	}

	if (!data->valid) {
		return -EIO;
	}

	return sprintf(buf, "%d\n", data->present);
}

static ssize_t show_pdu(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct i2c_client *client = to_i2c_client(dev);
    struct omp800_fc_pdu_data *data = i2c_get_clientdata(client);  

	if (!data->enable) {
		//DEBUG_PRINT("Pdu board is disabled");
		return sprintf(buf, "0\n");
	}

	if (attr->index == PDU_INDEX) {
		return sprintf(buf, "%d\n", data->index);
	}
	
	omp800_fc_pdu_update_device(dev);
	if (!data->valid) {
		return -EIO;
	}

	if (!data->present) {
		return -ENXIO;
	}
	
    return sprintf(buf, "%d\n", data->status[0]);
}

static ssize_t show_psu(struct device *dev, struct device_attribute *da,
             char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct omp800_fc_pdu_data *data = omp800_fc_pdu_update_device(dev);
    u8 value = 0;
	int psu_id = 0;

	if (!data->enable) {
		//DEBUG_PRINT("Pdu board is disabled");
		return sprintf(buf, "0\n");
	}

	if (!data->valid) {
		return -EIO;
	}

	if (!data->present) {
		return -ENXIO;
	}

	psu_id = (attr->index - PSU_ATTRIBUTE_BEGIN) / NUM_OF_PSU_ATTR;
	DEBUG_PRINT("PSU ID = (%d)\r\n", psu_id);

	switch (attr->index) {
	case PSU1_PRESENT:
	case PSU2_PRESENT:
	case PSU3_PRESENT:
		value = !(data->status[1] & (1 << (7-psu_id)));
		break;
	case PSU1_INPUT_POWER:
	case PSU2_INPUT_POWER:
	case PSU3_INPUT_POWER:
		value = !(data->status[2] & (1 << (7-psu_id)));
		break;
	case PSU1_OUTPUT_POWER:
	case PSU2_OUTPUT_POWER:
	case PSU3_OUTPUT_POWER:
		value = !(data->status[3] & (1 << (7-psu_id)));
		break;
	case PSU1_ENABLE:
	case PSU2_ENABLE:
	case PSU3_ENABLE:
		value = !(data->status[4] & (1 << (7-psu_id)));
		break;
	}

	return sprintf(buf, "%d\n", value);
}

static ssize_t psu_set_enable(struct device *dev, struct device_attribute *da,
			const char *buf, size_t count)
{
	u8 mask;
	int status, enable, psu_id;
	struct i2c_client *client = to_i2c_client(dev);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
    struct omp800_fc_pdu_data *data = i2c_get_clientdata(client);

	if (!data->enable) {
		//DEBUG_PRINT("Pdu board is disabled");
		return count;
	}

	status = kstrtoint(buf, 10, &enable);
	if (status) {
		return status;
	}

	psu_id = (attr->index - PSU_ATTRIBUTE_BEGIN) / NUM_OF_PSU_ATTR;
	mask = 1 << (7-psu_id);
	DEBUG_PRINT("PSU ID = (%d), mask = (0x%x)\r\n", psu_id, mask);

	mutex_lock(&data->update_lock);
	
	status = i2c_smbus_read_byte_data(client, 0x14);
	if (status < 0) {
		goto exit;
	}

	enable = enable ? (status & ~mask) : (status | mask);
	status = i2c_smbus_write_byte_data(client, 0x14, enable);
	if (status < 0) {
		goto exit;
	}

	data->status[4] = enable;

exit:
	mutex_unlock(&data->update_lock);

	return (status < 0) ? status : count;
}

static const struct attribute_group omp800_fc_psu_group = {
    .attrs = omp800_fc_pdu_attributes,
};

static int omp800_fc_is_fabriccard(u8 cpld_val)
{
	return (cpld_val & 0x10) ? 1 : 0;
}

static int omp800_fc_cpu_id(u8 cpld_val)
{
	return (cpld_val & 0x80) ? 1 : 0; /* 0:CPU-A, 1:CPU-B */
}

static int omp800_fc_pdu_probe(struct i2c_client *client,
            const struct i2c_device_id *dev_id)
{
    struct omp800_fc_pdu_data *data;
    int status;

	/* Check if we sit on FabricCard CPU-A */
	status = omp800_cpld_read(0x60, 0x2);
	if (status < 0) {
		DEBUG_PRINT("cpld(0x60) reg(0x2) err %d", status);
		return -EIO;
	}

	if (!omp800_fc_is_fabriccard(status) || (omp800_fc_cpu_id(status) != 0)) {
		return -ENXIO;
	}

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		DEBUG_PRINT("I2C_FUNC_SMBUS_BYTE_DATA not supported");
        return -EIO;
    }

    data = kzalloc(sizeof(struct omp800_fc_pdu_data), GFP_KERNEL);
    if (!data) {
        return -ENOMEM;
    }

    i2c_set_clientdata(client, data);
    data->valid  = 0;
	data->enable = 0;
	data->index  = dev_id->driver_data;
    mutex_init(&data->update_lock);

    dev_info(&client->dev, "chip found\n");

    /* Register sysfs hooks */
    status = sysfs_create_group(&client->dev.kobj, &omp800_fc_psu_group);
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
    sysfs_remove_group(&client->dev.kobj, &omp800_fc_psu_group);
exit_free:
    kfree(data);

    return status;
}

static int omp800_fc_pdu_remove(struct i2c_client *client)
{
    struct omp800_fc_pdu_data *data = i2c_get_clientdata(client);

    hwmon_device_unregister(data->hwmon_dev);
    sysfs_remove_group(&client->dev.kobj, &omp800_fc_psu_group);
    kfree(data);
    
    return 0;
}

enum psu_index 
{ 
    omp800_fc_pdu
};

#if 0
static const struct i2c_device_id omp800_fc_pdu_id[] = {
    { "omp800_fc_pdu1", omp800_fc_pdu1 },
    { "omp800_fc_pdu2", omp800_fc_pdu2 },
    {}
};
#else
static const struct i2c_device_id omp800_fc_pdu_id[] = {
    { "omp800_fc_pdu", omp800_fc_pdu },
    {}
};
#endif
MODULE_DEVICE_TABLE(i2c, omp800_fc_pdu_id);

static struct i2c_driver omp800_fc_pdu_driver = {
    .class        = I2C_CLASS_HWMON,
    .driver = {
        .name     = "omp800_fc_pdu",
    },
    .probe        = omp800_fc_pdu_probe,
    .remove       = omp800_fc_pdu_remove,
    .id_table     = omp800_fc_pdu_id,
    .address_list = normal_i2c,
};

struct reg_data_byte {
	u8  reg;
	u8 *value;
};

static struct omp800_fc_pdu_data *omp800_fc_pdu_update_device(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct omp800_fc_pdu_data *data = i2c_get_clientdata(client);

	if (!data->enable) {
		return data;
	}

    mutex_lock(&data->update_lock);

    if (time_after(jiffies, data->last_updated + HZ + HZ / 2)
        || !data->valid) {
        int status, i;
		struct reg_data_byte regs_byte[] = { {0x01, &data->status[0]},
											 {0x10, &data->status[1]},
											 {0x11, &data->status[2]},
											 {0x12, &data->status[3]},
											 {0x14, &data->status[4]}};

		data->valid = 0;
		data->present = 0;
        dev_dbg(&client->dev, "Starting omp800_fc_pdu update\n");
		/* Check if PDU board is present */
		status = omp800_cpld_read(0x60, 0x48);
		if (status < 0) {
			DEBUG_PRINT("cpld(0x60) reg(0x48) err %d", status);
			goto exit;
		}

		data->present = !(status & 0x20);
	
		/* Read byte data */
		if (data->present) {
			for (i = 0; i < ARRAY_SIZE(regs_byte); i++) {
				status = i2c_smbus_read_byte_data(client, regs_byte[i].reg);

				if (status < 0) {
					dev_dbg(&client->dev, "reg %d, err %d\n", regs_byte[i].reg, status);
					goto exit;
				}
				else {
					*(regs_byte[i].value) = status;
				}
			}
		}

        data->last_updated = jiffies;
        data->valid = 1;
    }

exit:
    mutex_unlock(&data->update_lock);

    return data;
}

static int __init omp800_fc_pdu_init(void)
{
	extern int platform_accton_omp800(void);
	if (!platform_accton_omp800()) {
		return -ENODEV;
	}
	
    return i2c_add_driver(&omp800_fc_pdu_driver);
}

static void __exit omp800_fc_pdu_exit(void)
{
    i2c_del_driver(&omp800_fc_pdu_driver);
}

late_initcall(omp800_fc_pdu_init);
module_exit(omp800_fc_pdu_exit);

MODULE_AUTHOR("Brandon Chuang <brandon_chuang@accton.com.tw>");
MODULE_DESCRIPTION("omp800_fc_pdu driver");
MODULE_LICENSE("GPL");

