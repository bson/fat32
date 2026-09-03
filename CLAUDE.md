# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A small-footprint FAT32 filesystem implementation (~10k code, ~5-6k data) written in
constrained C++ (no exceptions, no RTTI, no dynamic allocation except in `fsck()`), intended
for firmware/embedded targets but developed and tested on Linux/POSIX. See README.md for the
full design rationale and caveats.

## Build and test

```
make            # builds test_fat32
make test       # builds and runs the test suite (test_fat32.cxx)
make valgrind   # runs the test suite under valgrind
make clean
```

There is no separate unit-test filter; `test_fat32.cxx` runs its full smoke-test suite as a
single binary against a POSIX-backed disk image (`test.img`, created by the test itself using
`PosixBlockDev`). To debug a failure, build with the existing debug flags
(`CXXFLAGS_DEBUG`, already `-O0 -g -fno-inline`) and run under gdb — the repo includes a
`.gdbinit`.

Build-time feature flags (set in `Makefile` `DEFS`, also documented in README.md):
- `FAT32_STRICT_MOUNT` — run integrity checks at the end of `mount()`
- `FAT32_FSCK_REPAIR` — compile `fsck()` with repair capability, not just check
- `FAT32_DATE_AND_TIME` — enable timestamp fields (requires a `fat32_now()` implementation)
- `FAT32_LOCK_IMPL_H` — which locking header to use: `fat32_posix_excl.h` (pthread recursive
  mutex, used for the test build) or `fat32_no_excl.h` (no-op, for single-threaded firmware)
- `FAT32_LOCK_DEBUG` — adds assertions that the lock is held/owned correctly

Changing target platform assumptions (sector size, endianness) is not a build flag — it
requires editing constants in headers directly (see README.md "Caveats").

## Architecture

### BlockDev layering

Everything is built on the abstract `BlockDev` interface (`blockdev.h`): `read_blocks`,
`write_blocks`, `flush`, `sector_size`, `size`. Layers are stacked, each wrapping the one
below, and any layer can be omitted if not needed:

- `SDCard` (`sdcard.h/.cxx`) — real SD card block device (uses `sdio.h`)
- `stm32_sdio.h/.cxx` — illustrative-only SDIO HAL skeleton for STM32; will not compile, not
  meant to be built
- `GPTMap::Mapper` (`gptmap.h/.cxx`) — translates a GPT partition's LBAs to absolute LBAs on
  the underlying device; `GPTMap::Table` reads/parses the GPT and identifies partition types
  (including probing for FAT32 when the partition type GUID is ambiguous, e.g. EFI System)
- `CacheBlockDev` (`cache.h/.cxx`) — fixed-size (16-entry) LRU sector cache, write-through or
  write-back
- `PosixBlockDev` (defined in `test_fat32.cxx`) — backs the block device with a plain file, used
  only by the test suite

`Fat32::FileSys` (`fat32.h/.cxx`) is constructed with a reference to whatever `BlockDev` sits
at the top of this stack — it doesn't know or care whether there's a cache, a GPT mapper, both,
or neither underneath.

### Fat32::FileSys

Single class owns all filesystem state and exposes POSIX-like operations (`mount`, `open`,
`create`, `unlink`, `rename`, `mkdir`, `rmdir`, `opendir`/`readdir`, `fsck`, `sync`). Nested
classes `File` and `DIR` are thin handles that carry a back-pointer to the owning `FileSys` and
must not outlive it.

Key internal mechanics (`fat32.cxx`):
- A single one-sector staging buffer (`_sector`) is shared across all operations under the
  filesystem lock — `load_sector`/`store_sector` manage it. This is why `FileSys` state is
  single-threaded per critical section even though the object supports concurrent callers.
- FAT chain walking/allocation/freeing (`fat_get`, `fat_set`, `fat_allocate`, `fat_free_chain`,
  `fat_cluster_at`) and multi-FAT mirroring go through `fat_set_single` per FAT copy.
- FSINFO (free cluster count, next free hint) is tracked in memory and flushed lazily via
  `sync()` when `_fsinfo_dirty`.
- Only 8.3 short file names are supported; LFN directory entries are deliberately not
  implemented (see README.md for why).
- `fsck()` requires `calloc` for its directory-tree cluster refcount table — the only dynamic
  allocation in the codebase. It's gated behind `FAT32_FSCK_REPAIR` for repair, and always
  needed if `fsck()` is linked in at all.

### Locking

One recursive lock type, selected at compile time via `FAT32_LOCK_IMPL_H`
(`fat32_posix_excl.h` for pthreads, `fat32_no_excl.h` as a no-op for single-threaded targets).
Lock order is always `File` before `FileSys` — `FileSys` never reaches up into a `File`, so
this order can't deadlock. Any new code taking both locks must preserve that order.

### Error handling

No exceptions. Every fallible method returns `int` (0 success / -1 error) and records a
`Fat32::Error` enum value retrievable via `last_error()`/`strerror()`. `with_error()` only
sets `_last_error` if it isn't already set to something else in the same call chain — don't
overwrite an existing error with a later, less specific one.

## Provenance note

Per README.md, the original scaffolding for this project was AI-generated (ChatGPT) and
substantially rewritten/debugged by hand since. Naming may still echo `dosfstools` in places.
