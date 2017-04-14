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
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/leds.h>
#include <linux/slab.h>

#define DRVNAME "accton_omp800_led"

#define DEBUG_MODE 0

#if (DEBUG_MODE == 1)
	#define DEBUG_PRINT(fmt, args...)										 \
		printk (KERN_INFO "%s:%s[%d]: " fmt "\r\n", __FILE__, __FUNCTION__, __LINE__, ##args)
#else
	#define DEBUG_PRINT(fmt, args...)
#endif

extern int omp800_cpld_read(unsigned short cpld_addr, u8 reg);
extern int omp800_cpld_write(unsigned short cpld_addr, u8 reg, u8 value);

enum omp800_platform {
	OMP800_FC,
	OMP800_LC
};

struct accton_omp800_led_data {
	enum omp800_platform platform;
	struct platform_device *pdev;
	struct mutex	update_lock;
	char			valid;		  /* != 0 if registers are valid */
	unsigned long	last_updated; /* In jiffies */
	u8				reg_val[5];	  /* Register value, 0 = RELEASE/DIAG LED,
													 1 = FAN/PSU LED,
													 2 ~ 4 = SYSTEM LED */
};

static struct accton_omp800_led_data  *ledctl = NULL;

/* LEDs for Fabric Card only (FAN/PSU) 
 */
#define LED_TYPE_REG_MASK		0x07
#define LED_MODE_GREEN_MASK		0x01
#define LED_MODE_RED_MASK		0x02
#define LED_MODE_BLUE_MASK		0x04
#define LED_MODE_OFF_MASK   	0x00

#define LED_BRIGHTNESS_ON_VALUE   0x0
#define LED_BRIGHTNESS_OFF_VALUE  0xFF

static const u8 led_reg[] = {
	0x41,	/* RELEASE/DIAG LED, */
	0x43,	/* SYSTEM LED */
	0x44,	/* SYSTEM LED */
	0x45,	/* SYSTEM LED */
	0x42,	/* FAN/PSU LED */
};	
	
enum led_type {
	LED_TYPE_RLS,
	LED_TYPE_DIAG,
	LED_TYPE_SYS,
	LED_TYPE_PSU,
	LED_TYPE_FAN,
};

/* FAN/PSU/DIAG/RELEASE led mode */
enum led_light_mode {
	LED_MODE_OFF = 0,
	LED_MODE_GREEN,
	LED_MODE_GREEN_BLINK,
	LED_MODE_AMBER,
	LED_MODE_AMBER_BLINK,
	LED_MODE_RED,
	LED_MODE_RED_BLINK,
	LED_MODE_BLUE,
	LED_MODE_BLUE_BLINK,
	LED_MODE_AUTO,
	LED_MODE_UNKNOWN
};

struct led_type_mode {
	enum led_type type;
	int  type_mask;
	enum led_light_mode mode;
	int  mode_mask;
};

static struct led_type_mode led_type_mode_data[] = {
{LED_TYPE_PSU, LED_TYPE_REG_MASK << 4, LED_MODE_OFF, 	LED_MODE_OFF_MASK << 4},
{LED_TYPE_PSU, LED_TYPE_REG_MASK << 4, LED_MODE_GREEN, 	LED_MODE_GREEN_MASK << 4},
{LED_TYPE_PSU, LED_TYPE_REG_MASK << 4, LED_MODE_RED,  	LED_MODE_RED_MASK << 4},
{LED_TYPE_PSU, LED_TYPE_REG_MASK << 4, LED_MODE_BLUE,	LED_MODE_BLUE_MASK << 4},
{LED_TYPE_FAN, LED_TYPE_REG_MASK, LED_MODE_OFF, 	LED_MODE_OFF_MASK},
{LED_TYPE_FAN, LED_TYPE_REG_MASK, LED_MODE_GREEN, 	LED_MODE_GREEN_MASK},
{LED_TYPE_FAN, LED_TYPE_REG_MASK, LED_MODE_RED,  	LED_MODE_RED_MASK},
{LED_TYPE_FAN, LED_TYPE_REG_MASK, LED_MODE_BLUE,	LED_MODE_BLUE_MASK},
{LED_TYPE_RLS, LED_TYPE_REG_MASK << 4, LED_MODE_OFF, 	LED_MODE_OFF_MASK << 4},
{LED_TYPE_RLS, LED_TYPE_REG_MASK << 4, LED_MODE_GREEN, 	LED_MODE_GREEN_MASK << 4},
{LED_TYPE_RLS, LED_TYPE_REG_MASK << 4, LED_MODE_RED,  	LED_MODE_RED_MASK << 4},
{LED_TYPE_RLS, LED_TYPE_REG_MASK << 4, LED_MODE_BLUE,	LED_MODE_BLUE_MASK << 4},
{LED_TYPE_DIAG, LED_TYPE_REG_MASK, LED_MODE_OFF, 	LED_MODE_OFF_MASK},
{LED_TYPE_DIAG, LED_TYPE_REG_MASK, LED_MODE_GREEN, 	LED_MODE_GREEN_MASK},
{LED_TYPE_DIAG, LED_TYPE_REG_MASK, LED_MODE_RED,  	LED_MODE_RED_MASK},
{LED_TYPE_DIAG, LED_TYPE_REG_MASK, LED_MODE_BLUE,	LED_MODE_BLUE_MASK},
};

static int led_reg_val_to_light_mode(enum led_type type, u8 reg_val) {
	int i;
	
	for (i = 0; i < ARRAY_SIZE(led_type_mode_data); i++) {
		if (type != led_type_mode_data[i].type) {
			continue;
		}

		if ((led_type_mode_data[i].type_mask & ~reg_val) == 
			 led_type_mode_data[i].mode_mask) {
			return led_type_mode_data[i].mode;
		}
	}
	
	return LED_MODE_UNKNOWN;
}

static u8 led_light_mode_to_reg_val(enum led_type type, 
									enum led_light_mode mode, u8 reg_val) {
	int i;
									  
	for (i = 0; i < ARRAY_SIZE(led_type_mode_data); i++) {
		int type_mask, mode_mask;
		
		if (type != led_type_mode_data[i].type)
			continue;

		if (mode != led_type_mode_data[i].mode)
			continue;

		type_mask = led_type_mode_data[i].type_mask;
		mode_mask = led_type_mode_data[i].mode_mask;
		reg_val = (~mode_mask & type_mask) | (reg_val & ~type_mask);
	}
	
	return reg_val;
}

static int accton_omp800_led_read_value(u8 reg)
{
	return omp800_cpld_read(0x60, reg);
}

static int accton_omp800_led_write_value(u8 reg, u8 value)
{
	return omp800_cpld_write(0x60, reg, value);
}

static void accton_omp800_led_update(void)
{
	mutex_lock(&ledctl->update_lock);

	if (time_after(jiffies, ledctl->last_updated + HZ + HZ / 2)
		|| !ledctl->valid) {
		int i, nLedRegs;

		dev_dbg(&ledctl->pdev->dev, "Starting accton_omp800_led update\n");
		ledctl->valid = 0;
		
		
		/* Update LED data
		 */
		nLedRegs = ledctl->platform == OMP800_FC ? 5 : 4;
		
		for (i = 0; i < nLedRegs; i++) {
			int status = accton_omp800_led_read_value(led_reg[i]);
			
			if (status < 0) {
				dev_dbg(&ledctl->pdev->dev, "reg %d, err %d\n", led_reg[i], status);
				goto exit;
			}
			else
				ledctl->reg_val[i] = status;
		}
		
		ledctl->last_updated = jiffies;
		ledctl->valid = 1;
	}

exit:
	mutex_unlock(&ledctl->update_lock);
}

static void accton_omp800_led_set(struct led_classdev *led_cdev,
									  enum led_brightness led_light_mode, 
									  u8 reg, enum led_type type)
{
	int reg_val;
	
	mutex_lock(&ledctl->update_lock);
	reg_val = accton_omp800_led_read_value(reg);
	
	if (reg_val < 0) {
		dev_dbg(&ledctl->pdev->dev, "reg %d, err %d\n", reg, reg_val);
		goto exit;
	}

	reg_val = led_light_mode_to_reg_val(type, led_light_mode, reg_val);
	accton_omp800_led_write_value(reg, reg_val);
	ledctl->valid = 0;
	
exit:
	mutex_unlock(&ledctl->update_lock);
}

static void accton_omp800_led_psu_set(struct led_classdev *led_cdev,
											enum led_brightness led_light_mode)
{
	accton_omp800_led_set(led_cdev, led_light_mode, led_reg[4], LED_TYPE_PSU);
}

static enum led_brightness accton_omp800_led_psu_get(struct led_classdev *cdev)
{
	accton_omp800_led_update();
	return led_reg_val_to_light_mode(LED_TYPE_PSU, ledctl->reg_val[4]);
}

static void accton_omp800_led_fan_set(struct led_classdev *led_cdev,
										  enum led_brightness led_light_mode)
{
	accton_omp800_led_set(led_cdev, led_light_mode, led_reg[4], LED_TYPE_FAN);
}

static enum led_brightness accton_omp800_led_fan_get(struct led_classdev *cdev)
{
	accton_omp800_led_update();
	return led_reg_val_to_light_mode(LED_TYPE_FAN, ledctl->reg_val[4]);
}

static void accton_omp800_led_diag_set(struct led_classdev *led_cdev,
										   enum led_brightness led_light_mode)
{
	accton_omp800_led_set(led_cdev, led_light_mode, led_reg[0], LED_TYPE_DIAG);
}

static enum led_brightness accton_omp800_led_diag_get(struct led_classdev *cdev)
{
	accton_omp800_led_update();
	return led_reg_val_to_light_mode(LED_TYPE_DIAG, ledctl->reg_val[0]);
}

static void accton_omp800_led_release_set(struct led_classdev *led_cdev,
										  enum led_brightness led_light_mode)
{
	accton_omp800_led_set(led_cdev, led_light_mode, led_reg[0], LED_TYPE_RLS);
}

static enum led_brightness accton_omp800_led_release_get(struct led_classdev *cdev)
{
	accton_omp800_led_update();
	return led_reg_val_to_light_mode(LED_TYPE_RLS, ledctl->reg_val[0]);
}

static void accton_omp800_led_sys_set(struct led_classdev *led_cdev,
											enum led_brightness led_light_mode)
{
	if (LED_MODE_OFF == (enum led_light_mode)led_light_mode) {
		accton_omp800_led_write_value(led_reg[1], LED_BRIGHTNESS_OFF_VALUE);
		accton_omp800_led_write_value(led_reg[2], LED_BRIGHTNESS_OFF_VALUE);
		accton_omp800_led_write_value(led_reg[3], LED_BRIGHTNESS_OFF_VALUE);
		return;
	}
	
	if (LED_MODE_GREEN == (enum led_light_mode)led_light_mode) {
		accton_omp800_led_write_value(led_reg[1], LED_BRIGHTNESS_OFF_VALUE);
		accton_omp800_led_write_value(led_reg[2], LED_BRIGHTNESS_ON_VALUE);
		accton_omp800_led_write_value(led_reg[3], LED_BRIGHTNESS_OFF_VALUE);
		return;
	}
	
	if (LED_MODE_RED == (enum led_light_mode)led_light_mode) {
		accton_omp800_led_write_value(led_reg[1], LED_BRIGHTNESS_ON_VALUE);
		accton_omp800_led_write_value(led_reg[2], LED_BRIGHTNESS_OFF_VALUE);
		accton_omp800_led_write_value(led_reg[3], LED_BRIGHTNESS_OFF_VALUE);
		return;
	}
	
	if (LED_MODE_BLUE == (enum led_light_mode)led_light_mode) {
		accton_omp800_led_write_value(led_reg[1], LED_BRIGHTNESS_OFF_VALUE);
		accton_omp800_led_write_value(led_reg[2], LED_BRIGHTNESS_OFF_VALUE);
		accton_omp800_led_write_value(led_reg[3], LED_BRIGHTNESS_ON_VALUE);
		return;
	}
}

static enum led_brightness accton_omp800_led_sys_get(struct led_classdev *cdev)
{
	u8 is_red_on, is_green_on, is_bule_on;
	accton_omp800_led_update();

	is_red_on   = (ledctl->reg_val[1] == LED_BRIGHTNESS_OFF_VALUE) ? 0 : 1;
	is_green_on = (ledctl->reg_val[2] == LED_BRIGHTNESS_OFF_VALUE) ? 0 : 1;
	is_bule_on  = (ledctl->reg_val[3] == LED_BRIGHTNESS_OFF_VALUE) ? 0 : 1;

	if (is_red_on) {
	    return LED_MODE_RED;
	}
	
	if (is_green_on) {
		return LED_MODE_GREEN;
	}
	
	if (is_bule_on) {
		return LED_MODE_BLUE;
	}

	return LED_MODE_OFF;
}

static struct led_classdev accton_omp800_leds[] = {
	[LED_TYPE_RLS] = {
		.name			 = "accton_omp800_led::release",
		.default_trigger = "unused",
		.brightness_set	 = accton_omp800_led_release_set,
		.brightness_get  = accton_omp800_led_release_get,
		.max_brightness  = LED_MODE_BLUE,
	},
	[LED_TYPE_DIAG] = {
		.name			 = "accton_omp800_led::diag",
		.default_trigger = "unused",
		.brightness_set	 = accton_omp800_led_diag_set,
		.brightness_get  = accton_omp800_led_diag_get,
		.max_brightness  = LED_MODE_BLUE,
	},
	[LED_TYPE_SYS] = {
		.name			 = "accton_omp800_led::sys",
		.default_trigger = "unused",
		.brightness_set	 = accton_omp800_led_sys_set,
		.brightness_get  = accton_omp800_led_sys_get,
		.max_brightness  = LED_MODE_BLUE,
	},
	[LED_TYPE_PSU] = {
		.name			 = "accton_omp800_led::psu",
		.default_trigger = "unused",
		.brightness_set	 = accton_omp800_led_psu_set,
		.brightness_get  = accton_omp800_led_psu_get,
		.max_brightness  = LED_MODE_BLUE,
	},
	[LED_TYPE_FAN] = {
		.name			 = "accton_omp800_led::fan",
		.default_trigger = "unused",
		.brightness_set	 = accton_omp800_led_fan_set,
		.brightness_get  = accton_omp800_led_fan_get,
		.max_brightness  = LED_MODE_BLUE,
	},
};

static int accton_omp800_led_probe(struct platform_device *pdev)
{
	int ret, i, nLeds;

	nLeds = (ledctl->platform == OMP800_FC) ? 5 : 3;
	
	for (i = 0; i < nLeds; i++) {
		ret = led_classdev_register(&pdev->dev, &accton_omp800_leds[i]);
		
		if (ret < 0) {
			break;
		}
	}
	
	/* Check if all LEDs were successfully registered */
	if (i != nLeds){
		int j;
		
		/* only unregister the LEDs that were successfully registered */
		for (j = 0; j < i; j++) {
			led_classdev_unregister(&accton_omp800_leds[i]);
		}
	}

	return ret;
}

static int accton_omp800_led_remove(struct platform_device *pdev)
{
	int i, nLeds;
	nLeds = (ledctl->platform == OMP800_FC) ? 5 : 3;

	for (i = 0; i < nLeds; i++) {
		led_classdev_unregister(&accton_omp800_leds[i]);
	}

	return 0;
}

static struct platform_driver accton_omp800_led_driver = {
	.probe	= accton_omp800_led_probe,
	.remove	= accton_omp800_led_remove,
	.driver = {
		.name   = DRVNAME,
		.owner  = THIS_MODULE,
	},
};

static int __init accton_omp800_led_init(void)
{
	int ret;
#if 0
	extern int platform_accton_omp800(void);
	enum omp800_platform platform;

	if (platform_accton_omp800_fc()) {
		platform = OMP800_FC;
	}
	else if (platform_accton_omp800_lc()) {
		platform = OMP800_LC;
	}
	else {
		return -ENODEV;
	}	
#else
	extern int platform_accton_omp800(void);
	if (!platform_accton_omp800()) {
		return -ENODEV;
	}	
#endif
	
	ret = platform_driver_register(&accton_omp800_led_driver);
	if (ret < 0) {
		goto exit;
	}

	ledctl = kzalloc(sizeof(struct accton_omp800_led_data), GFP_KERNEL);
	if (!ledctl) {
		ret = -ENOMEM;
		goto exit_driver;
	}

#if 0
	ledctl->platform = platform;
#else
	ledctl->platform = OMP800_FC;
#endif
	mutex_init(&ledctl->update_lock);

	ledctl->pdev = platform_device_register_simple(DRVNAME, -1, NULL, 0);
	if (IS_ERR(ledctl->pdev)) {
		ret = PTR_ERR(ledctl->pdev);
		goto exit_free;
	}

	return 0;

exit_free:
	kfree(ledctl);
exit_driver:
	platform_driver_unregister(&accton_omp800_led_driver);
exit:
	return ret;
}

static void __exit accton_omp800_led_exit(void)
{
	platform_device_unregister(ledctl->pdev);
	platform_driver_unregister(&accton_omp800_led_driver);
	kfree(ledctl);
}

late_initcall(accton_omp800_led_init);
module_exit(accton_omp800_led_exit);

MODULE_AUTHOR("Brandon Chuang <brandon_chuang@accton.com.tw>");
MODULE_DESCRIPTION("accton_omp800_led driver");
MODULE_LICENSE("GPL");

