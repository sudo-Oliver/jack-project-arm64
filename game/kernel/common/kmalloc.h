#pragma once

#include "common/common_types.h"

#include "game/kernel/common/Ptr.h"

/*!
 * A kheap has a top/bottom linear allocator
 */
struct kheapinfo {
  Ptr<u8> base;      //! beginning of heap
  Ptr<u8> top;       //! current location of bottom of top allocations
  Ptr<u8> current;   //! current location of top of bottom allocations
  Ptr<u8> top_base;  //! end of heap
};

// Kernel heaps
extern Ptr<kheapinfo> kglobalheap;
extern Ptr<kheapinfo> kdebugheap;
#if defined(__aarch64__) && defined(__APPLE__)
extern Ptr<kheapinfo> kcodeheap;  // MAP_JIT code region; data heaps are regular mmap
#endif
extern bool kheaplogging;

// flags for kmalloc/ksmalloc
constexpr u32 KMALLOC_TOP = 0x2000;     //! Flag to allocate temporary memory from heap top
constexpr u32 KMALLOC_MEMSET = 0x1000;  //! Flag to clear memory
constexpr u32 KMALLOC_ALIGN_256 = 0x100;
constexpr u32 KMALLOC_ALIGN_64 = 0x40;
constexpr u32 KMALLOC_ALIGN_16 = 0x10;

#if defined(__aarch64__) && defined(__APPLE__)
//! Flag for memory that will hold code. Apple Silicon will not map a page both writable and
//! executable, so such an allocation is aligned to a page and rounded up to whole pages: nothing
//! that has to stay writable may share a page with it. Small objects named "function" are the
//! exception -- they are packed together on pages that hold nothing else.
constexpr u32 KMALLOC_EXECUTABLE = 0x4000;
#endif

void kmalloc_init_globals_common();

Ptr<u8> ksmalloc(Ptr<kheapinfo> heap, s32 size, u32 flags, char const* name);
Ptr<kheapinfo> kheapstatus(Ptr<kheapinfo> heap);
Ptr<kheapinfo> kinitheap(Ptr<kheapinfo> heap, Ptr<u8> mem, s32 size);
u32 kheapused(Ptr<kheapinfo> heap);
Ptr<u8> kmalloc(Ptr<kheapinfo> heap, s32 size, u32 flags, char const* name);
void kfree(Ptr<u8> a);