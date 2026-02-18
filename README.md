# A simple FAT32 implementation
Simple FAT32 implementation with a static footprint, suitable for firmware.

## About this

The total footprint is about 10k all told, plus about 5-6k of data,
most of which is a very simple sector cache with LRU eviction
semantics.  The cache can be made write-through or read-write; in the
former case all writes are made to the underlying storage immediately.

If compiling with `-ffunction-sections` and `-fdata-sections`, then if
linking with `-Wl,--gc-sections` the linker will omit all unused
functions, so if you don't use truncate() for example you don't have
to include the code for it.

Only short file names are used; it completely ignores LFN directory
entries.  Adding LFN support, even as a separate layer would
substantially increase the data footprint, because adding an LFN for
an SFN requires inserting multiple directory entries prior to the SFN,
which realistically requires holding at least two full clusters in
memory. LFNs are ignored as I don't see any embedded device looking
for a small-footprint implementation to really have any use for it.  A
lot of common consumer devices, like cameras, already don't support
them.

A single recursive perimenter lock is used.  There is one for POSIX
platforms, to run the test, and another which is a no-op.  For any
other target platform it should be reimplemented in whatever way makes
sense.  If only one thread performs file I/O the no-op is fine.  (Or
if there is effectively only one thread such as many projects with
no context switching or scheduling at all.)

You probably want to Linux (or some other GNU and POSIX platform) to
work on this, so you can run the test, use the Makefile, etc.


## Caveats

Only mount each filesystem once.  Multiple mounts will produce
corruption.

File paths use the Linux/Darwin-style '/' and are always relative to
the root of the file system.  There is no notion of a working
directory, so there is no distinction between absolute and relative
paths.  In fact, any leading '/' is simply ignored.

The read-only and hidden attribute bits are advisory only and not
enforced in any way.  They can be found either by `stat()` or by
`File::attributes()` after `open()`.

It's assumed all storage devices use 512-byte sectoring. It's possible
other sizes might work, but you're on your own and changing it requires
changing constants in headers.

There is no read-only mount capability.


## Structure

FAT32 operates on a BlockDev which is passed into the constructor.


### BlockDev

BlockDevs are stacked, and what this filesystem uses for storage.

* `CacheBlockDev` - an LRU sector cache that can sit underneath the
filesystem or the partition mapper

* `GPTMap` - a GPT partition mapper that translates block numbers from
a partition to the underlying storage.  It knows of a few basic
partition GUID types and will probe for FAT32.

* `SDCard` - an SD card implementation of BlockDev that can be used to
read/write

* `PosixBlockDev` - used by the tests to access a GPT disk image

Layers can be included as needed.  The cache can be omitted, as can
the GPT mapper, for raw access to a card with a file system on it with
only the boot block (aka BIOS parameter block).


### SDIO

This implements a physical 4-bit SDIO interface.  stm32_sdio.{h,cxx}
conatains a token skeleton, using ST's HAL.  It won't compile and is
just for illustration.

A future enhancement here for the SDCard BlockDev is error resilience
and timeouts (with reset and reinit) to recover from errors.  Retry on
CRC errors.  This requires some additions to the SDIO interface.
There are also some places where the skeleton will wait indefinitely;
this is makes it a bit of a toy.

The dummy SDIO outline code will spin waits and polled transfers.  In
many actual real-world scenarios it should use interrupts, scheduling
primitives to wake the blocked thread, and DMA transfers.  However,
the dummy is a good first "blinking LED" kind of starting point.


## Repair

There is a `fsck()` method in `Fat32::FileSys` to check and repair.
The check is reasonably complete, but the repair is elementary and
doesn't handle many problems well, or at all.  Most notably it won't
repair filenames, directory links (especially . and ..), or use
majority voting to reconcile FAT consistencies (when there are 3 or
more FATs).  Repair functionality is under an `#ifdef` (see below)
since it's not easily split out into separate functions that can
simply be omitted.

## Build options

There are a few build options:

* `-DFAT32_STRICT_MOUNT=1` - makes `mount()` perform integrity checks
  as its last mount step.  If these fail it returns -1 and sets the
  last error to `FS_NEEDS_REPAIR`.  It's still mounted and usable, and
  can be repaired.  The reason for this to be a build option is that
  it adds a dependency on the rather sizeable `fsck()` and increases
  mount times.

* `-DFAT32_FSCK_REPAIR=1` - `fsck()` has an argument to fix problems
  (repair).  Unless this #defined the `fix` argument ignored and
  `fsck()` will be built to only check.  This shrinks the footprint
  and is useful if the target system is never intended to perform
  repairs but just refuse to use a dirty FS.

* `-DFAT32_DATE_AND_TIME=1` - adds date and time management.  Needs a
  global function `void fat32_now(uint16_t* fat_date, uint16_t*
  fat_time)` that returns the current date and time in FAT format.
  This is system specific.  test_fat32 has a POSIX implementation.
  FAT uses the local system time, not UTC. This is a build option
  since many small systems don't have reliable date and time
  functions, and having to provide one for no benefit, plus a little
  additional overhead, for no benefit, is pointless.  Without this the
  fields will be zero, which means midnight Jan 1, 1980.  For file
  creation time, the tenths part is always set to zero; write times
  have only seconds and access date is only a date.


## How this project came about

This came out of an experiment to see how useful ChatGPT would be for
code generation.  I think it borrowed naming and structure from
existing open source projects, mainly `dosfstools`.  Since those are
GPL I'll use the same licensing for this repo.

I needed a FAT32 filesystem with these properties, and since I didn't
really have anything to start with and wanted to create it from
scratch, it made a good test case to see how AI coding could help, or
if it could help at all.  It's not useful for the actual code, but I
found it helpful to quickly generate structure and outlines; probably
because people have solved these problems before.  I chode ChatGPT
because I could use it for free, so made a good test case; other tools
might work differently.  The code quality was very poor, but not
entirely wrong and pretty easily cleaned up.  Some bugs were terrible,
but not that hard to fix under gdb.  The ability to quickly generate
some basic tests was very useful here.

As alluded to above, a large number of bugs were fixed in the output,
some functionality added, complicated constructs simplified, and
memory footprint reduced.  Proper error codes and handling was added,
and it was turned it into C++.  I explicitly didn't want C++ with
exceptions and tons of memory allocations, or generics that create
code duplication.  ChatGPT generated the tests, calling it a
verification suite, but to my eyes it's more of a smoke test.  But it
offers at least some confidence it will actually work.  I build, run,
and debug the tests on Linux using just plain old gdb from emacs.  The
tests found tons of bugs, which were fixed.  I think the main
usefulness of ChatGPT here was to just generate a code outline and
structure - this is a huge timesaves.  Cleaning it up and adding
missing functionality was relatively simple.

When relying on AI, the following needs to be kept in mind:

* It's often not correct.  It's important to read and understand the
code it generates.

* Shortcomings may not be obvious.  These were numerous like this, for
example, the summary PSINFO wasn't properly tracked (free cluster
count, next free cluster) or written.

* The code generates is very repetitive. In this project for exampe a
lot of the file tree walking and directory iteration could be
generalized, say with C++ lambdas.  This is probably not worth the
effort for this project though, but it's something to keep in mind.

* Depending on where its associations come from, things change between
prompts.  Function names, parameter order, and parameters.  This
obviously reflects that it's using different source material.  This
needs cleaning up.

* I worked incrementally, asking to add things as I went along.  It
tried to make suggestions for imprements - some of those were good and
things I hadn't thought off, others out of scope.  Such as
defragmentation for it to become production-grade.

* It often has no notion of things that are good engineering practice,
like meaningful error codes, if none of its sources included this.
You might have to add it yourself.