obj-m := gpu_peek_gb206.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all module selftest clean run

all: selftest module

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

selftest:
	$(CC) -Wall -Wextra -Werror -O2 -o selftest selftest.c
	./selftest

# Needs root: loads the module, captures the log, unloads it again.
run: all
	sudo ./run.sh

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f selftest
