THIS_ACCEL_MODULES_DIR := $(shell dirname $(lastword $(MAKEFILE_LIST)))
TOP_ACCEL_MODULES_DIR := $(shell dirname $(THIS_ACCEL_MODULES_DIR))

include $(TOP_ACCEL_MODULES_DIR)/Kbuild.common

obj-m += ipoe.o

default:
	make -C $(KDIR) M=$(PWD) modules
