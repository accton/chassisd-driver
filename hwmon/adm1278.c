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

static ssize_t show_hot_swap(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t set_hot_swap(struct device *dev, struct device_attribute *da,
			const char *buf, size_t count);

/*
 * OPERATION
 */
#define PB_OPERATION_OFFSET				0x01
#define PB_OPERATION_CONTROL_ON         0x80

/* Addresses scanned 
 */
static const unsigned short normal_i2c[] = { I2C_CLIENT_END };

/* Each client has this additional data 
 */
struct adm1278_data {
    struct device      *hwmon_dev;
};

/* sysfs attributes for hwmon 
 */
static SENSOR_DEVICE_ATTR(hot_swap_on, S_IWUSR | S_IRUGO, show_hot_swap, set_hot_swap, 0);

static struct attribute *adm1278_attributes[] = {
    &sensor_dev_attr_hot_swap_on.dev_attr.attr,
    NULL
};

static const struct attribute_group adm1278_group = {
    .attrs = adm1278_attributes,
};

static ssize_t show_hot_swap(struct device *dev, struct device_attribute *da,
             char *buf)
{
    struct i2c_client *client = to_i2c_client(dev);
    int status = 0;

	status = i2c_smbus_read_byte_data(client, PB_OPERATION_OFFSET);
	if (status < 0) {
		return status;
	}

    return sprintf(buf, "%d\n", !!(status & PB_OPERATION_CONTROL_ON));
}

static ssize_t set_hot_swap(struct device *dev, struct device_attribute *da,
			const char *buf, size_t count)
{
	int error, enable;
	struct i2c_client *client = to_i2c_client(dev);
	u8 data;
	
	error = kstrtoint(buf, 10, &enable);
	if (error) {
		return error;
	}

	data = enable ? PB_OPERATION_CONTROL_ON : 0;

	error = i2c_smbus_write_byte_data(client, PB_OPERATION_OFFSET, data);
	if (error < 0) {
		DEBUG_PRINT("Unable to set hot swap controller (0x%x), error (%d)\n", client->addr, error);
	}

	return count;
}

static int adm1278_probe(struct i2c_client *client,
            const struct i2c_device_id *dev_id)
{
    struct adm1278_data *data;
    int status;

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
        status = -EIO;
        goto exit;
    }

    data = kzalloc(sizeof(struct adm1278_data), GFP_KERNEL);
    if (!data) {
        status = -ENOMEM;
        goto exit;
    }

    i2c_set_clientdata(client, data);
    dev_info(&client->dev, "chip found\n");

    /* Register sysfs hooks */
    status = sysfs_create_group(&client->dev.kobj, &adm1278_group);
    if (status) {
        goto exit_free;
    }

    data->hwmon_dev = hwmon_device_register(&client->dev);
    if (IS_ERR(data->hwmon_dev)) {
        status = PTR_ERR(data->hwmon_dev);
        goto exit_remove;
    }

    dev_info(&client->dev, "%s: adm1278 '%s'\n",
         dev_name(data->hwmon_dev), client->name);

    return 0;

exit_remove:
    sysfs_remove_group(&client->dev.kobj, &adm1278_group);
exit_free:
    kfree(data);
exit:
    
    return status;
}

static int adm1278_remove(struct i2c_client *client)
{
    struct adm1278_data *data = i2c_get_clientdata(client);

    hwmon_device_unregister(data->hwmon_dev);
    sysfs_remove_group(&client->dev.kobj, &adm1278_group);
    kfree(data);
    
    return 0;
}

enum chips
{ 
    adm1278
};

static const struct i2c_device_id adm1278_id[] = {
    { "adm1278", adm1278 },
    {}
};
MODULE_DEVICE_TABLE(i2c, adm1278_id);

static struct i2c_driver adm1278_driver = {
    .class        = I2C_CLASS_HWMON,
    .driver = {
        .name     = "adm1278",
    },
    .probe        = adm1278_probe,
    .remove       = adm1278_remove,
    .id_table     = adm1278_id,
    .address_list = normal_i2c,
};

module_i2c_driver(adm1278_driver);

MODULE_AUTHOR("Brandon Chuang <brandon_chuang@accton.com.tw>");
MODULE_DESCRIPTION("adm1278 driver");
MODULE_LICENSE("GPL");

