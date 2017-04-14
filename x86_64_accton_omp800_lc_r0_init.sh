#!/bin/bash

########### initialize I2C bus 0 ###########
# initiate root multiplexer (PCA9548)
echo pca9548 0x76 > /sys/bus/i2c/devices/i2c-0/new_device

echo omp800_cpld1 0x60 > /sys/bus/i2c/devices/i2c-4/new_device
echo omp800_cpld2 0x62 > /sys/bus/i2c/devices/i2c-5/new_device

########### initialize I2C bus 1 ###########
# initiate root multiplexer (PCA9548)
echo pca9548 0x71 > /sys/bus/i2c/devices/i2c-1/new_device

echo pca9548 0x72 > /sys/bus/i2c/devices/i2c-1/new_device
echo pca9548 0x73 > /sys/bus/i2c/devices/i2c-1/new_device

# initiate System EEPROM
echo 24c02 0x57 > /sys/bus/i2c/devices/i2c-1/new_device

if [ -f /sys/bus/i2c/devices/4-0060/cpu_id ]
then
    CPU_ID=$(cat /sys/bus/i2c/devices/4-0060/cpu_id)
    if [ $CPU_ID == "0" ]
    then
        # initialize QSFP port 1~16 (LC-CPU-A)
        echo omp800_lc_sfp9 0x50 > /sys/bus/i2c/devices/i2c-18/new_device
        echo omp800_lc_sfp10 0x50 > /sys/bus/i2c/devices/i2c-19/new_device
        echo omp800_lc_sfp11 0x50 > /sys/bus/i2c/devices/i2c-20/new_device
        echo omp800_lc_sfp12 0x50 > /sys/bus/i2c/devices/i2c-21/new_device
        echo omp800_lc_sfp1 0x50 > /sys/bus/i2c/devices/i2c-22/new_device
        echo omp800_lc_sfp2 0x50 > /sys/bus/i2c/devices/i2c-23/new_device
        echo omp800_lc_sfp3 0x50 > /sys/bus/i2c/devices/i2c-24/new_device
        echo omp800_lc_sfp4 0x50 > /sys/bus/i2c/devices/i2c-25/new_device
        echo omp800_lc_sfp6 0x50 > /sys/bus/i2c/devices/i2c-26/new_device
        echo omp800_lc_sfp5 0x50 > /sys/bus/i2c/devices/i2c-27/new_device
        echo omp800_lc_sfp8 0x50 > /sys/bus/i2c/devices/i2c-28/new_device
        echo omp800_lc_sfp7 0x50 > /sys/bus/i2c/devices/i2c-29/new_device
        echo omp800_lc_sfp13 0x50 > /sys/bus/i2c/devices/i2c-30/new_device
        echo omp800_lc_sfp14 0x50 > /sys/bus/i2c/devices/i2c-31/new_device
        echo omp800_lc_sfp15 0x50 > /sys/bus/i2c/devices/i2c-32/new_device
        echo omp800_lc_sfp16 0x50 > /sys/bus/i2c/devices/i2c-33/new_device
    elif [ $CPU_ID == "1" ]
    then
        # initialize QSFP port 17~32 (LC-CPU-B)
        echo omp800_lc_sfp17 0x50 > /sys/bus/i2c/devices/i2c-18/new_device
        echo omp800_lc_sfp18 0x50 > /sys/bus/i2c/devices/i2c-19/new_device
        echo omp800_lc_sfp19 0x50 > /sys/bus/i2c/devices/i2c-20/new_device
        echo omp800_lc_sfp20 0x50 > /sys/bus/i2c/devices/i2c-21/new_device
        echo omp800_lc_sfp25 0x50 > /sys/bus/i2c/devices/i2c-22/new_device
        echo omp800_lc_sfp26 0x50 > /sys/bus/i2c/devices/i2c-23/new_device
        echo omp800_lc_sfp27 0x50 > /sys/bus/i2c/devices/i2c-24/new_device
        echo omp800_lc_sfp28 0x50 > /sys/bus/i2c/devices/i2c-25/new_device
        echo omp800_lc_sfp29 0x50 > /sys/bus/i2c/devices/i2c-26/new_device
        echo omp800_lc_sfp30 0x50 > /sys/bus/i2c/devices/i2c-27/new_device
        echo omp800_lc_sfp31 0x50 > /sys/bus/i2c/devices/i2c-28/new_device
        echo omp800_lc_sfp32 0x50 > /sys/bus/i2c/devices/i2c-29/new_device
        echo omp800_lc_sfp21 0x50 > /sys/bus/i2c/devices/i2c-30/new_device
        echo omp800_lc_sfp22 0x50 > /sys/bus/i2c/devices/i2c-31/new_device
        echo omp800_lc_sfp23 0x50 > /sys/bus/i2c/devices/i2c-32/new_device
        echo omp800_lc_sfp24 0x50 > /sys/bus/i2c/devices/i2c-33/new_device
    fi
fi
