ROOT	:= $(dir $(M))

ccflags-y := -Iinclude/drm -I${ROOT}/../../../include
zocl-y := \
	sched_exec.o \
	zocl_sysfs.o \
	zocl_ioctl.o \
	zocl_ert.o \
	zocl_drv.o \
	zocl_bo.o

obj-m	+= zocl.o
