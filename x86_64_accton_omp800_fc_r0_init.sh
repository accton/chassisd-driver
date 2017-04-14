#!/bin/bash
########### initialize I2C bus 0 ###########
# initiate root multiplexer (PCA9548)
echo pca9548 0x76 > /sys/bus/i2c/devices/i2c-0/new_device

# initiate local cpld
echo omp800_cpld1 0x60 > /sys/bus/i2c/devices/i2c-4/new_device

# initiate chassis fan
echo omp800_fc_fan 0x66 > /sys/bus/i2c/devices/i2c-2/new_device
echo omp800_fc_fan 0x66 > /sys/bus/i2c/devices/i2c-8/new_device

# initiate line card / fabric card power on/off controller
echo adm1278 0x10 > /sys/bus/i2c/devices/i2c-6/new_device
echo adm1278 0x13 > /sys/bus/i2c/devices/i2c-6/new_device
echo adm1278 0x50 > /sys/bus/i2c/devices/i2c-6/new_device
echo adm1278 0x53 > /sys/bus/i2c/devices/i2c-6/new_device
echo adm1278 0x44 > /sys/bus/i2c/devices/i2c-6/new_device
echo adm1278 0x47 > /sys/bus/i2c/devices/i2c-6/new_device

# initiate remote cpld
echo omp800_cpld_remote 0x65 > /sys/bus/i2c/devices/i2c-6/new_device
echo omp800_cpld_remote 0x64 > /sys/bus/i2c/devices/i2c-6/new_device
echo omp800_cpld_remote 0x67 > /sys/bus/i2c/devices/i2c-6/new_device
echo omp800_cpld_remote 0x66 > /sys/bus/i2c/devices/i2c-6/new_device
echo omp800_cpld_remote 0x60 > /sys/bus/i2c/devices/i2c-6/new_device
echo omp800_cpld_remote 0x61 > /sys/bus/i2c/devices/i2c-6/new_device


########### initialize I2C bus 1 ###########
# initiate root multiplexer (PCA9548)
echo pca9548 0x71 > /sys/bus/i2c/devices/i2c-1/new_device

# initiate PDU
echo omp800_fc_pdu 0x61 > /sys/bus/i2c/devices/i2c-10/new_device

# initialize multiplexer (PCA9548)
echo pca9548 0x74 > /sys/bus/i2c/devices/i2c-10/new_device

# initialize psu-pmbus
echo pfe3000 0x10 > /sys/bus/i2c/devices/i2c-20/new_device
echo pfe3000 0x10 > /sys/bus/i2c/devices/i2c-19/new_device
echo pfe3000 0x10 > /sys/bus/i2c/devices/i2c-18/new_device

# initiate System EEPROM
echo 24c02 0x57 > /sys/bus/i2c/devices/i2c-1/new_device
