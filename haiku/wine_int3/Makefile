# Builds the wine_int3 driver and its tests on Haiku (the same way as wine_compat).
CXX = g++
CC = gcc
HDR = /boot/system/develop/headers
KFLAGS = -O2 -fno-pic -mno-red-zone -fno-omit-frame-pointer -fno-stack-protector -fno-exceptions -fno-rtti \
	-ffreestanding -fno-builtin -Wall -Wno-multichar -D_KERNEL_MODE=1 -DHAIKU_TARGET_PLATFORM_HAIKU \
	-I$(HDR)/os -I$(HDR)/os/drivers -I$(HDR)/os/kernel -I$(HDR)/os/support -I$(HDR)/private/kernel -I$(HDR)/private
ADDONS = $(HOME)/config/non-packaged/add-ons/kernel/drivers

all: wine_int3 int3test

wine_int3: wine_int3.cpp
	$(CXX) $(KFLAGS) -fPIC -c wine_int3.cpp -o wine_int3.o
	$(CXX) -shared -nostdlib -Xlinker -soname=wine_int3 -o wine_int3 wine_int3.o /boot/system/develop/lib/_KERNEL_

int3test: int3test.c
	$(CC) -O1 -msse2 -o int3test int3test.c -lpthread

install: wine_int3
	mkdir -p $(ADDONS)/bin $(ADDONS)/dev/misc
	cp wine_int3 $(ADDONS)/bin/wine_int3
	ln -sf ../../bin/wine_int3 $(ADDONS)/dev/misc/wine_int3

uninstall:
	rm -f $(ADDONS)/bin/wine_int3 $(ADDONS)/dev/misc/wine_int3

clean:
	rm -f wine_int3 wine_int3.o int3test

.PHONY: all install uninstall clean
