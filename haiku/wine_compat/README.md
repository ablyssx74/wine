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

Tested on hrev60299+6, hrev60419 and hrev60437 (x86_64). It depends on kernel internals, so another build may not be supported: the driver then reports that it could
not install its hook and Wine disables 32-bit programs instead of crashing. It is not part of Haiku.

## Loading address

The call it patches is a 5-byte `call rel32`, which reaches 2 GB. The kernel gives an add-on whatever address its allocator picks (`B_ANY_KERNEL_ADDRESS`), so
it varies from load to load. On hrev60419 it was seen both within reach and about 64 GB from the kernel's code; on hrev60299+6 it patched without trouble in the tests.
Why it differs is not fully known, but a test on one machine (hrev60419, the add-on reloaded 8 times on a fresh boot and 8 times after a Wine game had run for
about 5 minutes and was closed) shows that it depends on the state of the machine, not only on the kernel version: on the fresh boot all 8 loads were within reach
(1.44 to 1.57 GB from the call); after the game 7 of 8 were within reach (1.2 to 1.78 GB, nearer the top of the address space) and one was 61 GB below the call
(out of reach). That fits the region within reach filling up, so that the allocator sometimes picks a free range far below it; one game and 16 loads are too few
to be sure. The kernel's add-on loader (`elf_load_kspace()`) asks for `B_ANY_KERNEL_ADDRESS` and has no randomization, and has not changed since March 2026.
Before the jump described below existed, a load out of reach made the driver log `the driver (...) is out of reach of the call (...); not patching`, and Wine disabled
32-bit programs (Steam: "Failed to start process", error 0x54F); the way round it was to remove the add-on, wait about 10 seconds and install it again until the syslog
said `patched the call at ...`.

### The jump in the unused end of the kernel's text

The driver no longer needs to be within reach. The kernel image is mapped in whole pages, so the part of the last page of its executable segment after the last
byte of the text is unused, zero and executable, and always within reach of any call in the image (on hrev60419 it is 2,000 bytes at `0xffffffff801ce830`). When the
driver is out of reach, a 14-byte absolute jump (`jmp [rip+0]` and the 8-byte address of the hook) is written there and the call is pointed at it. Nothing is allocated and no
address is guessed: the place comes from the kernel's own ELF program headers (the image is mapped from its first byte), it must be inside the page and hold zeros (or
this driver's own earlier jump), it is written through a second mapping of the physical page like the call itself, and it is read back from the kernel's address before the
call is changed. It is cleared again when the driver is unloaded. The syslog then says `... is out of reach of the call (...): the call goes through a jump at ... in the unused
end of the kernel's text` and `patched the call at ... to go through <that address>`.

Tested on hrev60419 with the jump used on every load (even when the driver was within reach): 19 loads, two of them out of reach (61.4 GB and 62.3 GB below the call).
After the first load and after each of the last ten, a test program that makes a fault in 32-bit code (`compat_test`: the handler runs, the 32-bit code resumes and returns)
passed, including after the far load among them; a 32-bit Windows program (`syswow64\cmd.exe`) ran through Wine after the first load. Build options: `make EXTRA=-DWINE_COMPAT_CAVE_ALWAYS` uses
the jump on every load (a test build), `make EXTRA=-DWINE_COMPAT_NO_CAVE` leaves it out. The default build, which uses the jump only when the driver is out of reach, was tested with 14 further loads: two were out of reach (62 GB below the call), went through the jump and passed `compat_test`, and so did the 12 direct loads.
On hrev60437 (a nightly installed over hrev60419) the kernel file is almost the same: the same size, the same end of the executable segment, the call at the same address
(its target moved by 16 bytes) and the same 2,000 zero bytes at `0xffffffff801ce830`. The default build patched the call at boot, `compat_test` passed, and a 64-bit and a 32-bit
Windows program (`system32\cmd.exe`, `syswow64\cmd.exe`) ran under Wine. Reloading the add-on 10 times on a fresh boot gave 10 loads within reach, all followed by a passing
`compat_test`; the build that uses the jump on every load, 8 loads, all passed; and after about 5 minutes of a Wine game (CS2) 12 reloads of the default build gave one load
61.9 GB below the call, which went through the jump and passed, and 11 direct loads that passed. Close every Wine program (`wineserver -k`, then check that no `wine` or `wineserver` process is
left) before reloading the add-on.
If the kernel has no unused space there, the driver falls back as before. A stub placed near the call is also in the source behind `-DWINE_COMPAT_TRY_STUB`: the first version of
it froze a machine while it loaded, the second placement was never exercised, so the default build leaves it out.

## Install

    make                 # needs g++ on Haiku; links against /boot/system/develop/lib/_KERNEL_
    make install         # copies it to ~/config/non-packaged/add-ons/kernel/drivers and links it into dev/misc
    make uninstall

A prebuilt binary (with the jump, built against the hrev60419 headers) is in `prebuilt/` (`wine_compat.md5` has its checksum). Opening `/dev/misc/wine_compat` loads the driver (the first 32-bit
program does that); to reload a new build, remove it, wait a few seconds, then install again.

Tests (`make fs32_test compat_test fsbase_driver_test`) check the ioctls and the 32-bit `%fs` handling from two threads.
