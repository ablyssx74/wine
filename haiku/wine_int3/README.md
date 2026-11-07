# wine_int3: a kernel add-on that lets Wine programs use int3 / DebugBreak()

Haiku's kernel does not turn an `int3` into `SIGTRAP`: it only reports a breakpoint to the team's debugger, and a team without one is
killed by debug_server. Chromium, WebView2 and the Firefox sandbox execute `int3`, so they die without this. The add-on makes the kernel raise
`SIGTRAP` for Wine processes (and only those), which is what Wine's signal handler expects. No debugger is attached, so nothing else changes
(a team with a debugger installed loses its vector registers every time a signal handler returns, which is why Wine does not use one).

It is optional: without it Wine runs as before, and a program that executes `int3` is killed.

## What it does

* The kernel's breakpoint handler ends in a tail call, `jmp user_debug_breakpoint_hit`. The driver finds that jump by itself (the only
  `E9 rel32` in the first 96 bytes of `x86_handle_breakpoint_exception` whose target is that function and that is preceded by
  `mov $1,%edi`), and, when the first Wine process asks, points it at its own function through a second mapping of the page (the kernel
  text is read-only). Nothing is written to disk and a reboot or unloading the driver restores the kernel exactly.
* For a user-mode `int3` from a team that has marked itself, the hook moves the instruction pointer past the `int3` and sends the thread
  `SIGTRAP`. For everything else it calls the original function.
* It refuses to patch (and says why in the syslog) if the kernel does not match: another Haiku build whose code differs leaves Wine working
  as it does without the add-on.
* Wine (`dlls/ntdll/unix/signal_x86_64.c`) opens `/dev/misc/wine_int3` at process start and marks its team with an ioctl. The mark is dropped
  when the process ends. `WINE_HAIKU_INT3=0` stops Wine from using it.
* Control: ioctls on `/dev/misc/wine_int3` (status, probe, mark). `int3test probe` shows what the driver finds without patching anything.

Tested on hrev60377 (x86_64): 1.6 million traps from 8 threads with a canary in a vector register, no register lost.

## Install

    make                 # needs g++ on Haiku; links against /boot/system/develop/lib/_KERNEL_
    make install         # copies it to ~/config/non-packaged/add-ons/kernel/drivers and links it into dev/misc
    make uninstall

A prebuilt binary is in `prebuilt/` (`wine_int3.md5` has its checksum). To install it by hand:

    wineserver -k
    mkdir -p ~/config/non-packaged/add-ons/kernel/drivers/bin ~/config/non-packaged/add-ons/kernel/drivers/dev/misc
    cp prebuilt/wine_int3 ~/config/non-packaged/add-ons/kernel/drivers/bin/wine_int3
    ln -sf ~/config/non-packaged/add-ons/kernel/drivers/bin/wine_int3 ~/config/non-packaged/add-ons/kernel/drivers/dev/misc/wine_int3

The driver loads when the first Wine program starts. Check with `ls /dev/misc/wine_int3` and `grep wine_int3 /var/log/syslog | tail -5`
("patched the jump at ..." means it is active, "not patching" that this kernel is not recognised). To update or remove it, close all Wine
programs, remove the two files, wait about 10 seconds, and install the new one.
