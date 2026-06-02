/*!
 * @file runtime.cpp
 * Setup and launcher for the runtime.
 */

#include "common/common_types.h"
#ifdef OS_POSIX
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach/mach.h>
#endif
#elif _WIN32
#include <io.h>

#include "third-party/mman/mman.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include <chrono>
#include <cstring>
#include <thread>

#include "runtime.h"

#include "common/cross_os_debug/xdbg.h"
#include "common/global_profiler/GlobalProfiler.h"
#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/versions/versions.h"

#include "game/external/discord.h"
#include "game/graphics/gfx.h"
#include "game/kernel/common/fileio.h"
#include "game/kernel/common/kdgo.h"
#include "game/kernel/common/kdsnetm.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/klisten.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kmemcard.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kboot.h"
#include "game/kernel/jak1/kdgo.h"
#include "game/kernel/jak1/klisten.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/kernel/jak2/kboot.h"
#include "game/kernel/jak2/kdgo.h"
#include "game/kernel/jak2/klisten.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/kernel/jak3/kboot.h"
#include "game/kernel/jak3/kdgo.h"
#include "game/kernel/jak3/klisten.h"
#include "game/kernel/jak3/kscheme.h"
#include "game/kernel/jakx/kboot.h"
#include "game/overlord/common/fake_iso.h"
#include "game/overlord/common/iso.h"
#include "game/overlord/common/sbank.h"
#include "game/overlord/common/srpc.h"
#include "game/overlord/common/ssound.h"
#include "game/overlord/jak1/dma.h"
#include "game/overlord/jak1/fake_iso.h"
#include "game/overlord/jak1/iso.h"
#include "game/overlord/jak1/iso_queue.h"
#include "game/overlord/jak1/overlord.h"
#include "game/overlord/jak1/ramdisk.h"
#include "game/overlord/jak1/srpc.h"
#include "game/overlord/jak1/stream.h"
#include "game/overlord/jak2/dma.h"
#include "game/overlord/jak2/iso_cd.h"
#include "game/overlord/jak2/iso_queue.h"
#include "game/overlord/jak2/overlord.h"
#include "game/overlord/jak2/spustreams.h"
#include "game/overlord/jak2/srpc.h"
#include "game/overlord/jak2/ssound.h"
#include "game/overlord/jak2/stream.h"
#include "game/overlord/jak2/streamlist.h"
#include "game/overlord/jak2/vag.h"
#include "game/overlord/jak3/overlord.h"
#include "game/system/Deci2Server.h"
#include "game/system/iop_thread.h"
#include "sce/deci2.h"
#include "sce/iop.h"
#include "sce/libcdvd_ee.h"
#include "sce/sif_ee.h"
#include "system/SystemThread.h"

u8* g_ee_main_mem = nullptr;
bool g_ee_jit_code_dirty = false;
#if defined(__APPLE__) && defined(__aarch64__)
// 16MB GOAL execution stack in normal PROT_READ|PROT_WRITE memory (not MAP_JIT).
// On Darwin 25, GOAL stack cannot be in MAP_JIT memory (W^X enforcement).
static uint8_t g_goal_jit_stack_buf[16 * 1024 * 1024];
u8* g_goal_jit_stack_top = g_goal_jit_stack_buf + sizeof(g_goal_jit_stack_buf);
#else
u8* g_goal_jit_stack_top = nullptr;
#endif
std::thread::id g_main_thread_id = std::thread::id();
GameVersion g_game_version = GameVersion::Jak1;
const char* g_current_goal_module = "(none)";
BackgroundWorker g_background_worker;
int g_server_port = DECI2_PORT;

namespace {

int g_argc = 0;
const char** g_argv = nullptr;

/*!
 * SystemThread function for running the DECI2 communication with the GOAL compiler.
 */

void deci2_runner(SystemThreadInterface& iface) {
#if defined(__APPLE__) && defined(__aarch64__)
  pthread_jit_write_protect_np(0);
#endif
  // callback function so the server knows when to give up and shutdown
  std::function<bool()> shutdown_callback = [&]() { return iface.get_want_exit(); };

  // create and register server
  Deci2Server server(shutdown_callback, DECI2_PORT - 1 + (int)g_game_version);
  ee::LIBRARY_sceDeci2_register(&server);

  // now its ok to continue with initialization
  iface.initialization_complete();

  // in our own thread, wait for the EE to register the first protocol driver
  lg::debug("[DECI2] Waiting for EE to register protos");
  if (!server.wait_for_protos_ready()) {
    // requested shutdown before protos became ready.
    return;
  }
  // then allow the server to accept connections
  bool server_ok = server.init_server();
  if (!server_ok) {
    lg::error("[DECI2] failed to initialize, REPL will not work.");
  }

  lg::debug("[DECI2] Waiting for listener...");
  bool saw_listener = false;
  while (!iface.get_want_exit()) {
    if (server_ok && server.is_client_connected()) {
      if (!saw_listener) {
        lg::debug("[DECI2] Connected!");
      }
      saw_listener = true;
      // we have a listener, run!
      server.read_data();
    } else {
      // no connection yet.  Do a sleep so we don't spam checking the listener.
      std::this_thread::sleep_for(std::chrono::microseconds(50000));
    }
  }
}

// EE System

/*!
 * SystemThread Function for the EE (PS2 Main CPU)
 */
void ee_runner(SystemThreadInterface& iface) {
#if defined(__APPLE__) && defined(__aarch64__)
  // Install a per-thread alternate signal stack so the SIGILL handler (SA_ONSTACK)
  // can run even if the EE/GOAL thread stack pointer is corrupt at crash time.
  static char s_ee_altstack[65536];
  stack_t ee_ss{};
  ee_ss.ss_sp = s_ee_altstack;
  ee_ss.ss_size = sizeof(s_ee_altstack);
  ee_ss.ss_flags = 0;
  sigaltstack(&ee_ss, nullptr);
#endif
  prof().root_event();
  // Allocate Main RAM (EE memory).
  //
  // On Darwin ARM64 (macOS 26+ / Darwin 25): W^X is enforced via APRR hardware.
  // MAP_JIT is required for any page that will be executed after being written.
  // Strategy (validated by tools/arm64_jit_layout_test.cpp Phase 0):
  //   1. Allocate the full EE as MAP_JIT (kernel honours the address hint).
  //   2. Overwrite the two data regions with regular mmap pages (MAP_FIXED).
  //      The 16 MB code region [EE_CODE_HEAP_START, EE_CODE_HEAP_END) stays MAP_JIT.
  //   3. GOAL heap writes land in the data regions → no W^X SIGBUS.
  //      The linker toggles write-protect around code-region writes explicitly.
  // NOTE: MAP_FIXED over a MAP_JIT range replaces those VAs with regular pages
  // (non-MAP_JIT physical pages). This is an observed-to-work Darwin behaviour;
  // vm_remap aliasing of MAP_JIT pages fails with KERN_PROTECTION_FAILURE (kr=2).
  if (EE_MEM_LOW_MAP) {
    g_ee_main_mem =
        (u8*)mmap((void*)0x10000000, EE_MAIN_MEM_SIZE, PROT_EXEC | PROT_READ | PROT_WRITE,
#if defined(__aarch64__) && defined(__APPLE__)
                  MAP_ANONYMOUS | MAP_PRIVATE | MAP_JIT, -1, 0);
#elif defined(__APPLE__)
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
#else
                  MAP_ANONYMOUS | MAP_32BIT | MAP_PRIVATE | MAP_POPULATE, 0, 0);
#endif
  } else {
#if defined(__aarch64__) && defined(__APPLE__)
    // Apple Silicon has a 16 KB hardware page size, so the null-guard prefix
    // and all MAP_FIXED replacements must be 16 KB aligned.
    // Allocate EE_MAIN_MEM_SIZE + 16384: the first 16 KB is the null-guard.
    // g_ee_main_mem = raw + 16384, so EE[-4] (= raw + 16380) reads zero
    // (MAP_ANONYMOUS zero-fills), simulating PS2 null-pointer type-tag behaviour.
    static constexpr size_t kNullGuard = 16384;
    void* raw = mmap((void*)EE_MAIN_MEM_MAP, EE_MAIN_MEM_SIZE + kNullGuard,
                     PROT_EXEC | PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE | MAP_JIT, -1, 0);
    if (raw == MAP_FAILED) {
      lg::debug("Failed to initialize main memory! {}", strerror(errno));
      iface.initialization_complete();
      return;
    }
    g_ee_main_mem = (u8*)raw + kNullGuard;
    // The null-guard (raw[0..kNullGuard)) is zero-filled by MAP_ANONYMOUS.
    // No mprotect needed: MAP_JIT exec mode makes it naturally read-only.
    lg::info("Null-guard page at 0x{:016x} (EE base 0x{:016x})", (u64)raw, (u64)g_ee_main_mem);
#else
    g_ee_main_mem =
        (u8*)mmap((void*)EE_MAIN_MEM_MAP, EE_MAIN_MEM_SIZE, PROT_EXEC | PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
#endif
  }

#if !defined(__aarch64__) || !defined(__APPLE__)
  if (g_ee_main_mem == (u8*)(-1)) {
    lg::debug("Failed to initialize main memory! {}", strerror(errno));
    iface.initialization_complete();
    return;
  }
#endif

#if defined(__aarch64__) && defined(__APPLE__)
  // Replace data regions with regular (non-MAP_JIT) pages so GOAL heap writes
  // in exec mode produce no SIGBUS.  The code region [EE_CODE_HEAP_START,
  // EE_CODE_HEAP_END) is left as MAP_JIT.
  //
  // macOS MAP_FIXED over a MAP_JIT region must start at the allocation base (raw).
  // r1 covers [raw, raw + EE_CODE_HEAP_START + kNullGuard): the 16 KB null-guard
  // plus data region 1.  All bounds are 16 KB page-aligned (Apple Silicon).
  {
    static constexpr size_t kNullGuard = 16384;
    void* r1 = mmap((u8*)g_ee_main_mem - kNullGuard, EE_CODE_HEAP_START + kNullGuard,
                    PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    void* r2 = mmap(g_ee_main_mem + EE_CODE_HEAP_END,
                    EE_MAIN_MEM_SIZE - EE_CODE_HEAP_END,
                    PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (r1 == MAP_FAILED || r2 == MAP_FAILED) {
      lg::error("EE data-region split mmap failed: {}", strerror(errno));
      iface.initialization_complete();
      return;
    }
  }
  // Start in write mode for C-side init (memset, linker, kinitheap, etc.).
  pthread_jit_write_protect_np(0);
#endif

  lg::info("Main memory mapped at 0x{:016x}", (u64)(g_ee_main_mem));
  lg::info("Main memory size 0x{:x} bytes ({:.3f} MB)", EE_MAIN_MEM_SIZE,
           (double)EE_MAIN_MEM_SIZE / (1 << 20));

  lg::info("[EE] Initialization complete!");
  iface.initialization_complete();

  lg::info("[EE] Run!");
  memset((void*)g_ee_main_mem, 0, EE_MAIN_MEM_SIZE);

  // On x86-64, make the first 512 kB PROT_NONE to catch null GOAL pointer dereferences.
  // On ARM64/macOS: skip the 512 KB PROT_READ zone. The native ARM64 stack (ARM64 SP) is set
  // to EE top by call_goal_on_stack and grows DOWN through EE memory. GOAL process stacks are
  // allocated from the heap at EE+0x13FD20, but the kernel ARM64 SP can drift into the low
  // EE region during game boot. A 512 KB PROT_READ zone would block those stack writes with
  // KERN_PROTECTION_FAILURE (the crash we saw). The null guard (16 KB before g_ee_main_mem)
  // already handles null-read detection; the additional 512 KB write guard is not needed here.
#if defined(__aarch64__) && defined(__APPLE__)
  // No low-memory protection on ARM64 — null guard prefix handles null-pointer reads.
#else
  mprotect((void*)g_ee_main_mem, EE_MAIN_MEM_LOW_PROTECT, PROT_NONE);
#endif
  fileio_init_globals();
  jak1::kboot_init_globals();
  jak2::kboot_init_globals();
  jak3::kboot_init_globals();

  kboot_init_globals_common();
  kdgo_init_globals();
  jak1::kdgo_init_globals();
  jak2::kdgo_init_globals();
  jak3::kdgo_init_globals();

  kdsnetm_init_globals_common();
  klink_init_globals();

  kmachine_init_globals_common();
  jak1::kscheme_init_globals();
  jak2::kscheme_init_globals();
  jak3::kscheme_init_globals();
  kscheme_init_globals_common();
  kmalloc_init_globals_common();

  klisten_init_globals();
  jak1::klisten_init_globals();
  jak2::klisten_init_globals();
  jak3::klisten_init_globals();

  jak2::vag_init_globals();

  jak2::init_globals_streamlist();

  kmemcard_init_globals();
  kprint_init_globals_common();
  // Added for OpenGOAL's debugger
  xdbg::allow_debugging();

  switch (g_game_version) {
    case GameVersion::Jak1:
      jak1::goal_main(g_argc, g_argv);
      break;
    case GameVersion::Jak2:
      jak2::goal_main(g_argc, g_argv);
      break;
    case GameVersion::Jak3:
      jak3::goal_main(g_argc, g_argv);
      break;
    case GameVersion::JakX:
      jakx::goal_main(g_argc, g_argv);
    default:
      ASSERT_MSG(false, "Unsupported game version");
  }
  lg::debug("[EE] Done!");

  //  // kill the IOP todo
  iop::LIBRARY_kill();

  // after main returns, trigger a shutdown.
  iface.trigger_shutdown();
}

/*!
 * SystemThread Function for the EE Worker Thread (general purpose background tasks from the EE to
 * be non-blocking)
 */
void ee_worker_runner(SystemThreadInterface& iface) {
#if defined(__APPLE__) && defined(__aarch64__)
  pthread_jit_write_protect_np(0);
#endif
  iface.initialization_complete();
  while (!iface.get_want_exit()) {
    const auto queues_weres_empty = !g_background_worker.process_queues();
    if (queues_weres_empty) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

/*!
 * SystemThread function for running the IOP (separate I/O Processor)
 */
void iop_runner(SystemThreadInterface& iface, GameVersion version) {
#if defined(__APPLE__) && defined(__aarch64__)
  // Darwin 25: new threads start in MAP_JIT exec mode; IOP only writes to EE memory.
  pthread_jit_write_protect_np(0);
#endif
  prof().root_event();
  prof().begin_event("iop-init");
  IOP iop;
  lg::debug("[IOP] Restart!");
  iop.reset_allocator();
  ee::LIBRARY_sceSif_register(&iop);
  iop::LIBRARY_register(&iop);
  Gfx::register_vsync_callback([&iop]() { iop.kernel.signal_vblank(); });

  if (version != GameVersion::Jak3 && version != GameVersion::JakX) {
    jak1::dma_init_globals();
    jak2::dma_init_globals();

    iso_init_globals();
    jak1::iso_init_globals();
    jak2::iso_init_globals();

    fake_iso_init_globals();
    jak1::fake_iso_init_globals();
    jak2::iso_cd_init_globals();

    jak1::iso_queue_init_globals();
    jak2::iso_queue_init_globals();

    jak2::spusstreams_init_globals();
    jak1::ramdisk_init_globals();
    sbank_init_globals();

    // soundcommon
    jak1::srpc_init_globals();
    jak2::srpc_init_globals();
    srpc_init_globals();
    ssound_init_globals();
    jak2::ssound_init_globals();

    jak1::stream_init_globals();
    jak2::stream_init_globals();
  }

  prof().end_event();
  iface.initialization_complete();

  lg::debug("[IOP] Wait for OVERLORD to start...");
  {
    auto p = scoped_prof("iop-wait-for-ee");
    iop.wait_for_overlord_start_cmd();
  }
  if (iop.status == IOP_OVERLORD_INIT) {
    lg::debug("[IOP] Run!");
  } else {
    lg::debug("[IOP] Shutdown!");
    return;
  }

  iop.reset_allocator();

  // init

  bool complete = false;
  {
    auto p = scoped_prof("overlord-start");
    switch (version) {
      case GameVersion::Jak1:
        jak1::start_overlord_wrapper(iop.overlord_argc, iop.overlord_argv, &complete);
        break;
      case GameVersion::Jak2:
        jak2::start_overlord_wrapper(iop.overlord_argc, iop.overlord_argv, &complete);
        break;
      case GameVersion::Jak3:
      case GameVersion::JakX:
        jak3::start_overlord_wrapper(&complete);
        break;
      default:
        ASSERT_NOT_REACHED();
    }
  }

  {
    auto p = scoped_prof("overlord-wait-for-init");
    while (complete == false) {
      iop.kernel.dispatch();
    }
  }

  // unblock the EE, the overlord is set up!
  iop.signal_overlord_init_finish();

  // IOP Kernel loop
  while (!iface.get_want_exit() && !iop.want_exit) {
    // prof().root_event();
    // The IOP scheduler informs us of how many microseconds are left until it has something to do.
    // So we can wait for that long or until something else needs it to wake up.
    auto wait_duration = iop.kernel.dispatch();
    if (wait_duration) {
      iop.wait_run_iop(*wait_duration);
    }
  }

  Gfx::clear_vsync_callback();
}
}  // namespace

/*!
 * SystemThread function for running NothingTM.
 */
void null_runner(SystemThreadInterface& iface) {
  iface.initialization_complete();
}

#if defined(__APPLE__) && defined(__aarch64__)
// Rich crash context dump: dladdr symbol lookup, GPR state, surrounding instructions,
// and backtrace. Called from both the SIGBUS and SIGILL handlers.
// Uses only async-signal-safe calls (write, dladdr, backtrace, backtrace_symbols_fd).
static void dump_arm64_crash_context(uint64_t pc,
                                     uint64_t lr,
                                     uint64_t sp,
                                     const uint64_t* xregs,  // x0..x28, 29 elements
                                     uint64_t fp,
                                     uintptr_t fault_addr) {
  char buf[512];
  int n;

  // Identify the function at PC via dladdr
  Dl_info di{};
  dladdr((void*)pc, &di);
  n = __builtin_snprintf(buf, sizeof(buf),
    "[EE-CRASH] PC  0x%016llx  %s ! %s + 0x%llx\n",
    (unsigned long long)pc,
    di.dli_fname ? di.dli_fname : "?",
    di.dli_sname ? di.dli_sname : "?",
    di.dli_saddr ? (unsigned long long)(pc - (uintptr_t)di.dli_saddr) : 0ULL);
  write(2, buf, n);

  // Identify the caller at LR
  Dl_info di_lr{};
  dladdr((void*)lr, &di_lr);
  n = __builtin_snprintf(buf, sizeof(buf),
    "[EE-CRASH] LR  0x%016llx  %s ! %s + 0x%llx\n",
    (unsigned long long)lr,
    di_lr.dli_fname ? di_lr.dli_fname : "?",
    di_lr.dli_sname ? di_lr.dli_sname : "?",
    di_lr.dli_saddr ? (unsigned long long)(lr - (uintptr_t)di_lr.dli_saddr) : 0ULL);
  write(2, buf, n);

  // Current GOAL module
  n = __builtin_snprintf(buf, sizeof(buf),
    "[EE-CRASH] module   %s\n",
    g_current_goal_module ? g_current_goal_module : "(unknown)");
  write(2, buf, n);

  // Fault address context
  if (g_ee_main_mem) {
    uintptr_t base = (uintptr_t)g_ee_main_mem;
    uintptr_t code_heap_start = base + EE_CODE_HEAP_START;
    n = __builtin_snprintf(buf, sizeof(buf),
      "[EE-CRASH] fault addr 0x%016llx  EE offset 0x%llx\n"
      "[EE-CRASH] PC code-heap offset 0x%llx  LR code-heap offset 0x%llx\n",
      (unsigned long long)fault_addr,
      (unsigned long long)(fault_addr - base),
      (unsigned long long)(pc - code_heap_start),
      (unsigned long long)(lr - code_heap_start));
    write(2, buf, n);
  }

  // GPR dump x0-x28
  write(2, "[EE-CRASH] GPRs:\n", 17);
  for (int i = 0; i < 28; i += 4) {
    int lim = (i + 4 <= 28) ? 4 : (28 - i);
    switch (lim) {
      case 4: n = __builtin_snprintf(buf, sizeof(buf),
          "  x%02d=%016llx x%02d=%016llx x%02d=%016llx x%02d=%016llx\n",
          i,   (unsigned long long)xregs[i],
          i+1, (unsigned long long)xregs[i+1],
          i+2, (unsigned long long)xregs[i+2],
          i+3, (unsigned long long)xregs[i+3]); break;
      case 3: n = __builtin_snprintf(buf, sizeof(buf),
          "  x%02d=%016llx x%02d=%016llx x%02d=%016llx\n",
          i,   (unsigned long long)xregs[i],
          i+1, (unsigned long long)xregs[i+1],
          i+2, (unsigned long long)xregs[i+2]); break;
      default: n = __builtin_snprintf(buf, sizeof(buf),
          "  x%02d=%016llx\n",
          i,   (unsigned long long)xregs[i]); break;
    }
    write(2, buf, n);
  }
  n = __builtin_snprintf(buf, sizeof(buf),
    "  x28=%016llx fp=%016llx lr=%016llx sp=%016llx\n",
    (unsigned long long)xregs[28],
    (unsigned long long)fp,
    (unsigned long long)lr,
    (unsigned long long)sp);
  write(2, buf, n);

  // Instructions around PC (16 before, 8 after)
  write(2, "[EE-CRASH] Instructions around PC:\n", 35);
  {
    uintptr_t ee_base = (uintptr_t)g_ee_main_mem;
    uintptr_t ee_end = ee_base + EE_MAIN_MEM_SIZE;
    if (pc >= ee_base && pc + (8 * sizeof(uint32_t)) < ee_end &&
        pc >= ee_base + (16 * sizeof(uint32_t))) {
      const uint32_t* code = (const uint32_t*)pc;
      for (int i = -16; i <= 8; i++) {
        n = __builtin_snprintf(buf, sizeof(buf),
          "  [%+3d] 0x%016llx: 0x%08x%s\n",
          i, (unsigned long long)(code + i), code[i],
          (i == 0) ? "  <-- FAULT" : "");
        write(2, buf, n);
      }
    } else {
      write(2, "  PC outside EE memory; skipped\n", 32);
    }
  }

  // Dump EE memory around x0 (EE-relative pointer = first arg at crash call site)
  // and x1 (second arg). Both are EE-relative offsets from x22 (g_ee_main_mem).
  {
    uintptr_t ee_base = (uintptr_t)g_ee_main_mem;
    uintptr_t ee_end  = ee_base + EE_MAIN_MEM_SIZE;
    // x0 and x1 at crash: likely the args to the function being called (BLR X8)
    uint32_t x0_rel = (uint32_t)xregs[0];
    uint32_t x1_rel = (uint32_t)xregs[1];
    n = __builtin_snprintf(buf, sizeof(buf),
      "[EE-CRASH] EE args at crash: x0(EE-rel)=0x%08x  x1(EE-rel)=0x%08x  x22(base)=0x%016llx\n",
      x0_rel, x1_rel, (unsigned long long)xregs[22]);
    write(2, buf, n);
    // Dump EE[x0-4 .. x0+32]: shows the object x0 points into (task-cstage or task-control)
    uintptr_t x0_host = ee_base + x0_rel;
    if (x0_rel >= 4 && x0_host + 36 < ee_end) {
      write(2, "[EE-CRASH] EE[x0-4..x0+32]:\n", 29);
      const uint32_t* mem = (const uint32_t*)(x0_host - 4);
      for (int i = -1; i <= 8; i++) {
        n = __builtin_snprintf(buf, sizeof(buf),
          "  [%+3d] EE+0x%08x: 0x%08x\n",
          i*4, (uint32_t)(x0_rel - 4 + i*4), mem[i+1]);
        write(2, buf, n);
      }
    }
    // Dump EE[x1-4 .. x1+32] if x1 looks valid
    uintptr_t x1_host = ee_base + x1_rel;
    if (x1_rel >= 4 && x1_host + 36 < ee_end) {
      write(2, "[EE-CRASH] EE[x1-4..x1+32]:\n", 29);
      const uint32_t* mem2 = (const uint32_t*)(x1_host - 4);
      for (int i = -1; i <= 8; i++) {
        n = __builtin_snprintf(buf, sizeof(buf),
          "  [%+3d] EE+0x%08x: 0x%08x\n",
          i*4, (uint32_t)(x1_rel - 4 + i*4), mem2[i+1]);
        write(2, buf, n);
      }
    }
    // x20 is GOAL pp on ARM64. Dump first process fields to diagnose null main/top thread.
    uint32_t pp_rel = (uint32_t)xregs[20];
    uintptr_t pp_host = ee_base + pp_rel;
    if (pp_rel >= 4 && pp_host + 160 < ee_end) {
      write(2, "[EE-CRASH] EE[pp-4..pp+156]:\n", 30);
      const uint32_t* mem3 = (const uint32_t*)(pp_host - 4);
      for (int i = -1; i <= 39; i++) {
        n = __builtin_snprintf(buf, sizeof(buf),
          "  [%+4d] EE+0x%08x: 0x%08x\n",
          i * 4, (uint32_t)(pp_rel - 4 + i * 4), mem3[i + 1]);
        write(2, buf, n);
      }
    }
  }

  // Instructions around LR (caller site) — helps identify the faulting call
  {
    uintptr_t ee_base = (uintptr_t)g_ee_main_mem;
    uintptr_t ee_end  = ee_base + EE_MAIN_MEM_SIZE;
    if (lr >= ee_base && lr < ee_end) {
      write(2, "[EE-CRASH] Instructions around LR (caller):\n", 44);
      const uint32_t* lrcode = (const uint32_t*)lr;
      for (int i = -120; i <= 2; i++) {
        n = __builtin_snprintf(buf, sizeof(buf),
          "  [%+3d] 0x%016llx: 0x%08x%s\n",
          i, (unsigned long long)(lrcode + i), lrcode[i],
          (i == -1) ? "  <-- CALL SITE" : (i == 0) ? "  <-- LR (next)" : "");
        write(2, buf, n);
      }
    }
  }

  // Backtrace
  write(2, "[EE-CRASH] Backtrace:\n", 22);
  void* frames[32];
  int nframes = backtrace(frames, 32);
  backtrace_symbols_fd(frames, nframes, 2);
}
#endif  // __APPLE__ && __aarch64__

/*!
 * Main function to launch the runtime.
 * GOAL kernel arguments are currently ignored.
 */
static void sigbus_handler(int sig, siginfo_t* info, void* ctx) {

#if defined(__aarch64__) && defined(__APPLE__)
  // SIGBUS counter: print stats every 10k faults so we can gauge performance.
  {
    static _Atomic uint64_t s_sigbus_count = 0;
    uint64_t n = ++s_sigbus_count;
    if ((n % 10000) == 0) {
      char buf[64];
      int len = __builtin_snprintf(buf, sizeof(buf), "[SIGBUS] count=%llu\n", (unsigned long long)n);
      write(2, buf, len);
    }
  }
#endif

#if defined(__aarch64__) && defined(__APPLE__)
  // On Darwin 25, GOAL code runs in MAP_JIT exec mode (W^X enforced).
  // GOAL heap writes (str/strb/strh/stp to EE memory) cause SIGBUS.
  // We emulate the faulting ARM64 register-offset store instruction by:
  //   1. Switching to write mode (pthread_jit_write_protect_np(0))
  //   2. Performing the store manually
  //   3. Switching back to exec mode (pthread_jit_write_protect_np(1))
  //   4. Advancing PC by 4 (ARM64 instructions are always 4 bytes)
  if (ctx && g_ee_main_mem) {
    ucontext_t* uctx = (ucontext_t*)ctx;

    // Fault must be in the MAP_JIT code region to be a legitimate linker/JIT write.
    // After the EE memory split, data regions are regular mmap — they cannot SIGBUS.
    uintptr_t fault_addr = (uintptr_t)info->si_addr;
    uintptr_t ee_base = (uintptr_t)g_ee_main_mem;
    uintptr_t ee_code_start = ee_base + EE_CODE_HEAP_START;
    uintptr_t ee_code_end   = ee_base + EE_CODE_HEAP_END;

    // Crash loudly if fault is in EE but outside the MAP_JIT code region.
    if (fault_addr >= ee_base && fault_addr < ee_base + EE_MAIN_MEM_SIZE &&
        (fault_addr < ee_code_start || fault_addr >= ee_code_end)) {
      char buf2[256];
      int n2 = __builtin_snprintf(buf2, sizeof(buf2),
        "[EE-CRASH] SIGBUS in DATA region at EE+0x%zx — "
        "data regions are not MAP_JIT; this is a runtime bug.\n",
        fault_addr - ee_base);
      write(2, buf2, n2);
      dump_arm64_crash_context(uctx->uc_mcontext->__ss.__pc,
                               uctx->uc_mcontext->__ss.__lr,
                               uctx->uc_mcontext->__ss.__sp,
                               uctx->uc_mcontext->__ss.__x,
                               uctx->uc_mcontext->__ss.__fp,
                               fault_addr);
      struct sigaction sa_def{};
      sa_def.sa_handler = SIG_DFL;
      sigaction(SIGBUS, &sa_def, nullptr);
      raise(SIGBUS);
      return;
    }

    if (fault_addr >= ee_code_start && fault_addr < ee_code_end) {

      auto& ss = uctx->uc_mcontext->__ss;
      auto& ns = uctx->uc_mcontext->__ns;
      auto gpr = [&](int r) -> uint64_t {
        if (r == 31) return 0;  // XZR / WZR
        if (r == 29) return ss.__fp;
        if (r == 30) return ss.__lr;
        return ss.__x[r];
      };
      auto gpr_base = [&](int r) -> uint64_t {  // r31 = SP not XZR
        if (r == 31) return ss.__sp;
        if (r == 29) return ss.__fp;
        if (r == 30) return ss.__lr;
        return ss.__x[r];
      };
      auto write_reg = [&](int r, uint64_t v) {
        if      (r < 29)  ss.__x[r] = v;
        else if (r == 29) ss.__fp = v;
        else if (r == 31) ss.__sp = v;
        // r==30 (LR) write-back not needed by any store pattern
      };

      // Execute one store instruction (already in write mode).
      // Updates cur_pc by 4 on success; updates context registers for write-back forms.
      // Returns true if the instruction was a recognised store.
      //
      // NOTE: addr must be validated to be within EE memory before writing.
      // Writing to an address outside EE memory would generate a recursive SIGBUS
      // (SIGBUS is blocked during the signal handler), which kills the process silently.
      auto check_addr = [&](uint64_t addr, int bytes, uint64_t pc, uint32_t instr_word) -> bool {
        uintptr_t base = (uintptr_t)g_ee_main_mem;
        if (addr < base || addr + (uint64_t)bytes > base + EE_MAIN_MEM_SIZE) {
          char bad[256];
          int bn = __builtin_snprintf(bad, sizeof(bad),
            "[EE-CRASH] exec_one: store addr 0x%llx outside EE [0x%llx,0x%llx) "
            "instr=0x%08x pc=0x%llx\n",
            (unsigned long long)addr, (unsigned long long)base,
            (unsigned long long)(base + EE_MAIN_MEM_SIZE),
            (unsigned)instr_word, (unsigned long long)pc);
          write(2, bad, bn);
          int bfd = open("/tmp/gk_bad_store.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644);
          if (bfd >= 0) { write(bfd, bad, bn); fsync(bfd); close(bfd); }
          return false;
        }
        return true;
      };
      auto exec_one = [&](uint64_t& cur_pc) -> bool {
        uint32_t instr = *(const uint32_t*)cur_pc;

        // ── 1. Register-offset GPR stores (all extend options) ──
        // Handles LSL (GOAL JIT), UXTW/SXTW/SXTX (C++ compiler: e.g. g_ee_main_mem + u32_offset)
        // Mask 0xFFE00C00: preserves [31:22] and [11:10], clears Rm/option/S/Rn/Rt
        {
          uint32_t m = instr & 0xFFE00C00u;
          int bytes = 0;
          if      (m == 0xF8200800u) bytes = 8;
          else if (m == 0xB8200800u) bytes = 4;
          else if (m == 0x78200800u) bytes = 2;
          else if (m == 0x38200800u) bytes = 1;
          if (bytes) {
            int Rm     = (instr >> 16) & 0x1F;
            int option = (instr >> 13) & 0x7;
            int S      = (instr >> 12) & 0x1;
            int Rn     = (instr >>  5) & 0x1F;
            int Rt     = instr         & 0x1F;
            uint64_t rm_val = gpr(Rm);
            uint64_t offset;
            switch (option) {
              case 2: offset = (uint32_t)rm_val; break;                              // UXTW
              case 6: offset = (uint64_t)(int64_t)(int32_t)(uint32_t)rm_val; break;  // SXTW
              default: offset = rm_val; break;                                        // LSL/SXTX
            }
            if (S) offset <<= (bytes == 8 ? 3 : bytes == 4 ? 2 : bytes == 2 ? 1 : 0);
            uint64_t addr = gpr_base(Rn) + offset;
            if (!check_addr(addr, bytes, cur_pc, instr)) return false;
            uint64_t val  = gpr(Rt);
            switch (bytes) {
              case 1: *(uint8_t* )addr = (uint8_t )val; break;
              case 2: *(uint16_t*)addr = (uint16_t)val; break;
              case 4: *(uint32_t*)addr = (uint32_t)val; break;
              case 8: *(uint64_t*)addr = val;            break;
            }
            cur_pc += 4; return true;
          }
        }

        // ── 2. Register-offset SIMD stores ──
        // STR St [Xn,Xm] 0xBC206800  STR Qt [Xn,Xm] 0x3CA06800
        {
          uint32_t m = instr & 0xFFE0FC00u;
          int bytes = 0;
          if      (m == 0xBC206800u) bytes = 4;
          else if (m == 0x3CA06800u) bytes = 16;
          if (bytes) {
            int Rm = (instr >> 16) & 0x1F, Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t addr = gpr_base(Rn) + gpr(Rm);
            if (!check_addr(addr, bytes, cur_pc, instr)) return false;
            if (bytes == 16) {
              __uint128_t val = ns.__v[Rt];
              __builtin_memcpy((void*)addr, &val, 16);
            } else {
              uint32_t val; __builtin_memcpy(&val, &ns.__v[Rt], 4);
              *(uint32_t*)addr = val;
            }
            cur_pc += 4; return true;
          }
        }

        // ── 2b. SIMD scalar post/pre-index / unscaled stores ──
        {
          uint32_t m = instr & 0xFF800C00u;
          int bytes = 0;
          if      ((m & ~0xC00u) == 0x3C800000u) bytes = 16;
          else if ((m & ~0xC00u) == 0xFC000000u) bytes = 8;
          else if ((m & ~0xC00u) == 0xBC000000u) bytes = 4;
          else if ((m & ~0xC00u) == 0x7C000000u) bytes = 2;
          if (bytes) {
            uint32_t mode = instr & 0xC00u;
            bool is_post = (mode == 0x400u), is_pre = (mode == 0xC00u);
            if (mode == 0x800u) bytes = 0; // 10 = register-offset, not here
          }
          if (bytes) {
            int32_t imm9 = (int32_t)((instr >> 12) & 0x1FF);
            if (imm9 & 0x100) imm9 |= ~(int32_t)0x1FF;
            int Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint32_t mode = instr & 0xC00u;
            bool is_post = (mode == 0x400u), is_pre = (mode == 0xC00u);
            uint64_t base = gpr_base(Rn);
            uint64_t addr = is_post ? base : base + (int64_t)imm9;
            if (!check_addr(addr, bytes, cur_pc, instr)) return false;
            if (bytes == 16) {
              __uint128_t val = ns.__v[Rt];
              __builtin_memcpy((void*)addr, &val, 16);
            } else {
              uint64_t val = 0; __builtin_memcpy(&val, &ns.__v[Rt], bytes);
              switch (bytes) {
                case 2: *(uint16_t*)addr = (uint16_t)val; break;
                case 4: *(uint32_t*)addr = (uint32_t)val; break;
                case 8: *(uint64_t*)addr = val;            break;
              }
            }
            if (is_post || is_pre) write_reg(Rn, base + (int64_t)imm9);
            cur_pc += 4; return true;
          }
        }

        // ── 2c. Unsigned-offset SIMD stores (C compiler scalar/vector stores) ──
        // STR Qt [Xn,#imm*16] 0x3D800000  STR Dt 0xFD000000  STR St 0xBD000000
        {
          uint32_t top10 = instr & 0xFFC00000u;
          int bytes = 0;
          if      (top10 == 0x3D800000u) bytes = 16;
          else if (top10 == 0xFD000000u) bytes =  8;
          else if (top10 == 0xBD000000u) bytes =  4;
          if (bytes) {
            uint32_t imm12 = (instr >> 10) & 0xFFF;
            int Rn = (instr >> 5) & 0x1F;
            int Rt = instr & 0x1F;
            uint64_t addr = gpr_base(Rn) + (uint64_t)imm12 * (uint32_t)bytes;
            if (!check_addr(addr, bytes, cur_pc, instr)) return false;
            if (bytes == 16) {
              __uint128_t val = ns.__v[Rt];
              __builtin_memcpy((void*)addr, &val, 16);
            } else {
              uint64_t val = 0; __builtin_memcpy(&val, &ns.__v[Rt], bytes);
              switch (bytes) {
                case 4: *(uint32_t*)addr = (uint32_t)val; break;
                case 8: *(uint64_t*)addr = val;            break;
              }
            }
            cur_pc += 4; return true;
          }
        }

        // ── 3. Unsigned-offset GPR stores ──
        {
          uint32_t top10 = instr & 0xFFC00000u;
          if (top10 == 0xF9000000u || top10 == 0xB9000000u ||
              top10 == 0x79000000u || top10 == 0x39000000u) {
            int scale = (instr >> 30) & 0x3;
            uint32_t imm12 = (instr >> 10) & 0xFFF;
            int Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t addr = gpr_base(Rn) + ((uint64_t)imm12 << scale);
            if (!check_addr(addr, 1 << scale, cur_pc, instr)) return false;
            uint64_t val  = gpr(Rt);
            switch (1 << scale) {
              case 1: *(uint8_t* )addr = (uint8_t )val; break;
              case 2: *(uint16_t*)addr = (uint16_t)val; break;
              case 4: *(uint32_t*)addr = (uint32_t)val; break;
              case 8: *(uint64_t*)addr = val;            break;
            }
            cur_pc += 4; return true;
          }
        }

        // ── 4. STUR (unscaled signed offset) ──
        {
          uint32_t m = instr & 0xFFE00C00u;
          if (m == 0xF8000000u || m == 0xB8000000u ||
              m == 0x78000000u || m == 0x38000000u) {
            int scale = (instr >> 30) & 0x3;
            int32_t imm9 = (int32_t)((instr >> 12) & 0x1FF);
            if (imm9 & 0x100) imm9 |= ~(int32_t)0x1FF;
            int Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t addr = gpr_base(Rn) + (int64_t)imm9;
            if (!check_addr(addr, 1 << scale, cur_pc, instr)) return false;
            uint64_t val  = gpr(Rt);
            switch (1 << scale) {
              case 1: *(uint8_t* )addr = (uint8_t )val; break;
              case 2: *(uint16_t*)addr = (uint16_t)val; break;
              case 4: *(uint32_t*)addr = (uint32_t)val; break;
              case 8: *(uint64_t*)addr = val;            break;
            }
            cur_pc += 4; return true;
          }
        }

        // ── 5. Post-indexed GPR stores ──
        {
          uint32_t m = instr & 0xFFE00C00u;
          if (m == 0xF8000400u || m == 0xB8000400u ||
              m == 0x78000400u || m == 0x38000400u) {
            int scale = (instr >> 30) & 0x3;
            int32_t imm9 = (int32_t)((instr >> 12) & 0x1FF);
            if (imm9 & 0x100) imm9 |= ~(int32_t)0x1FF;
            int Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t base = gpr_base(Rn);
            if (!check_addr(base, 1 << scale, cur_pc, instr)) return false;
            uint64_t val  = gpr(Rt);
            switch (1 << scale) {
              case 1: *(uint8_t* )base = (uint8_t )val; break;
              case 2: *(uint16_t*)base = (uint16_t)val; break;
              case 4: *(uint32_t*)base = (uint32_t)val; break;
              case 8: *(uint64_t*)base = val;            break;
            }
            write_reg(Rn, base + (int64_t)imm9);
            cur_pc += 4; return true;
          }
        }

        // ── 5b. Pre-indexed GPR stores ──
        {
          uint32_t m = instr & 0xFFE00C00u;
          if (m == 0xF8000C00u || m == 0xB8000C00u ||
              m == 0x78000C00u || m == 0x38000C00u) {
            int scale = (instr >> 30) & 0x3;
            int32_t imm9 = (int32_t)((instr >> 12) & 0x1FF);
            if (imm9 & 0x100) imm9 |= ~(int32_t)0x1FF;
            int Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t addr = gpr_base(Rn) + (int64_t)imm9;
            if (!check_addr(addr, 1 << scale, cur_pc, instr)) return false;
            uint64_t val  = gpr(Rt);
            switch (1 << scale) {
              case 1: *(uint8_t* )addr = (uint8_t )val; break;
              case 2: *(uint16_t*)addr = (uint16_t)val; break;
              case 4: *(uint32_t*)addr = (uint32_t)val; break;
              case 8: *(uint64_t*)addr = val;            break;
            }
            write_reg(Rn, addr);
            cur_pc += 4; return true;
          }
        }

        // ── 6. STP 64-bit GPR pair ──
        {
          uint32_t top10 = instr & 0xFFC00000u;
          if (top10 == 0xA8000000u || top10 == 0xA9000000u ||
              top10 == 0xA8800000u || top10 == 0xA9800000u) {
            int32_t imm7 = (int32_t)((instr >> 15) & 0x7F);
            if (imm7 & 0x40) imm7 |= ~(int32_t)0x7F;
            int Rt2 = (instr >> 10) & 0x1F, Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t base = gpr_base(Rn);
            uint64_t addr = (top10 == 0xA8800000u) ? base : base + (int64_t)imm7 * 8;
            if (!check_addr(addr, 16, cur_pc, instr)) return false;
            *(uint64_t*)addr       = gpr(Rt);
            *(uint64_t*)(addr + 8) = gpr(Rt2);
            if (top10 == 0xA8800000u || top10 == 0xA9800000u)
              write_reg(Rn, base + (int64_t)imm7 * 8);
            cur_pc += 4; return true;
          }
        }

        // ── 6b. STP 32-bit GPR pair ──
        {
          uint32_t top10 = instr & 0xFFC00000u;
          if (top10 == 0x28000000u || top10 == 0x29000000u ||
              top10 == 0x28800000u || top10 == 0x29800000u) {
            int32_t imm7 = (int32_t)((instr >> 15) & 0x7F);
            if (imm7 & 0x40) imm7 |= ~(int32_t)0x7F;
            int Rt2 = (instr >> 10) & 0x1F, Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t base = gpr_base(Rn);
            uint64_t addr = (top10 == 0x28800000u) ? base : base + (int64_t)imm7 * 4;
            if (!check_addr(addr, 8, cur_pc, instr)) return false;
            *(uint32_t*)addr       = (uint32_t)gpr(Rt);
            *(uint32_t*)(addr + 4) = (uint32_t)gpr(Rt2);
            if (top10 == 0x28800000u || top10 == 0x29800000u)
              write_reg(Rn, base + (int64_t)imm7 * 4);
            cur_pc += 4; return true;
          }
        }

        // ── 7. STP Q-pair (128-bit SIMD) ──
        {
          uint32_t top10 = instr & 0xFFC00000u;
          if (top10 == 0xAC000000u || top10 == 0xAD000000u) {
            int32_t imm7 = (int32_t)((instr >> 15) & 0x7F);
            if (imm7 & 0x40) imm7 |= ~(int32_t)0x7F;
            int Rt2 = (instr >> 10) & 0x1F, Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t addr = gpr_base(Rn) + (int64_t)imm7 * 16;
            if (!check_addr(addr, 32, cur_pc, instr)) return false;
            __uint128_t v1 = ns.__v[Rt], v2 = ns.__v[Rt2];
            __builtin_memcpy((void*)addr,        &v1, 16);
            __builtin_memcpy((void*)(addr + 16), &v2, 16);
            cur_pc += 4; return true;
          }
        }

        // ── 8. STP D-pair (64-bit SIMD) ──
        {
          uint32_t top10 = instr & 0xFFC00000u;
          if (top10 == 0x6C000000u || top10 == 0x6D000000u) {
            int32_t imm7 = (int32_t)((instr >> 15) & 0x7F);
            if (imm7 & 0x40) imm7 |= ~(int32_t)0x7F;
            int Rt2 = (instr >> 10) & 0x1F, Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t addr = gpr_base(Rn) + (int64_t)imm7 * 8;
            if (!check_addr(addr, 16, cur_pc, instr)) return false;
            uint64_t v1, v2;
            __builtin_memcpy(&v1, &ns.__v[Rt],  8);
            __builtin_memcpy(&v2, &ns.__v[Rt2], 8);
            *(uint64_t*)addr       = v1;
            *(uint64_t*)(addr + 8) = v2;
            cur_pc += 4; return true;
          }
        }

        // ── 9. STP S-pair (32-bit SIMD) ──
        {
          uint32_t top10 = instr & 0xFFC00000u;
          if (top10 == 0x2C000000u || top10 == 0x2D000000u) {
            int32_t imm7 = (int32_t)((instr >> 15) & 0x7F);
            if (imm7 & 0x40) imm7 |= ~(int32_t)0x7F;
            int Rt2 = (instr >> 10) & 0x1F, Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t addr = gpr_base(Rn) + (int64_t)imm7 * 4;
            if (!check_addr(addr, 8, cur_pc, instr)) return false;
            uint32_t v1, v2;
            __builtin_memcpy(&v1, &ns.__v[Rt],  4);
            __builtin_memcpy(&v2, &ns.__v[Rt2], 4);
            *(uint32_t*)addr       = v1;
            *(uint32_t*)(addr + 4) = v2;
            cur_pc += 4; return true;
          }
        }

        return false;
      }; // exec_one

      // Batch: switch to write mode ONCE, execute up to 32 consecutive store
      // instructions, switch back to exec mode ONCE.  Amortises the expensive
      // kernel signal-delivery round-trip over N stores instead of 1.
      uint64_t cur_pc = ss.__pc;
      pthread_jit_write_protect_np(0);
      int n = 0;
      for (; n < 32; ++n) {
        if (!exec_one(cur_pc)) break;
      }
      pthread_jit_write_protect_np(1);

      if (n > 0) {
        uctx->uc_mcontext->__ss.__pc = cur_pc;
        return;
      }

      // n==0: exec_one couldn't decode the instruction at PC.
      // If fault_addr == PC this is an instruction-fetch fault (write mode was active
      // when the CPU tried to execute MAP_JIT code).  exec mode was already restored
      // above; just return so the CPU retries at the same PC in exec mode.
      if (fault_addr == (uintptr_t)ss.__pc) {
        static std::atomic<uint64_t> fetch_fault_count{0};
        uint64_t fc = ++fetch_fault_count;
        if (fc <= 5 || (fc % 1000) == 0) {
          char fbuf[128];
          int fn = __builtin_snprintf(fbuf, sizeof(fbuf),
            "[EE-JIT] fetch-fault recovery #%llu at EE+0x%llx instr=0x%08x\n",
            (unsigned long long)fc,
            (unsigned long long)(fault_addr - (uintptr_t)g_ee_main_mem),
            *(const uint32_t*)fault_addr);
          write(2, fbuf, fn);
        }
        return;
      }

    }
  }
#endif

  // Unrecognized SIGBUS: print diagnostic and crash
  {
    char buf[128];
    int n = __builtin_snprintf(buf, sizeof(buf),
      "[EE-CRASH] SIGBUS at fault addr %p\n", info->si_addr);
    write(2, buf, n);
  }
#if defined(__APPLE__) && defined(__aarch64__)
  if (ctx) {
    ucontext_t* uctx = (ucontext_t*)ctx;
    auto& ss = uctx->uc_mcontext->__ss;
    uintptr_t fault = (uintptr_t)info->si_addr;
    uintptr_t base  = g_ee_main_mem ? (uintptr_t)g_ee_main_mem : 0;
    {
      char buf[128];
      int n = __builtin_snprintf(buf, sizeof(buf),
        "[EE-CRASH] fault %s EE memory\n",
        (base && fault >= base && fault < base + EE_MAIN_MEM_SIZE) ? "IN" : "OUTSIDE");
      write(2, buf, n);
    }
    dump_arm64_crash_context(ss.__pc, ss.__lr, ss.__sp, ss.__x, ss.__fp, fault);
  }
#endif
  struct sigaction sa{};
  sa.sa_handler = SIG_DFL;
  sigaction(sig, &sa, nullptr);
  raise(sig);
}

RuntimeExitStatus exec_runtime(GameLaunchOptions game_options, int argc, const char** argv) {
  // Install SIGILL handler to catch illegal-instruction crashes in GOAL code
#if defined(__APPLE__) && defined(__aarch64__)
  {
    // Use an alternate signal stack so the handler runs on a known-good stack
    // even if the faulting thread's SP is corrupt or near its limit.
    static char s_sigill_altstack[65536];
    stack_t ss_alt{};
    ss_alt.ss_sp = s_sigill_altstack;
    ss_alt.ss_size = sizeof(s_sigill_altstack);
    ss_alt.ss_flags = 0;
    sigaltstack(&ss_alt, nullptr);

    struct sigaction sa_ill{};
    sa_ill.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sa_ill.sa_sigaction = [](int, siginfo_t*, void* ctx) {
      // Write to fd 1 (stdout), fd 2 (stderr), AND a dedicated crash file.
      const char* header = "[EE-SIGILL] handler entered\n";
      write(1, header, 28);
      write(2, header, 28);
      int fd = open("/tmp/gk_sigill_crash.txt",
                    O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) write(fd, header, 28);
      if (!ctx) {
        const char* noct = "[EE-SIGILL] no ctx\n";
        write(2, noct, 19); if (fd >= 0) write(fd, noct, 19);
        if (fd >= 0) close(fd);
        raise(SIGILL); return;
      }
      ucontext_t* uctx = (ucontext_t*)ctx;
      auto& ss = uctx->uc_mcontext->__ss;
      char buf[256];
      int n = __builtin_snprintf(buf, sizeof(buf),
        "[EE-CRASH] SIGILL at PC=0x%016llx LR=0x%016llx\n",
        (unsigned long long)ss.__pc, (unsigned long long)ss.__lr);
      write(2, buf, n); if (fd >= 0) write(fd, buf, n);
      char buf2[512];
      int n2 = __builtin_snprintf(buf2, sizeof(buf2),
        "  x0=%016llx x1=%016llx x2=%016llx x3=%016llx\n"
        "  x4=%016llx x5=%016llx x6=%016llx x7=%016llx\n"
        "  x22(EEbase)=%016llx x23(GOALsp)=%016llx fp=%016llx sp=%016llx\n",
        ss.__x[0], ss.__x[1], ss.__x[2], ss.__x[3],
        ss.__x[4], ss.__x[5], ss.__x[6], ss.__x[7],
        ss.__x[22], ss.__x[23], ss.__fp, ss.__sp);
      write(2, buf2, n2); if (fd >= 0) write(fd, buf2, n2);
      char buf3[512];
      int n3 = __builtin_snprintf(buf3, sizeof(buf3),
        "  x8=%016llx x9=%016llx x10=%016llx x11=%016llx\n"
        "  x12=%016llx x13=%016llx x14=%016llx x15=%016llx\n"
        "  x19=%016llx x20=%016llx x21=%016llx x24=%016llx\n"
        "  x25=%016llx x26=%016llx x27=%016llx x28=%016llx\n",
        ss.__x[8],  ss.__x[9],  ss.__x[10], ss.__x[11],
        ss.__x[12], ss.__x[13], ss.__x[14], ss.__x[15],
        ss.__x[19], ss.__x[20], ss.__x[21], ss.__x[24],
        ss.__x[25], ss.__x[26], ss.__x[27], ss.__x[28]);
      write(2, buf3, n3); if (fd >= 0) write(fd, buf3, n3);
      if (fd >= 0) { fsync(fd); close(fd); }
      struct sigaction sa{};
      sa.sa_handler = SIG_DFL;
      sigaction(SIGILL, &sa, nullptr);
      raise(SIGILL);
    };
    sigaction(SIGILL, &sa_ill, nullptr);
  }

  // Install Mach exception handler for EXC_BAD_INSTRUCTION so we can catch SIGILL
  // from MAP_JIT code (which bypasses POSIX signal handlers on Darwin 25).
  {
    mach_port_t exc_port = MACH_PORT_NULL;
    kern_return_t kr_alloc = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exc_port);
    kern_return_t kr_right = (kr_alloc == KERN_SUCCESS) ?
        mach_port_insert_right(mach_task_self(), exc_port, exc_port, MACH_MSG_TYPE_MAKE_SEND) :
        KERN_FAILURE;
    if (kr_right == KERN_SUCCESS) {
      // Skip registering our own port if running under a debugger (lldb takes priority).
      // The debugger's exception handling gives us better backtraces.
      const char* is_debug = getenv("GK_MACH_HANDLER_DISABLE");
      kern_return_t kr = is_debug ? KERN_FAILURE :
          task_set_exception_ports(
          mach_task_self(),
          EXC_MASK_BAD_INSTRUCTION | EXC_MASK_BREAKPOINT,
          exc_port,
          static_cast<exception_behavior_t>(EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES),
          THREAD_STATE_NONE);
      fprintf(stderr, "[MACH-SETUP] task_set_exception_ports kr=%d exc_port=%u\n", kr, exc_port);
      fflush(stderr);
      if (kr == KERN_SUCCESS) {
        static mach_port_t s_exc_port = exc_port;
        pthread_t exc_thread;
        pthread_create(&exc_thread, nullptr, [](void*) -> void* {
          alignas(8) char msgbuf[8192]{};
          auto* hdr = reinterpret_cast<mach_msg_header_t*>(msgbuf);
          // Write immediate marker so we know handler thread is alive
          {
            const char* alive = "[MACH-EXC] thread waiting\n";
            write(2, alive, 26);
          }
          kern_return_t kr = mach_msg(hdr, MACH_RCV_MSG, 0, sizeof(msgbuf),
                       s_exc_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
          // Write to a dedicated file first (bypasses any broken stderr pipe)
          {
            int early_fd = open("/tmp/gk_mach_woke.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644);
            if (early_fd >= 0) {
              char tmp2[128];
              int tn2 = __builtin_snprintf(tmp2, sizeof(tmp2), "mach_msg returned kr=%d\n", (int)kr);
              write(early_fd, tmp2, tn2);
              fsync(early_fd);
              close(early_fd);
            }
          }
          {
            char tmp[64];
            int tn = __builtin_snprintf(tmp, sizeof(tmp), "[MACH-EXC] mach_msg returned kr=%d msz=%u\n",
                                        (int)kr, (unsigned)hdr->msgh_size);
            write(2, tmp, tn);
          }
          if (kr != MACH_MSG_SUCCESS) {
            _exit(132);
            return nullptr;
          }
          // Print raw message header for diagnosis
          {
            char tmp[256];
            int tn = __builtin_snprintf(tmp, sizeof(tmp),
              "[MACH-EXC] bits=0x%x size=%u remote=0x%x local=0x%x id=%d\n",
              (unsigned)hdr->msgh_bits, (unsigned)hdr->msgh_size,
              (unsigned)hdr->msgh_remote_port, (unsigned)hdr->msgh_local_port,
              (int)hdr->msgh_id);
            write(2, tmp, tn);
          }
          // Try to get faulting thread via body/ports
          auto* body = reinterpret_cast<mach_msg_body_t*>(msgbuf + sizeof(mach_msg_header_t));
          auto* ports = reinterpret_cast<mach_msg_port_descriptor_t*>(body + 1);
          mach_port_t thread_port = ports[0].name;  // ports[0]=thread, ports[1]=task
          {
            char tmp[64];
            int tn = __builtin_snprintf(tmp, sizeof(tmp),
              "[MACH-EXC] thread_port=0x%x\n", (unsigned)thread_port);
            write(2, tmp, tn);
          }
          arm_thread_state64_t state{};
          mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
          kern_return_t gsr = thread_get_state(thread_port, ARM_THREAD_STATE64,
                           reinterpret_cast<thread_state_t>(&state), &cnt);
          uint64_t pc = state.__pc;
          uint64_t lr = state.__lr;
          uint64_t sp = state.__sp;
          // Read instruction safely (pc might be garbage if thread_get_state failed)
          uint32_t instr = 0;
          if (gsr == KERN_SUCCESS && pc >= 0x1000 && (pc & 3) == 0) {
            // Use vm_read_overwrite instead of direct deref to avoid crashing
            vm_size_t out_sz = 0;
            vm_read_overwrite(mach_task_self(), (vm_address_t)pc, sizeof(uint32_t),
                              (vm_address_t)&instr, &out_sz);
          }
          char buf[1024];
          int n = __builtin_snprintf(buf, sizeof(buf),
            "[MACH-SIGILL] EXC_BAD_INSTRUCTION at PC=0x%016llx LR=0x%016llx SP=0x%016llx\n"
            "  gsr=%d instr=0x%08x\n"
            "  x0 =0x%016llx x1 =0x%016llx x2 =0x%016llx x3 =0x%016llx\n"
            "  x4 =0x%016llx x5 =0x%016llx x6 =0x%016llx x7 =0x%016llx\n"
            "  x8 =0x%016llx x9 =0x%016llx x10=0x%016llx x11=0x%016llx\n"
            "  x16=0x%016llx x17=0x%016llx x19=0x%016llx x20=0x%016llx\n"
            "  x21=0x%016llx x22=0x%016llx x23=0x%016llx x29=0x%016llx\n",
            (unsigned long long)pc, (unsigned long long)lr, (unsigned long long)sp,
            (int)gsr, (unsigned)instr,
            (unsigned long long)state.__x[0],  (unsigned long long)state.__x[1],
            (unsigned long long)state.__x[2],  (unsigned long long)state.__x[3],
            (unsigned long long)state.__x[4],  (unsigned long long)state.__x[5],
            (unsigned long long)state.__x[6],  (unsigned long long)state.__x[7],
            (unsigned long long)state.__x[8],  (unsigned long long)state.__x[9],
            (unsigned long long)state.__x[10], (unsigned long long)state.__x[11],
            (unsigned long long)state.__x[16], (unsigned long long)state.__x[17],
            (unsigned long long)state.__x[19], (unsigned long long)state.__x[20],
            (unsigned long long)state.__x[21], (unsigned long long)state.__x[22],
            (unsigned long long)state.__x[23], (unsigned long long)state.__fp);
          write(2, buf, n);
          int fd = open("/tmp/gk_mach_sigill.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644);
          if (fd >= 0) { write(fd, buf, n); fsync(fd); close(fd); }
          // Pause process so lldb can attach for backtrace, then exit
          raise(SIGSTOP);
          _exit(132);
          return nullptr;
        }, nullptr);
        pthread_detach(exc_thread);
      }
    }
  }
#endif
  // Install SIGBUS handler to diagnose MAP_JIT protection faults
  {
    struct sigaction sa{};
    sa.sa_sigaction = sigbus_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, nullptr);
  }
#if defined(__APPLE__) && defined(__aarch64__)
  // Install SIGSEGV handler: dumps PC/LR/registers for any segfault not handled by
  // sigbus_handler (e.g. null-deref in OpenGL renderer on the main thread).
  // This overwrites the sigbus_handler SIGSEGV registration above intentionally —
  // CGO loading is complete by the time we reach the game loop, so MAP_JIT writes
  // from C++ code to the code heap are no longer expected.
  {
    static char s_sigsegv_altstack[65536];
    stack_t ss_segv{};
    ss_segv.ss_sp = s_sigsegv_altstack;
    ss_segv.ss_size = sizeof(s_sigsegv_altstack);
    ss_segv.ss_flags = 0;
    sigaltstack(&ss_segv, nullptr);

    struct sigaction sa_segv{};
    sa_segv.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sa_segv.sa_sigaction = [](int, siginfo_t* info, void* ctx) {
      write(2, "[EE-SIGSEGV] handler entered\n", 29);
      if (!ctx) { raise(SIGSEGV); return; }
      ucontext_t* uctx = (ucontext_t*)ctx;
      auto& ss = uctx->uc_mcontext->__ss;
      {
        char buf[256];
        int n = __builtin_snprintf(buf, sizeof(buf),
          "[EE-CRASH] SIGSEGV at PC=0x%016llx LR=0x%016llx fault=0x%016llx\n",
          (unsigned long long)ss.__pc, (unsigned long long)ss.__lr,
          (unsigned long long)(uintptr_t)(info ? info->si_addr : nullptr));
        write(2, buf, n);
        char buf2[512];
        int n2 = __builtin_snprintf(buf2, sizeof(buf2),
          "  x0=%016llx x1=%016llx x2=%016llx x3=%016llx\n"
          "  x4=%016llx x5=%016llx x6=%016llx x7=%016llx\n"
          "  x22(EEbase)=%016llx x23(GOALsp)=%016llx fp=%016llx sp=%016llx\n",
          ss.__x[0], ss.__x[1], ss.__x[2], ss.__x[3],
          ss.__x[4], ss.__x[5], ss.__x[6], ss.__x[7],
          ss.__x[22], ss.__x[23], ss.__fp, ss.__sp);
        write(2, buf2, n2);
        char buf3[512];
        int n3 = __builtin_snprintf(buf3, sizeof(buf3),
          "  x8=%016llx x9=%016llx x10=%016llx x11=%016llx\n"
          "  x12=%016llx x13=%016llx x14=%016llx x15=%016llx\n"
          "  x19=%016llx x20=%016llx x21=%016llx x24=%016llx\n"
          "  x25=%016llx x26=%016llx x27=%016llx x28=%016llx\n",
          ss.__x[8],  ss.__x[9],  ss.__x[10], ss.__x[11],
          ss.__x[12], ss.__x[13], ss.__x[14], ss.__x[15],
          ss.__x[19], ss.__x[20], ss.__x[21], ss.__x[24],
          ss.__x[25], ss.__x[26], ss.__x[27], ss.__x[28]);
        write(2, buf3, n3);
      }
      struct sigaction sa{};
      sa.sa_handler = SIG_DFL;
      sigaction(SIGSEGV, &sa, nullptr);
      raise(SIGSEGV);
    };
    sigaction(SIGSEGV, &sa_segv, nullptr);
  }
  // SIGABRT: catches assert()/abort() calls.  Same register-dump pattern.
  {
    static char s_sigabrt_altstack[65536];
    stack_t ss_abrt{};
    ss_abrt.ss_sp = s_sigabrt_altstack;
    ss_abrt.ss_size = sizeof(s_sigabrt_altstack);
    ss_abrt.ss_flags = 0;
    sigaltstack(&ss_abrt, nullptr);

    struct sigaction sa_abrt{};
    sa_abrt.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sa_abrt.sa_sigaction = [](int, siginfo_t*, void* ctx) {
      write(2, "[EE-SIGABRT] handler entered\n", 29);
      if (ctx) {
        ucontext_t* uctx = (ucontext_t*)ctx;
        auto& ss = uctx->uc_mcontext->__ss;
        char buf[256];
        int n = __builtin_snprintf(buf, sizeof(buf),
          "[EE-CRASH] SIGABRT at PC=0x%016llx LR=0x%016llx\n",
          (unsigned long long)ss.__pc, (unsigned long long)ss.__lr);
        write(2, buf, n);
        char buf2[512];
        int n2 = __builtin_snprintf(buf2, sizeof(buf2),
          "  x0=%016llx x1=%016llx x2=%016llx x3=%016llx\n"
          "  x22(EEbase)=%016llx x23(GOALsp)=%016llx fp=%016llx sp=%016llx\n",
          ss.__x[0], ss.__x[1], ss.__x[2], ss.__x[3],
          ss.__x[22], ss.__x[23], ss.__fp, ss.__sp);
        write(2, buf2, n2);
      }
      struct sigaction sa{};
      sa.sa_handler = SIG_DFL;
      sigaction(SIGABRT, &sa, nullptr);
      raise(SIGABRT);
    };
    sigaction(SIGABRT, &sa_abrt, nullptr);
  }
  // SIGPIPE: ignore — tee/pipe breakage should not kill gk.
  signal(SIGPIPE, SIG_IGN);
#endif
  prof().root_event();
  g_argc = argc;
  g_argv = argv;
  g_main_thread_id = std::this_thread::get_id();

  bool enable_display = !game_options.disable_display;
  g_game_version = game_options.game_version;
  g_server_port = game_options.server_port;

  gStartTime = time(nullptr);
  prof().instant_event("ROOT");
  {
    auto p = scoped_prof("startup::exec_runtime::init_discord_rpc");
    init_discord_rpc();
  }

  // initialize graphics first - the EE code will upload textures during boot and we
  // want the graphics system to catch them.
  {
    auto p = scoped_prof("startup::exec_runtime::init_gfx");
    if (enable_display) {
      Gfx::Init(g_game_version);
    }
  }

  // step 1: sce library prep
  {
    auto p = scoped_prof("startup::exec_runtime::library_prep");
    iop::LIBRARY_INIT();
    ee::LIBRARY_INIT_sceCd();
    ee::LIBRARY_INIT_sceDeci2();
    ee::LIBRARY_INIT_sceSif();
  }

  // step 2: system prep
  prof().begin_event("startup::exec_runtime::system_prep");
  SystemThreadManager tm;
  auto& deci_thread = tm.create_thread("DMP");
  auto& iop_thread = tm.create_thread("IOP");
  auto& ee_thread = tm.create_thread("EE");
  // a general worker thread to perform background operations from the EE thread (to not block the
  // game)
  auto& ee_worker_thread = tm.create_thread("EE-Worker");
  prof().end_event();

  // step 3: start the EE!
  {
    auto p = scoped_prof("startup::exec_runtime::iop-start");
    iop_thread.start([=](SystemThreadInterface& sti) { iop_runner(sti, g_game_version); });
  }
  {
    auto p = scoped_prof("startup::exec_runtime::deci-start");
    deci_thread.start(deci2_runner);
  }
  {
    auto p = scoped_prof("startup::exec_runtime::ee-worker-start");
    ee_worker_thread.start(ee_worker_runner);
  }
  {
    auto p = scoped_prof("startup::exec_runtime::ee-start");
    ee_thread.start(ee_runner);
  }

  // step 4: wait for EE to signal a shutdown. meanwhile, run video loop on main thread.
  // TODO relegate this to its own function
  if (enable_display) {
    try {
      Gfx::Loop([]() { return MasterExit == RuntimeExitStatus::RUNNING; });
    } catch (std::exception& e) {
      lg::error("Exception thrown from graphics loop: {}", e.what());
      lg::error("Everything will crash now. good luck");
      throw;
    }
  }

  // hack to make the IOP die quicker if it's loading/unloading music
  gMusicFade = 0;

  // if we have no display, wait here for DECI to shutdown
  deci_thread.join();

  // fully shut down EE first before stopping the other threads
  ee_thread.join();

  // to be extra sure
  tm.shutdown();

  // join and exit
  tm.join();

  // kill renderer after all threads are stopped.
  // this makes sure the std::shared_ptr<Display> is destroyed in the main thread.
  if (enable_display) {
    Gfx::Exit();
  }
  lg::info("GOAL Runtime Shutdown (code {})", fmt::underlying(MasterExit));
#if defined(__aarch64__) && defined(__APPLE__)
  munmap((u8*)g_ee_main_mem - 16384, EE_MAIN_MEM_SIZE + 16384);
#else
  munmap(g_ee_main_mem, EE_MAIN_MEM_SIZE);
#endif
  Discord_Shutdown();
  return MasterExit;
}
