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
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>

#define DEBUG_MODE 0

#if (DEBUG_MODE == 1)
	#define DEBUG_PRINT(fmt, args...)										 \
		printk (KERN_INFO "%s:%s[%d]: " fmt "\r\n", __FILE__, __FUNCTION__, __LINE__, ##args)
#else
	#define DEBUG_PRINT(fmt, args...)
#endif

static LIST_HEAD(cpld_client_list);
static struct mutex	 list_lock;

#define OMP800_CPLD1_I2C_SLAVE_ADDR	0x60
#define OMP800_CPLD2_I2C_SLAVE_ADDR 0x62

enum sysfs_cpld_attributes {
	VERSION,
	CPU_ID,
	CARD_TYPE,
	CARD_SLOT_ID,
	CHASSIS_SLOT_ID,
	CPU_THERMAL,
	MAC_THERMAL,
	RESET_CPU_A,
	RESET_CPU_B,
	RESET_MAC_A,
	RESET_MAC_B
};

enum omp800_card_type {
	CT_LINECARD,
	CT_FABRICCARD,
	CT_UNKNOWN
};

struct omp800_cpld_data {
	u8 driver_type;
	struct device *hwmon_dev;
	struct mutex   update_lock;
	char			 valid;		   /* != 0 if registers are valid */
	unsigned long	 last_updated;	/* In jiffies */
	s8 temp_input[2];
	u8 version;
    u8 slot_id;
};

struct cpld_client_node {
	struct i2c_client *client;
	struct list_head   list;
};

static enum omp800_card_type card_type = CT_UNKNOWN;

u8 temp_regs[] = {
0x30, /* CPU thermal */
0x31  /* MAC thermal */
};

static int omp800_is_linecard(u8 cpld_val)
{
	return (cpld_val & 0x10) ? 0 : 1;
}

static void omp800_cpld_add_client(struct i2c_client *client)
{
	struct cpld_client_node *node = kzalloc(sizeof(struct cpld_client_node), GFP_KERNEL);
	
	if (!node) {
		dev_dbg(&client->dev, "Can't allocate cpld_client_node (0x%x)\n", client->addr);
		return;
	}
	
	node->client = client;
	
	mutex_lock(&list_lock);
	list_add(&node->list, &cpld_client_list);
	mutex_unlock(&list_lock);
}

static void omp800_cpld_remove_client(struct i2c_client *client)
{
	struct list_head		*list_node = NULL;
	struct cpld_client_node *cpld_node = NULL;
	int found = 0;
	
	mutex_lock(&list_lock);

	list_for_each(list_node, &cpld_client_list)
	{
		cpld_node = list_entry(list_node, struct cpld_client_node, list);
		
		if (cpld_node->client == client) {
			found = 1;
			break;
		}
	}
	
	if (found) {
		list_del(list_node);
		kfree(cpld_node);
	}
	
	mutex_unlock(&list_lock);
}

static ssize_t show_data(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct i2c_client *client = to_i2c_client(dev);
	struct omp800_cpld_data *data = i2c_get_clientdata(client);
	int val;

	switch (attr->index) {
	case VERSION:
		val = data->version;
		break;
	case CPU_ID:
		val = (data->slot_id & 0x80) >> 7;
		break;
	case CARD_TYPE:
		val = (data->slot_id & 0x10) >> 4;
		break;
	case CARD_SLOT_ID:
		val = (data->slot_id & 0x7);
		break;
	case CHASSIS_SLOT_ID:
		val = (data->slot_id & 0x8) >> 3;
		break;
	default:
		return -ENOENT;
	};

	return sprintf(buf, "%d\n", val);
}

struct reg_data_byte {
	u8	 reg;
	u8	*value;
};

static struct omp800_cpld_data *omp800_cpld_update_temp(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct omp800_cpld_data *data = i2c_get_clientdata(client);

	mutex_lock(&data->update_lock);

	if (time_after(jiffies, data->last_updated + HZ + HZ / 2) || 
		!data->valid) {
		int i;
		struct reg_data_byte regs_byte[] = { {temp_regs[0], &data->temp_input[0]},
											 {temp_regs[1], &data->temp_input[1]}};

		dev_dbg(&client->dev, "Starting omp800_cpld temp update\n");
		data->valid = 0;
		
		/* Update temp data
		 */
		for (i = 0; i < ARRAY_SIZE(regs_byte); i++) {
			int status = i2c_smbus_read_byte_data(client, regs_byte[i].reg);
			
			if (status < 0) {
				dev_dbg(&client->dev, "reg %x, err %d\n", regs_byte[i].reg, status);
				goto exit;
			}
			else {
				*(regs_byte[i].value) = (s8)status;
			}
		}
		
		data->last_updated = jiffies;
		data->valid = 1;
	}

exit:
	mutex_unlock(&data->update_lock);
	return data;
}

static ssize_t set_temp(struct device *dev, struct device_attribute *da,
			const char *buf, size_t count) 
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct i2c_client *client = to_i2c_client(dev);
	struct omp800_cpld_data *data = i2c_get_clientdata(client);
	int nr = attr->index - CPU_THERMAL;
	long temp_input;
	int error;

	error = kstrtol(buf, 10, &temp_input);
	if (error) {
		return error;
	}

	if (temp_input < -128 || temp_input > 127) {
		return -EINVAL;
	}

	mutex_lock(&data->update_lock);
	data->temp_input[nr] = temp_input;
	error = i2c_smbus_write_byte_data(client, temp_regs[nr], (s8)temp_input);
	mutex_unlock(&data->update_lock);

	return count;
}

static ssize_t show_temp(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct omp800_cpld_data *data = omp800_cpld_update_temp(dev);

	if (!data->valid) {
		return 0;
	}

	return sprintf(buf, "%d\n", data->temp_input[attr->index - CPU_THERMAL]);
}

static ssize_t set_cpu_mac_reset(struct device *dev, struct device_attribute *da,
			const char *buf, size_t count) 
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct i2c_client *client = to_i2c_client(dev);
	int status, mask = 0;
	long reset;

	status = kstrtol(buf, 10, &reset);
	if (unlikely(status)) {
		return status;
	}

	/* Read reset status */
	status = i2c_smbus_read_byte_data(client, 0x8);
	DEBUG_PRINT("Reset reg (0x8) status = (0x%x)", status);
	if (unlikely(status < 0)) {
		return status;
	}
	
	switch (attr->index) {
		case RESET_CPU_A:
			mask = 0x1;
			break;
		case RESET_CPU_B:
			mask = 0x10;
			break;
		case RESET_MAC_A:
			mask = 0x2;
			break;
		case RESET_MAC_B:
			mask = 0x20;
			break;
		default:
			break;
	}

	status = reset ? (status & ~mask) : (status | mask);
	DEBUG_PRINT("Reset reg (0x8) write data = (0x%x)", status);
	status = i2c_smbus_write_byte_data(client, 0x8, status);
	
	if (unlikely(status < 0)) {
		return status;
	}

	return count;
}

static ssize_t show_cpu_mac_reset(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct i2c_client *client = to_i2c_client(dev);
	int status, mask = 0;

	/* Read reset status */
	status = i2c_smbus_read_byte_data(client, 0x8);
	if (unlikely(status < 0)) {
		return status;
	}
	
	switch (attr->index) {
		case RESET_CPU_A:
			mask = 0x1;
			break;
		case RESET_CPU_B:
			mask = 0x10;
			break;
		case RESET_MAC_A:
			mask = 0x2;
			break;
		case RESET_MAC_B:
			mask = 0x20;
			break;
		default:
			break;
	}

	return sprintf(buf, "%d\n", !(status & mask));
}

static SENSOR_DEVICE_ATTR(version, S_IRUGO, show_data, NULL, VERSION);
static SENSOR_DEVICE_ATTR(cpu_id, S_IRUGO, show_data, NULL, CPU_ID);
static SENSOR_DEVICE_ATTR(card_type, S_IRUGO, show_data, NULL, CARD_TYPE);
static SENSOR_DEVICE_ATTR(card_slot_id, S_IRUGO, show_data, NULL, CARD_SLOT_ID);
static SENSOR_DEVICE_ATTR(chassis_slot_id, S_IRUGO, show_data, NULL, CHASSIS_SLOT_ID);
static SENSOR_DEVICE_ATTR(temp1_input, S_IWUSR | S_IRUGO, show_temp, set_temp, CPU_THERMAL);
static SENSOR_DEVICE_ATTR(temp2_input, S_IWUSR | S_IRUGO, show_temp, set_temp, MAC_THERMAL);
static SENSOR_DEVICE_ATTR(reset_cpu_a, S_IWUSR | S_IRUGO, show_cpu_mac_reset, set_cpu_mac_reset, RESET_CPU_A);
static SENSOR_DEVICE_ATTR(reset_cpu_b, S_IWUSR | S_IRUGO, show_cpu_mac_reset, set_cpu_mac_reset, RESET_CPU_B);
static SENSOR_DEVICE_ATTR(reset_mac_a, S_IWUSR | S_IRUGO, show_cpu_mac_reset, set_cpu_mac_reset, RESET_MAC_A);
static SENSOR_DEVICE_ATTR(reset_mac_b, S_IWUSR | S_IRUGO, show_cpu_mac_reset, set_cpu_mac_reset, RESET_MAC_B);

static struct attribute *cpld1_attr[] = {
	&sensor_dev_attr_version.dev_attr.attr,
	&sensor_dev_attr_cpu_id.dev_attr.attr,
	&sensor_dev_attr_card_type.dev_attr.attr,
	&sensor_dev_attr_card_slot_id.dev_attr.attr,
	&sensor_dev_attr_chassis_slot_id.dev_attr.attr,
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_temp2_input.dev_attr.attr,
	NULL
};

static struct attribute *cpld2_attr[] = {
	&sensor_dev_attr_version.dev_attr.attr,
	NULL
};

static struct attribute *cpld_remote_attr[] = {
	&sensor_dev_attr_reset_cpu_a.dev_attr.attr,
	&sensor_dev_attr_reset_cpu_b.dev_attr.attr,
	&sensor_dev_attr_reset_mac_a.dev_attr.attr,
	&sensor_dev_attr_reset_mac_b.dev_attr.attr,
	NULL
};

static const struct attribute_group cpld1_group = {
	.attrs = cpld1_attr,
};

static const struct attribute_group cpld2_group = {
	.attrs = cpld2_attr,
};

static const struct attribute_group cpld_remote_group = {
	.attrs = cpld_remote_attr,
};

enum cpld_device_id {
omp800_cpld1,
omp800_cpld2,
omp800_cpld_remote
};

static int omp800_cpld_probe(struct i2c_client *client,
			const struct i2c_device_id *dev_id)
{
	int status;
	struct omp800_cpld_data *data;
	const struct attribute_group *group = NULL;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_dbg(&client->dev, "i2c_check_functionality failed (0x%x)\n", client->addr);
		return -EIO;
	}

	data = devm_kzalloc(&client->dev, sizeof(struct omp800_cpld_data), GFP_KERNEL);
	if (!data) {
		return -ENOMEM;
	}

	i2c_set_clientdata(client, data);
	data->slot_id = -1;
	data->version = -1;
	mutex_init(&data->update_lock);

	/* Get card type */
	if (dev_id->driver_data == omp800_cpld1) {
		status = i2c_smbus_read_byte_data(client, 0x2);

		if (status < 0) {
			dev_dbg(&client->dev, "reg %d, err %d\n", 0x2, status);
			goto exit_free;
		}

		card_type = omp800_is_linecard(status) ? CT_LINECARD : CT_FABRICCARD;
		group = &cpld1_group;
		data->slot_id = status;
	}
	else if (dev_id->driver_data == omp800_cpld2) {
		DEBUG_PRINT("Card Type = (%d)", card_type);
		if (card_type != CT_LINECARD) {
			return -ENXIO;
		}

		group = &cpld2_group;
	}
	else if (dev_id->driver_data == omp800_cpld_remote) {
		group = &cpld_remote_group;
	}
	else {
		return -ENXIO;
	}

	data->driver_type = dev_id->driver_data;

	if (dev_id->driver_data == omp800_cpld1 || dev_id->driver_data == omp800_cpld2) {
		/* Get version information */
		status = i2c_smbus_read_byte_data(client, 0x1);
		if (status < 0) {
			dev_dbg(&client->dev, "reg %d, err %d\n", 0x1, status);
			goto exit_free;
		}
		data->version = status;
	}

	status = sysfs_create_group(&client->dev.kobj, group);
	if (status) {
		goto exit_free;
	}

	data->hwmon_dev = hwmon_device_register(&client->dev);
	if (IS_ERR(data->hwmon_dev)) {
		status = PTR_ERR(data->hwmon_dev);
		goto exit_remove;
	}

	dev_info(&client->dev, "chip found\n");
	
	if (dev_id->driver_data == omp800_cpld1 || dev_id->driver_data == omp800_cpld2) {
		omp800_cpld_add_client(client);
	}
	
	return 0;

exit_remove:
	sysfs_remove_group(&client->dev.kobj, group);
exit_free:
	kfree(data);
	return status;
}

static int omp800_cpld_remove(struct i2c_client *client)
{
	struct omp800_cpld_data *data = i2c_get_clientdata(client);

	hwmon_device_unregister(data->hwmon_dev);

	if (data->driver_type == omp800_cpld1) {
		sysfs_remove_group(&client->dev.kobj, &cpld1_group);
	}
	else if (data->driver_type == omp800_cpld2) { /* omp800_cpld2 */
		sysfs_remove_group(&client->dev.kobj, &cpld2_group);
	}
	else if (data->driver_type == omp800_cpld_remote) {
		sysfs_remove_group(&client->dev.kobj, &cpld_remote_group);
	}
	
	kfree(data);

	omp800_cpld_remove_client(client);
	return 0;
}

static const struct i2c_device_id omp800_cpld_id[] = {
	{ "omp800_cpld1", omp800_cpld1 },
	{ "omp800_cpld2", omp800_cpld2 },
	{ "omp800_cpld_remote", omp800_cpld_remote },
	{}
};
MODULE_DEVICE_TABLE(i2c, omp800_cpld_id);

static struct i2c_driver omp800_cpld_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name = "omp800_cpld",
	},
	.probe		= omp800_cpld_probe,
	.remove	   	= omp800_cpld_remove,
	.id_table	= omp800_cpld_id,
};

int omp800_cpld_read(unsigned short cpld_addr, u8 reg)
{
	struct list_head   *list_node = NULL;
	struct cpld_client_node *cpld_node = NULL;
	int ret = -EPERM;
	
	mutex_lock(&list_lock);

	list_for_each(list_node, &cpld_client_list)
	{
		cpld_node = list_entry(list_node, struct cpld_client_node, list);
		
		if (cpld_node->client->addr == cpld_addr) {
			ret = i2c_smbus_read_byte_data(cpld_node->client, reg);
			break;
		}
	}
	
	mutex_unlock(&list_lock);

	return ret;
}
EXPORT_SYMBOL(omp800_cpld_read);

int omp800_cpld_write(unsigned short cpld_addr, u8 reg, u8 value)
{
	struct list_head   *list_node = NULL;
	struct cpld_client_node *cpld_node = NULL;
	int ret = -EIO;
	
	mutex_lock(&list_lock);

	list_for_each(list_node, &cpld_client_list)
	{
		cpld_node = list_entry(list_node, struct cpld_client_node, list);
		
		if (cpld_node->client->addr == cpld_addr) {
			ret = i2c_smbus_write_byte_data(cpld_node->client, reg, value);
			break;
		}
	}
	
	mutex_unlock(&list_lock);

	return ret;
}
EXPORT_SYMBOL(omp800_cpld_write);

static int __init omp800_cpld_init(void)
{
	mutex_init(&list_lock);
	return i2c_add_driver(&omp800_cpld_driver);
}

static void __exit omp800_cpld_exit(void)
{
	i2c_del_driver(&omp800_cpld_driver);
}
	
static struct dmi_system_id omp800_dmi_table[] = {
	{
		.ident = "Accton OMP800",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Accton"),
			DMI_MATCH(DMI_PRODUCT_NAME, "OMP800"),
		},
	}
};

int platform_accton_omp800(void)
{
	//return dmi_check_system(omp800_dmi_table);
	return 1; /* return 1 for test */
}
EXPORT_SYMBOL(platform_accton_omp800);

MODULE_AUTHOR("Brandon Chuang <brandon_chuang@accton.com.tw>");
MODULE_DESCRIPTION("omp800_cpld driver");
MODULE_LICENSE("GPL");

module_init(omp800_cpld_init);
module_exit(omp800_cpld_exit);

