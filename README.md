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

It only supports 8.3 short filenames (SFN).

There is currently no locking and it's not MT-safe.

Each volume should only be mounted in a single file system.

File paths use the Linux/Darwin-style '/'.

It really only is assumed to work with 512-byte sectored storage devices. It's possible other sizes might work, but you're on your own.  The default setting of MAX_SECTOR_SIZE in fat32.h will have it refuse to mount anything else.  The sector size is part of the formatting, and it's exceedingly unlikely this will ever be used with hardware that can't use 512-byte sectors.  Still, some SD cards have soft sector sizes...
