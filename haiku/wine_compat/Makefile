# Builds the wine_compat kernel add-on and its tests on Haiku.  make install puts it in the user add-ons folder.
CXX = g++
CC = gcc
HDR = /boot/system/develop/headers
KFLAGS = -O2 -fno-pic -mno-red-zone -fno-omit-frame-pointer -fno-stack-protector -fno-exceptions -fno-rtti \
	-ffreestanding -fno-builtin -Wall -Wno-multichar -D_KERNEL_MODE=1 -DHAIKU_TARGET_PLATFORM_HAIKU \
	-I$(HDR)/os -I$(HDR)/os/drivers -I$(HDR)/os/kernel -I$(HDR)/os/support -I$(HDR)/private/kernel -I$(HDR)/private
ADDONS = $(HOME)/config/non-packaged/add-ons/kernel/drivers

all: wine_compat compat_test fsbase_driver_test fs32_test

wine_compat: wine_compat.cpp
	$(CXX) $(KFLAGS) -fPIC -c wine_compat.cpp -o wine_compat.o
	$(CXX) -shared -nostdlib -Xlinker -soname=wine_compat -o wine_compat wine_compat.o /boot/system/develop/lib/_KERNEL_

compat_test: compat_test.c
	$(CC) -O0 -o compat_test compat_test.c

install: wine_compat
	mkdir -p $(ADDONS)/bin $(ADDONS)/dev/misc
	cp wine_compat $(ADDONS)/bin/wine_compat
	ln -sf ../../bin/wine_compat $(ADDONS)/dev/misc/wine_compat

uninstall:
	rm -f $(ADDONS)/bin/wine_compat $(ADDONS)/dev/misc/wine_compat

fsbase_driver_test: fsbase_driver_test.c
	$(CC) -O0 -o fsbase_driver_test fsbase_driver_test.c -lpthread

fs32_test: fs32_test.c
	$(CC) -O0 -o fs32_test fs32_test.c -lpthread

clean:
	rm -f wine_compat wine_compat.o compat_test

.PHONY: all install uninstall clean
