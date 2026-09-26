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
  prof().root_event();
#if defined(__APPLE__) && defined(__aarch64__)
  // A GOAL stack overflow faults with no room left to build a signal frame, so without an
  // alternate stack the handler cannot run and the process dies unreported. Give the EE thread
  // its own handler stack; the handlers below are installed with SA_ONSTACK.
  {
    static thread_local char ee_sigstack[SIGSTKSZ * 4];
    stack_t ss{};
    ss.ss_sp = ee_sigstack;
    ss.ss_size = sizeof(ee_sigstack);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) != 0) {
      lg::warn("[EE] sigaltstack failed: {}", strerror(errno));
    }
  }
#endif
  // Allocate Main RAM (EE memory).
  //
  // On Darwin ARM64, W^X is enforced: a page is writable or executable, never both, and only a
  // MAP_JIT page may become executable at all.
  //
  // The whole EE is one MAP_JIT region mapped read-write. Any page of it can then be flipped to
  // read-execute with mprotect, whichever heap it belongs to, which is what lets the linker put a
  // segment's code where the original game put it instead of in a fixed code region.
  //
  // It is mapped read-write rather than read-write-execute on purpose: asking for all three puts
  // the region under the per-thread APRR switch, where it is only writable in write mode. We want
  // the page table to decide instead, so every thread sees the same protection.
  if (EE_MEM_LOW_MAP) {
#if defined(__aarch64__) && defined(__APPLE__)
    g_ee_main_mem = (u8*)mmap((void*)0x10000000, EE_MAIN_MEM_SIZE, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_PRIVATE | MAP_JIT, -1, 0);
#elif defined(__APPLE__)
    g_ee_main_mem =
        (u8*)mmap((void*)0x10000000, EE_MAIN_MEM_SIZE, PROT_EXEC | PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
#else
    g_ee_main_mem =
        (u8*)mmap((void*)0x10000000, EE_MAIN_MEM_SIZE, PROT_EXEC | PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_32BIT | MAP_PRIVATE | MAP_POPULATE, 0, 0);
#endif
  } else {
#if defined(__aarch64__) && defined(__APPLE__)
    // Apple Silicon has a 16 KB hardware page size, so the null-guard prefix must be 16 KB
    // aligned. Allocate EE_MAIN_MEM_SIZE + 16384: the first 16 KB is the null-guard.
    // g_ee_main_mem = raw + 16384, so EE[-4] (= raw + 16380) reads zero
    // (MAP_ANONYMOUS zero-fills), simulating PS2 null-pointer type-tag behaviour.
    static constexpr size_t kNullGuard = 16384;
    void* raw = mmap((void*)EE_MAIN_MEM_MAP, EE_MAIN_MEM_SIZE + kNullGuard,
                     PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_JIT, -1, 0);
    if (raw == MAP_FAILED) {
      lg::debug("Failed to initialize main memory! {}", strerror(errno));
      iface.initialization_complete();
      return;
    }
    g_ee_main_mem = (u8*)raw + kNullGuard;
    // The null-guard (raw[0..kNullGuard)) is zero-filled by MAP_ANONYMOUS, so EE[-4] reads zero
    // the way a PS2 null type tag does.
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

  lg::info("Main memory mapped at 0x{:016x}", (u64)(g_ee_main_mem));
  lg::info("Main memory size 0x{:x} bytes ({:.3f} MB)", EE_MAIN_MEM_SIZE,
           (double)EE_MAIN_MEM_SIZE / (1 << 20));

  lg::info("[EE] Initialization complete!");
  iface.initialization_complete();

  lg::info("[EE] Run!");
  memset((void*)g_ee_main_mem, 0, EE_MAIN_MEM_SIZE);

  // Make the first 512 kB PROT_NONE to catch null GOAL pointer dereferences.
  mprotect((void*)g_ee_main_mem, EE_MAIN_MEM_LOW_PROTECT, PROT_NONE);
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
    n = __builtin_snprintf(buf, sizeof(buf),
      "[EE-CRASH] fault addr 0x%016llx  EE offset 0x%llx\n"
      "[EE-CRASH] PC EE offset 0x%llx  LR EE offset 0x%llx\n",
      (unsigned long long)fault_addr,
      (unsigned long long)(fault_addr - base),
      (unsigned long long)(pc - base),
      (unsigned long long)(lr - base));
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

  // Manual frame-pointer unwind. backtrace() gives up almost immediately here because the EE
  // thread's frame pointer lives inside EE memory, but the chain itself is intact, so walking it
  // by hand recovers the C++ frames that led into a mips2c helper.
  {
    uintptr_t cur_fp = fp;
    write(2, "[EE-CRASH] FP-chain:\n", 21);
    for (int depth = 0; depth < 24 && cur_fp && (cur_fp & 0xf) == 0; depth++) {
      uintptr_t next_fp = ((const uintptr_t*)cur_fp)[0];
      uintptr_t ret_addr = ((const uintptr_t*)cur_fp)[1];
      if (!ret_addr) {
        break;
      }
      Dl_info di_f{};
      dladdr((void*)ret_addr, &di_f);
      n = __builtin_snprintf(buf, sizeof(buf), "  [%2d] 0x%016llx  %s + 0x%llx\n", depth,
                             (unsigned long long)ret_addr,
                             di_f.dli_sname ? di_f.dli_sname : "?",
                             di_f.dli_saddr ? (unsigned long long)(ret_addr - (uintptr_t)di_f.dli_saddr) : 0ULL);
      write(2, buf, n);
      if (next_fp <= cur_fp) {
        break;  // frame pointers must grow toward the stack base
      }
      cur_fp = next_fp;
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
  // Fault counter. The code heap is protected explicitly now -- writable while the linker fills
  // it, executable while GOAL runs -- so a fault here means some path writes into it without
  // asking first, and grepping for this line is how a boot is checked.
  {
    static _Atomic uint64_t s_sigbus_count = 0;
    uint64_t n = ++s_sigbus_count;
    if ((n % 10000) == 0) {
      char buf[64];
      int len = __builtin_snprintf(buf, sizeof(buf), "[SIGBUS] count=%llu\n", (unsigned long long)n);
      write(2, buf, len);
    }
  }

  if (ctx && g_ee_main_mem) {
    ucontext_t* uctx = (ucontext_t*)ctx;
    uintptr_t fault_addr = (uintptr_t)info->si_addr;
    uintptr_t ee_base = (uintptr_t)g_ee_main_mem;

    if (fault_addr >= ee_base && fault_addr < ee_base + EE_MAIN_MEM_SIZE) {
      char buf[256];
      int n = __builtin_snprintf(
          buf, sizeof(buf),
          "[EE-CRASH] unexpected W^X fault at EE+0x%zx: either a write to a page that is "
          "currently executable, or a null dereference.\n",
          (size_t)(fault_addr - ee_base));
      write(2, buf, n);
      auto& ss = uctx->uc_mcontext->__ss;
      dump_arm64_crash_context(ss.__pc, ss.__lr, ss.__sp, ss.__x, ss.__fp, fault_addr);
      struct sigaction sa_def{};
      sa_def.sa_handler = SIG_DFL;
      sigaction(sig, &sa_def, nullptr);
      raise(sig);
      return;
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
    struct sigaction sa_ill{};
    sa_ill.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sa_ill.sa_sigaction = [](int, siginfo_t*, void* ctx) {
      if (!ctx) { raise(SIGILL); return; }
      ucontext_t* uctx = (ucontext_t*)ctx;
      auto& ss = uctx->uc_mcontext->__ss;
      {
        // Report PC before dereferencing it: if PC itself is unmapped, reading the
        // instruction faults inside this handler and the crash goes unreported.
        char buf[128];
        int n = __builtin_snprintf(buf, sizeof(buf),
          "[EE-CRASH] SIGILL at PC=0x%016llx LR=0x%016llx\n",
          (unsigned long long)ss.__pc, (unsigned long long)ss.__lr);
        write(2, buf, n);
      }
      dump_arm64_crash_context(ss.__pc, ss.__lr, ss.__sp, ss.__x, ss.__fp,
                               ss.__pc);  // fault addr = pc for SIGILL
      struct sigaction sa{};
      sa.sa_handler = SIG_DFL;
      sigaction(SIGILL, &sa, nullptr);
      raise(SIGILL);
    };
    sigaction(SIGILL, &sa_ill, nullptr);
  }
#endif
  // Install SIGBUS handler to diagnose MAP_JIT protection faults
  {
    struct sigaction sa{};
    sa.sa_sigaction = sigbus_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGBUS, &sa, nullptr);
#if defined(__APPLE__) && defined(__aarch64__)
    // Writes to MAP_JIT pages from non-JIT code (e.g. C kernel calling kstrncat into
    // a string allocated in kcodeheap) are reported as SIGSEGV (EXC_BAD_ACCESS code=2),
    // not SIGBUS. Use the same handler — it gates on fault_addr being inside the
    // kcodeheap region and falls through to default if the instruction can't be decoded.
    sigaction(SIGSEGV, &sa, nullptr);
#endif
  }
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
