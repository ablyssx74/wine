# wine_compat: a kernel add-on that lets Wine run 32-bit programs on 64-bit Haiku

Wine runs a 32-bit Windows program inside a 64-bit process (WoW64) and has to switch the `%fs` segment base every time it moves between
64-bit and 32-bit code. Stock Haiku has no way to set the FS base of a thread that survives a thread switch or a signal. This drop-in
kernel driver provides it, without patching or replacing the kernel.

## What it does

* Hooks the kernel's signal delivery (it finds the one `call arch_setup_signal_frame` inside `handle_signals`, so it needs a kernel that
  exports `handle_signals`, `x86_get_current_iframe` and `arch_setup_signal_frame`).
* Keeps a per-thread FS base, found at run time through the offset of `Thread::user_local_storage` (the word at `gs:0` is the `Thread*`).
* Loads the 32-bit `%fs` selector (0x23) on every CPU, because a null selector makes every 32-bit `%fs:` access fault.
* Is controlled by ioctls on `/dev/misc/wine_compat` (status, set/get FS base, set the FS base of another thread, probe). The Wine port
  (`dlls/ntdll/unix/signal_x86_64.c`) opens it, checks that the signal hook is active and uses it; without it, 64-bit programs run as
  before and a 32-bit program stops with a message.

Tested on hrev60299+6 (x86_64). It depends on kernel internals, so another build may not be supported: the driver then reports that it could
not install its hook and Wine disables 32-bit programs instead of crashing. It is not part of Haiku.

## Install

    make                 # needs g++ on Haiku; links against /boot/system/develop/lib/_KERNEL_
    make install         # copies it to ~/config/non-packaged/add-ons/kernel/drivers and links it into dev/misc
    make uninstall

A prebuilt binary is in `prebuilt/` (`wine_compat.md5` has its checksum). Opening `/dev/misc/wine_compat` loads the driver (the first 32-bit
program does that); to reload a new build, remove it, wait a few seconds, then install again.

Tests (`make fs32_test compat_test fsbase_driver_test`) check the ioctls and the 32-bit `%fs` handling from two threads.
