obj-m := gwmi-wmi.o

# Под DKMS KERNELRELEASE задаётся снаружи; при ручной сборке берём текущее ядро.
KDIR ?= /lib/modules/$(if $(KERNELRELEASE),$(KERNELRELEASE),$(shell uname -r))/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
