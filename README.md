# fat32
Simple FAT32 implementation with a static footprint, suitable for firmware.

This came out of an experiment to see how useful ChatGPT would be for code generation. I mostly fixed lots of bugs
in its code, added proper error codes and handling, and turned it into C++.  I didn't want it to generate some mess
metaprogramming so I had it output C and converted it; the C++ is mainly for namespace hygiene and conciseness.
ChatGPT generated the tests, calling it a verification suite, but to my eyes it's more of a smoke test.  But it
offers at least some confidence it will actually work.  I build, run, and debug the tests on Linux using just plain
old gdb from emacs.

Footprint is about 10k all told, plus about 4.5k of data, most of which is a very simple sector cache with LRU
eviction semantics and optional write-through (which I intend to use).

If compiling with `-ffunction-sections` and `-fdata-sections`, then if linking with `-Wl,--gc-sections` the linker
will omit all unused functions, so if you don't use truncate() for example you don't have to include the code for it.

It only supports 8.3 short filenames (SFN).  While LFN mechanics aren't complicated, UTF is beyond the scope of a small library with a static footprint.  If nothing else they will need significant pure-section data tables and support functionality.  As a result, when files are created or renamed no LFNs will be created and old LFNs will remain.  They're simply ignored.  This is a significant caveat!

There is currently no locking and it's not MT-safe.

Each volume should only be mounted in a single file system.

File paths use the Linux/Darwin-style '/'.

The read-only and hidden attribute bits are advisory only and not enforced in any way.  They can be found either by `stat()` or by `File::attributes()` after `open()`.

It really only is assumed to work with 512-byte sectored storage devices. It's possible other sizes might work, but you're on your own.  The default setting of MAX_SECTOR_SIZE in fat32.h will have it refuse to mount anything else.  The sector size is part of the formatting, and it's exceedingly unlikely this will ever be used with hardware that can't use 512-byte sectors.  Still, some SD cards have soft sector sizes...

There are a few build options:
 * `-DFAT32_STRICT_MOUNT=1` - makes `mount()` perform integrity checks as its last mount step.  If these fail it returns -1 and sets the last error to `FS_NEEDS_REPAIR`.  It's still mounted and usable, and can be repaired.  The reason for this to be a build option is that it adds a dependency on the rather sizeable `fsck()` and increases mount times.
 * `-DFAT32_FSCK_REPAIR=1` - `fsck()` has an argument to fix problems (repair).  Unless this #defined the `fix` argument ignored and `fsck()` will be built to only check.  This shrinks the footprint and is useful if the target system is never intended to perform repairs but just refuse to use a dirty FS.
 * `-DFAT32_DATE_AND_TIME=1` - adds date and time management.  Needs a global function `void fat32_now(uint16_t* fat_date, uint16_t* fat_time)` that returns the current date and time in FAT format.  This is system specific.  test_fat32 has a POSIX implementation.  FAT uses the local system time, not UTC.
 