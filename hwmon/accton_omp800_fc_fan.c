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
#include <linux/dmi.h>

#define DRVNAME "omp800_fc_fan"

#define DEBUG_MODE 0

#if (DEBUG_MODE == 1)
	#define DEBUG_PRINT(fmt, args...)										 \
		printk (KERN_INFO "%s:%s[%d]: " fmt "\r\n", __FILE__, __FUNCTION__, __LINE__, ##args)
#else
	#define DEBUG_PRINT(fmt, args...)
#endif

#define NUM_OF_CARD				6
#define NUM_OF_THERMAL_PER_CARD 6
#define NUM_OF_THERMAL_SENSORS  (NUM_OF_CARD * NUM_OF_THERMAL_PER_CARD)

static struct omp800_fc_fan_data *omp800_fc_fan_update_device(struct device *dev);
static ssize_t fan_show_enable(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t fan_set_enable(struct device *dev, struct device_attribute *da,
			const char *buf, size_t count);
static ssize_t fan_show_value(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t temp_show_value(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t temp_show_warning(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t temp_show_shutdown(struct device *dev, struct device_attribute *da, char *buf);
static ssize_t set_duty_cycle(struct device *dev, struct device_attribute *da,
			const char *buf, size_t count);
extern int omp800_cpld_read(unsigned short cpld_addr, u8 reg);

/* fan related data, the index should match sysfs_fan_attributes
 */
static const u8 fan_reg[] = {
	0x0F,	   /* fan 1-4 present status */
	0x11,	   /* fan PWM(for all fan) */
	0x01,	   /* fan cpld version */
	0x12,	   /* front fan 1 speed(rpm) */
	0x13,	   /* front fan 2 speed(rpm) */
	0x14,	   /* front fan 3 speed(rpm) */
	0x15,	   /* front fan 4 speed(rpm) */
	0x22,	   /* rear fan 1 speed(rpm) */
	0x23,	   /* rear fan 2 speed(rpm) */
	0x24,	   /* rear fan 3 speed(rpm) */
	0x25,	   /* rear fan 4 speed(rpm) */
};

/* Each client has this additional data */
struct omp800_fc_fan_data {
	struct device   *hwmon_dev;
	struct mutex	 update_lock;
	u8				 enable;	   /* Enable or Disable fan board i2c access */
	char			 valid;		   /* != 0 if registers are valid */
	unsigned long	 last_updated;	/* In jiffies */
	u8			     reg_val[ARRAY_SIZE(fan_reg)]; /* Register value */
	struct mutex	 temp_update_lock;
	unsigned long	 temp_last_updated;	/* In jiffies */
	u8				 temp_valid;	  /* != 0 if registers are valid */
	u8				 temp_reg_val[NUM_OF_THERMAL_SENSORS]; /* Thermal sensor */
};

/* CPU: >0x40, MAC: >0x52, LM75a: >0x3C, LM75b: >0x41, LM75c: >0x45, LM75d: >0x3E
 */
static int temp_warning_degree[NUM_OF_THERMAL_PER_CARD] = {
64000, /* CPU:0x40 */
82000, /* MAC:0x52 */
60000, /* LM75a:0x3C */
65000, /* LM75b:0x41 */
69000, /* LM75c:0x45 */
62000, /* LM75d:0x3E */
};

/* CPU: >0x5F, MAC: >0x69, LM75a: >0x55, LM75b: >0x5A, LM75c: >0x5E, LM75d: >0x57
 */
static int temp_shutdown_degree[NUM_OF_THERMAL_PER_CARD] = {
95000, /* CPU:0x5F */
105000,/* MAC:0x69 */
85000, /* LM75a:0x55 */
90000, /* LM75b:0x5A */
94000, /* LM75c:0x5E */
87000, /* LM75d:0x57 */
};

enum fan_id {
	FAN1_ID,
	FAN2_ID,
	FAN3_ID,
	FAN4_ID
};

#define LINECARD_TEMP_INPUT(LCID) \
LC##LCID##_TEMP1_INPUT,	\
LC##LCID##_TEMP2_INPUT,	\
LC##LCID##_TEMP3_INPUT,	\
LC##LCID##_TEMP4_INPUT,	\
LC##LCID##_TEMP5_INPUT,	\
LC##LCID##_TEMP6_INPUT

#define LINECARD_TEMP_WARNING(LCID) \
LC##LCID##_TEMP1_WARNING,	\
LC##LCID##_TEMP2_WARNING,	\
LC##LCID##_TEMP3_WARNING,	\
LC##LCID##_TEMP4_WARNING,	\
LC##LCID##_TEMP5_WARNING,	\
LC##LCID##_TEMP6_WARNING

#define LINECARD_TEMP_SHUTDOWN(LCID) \
LC##LCID##_TEMP1_SHUTDOWN,	\
LC##LCID##_TEMP2_SHUTDOWN,	\
LC##LCID##_TEMP3_SHUTDOWN,	\
LC##LCID##_TEMP4_SHUTDOWN,	\
LC##LCID##_TEMP5_SHUTDOWN,	\
LC##LCID##_TEMP6_SHUTDOWN

#define FABRICCARD_TEMP_INPUT(FCID) \
FC##FCID##_TEMP1_INPUT,	\
FC##FCID##_TEMP2_INPUT,	\
FC##FCID##_TEMP3_INPUT,	\
FC##FCID##_TEMP4_INPUT,	\
FC##FCID##_TEMP5_INPUT, \
FC##FCID##_TEMP6_INPUT

#define FABRICCARD_TEMP_WARNING(FCID) \
FC##FCID##_TEMP1_WARNING,	\
FC##FCID##_TEMP2_WARNING,	\
FC##FCID##_TEMP3_WARNING,	\
FC##FCID##_TEMP4_WARNING,	\
FC##FCID##_TEMP5_WARNING, 	\
FC##FCID##_TEMP6_WARNING

#define FABRICCARD_TEMP_SHUTDOWN(FCID) \
FC##FCID##_TEMP1_SHUTDOWN,	\
FC##FCID##_TEMP2_SHUTDOWN,	\
FC##FCID##_TEMP3_SHUTDOWN,	\
FC##FCID##_TEMP4_SHUTDOWN,	\
FC##FCID##_TEMP5_SHUTDOWN,	\
FC##FCID##_TEMP6_SHUTDOWN

enum sysfs_fan_attributes {
	FAN_PRESENT_REG,
	FAN_DUTY_CYCLE_PERCENTAGE, /* Only one CPLD register to control duty cycle for all fans */
	FAN_VERSION,
	/*FAN_DIRECTION,*/
	FAN1_FRONT_SPEED_RPM,
	FAN2_FRONT_SPEED_RPM,
	FAN3_FRONT_SPEED_RPM,
	FAN4_FRONT_SPEED_RPM,
	FAN1_REAR_SPEED_RPM,
	FAN2_REAR_SPEED_RPM,
	FAN3_REAR_SPEED_RPM,
	FAN4_REAR_SPEED_RPM,
	FAN_ENABLE,
	FAN_PRESENT, /* FAN Board present status */
	FAN1_PRESENT,
	FAN2_PRESENT,
	FAN3_PRESENT,
	FAN4_PRESENT,
	FAN1_FAULT,
	FAN2_FAULT,
	FAN3_FAULT,
	FAN4_FAULT,
	LINECARD_TEMP_INPUT(1),   	/* Line card 0/4 temp input */
	LINECARD_TEMP_INPUT(2),   	/* Line card 1/5 temp input */
	LINECARD_TEMP_INPUT(3),   	/* Line card 2/7 temp input */
	LINECARD_TEMP_INPUT(4),   	/* Line card 3/8 temp input */
	FABRICCARD_TEMP_INPUT(1), 	/* Fabric card 0(CPU-A/CPU-B) temp input */
	FABRICCARD_TEMP_INPUT(2), 	/* Fabric card 1(CPU-A/CPU-B) temp input */
	LINECARD_TEMP_WARNING(1),   /* Line card 0/4 temp warning degree */
	LINECARD_TEMP_WARNING(2),   /* Line card 1/5 temp warning degree */
	LINECARD_TEMP_WARNING(3),   /* Line card 2/7 temp warning degree */
	LINECARD_TEMP_WARNING(4),   /* Line card 3/8 temp warning degree */
	FABRICCARD_TEMP_WARNING(1), /* Fabric card 0(CPU-A/CPU-B) temp warning degree */
	FABRICCARD_TEMP_WARNING(2), /* Fabric card 1(CPU-A/CPU-B) temp warning degree */
	LINECARD_TEMP_SHUTDOWN(1),	/* Line card 0/4 temp shutdown degree */
	LINECARD_TEMP_SHUTDOWN(2),	/* Line card 1/5 temp shutdown degree */
	LINECARD_TEMP_SHUTDOWN(3),	/* Line card 2/7 temp shutdown degree */
	LINECARD_TEMP_SHUTDOWN(4),	/* Line card 3/8 temp shutdown degree */
	FABRICCARD_TEMP_SHUTDOWN(1),/* Fabric card 0(CPU-A/CPU-B) temp shutdown degree */
	FABRICCARD_TEMP_SHUTDOWN(2),/* Fabric card 1(CPU-A/CPU-B) temp shutdown degree */
	TEMP_INPUT_MIN = LC1_TEMP1_INPUT,
	TEMP_WARNING_MIN = LC1_TEMP1_WARNING,
	TEMP_SHUTDOWN_MIN = LC1_TEMP1_SHUTDOWN
};

/* Define attributes
 */
static SENSOR_DEVICE_ATTR(fan_enable, S_IWUSR | S_IRUGO, fan_show_enable, fan_set_enable, FAN_ENABLE);
static SENSOR_DEVICE_ATTR(fan_version, S_IRUGO, fan_show_value, NULL, FAN_VERSION);

#define DECLARE_FAN_FAULT_SENSOR_DEV_ATTR(index) \
	static SENSOR_DEVICE_ATTR(fan##index##_fault, S_IRUGO, fan_show_value, NULL, FAN##index##_FAULT)
#define DECLARE_FAN_FAULT_ATTR(index)	  &sensor_dev_attr_fan##index##_fault.dev_attr.attr

#define DECLARE_FAN_DIRECTION_SENSOR_DEV_ATTR(index) \
	static SENSOR_DEVICE_ATTR(fan##index##_direction, S_IRUGO, fan_show_value, NULL, FAN##index##_DIRECTION)
#define DECLARE_FAN_DIRECTION_ATTR(index)  &sensor_dev_attr_fan##index##_direction.dev_attr.attr

#define DECLARE_FAN_DUTY_CYCLE_SENSOR_DEV_ATTR(index) \
	static SENSOR_DEVICE_ATTR(fan##index##_duty_cycle_percentage, S_IWUSR | S_IRUGO, fan_show_value, set_duty_cycle, FAN##index##_DUTY_CYCLE_PERCENTAGE)
#define DECLARE_FAN_DUTY_CYCLE_ATTR(index) &sensor_dev_attr_fan##index##_duty_cycle_percentage.dev_attr.attr

#define DECLARE_FAN_PRESENT_SENSOR_DEV_ATTR(index) \
	static SENSOR_DEVICE_ATTR(fan##index##_present, S_IRUGO, fan_show_value, NULL, FAN##index##_PRESENT)
#define DECLARE_FAN_PRESENT_ATTR(index)	  &sensor_dev_attr_fan##index##_present.dev_attr.attr

#define DECLARE_FAN_SPEED_RPM_SENSOR_DEV_ATTR(index) \
	static SENSOR_DEVICE_ATTR(fan##index##_front_speed_rpm, S_IRUGO, fan_show_value, NULL, FAN##index##_FRONT_SPEED_RPM);\
	static SENSOR_DEVICE_ATTR(fan##index##_rear_speed_rpm, S_IRUGO, fan_show_value, NULL, FAN##index##_REAR_SPEED_RPM)
#define DECLARE_FAN_SPEED_RPM_ATTR(index)  &sensor_dev_attr_fan##index##_front_speed_rpm.dev_attr.attr, \
										   &sensor_dev_attr_fan##index##_rear_speed_rpm.dev_attr.attr

/* lc[0-3]_temp[1-6]_input */
#define DECLARE_LC_THERMAL_SENSOR_DEV_ATTR(lcid, tid) \
	static SENSOR_DEVICE_ATTR(lc##lcid##_temp##tid##_input, S_IRUGO, temp_show_value, NULL, LC##lcid##_TEMP##tid##_INPUT);\
	static SENSOR_DEVICE_ATTR(lc##lcid##_temp##tid##_warning, S_IRUGO, temp_show_warning, NULL, LC##lcid##_TEMP##tid##_WARNING);\
	static SENSOR_DEVICE_ATTR(lc##lcid##_temp##tid##_shutdown, S_IRUGO, temp_show_shutdown, NULL, LC##lcid##_TEMP##tid##_SHUTDOWN)

#define DECLARE_LC_THERMAL_SENSOR_DEV_ATTRS(lcid) 	\
	DECLARE_LC_THERMAL_SENSOR_DEV_ATTR(lcid, 1);	\
	DECLARE_LC_THERMAL_SENSOR_DEV_ATTR(lcid, 2);	\
	DECLARE_LC_THERMAL_SENSOR_DEV_ATTR(lcid, 3);	\
	DECLARE_LC_THERMAL_SENSOR_DEV_ATTR(lcid, 4);	\
	DECLARE_LC_THERMAL_SENSOR_DEV_ATTR(lcid, 5);	\
	DECLARE_LC_THERMAL_SENSOR_DEV_ATTR(lcid, 6)

#define DECLARE_LC_THERMAL_SENSOR_ATTR(lcid, tid)					\
	&sensor_dev_attr_lc##lcid##_temp##tid##_input.dev_attr.attr, 	\
	&sensor_dev_attr_lc##lcid##_temp##tid##_warning.dev_attr.attr,	\
	&sensor_dev_attr_lc##lcid##_temp##tid##_shutdown.dev_attr.attr
#define DECLARE_LC_THERMAL_SENSOR_ATTRS(lcid)	\
	DECLARE_LC_THERMAL_SENSOR_ATTR(lcid, 1),	\
	DECLARE_LC_THERMAL_SENSOR_ATTR(lcid, 2),	\
	DECLARE_LC_THERMAL_SENSOR_ATTR(lcid, 3),	\
	DECLARE_LC_THERMAL_SENSOR_ATTR(lcid, 4),	\
	DECLARE_LC_THERMAL_SENSOR_ATTR(lcid, 5),	\
	DECLARE_LC_THERMAL_SENSOR_ATTR(lcid, 6)

/* fc[0-1]_temp[1-6]_input */
#define DECLARE_FC_THERMAL_SENSOR_DEV_ATTR(fcid, tid)	\
	static SENSOR_DEVICE_ATTR(fc##fcid##_temp##tid##_input, S_IRUGO, temp_show_value, NULL, FC##fcid##_TEMP##tid##_INPUT);\
	static SENSOR_DEVICE_ATTR(fc##fcid##_temp##tid##_warning, S_IRUGO, temp_show_warning, NULL, FC##fcid##_TEMP##tid##_WARNING);\
	static SENSOR_DEVICE_ATTR(fc##fcid##_temp##tid##_shutdown, S_IRUGO, temp_show_shutdown, NULL, FC##fcid##_TEMP##tid##_SHUTDOWN)

#define DECLARE_FC_THERMAL_SENSOR_DEV_ATTRS(fcid)	\
	DECLARE_FC_THERMAL_SENSOR_DEV_ATTR(fcid, 1);	\
	DECLARE_FC_THERMAL_SENSOR_DEV_ATTR(fcid, 2);	\
	DECLARE_FC_THERMAL_SENSOR_DEV_ATTR(fcid, 3);	\
	DECLARE_FC_THERMAL_SENSOR_DEV_ATTR(fcid, 4);	\
	DECLARE_FC_THERMAL_SENSOR_DEV_ATTR(fcid, 5);	\
	DECLARE_FC_THERMAL_SENSOR_DEV_ATTR(fcid, 6)

#define DECLARE_FC_THERMAL_SENSOR_ATTR(fcid, tid)					\
	&sensor_dev_attr_fc##fcid##_temp##tid##_input.dev_attr.attr, 	\
	&sensor_dev_attr_fc##fcid##_temp##tid##_warning.dev_attr.attr,	\
	&sensor_dev_attr_fc##fcid##_temp##tid##_shutdown.dev_attr.attr
#define DECLARE_FC_THERMAL_SENSOR_ATTRS(fcid)	\
	DECLARE_FC_THERMAL_SENSOR_ATTR(fcid, 1),	\
	DECLARE_FC_THERMAL_SENSOR_ATTR(fcid, 2),	\
	DECLARE_FC_THERMAL_SENSOR_ATTR(fcid, 3),	\
	DECLARE_FC_THERMAL_SENSOR_ATTR(fcid, 4),	\
	DECLARE_FC_THERMAL_SENSOR_ATTR(fcid, 5),	\
	DECLARE_FC_THERMAL_SENSOR_ATTR(fcid, 6)

/* 4 fan fault attributes in this platform */
DECLARE_FAN_FAULT_SENSOR_DEV_ATTR(1);
DECLARE_FAN_FAULT_SENSOR_DEV_ATTR(2);
DECLARE_FAN_FAULT_SENSOR_DEV_ATTR(3);
DECLARE_FAN_FAULT_SENSOR_DEV_ATTR(4);

/* 4 fan speed(rpm) attributes in this platform */
DECLARE_FAN_SPEED_RPM_SENSOR_DEV_ATTR(1);
DECLARE_FAN_SPEED_RPM_SENSOR_DEV_ATTR(2);
DECLARE_FAN_SPEED_RPM_SENSOR_DEV_ATTR(3);
DECLARE_FAN_SPEED_RPM_SENSOR_DEV_ATTR(4);

/* 4 fan present attributes in this platform */
DECLARE_FAN_PRESENT_SENSOR_DEV_ATTR();
DECLARE_FAN_PRESENT_SENSOR_DEV_ATTR(1);
DECLARE_FAN_PRESENT_SENSOR_DEV_ATTR(2);
DECLARE_FAN_PRESENT_SENSOR_DEV_ATTR(3);
DECLARE_FAN_PRESENT_SENSOR_DEV_ATTR(4);

/* 1 fan duty cycle attribute in this platform */
DECLARE_FAN_DUTY_CYCLE_SENSOR_DEV_ATTR();

/* 6 thermal sensor for each line card */
DECLARE_LC_THERMAL_SENSOR_DEV_ATTRS(1);
DECLARE_LC_THERMAL_SENSOR_DEV_ATTRS(2);
DECLARE_LC_THERMAL_SENSOR_DEV_ATTRS(3);
DECLARE_LC_THERMAL_SENSOR_DEV_ATTRS(4);

/* 6 thermal sensor for each fabric card */
DECLARE_FC_THERMAL_SENSOR_DEV_ATTRS(1);
DECLARE_FC_THERMAL_SENSOR_DEV_ATTRS(2);

static struct attribute *omp800_fc_fan_attributes[] = {
	/* fan related attributes */
	&sensor_dev_attr_fan_version.dev_attr.attr,
	&sensor_dev_attr_fan_enable.dev_attr.attr,
	DECLARE_FAN_FAULT_ATTR(1),
	DECLARE_FAN_FAULT_ATTR(2),
	DECLARE_FAN_FAULT_ATTR(3),
	DECLARE_FAN_FAULT_ATTR(4),
	DECLARE_FAN_SPEED_RPM_ATTR(1),
	DECLARE_FAN_SPEED_RPM_ATTR(2),
	DECLARE_FAN_SPEED_RPM_ATTR(3),
	DECLARE_FAN_SPEED_RPM_ATTR(4),
	DECLARE_FAN_PRESENT_ATTR(),
	DECLARE_FAN_PRESENT_ATTR(1),
	DECLARE_FAN_PRESENT_ATTR(2),
	DECLARE_FAN_PRESENT_ATTR(3),
	DECLARE_FAN_PRESENT_ATTR(4),
	DECLARE_FAN_DUTY_CYCLE_ATTR(),
	DECLARE_LC_THERMAL_SENSOR_ATTRS(1),
	DECLARE_LC_THERMAL_SENSOR_ATTRS(2),
	DECLARE_LC_THERMAL_SENSOR_ATTRS(3),
	DECLARE_LC_THERMAL_SENSOR_ATTRS(4),
	DECLARE_FC_THERMAL_SENSOR_ATTRS(1),
	DECLARE_FC_THERMAL_SENSOR_ATTRS(2),
	NULL
};

#define FAN_DUTY_CYCLE_REG_MASK		 	0xF
#define FAN_MAX_DUTY_CYCLE			  	100
#define FAN_REG_VAL_TO_SPEED_RPM_STEP   100

static int omp800_fc_fan_read_value(struct i2c_client *client, u8 reg)
{
	return i2c_smbus_read_byte_data(client, reg);
}

static int omp800_fc_fan_write_value(struct i2c_client *client, u8 reg, u8 value)
{
	return i2c_smbus_write_byte_data(client, reg, value);
}

/* fan utility functions
 */
static u32 reg_val_to_duty_cycle(u8 reg_val) 
{
	reg_val &= FAN_DUTY_CYCLE_REG_MASK;
	
	if (reg_val == 0) {
		return 0;
	}
	
	return ((u32)(reg_val+1) * 625 + 75)/ 100;
}

static u8 duty_cycle_to_reg_val(u8 duty_cycle) 
{
	if (duty_cycle <= 6) {
		return 0;
	}
	
	return ((u32)duty_cycle * 100 / 625) - 1;
}

static u32 reg_val_to_speed_rpm(u8 reg_val)
{
	return (u32)reg_val * FAN_REG_VAL_TO_SPEED_RPM_STEP;
}

static u8 reg_val_to_is_present(u8 reg_val, enum fan_id id)
{
	u8 mask = (1 << id);

	reg_val &= mask;

	return reg_val ? 0 : 1;
}

static u8 is_fan_fault(struct omp800_fc_fan_data *data, enum fan_id id)
{
	u8 ret = 1;
	int front_fan_index = FAN1_FRONT_SPEED_RPM + id;
	int rear_fan_index  = FAN1_REAR_SPEED_RPM  + id;

	/* Check if the speed of front or rear fan is ZERO,  
	 */
	if (reg_val_to_speed_rpm(data->reg_val[front_fan_index]) &&
		reg_val_to_speed_rpm(data->reg_val[rear_fan_index]))  {
		ret = 0;
	}

	return ret;
}

static ssize_t set_duty_cycle(struct device *dev, struct device_attribute *da,
			const char *buf, size_t count) 
{
	int error, value;
	struct i2c_client *client = to_i2c_client(dev);
	struct omp800_fc_fan_data *data = i2c_get_clientdata(client);

	if (!data->enable) {
		//DEBUG_PRINT("Fan board is disabled");
		return count;
	}
	
	error = kstrtoint(buf, 10, &value);
	if (error) {
		return error;
	}
		
	if (value < 0 || value > FAN_MAX_DUTY_CYCLE) {
		return -EINVAL;
	}
	
	omp800_fc_fan_write_value(client, 0x33, 0); /* Disable fan speed watch dog */
	omp800_fc_fan_write_value(client, fan_reg[FAN_DUTY_CYCLE_PERCENTAGE], duty_cycle_to_reg_val(value));
	//DEBUG_PRINT("Write to FanReg(0x%x), Value(0x%x)", fan_reg[FAN_DUTY_CYCLE_PERCENTAGE], duty_cycle_to_reg_val(value));
	return count;
}

static ssize_t fan_show_enable(struct device *dev, struct device_attribute *da, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct omp800_fc_fan_data *data = i2c_get_clientdata(client);

	return sprintf(buf, "%d\n", data->enable);
}

static ssize_t fan_set_enable(struct device *dev, struct device_attribute *da,
			const char *buf, size_t count)
{
	int error, value;
	struct i2c_client *client = to_i2c_client(dev);
	struct omp800_fc_fan_data *data = i2c_get_clientdata(client);

	error = kstrtoint(buf, 10, &value);
	if (error) {
		return error;
	}

	mutex_lock(&data->update_lock);
	data->enable = !!(value);
	mutex_unlock(&data->update_lock);
	
	return count;
}

static ssize_t fan_show_value(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct omp800_fc_fan_data *data = omp800_fc_fan_update_device(dev);
	ssize_t ret = 0;

	if (!data->enable) {
		//DEBUG_PRINT("Fan board is disabled");
		return sprintf(buf, "0\n");
	}

	if (attr->index == FAN_PRESENT) {
		return sprintf(buf, "%d\n", data->valid);
	}

	if (!data->valid) {
		return -EIO;
	}
	
	switch (attr->index) {
		case FAN_VERSION:
			ret = sprintf(buf, "%u\n", data->reg_val[FAN_VERSION]);
			break;
		case FAN_DUTY_CYCLE_PERCENTAGE:
		{
			u32 duty_cycle = reg_val_to_duty_cycle(data->reg_val[FAN_DUTY_CYCLE_PERCENTAGE]);
			ret = sprintf(buf, "%u\n", duty_cycle);
			break;
		}
		case FAN1_FRONT_SPEED_RPM:
		case FAN2_FRONT_SPEED_RPM:
		case FAN3_FRONT_SPEED_RPM:
		case FAN4_FRONT_SPEED_RPM:
		case FAN1_REAR_SPEED_RPM:
		case FAN2_REAR_SPEED_RPM:
		case FAN3_REAR_SPEED_RPM:
		case FAN4_REAR_SPEED_RPM:
			ret = sprintf(buf, "%u\n", reg_val_to_speed_rpm(data->reg_val[attr->index]));
			break;
		case FAN1_PRESENT:
		case FAN2_PRESENT:
		case FAN3_PRESENT:
		case FAN4_PRESENT:
			ret = sprintf(buf, "%d\n",
						  reg_val_to_is_present(data->reg_val[FAN_PRESENT_REG],
						  attr->index - FAN1_PRESENT));
			break;
		case FAN1_FAULT:
		case FAN2_FAULT:
		case FAN3_FAULT:
		case FAN4_FAULT:
			ret = sprintf(buf, "%d\n", is_fan_fault(data, attr->index - FAN1_FAULT));
			break;
		default:
			break;
	}		
	
	return ret;
}

static const struct attribute_group omp800_fc_fan_group = {
	.attrs = omp800_fc_fan_attributes,
};

static struct omp800_fc_fan_data *omp800_fc_fan_update_device(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct omp800_fc_fan_data *data = i2c_get_clientdata(client);

	if (!data->enable) {
		return data;
	}

	mutex_lock(&data->update_lock);

	if (time_after(jiffies, data->last_updated + HZ + HZ / 2) || 
		!data->valid) {
		int i;

		dev_dbg(&client->dev, "Starting omp800_fc_fan update\n");
		data->valid = 0;
		
		/* Update fan data
		 */
		for (i = 0; i < ARRAY_SIZE(data->reg_val); i++) {
			int status = omp800_fc_fan_read_value(client, fan_reg[i]);
			
			if (status < 0) {
				dev_dbg(&client->dev, "reg %d, err %d\n", fan_reg[i], status);
				goto exit;
			}
			else {
				data->reg_val[i] = status;
			}
		}
		
		data->last_updated = jiffies;
		data->valid = 1;
	}

exit:
	mutex_unlock(&data->update_lock);
	return data;
}

static struct omp800_fc_fan_data *omp800_fc_fan_update_temp(struct device *dev)
{
	int i = 0;
	struct i2c_client *client = to_i2c_client(dev);
	struct omp800_fc_fan_data *data = i2c_get_clientdata(client);

	if (!data->enable) {
		return data;
	}

	if (time_before(jiffies, data->temp_last_updated + HZ*3) &&
		data->temp_valid) {
		return data;
	}

	mutex_lock(&data->temp_update_lock);
	dev_dbg(&client->dev, "Starting omp800_fc_fan temp sensor update\n");
	data->temp_valid = 0;

	/* Update temp sensor data
	 */
	for (i = 0; i < ARRAY_SIZE(data->temp_reg_val); i++) {
		int reg = 0x50 + (i/6 << 4) + (i%6);
		int status;

		status = omp800_fc_fan_read_value(client, reg);

		if (status < 0) {
			dev_dbg(&client->dev, "reg %d, err %d\n", reg, status);
			goto exit;
		}
		else {
			data->temp_reg_val[i] = status;
		}

		//DEBUG_PRINT("temp reg (0x%x), value = (0x%x)", reg, data->temp_reg_val[i]);
	}

	data->temp_last_updated = jiffies;
	data->temp_valid = 1;

exit:
	mutex_unlock(&data->temp_update_lock);
	return data;
}

static ssize_t temp_show_warning(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	int index;
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	
	index = (attr->index - TEMP_WARNING_MIN) % NUM_OF_THERMAL_PER_CARD;
	return sprintf(buf, "%d\n", temp_warning_degree[index]);
}

static ssize_t temp_show_shutdown(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	int index;
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);

	index = (attr->index - TEMP_SHUTDOWN_MIN) % NUM_OF_THERMAL_PER_CARD;
	return sprintf(buf, "%d\n", temp_shutdown_degree[index]);	
}

static ssize_t temp_show_value(struct device *dev, struct device_attribute *da,
			 char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct omp800_fc_fan_data *data = omp800_fc_fan_update_temp(dev);

	if (!data->enable) {
		//DEBUG_PRINT("Fan board is disabled");
		return sprintf(buf, "0\n");
	}

	if (!data->temp_valid) {
		return -EIO;
	}

	//DEBUG_PRINT("temperature = (%d)", (s8)data->temp_reg_val[attr->index - TEMP_INPUT_MIN]);
	return sprintf(buf, "%d\n", (s8)data->temp_reg_val[attr->index - TEMP_INPUT_MIN] * 1000);
}

static int omp800_fc_is_fabriccard(u8 cpld_val)
{
	return (cpld_val & 0x10) ? 1 : 0;
}

static int omp800_fc_cpu_id(u8 cpld_val)
{
	return (cpld_val & 0x80) ? 1 : 0; /* 0:CPU-A, 1:CPU-B */
}

static int omp800_fc_fan_probe(struct i2c_client *client,
			const struct i2c_device_id *dev_id)
{
	struct omp800_fc_fan_data *data;
	int status;

	/* Check if we sit on FabricCard CPU-A */
	status = omp800_cpld_read(0x60, 0x2);
	if (status < 0) {
		//DEBUG_PRINT("cpld(0x60) reg(0x2) err %d", status);
		return -EIO;
	}

	if (!omp800_fc_is_fabriccard(status) || (omp800_fc_cpu_id(status) != 0)) {
		return -ENXIO;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		status = -EIO;
		goto exit;
	}

	data = kzalloc(sizeof(struct omp800_fc_fan_data), GFP_KERNEL);
	if (!data) {
		status = -ENOMEM;
		goto exit;
	}

	i2c_set_clientdata(client, data);
	mutex_init(&data->update_lock);
	mutex_init(&data->temp_update_lock);
	
	dev_info(&client->dev, "chip found\n");

	/* Register sysfs hooks */
	status = sysfs_create_group(&client->dev.kobj, &omp800_fc_fan_group);
	if (status) {
		goto exit_free;
	}

	data->hwmon_dev = hwmon_device_register(&client->dev);
	if (IS_ERR(data->hwmon_dev)) {
		status = PTR_ERR(data->hwmon_dev);
		goto exit_remove;
	}

	dev_info(&client->dev, "%s: fan '%s'\n",
		 dev_name(data->hwmon_dev), client->name);
	
	return 0;

exit_remove:
	sysfs_remove_group(&client->dev.kobj, &omp800_fc_fan_group);
exit_free:
	kfree(data);
exit:
	
	return status;
}

static int omp800_fc_fan_remove(struct i2c_client *client)
{
	struct omp800_fc_fan_data *data = i2c_get_clientdata(client);
	hwmon_device_unregister(data->hwmon_dev);
	sysfs_remove_group(&client->dev.kobj, &omp800_fc_fan_group);
	
	return 0;
}

/* Addresses to scan */
static const unsigned short normal_i2c[] = { 0x66, I2C_CLIENT_END };

static const struct i2c_device_id omp800_fc_fan_id[] = {
	{ "omp800_fc_fan", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, omp800_fc_fan_id);

static struct i2c_driver omp800_fc_fan_driver = {
	.class		= I2C_CLASS_HWMON,
	.driver = {
		.name	 = DRVNAME,
	},
	.probe			= omp800_fc_fan_probe,
	.remove	   		= omp800_fc_fan_remove,
	.id_table	 	= omp800_fc_fan_id,
	.address_list 	= normal_i2c,
};

static int __init omp800_fc_fan_init(void)
{
	extern int platform_accton_omp800(void);
	if (!platform_accton_omp800()) {
		return -ENODEV;
	}

	return i2c_add_driver(&omp800_fc_fan_driver);
}

static void __exit omp800_fc_fan_exit(void)
{
	i2c_del_driver(&omp800_fc_fan_driver);
}

late_initcall(omp800_fc_fan_init);
module_exit(omp800_fc_fan_exit);

MODULE_AUTHOR("Brandon Chuang <brandon_chuang@accton.com.tw>");
MODULE_DESCRIPTION("omp800_fc_fan driver");
MODULE_LICENSE("GPL");

