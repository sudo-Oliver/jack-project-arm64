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
  fprintf(stderr, "[EE-DEBUG] ee_runner started\n");
  fflush(stderr);
  prof().root_event();
  fprintf(stderr, "[EE-DEBUG] after prof().root_event()\n");
  fflush(stderr);
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
    g_ee_main_mem =
        (u8*)mmap((void*)EE_MAIN_MEM_MAP, EE_MAIN_MEM_SIZE, PROT_EXEC | PROT_READ | PROT_WRITE,
#if defined(__aarch64__) && defined(__APPLE__)
                  MAP_ANONYMOUS | MAP_PRIVATE | MAP_JIT, -1, 0);
#else
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
#endif
  }

  if (g_ee_main_mem == (u8*)(-1)) {
    fprintf(stderr, "[EE-DEBUG] mmap FAILED: %s\n", strerror(errno));
    fflush(stderr);
    lg::debug("Failed to initialize main memory! {}", strerror(errno));
    iface.initialization_complete();
    return;
  }

  fprintf(stderr, "[EE-DEBUG] mmap OK at %p\n", (void*)g_ee_main_mem);
  fflush(stderr);

#if defined(__aarch64__) && defined(__APPLE__)
  // Replace data regions with regular (non-MAP_JIT) pages so GOAL heap writes
  // in exec mode produce no SIGBUS.  The code region [EE_CODE_HEAP_START,
  // EE_CODE_HEAP_END) is left as MAP_JIT.
  {
    void* r1 = mmap(g_ee_main_mem, EE_CODE_HEAP_START,
                    PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    void* r2 = mmap(g_ee_main_mem + EE_CODE_HEAP_END,
                    EE_MAIN_MEM_SIZE - EE_CODE_HEAP_END,
                    PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (r1 == MAP_FAILED || r2 == MAP_FAILED) {
      fprintf(stderr, "[EE-DEBUG] EE data-region split mmap FAILED: %s\n", strerror(errno));
      fflush(stderr);
      iface.initialization_complete();
      return;
    }
    fprintf(stderr, "[EE-DEBUG] EE split: data[0..%uMB), code[%uMB..%uMB), data[%uMB..%uMB)\n",
            EE_CODE_HEAP_START >> 20,
            EE_CODE_HEAP_START >> 20, EE_CODE_HEAP_END >> 20,
            EE_CODE_HEAP_END >> 20, (unsigned)EE_MAIN_MEM_SIZE >> 20);
    fflush(stderr);
  }
  // Start in write mode for C-side init (memset, linker, kinitheap, etc.).
  pthread_jit_write_protect_np(0);
  fprintf(stderr, "[EE-DEBUG] MAP_JIT write mode enabled for C init\n");
  fflush(stderr);
#endif

  lg::info("Main memory mapped at 0x{:016x}", (u64)(g_ee_main_mem));
  lg::info("Main memory size 0x{:x} bytes ({:.3f} MB)", EE_MAIN_MEM_SIZE,
           (double)EE_MAIN_MEM_SIZE / (1 << 20));

  lg::info("[EE] Initialization complete!");
  iface.initialization_complete();

  lg::info("[EE] Run!");
  fprintf(stderr, "[EE-DEBUG] starting memset\n"); fflush(stderr);
  memset((void*)g_ee_main_mem, 0, EE_MAIN_MEM_SIZE);
  fprintf(stderr, "[EE-DEBUG] memset done\n"); fflush(stderr);

  // prevent access to the first 512 kB of memory.
  // On the PS2 this is the kernel and can't be accessed either.
  // this may not work well on systems with a page size > 1 MB.
  {
    int mp_result = mprotect((void*)g_ee_main_mem, EE_MAIN_MEM_LOW_PROTECT, PROT_NONE);
    fprintf(stderr, "[EE-DEBUG] mprotect LOW_PROTECT result=%d (0=ok, errno=%d)\n", mp_result, errno); fflush(stderr);
  }
  fileio_init_globals();
  fprintf(stderr, "[EE-DEBUG] fileio_init done\n"); fflush(stderr);
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
  fprintf(stderr, "[EE-DEBUG] all init_globals done, calling allow_debugging\n"); fflush(stderr);

  // Added for OpenGOAL's debugger
  xdbg::allow_debugging();
  fprintf(stderr, "[EE-DEBUG] calling goal_main\n"); fflush(stderr);

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

  // Fault address context
  if (g_ee_main_mem) {
    uintptr_t base = (uintptr_t)g_ee_main_mem;
    n = __builtin_snprintf(buf, sizeof(buf),
      "[EE-CRASH] fault addr 0x%016llx  EE offset 0x%llx\n",
      (unsigned long long)fault_addr,
      (unsigned long long)(fault_addr - base));
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

  // Instructions around PC (8 before, 4 after)
  write(2, "[EE-CRASH] Instructions around PC:\n", 35);
  const uint32_t* code = (const uint32_t*)pc;
  for (int i = -8; i <= 4; i++) {
    n = __builtin_snprintf(buf, sizeof(buf),
      "  [%+3d] 0x%016llx: 0x%08x%s\n",
      i, (unsigned long long)(code + i), code[i],
      (i == 0) ? "  <-- FAULT" : "");
    write(2, buf, n);
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
  (void)sig;

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
    uint64_t pc = uctx->uc_mcontext->__ss.__pc;

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

      // Read the faulting instruction (MAP_JIT is readable in exec mode)
      uint32_t instr = *(const uint32_t*)pc;

      auto& ss = uctx->uc_mcontext->__ss;
      auto& ns = uctx->uc_mcontext->__ns;
      auto gpr = [&](int r) -> uint64_t {
        if (r == 31) return 0;  // XZR / WZR reads as zero
        if (r == 29) return ss.__fp;
        if (r == 30) return ss.__lr;
        return ss.__x[r];
      };
      auto gpr_base = [&](int r) -> uint64_t {  // Rn base: r31 = SP not XZR
        if (r == 31) return ss.__sp;
        if (r == 29) return ss.__fp;
        if (r == 30) return ss.__lr;
        return ss.__x[r];
      };
      auto do_gpr_store = [&](uint64_t addr, int Rt, int bytes) {
        uint64_t val = gpr(Rt);
        pthread_jit_write_protect_np(0);
        switch (bytes) {
          case 1: *(uint8_t* )addr = (uint8_t )val; break;
          case 2: *(uint16_t*)addr = (uint16_t)val; break;
          case 4: *(uint32_t*)addr = (uint32_t)val; break;
          case 8: *(uint64_t*)addr = val; break;
        }
        pthread_jit_write_protect_np(1);
        uctx->uc_mcontext->__ss.__pc = pc + 4;
      };
      auto do_simd_store = [&](uint64_t addr, int Rt, int bytes) {
        pthread_jit_write_protect_np(0);
        if (bytes == 16) {
          __uint128_t val = ns.__v[Rt];
          __builtin_memcpy((void*)addr, &val, 16);
        } else {
          uint32_t val;
          __builtin_memcpy(&val, &ns.__v[Rt], 4);
          *(uint32_t*)addr = val;
        }
        pthread_jit_write_protect_np(1);
        uctx->uc_mcontext->__ss.__pc = pc + 4;
      };

      // ── 1. Register-offset GPR stores (all extend options) ───────────────────
      // STR  Xt/Wt/Wt/Wt [Xn, Rm, extend{#shift}]
      // Handles LSL (GOAL JIT), UXTW/SXTW/SXTX (C++ compiler: e.g. g_ee_main_mem + u32_offset)
      // Fixed bits: [31:22] = size/opc/bit21, [11:10] = 10 (always for register-offset)
      // Mask: preserve [31:22] and [11:10], clear Rm[20:16] + option/S[15:12] + Rn[9:5] + Rt[4:0]
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
          do_gpr_store(gpr_base(Rn) + offset, Rt, bytes);
          return;
        }
      }

      // ── 2. Register-offset SIMD stores (GOAL-generated) ───────────────────
      // STR St, [Xn, Xm]  0xBC206800   STR Qt, [Xn, Xm]  0x3CA06800
      {
        uint32_t m = instr & 0xFFE0FC00u;
        int bytes = 0;
        if      (m == 0xBC206800u) bytes = 4;
        else if (m == 0x3CA06800u) bytes = 16;
        if (bytes) {
          int Rm = (instr >> 16) & 0x1F;
          int Rn = (instr >>  5) & 0x1F;
          int Rt = instr & 0x1F;
          do_simd_store(gpr_base(Rn) + gpr(Rm), Rt, bytes);
          return;
        }
      }

      // ── 2b. Single-register SIMD post-index / pre-index stores ───────────────
      // STR Qt [Xn],#imm9  base 0x3C800400  (post: bits[11:10]=01)
      // STR Qt [Xn,#imm9]! base 0x3C800C00  (pre:  bits[11:10]=11)
      // STR Dt [Xn],#imm9  base 0xFC000400  STR St  base 0xBC000400
      // mask 0xFFE00C00 identifies the class; bytes = 16 (Q) / 8 (D) / 4 (S)
      {
        uint32_t m = instr & 0xFFE00C00u;
        int bytes = 0;
        bool pre = false;
        if      (m == 0x3C800400u) { bytes = 16; }
        else if (m == 0x3C800C00u) { bytes = 16; pre = true; }
        else if (m == 0xFC000400u) { bytes =  8; }
        else if (m == 0xFC000C00u) { bytes =  8; pre = true; }
        else if (m == 0xBC000400u) { bytes =  4; }
        else if (m == 0xBC000C00u) { bytes =  4; pre = true; }
        if (bytes) {
          int32_t imm9 = (int32_t)((instr >> 12) & 0x1FF);
          if (imm9 & 0x100) imm9 |= ~(int32_t)0x1FF;
          int Rn = (instr >> 5) & 0x1F;
          int Rt = instr & 0x1F;
          uint64_t base = gpr_base(Rn);
          uint64_t addr = pre ? (base + (int64_t)imm9) : base;
          do_simd_store(addr, Rt, bytes);
          uint64_t wb = base + (int64_t)imm9;  // same for pre and post
          if (Rn < 29)       ss.__x[Rn] = wb;
          else if (Rn == 29) ss.__fp = wb;
          else if (Rn == 31) ss.__sp = wb;
          return;
        }
      }

      // ── 2c. Unsigned-offset SIMD stores (C compiler scalar/vector stores) ────
      // STR Qt [Xn,#imm*16] 0x3D800000  STR Dt 0xFD000000  STR St 0xBD000000
      // mask 0xFFC00000; scale = 16 (Q) / 8 (D) / 4 (S)
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
          do_simd_store(addr, Rt, bytes);
          return;
        }
      }

      // ── 3. Unsigned-offset GPR stores (C compiler) ────────────────────────
      // STR Xt [Xn,#imm*8] 0xF9?????? STR Wt 0xB9 STRH 0x79 STRB 0x39
      // Fixed bits[31:22]: mask 0xFFC00000; scale = bits[31:30]
      {
        uint32_t top10 = instr & 0xFFC00000u;
        if (top10 == 0xF9000000u || top10 == 0xB9000000u ||
            top10 == 0x79000000u || top10 == 0x39000000u) {
          int scale   = (instr >> 30) & 0x3;
          uint32_t imm12 = (instr >> 10) & 0xFFF;
          int Rn = (instr >> 5) & 0x1F;
          int Rt = instr & 0x1F;
          uint64_t addr = gpr_base(Rn) + ((uint64_t)imm12 << scale);
          do_gpr_store(addr, Rt, 1 << scale);
          return;
        }
      }

      // ── 4. STUR (unscaled signed offset, bits[11:10]=00) ──────────────────
      // STUR Xt 0xF8???000 STUR Wt 0xB8???000 STURH 0x78 STURB 0x38
      // mask 0xFFE00C00, bits[11:10]=00
      {
        uint32_t m = instr & 0xFFE00C00u;
        if (m == 0xF8000000u || m == 0xB8000000u ||
            m == 0x78000000u || m == 0x38000000u) {
          int scale  = (instr >> 30) & 0x3;
          int32_t imm9 = (int32_t)((instr >> 12) & 0x1FF);
          if (imm9 & 0x100) imm9 |= ~(int32_t)0x1FF;  // sign-extend 9→32 bits
          int Rn = (instr >> 5) & 0x1F;
          int Rt = instr & 0x1F;
          uint64_t addr = gpr_base(Rn) + (int64_t)imm9;
          do_gpr_store(addr, Rt, 1 << scale);
          return;
        }
      }

      // ── 5. Post-indexed GPR stores (bits[11:10]=01) ──────────────────────
      // STR Xt, [Xn], #imm9   mask 0xFFE00C00, value bits[11:10]=01
      {
        uint32_t m = instr & 0xFFE00C00u;
        if (m == 0xF8000400u || m == 0xB8000400u ||
            m == 0x78000400u || m == 0x38000400u) {
          int scale  = (instr >> 30) & 0x3;
          int32_t imm9 = (int32_t)((instr >> 12) & 0x1FF);
          if (imm9 & 0x100) imm9 |= ~(int32_t)0x1FF;
          int Rn = (instr >> 5) & 0x1F;
          int Rt = instr & 0x1F;
          uint64_t base = gpr_base(Rn);
          do_gpr_store(base, Rt, 1 << scale);  // store to [Rn] before writeback
          uint64_t wb = base + (int64_t)imm9;
          if (Rn < 29)       ss.__x[Rn] = wb;
          else if (Rn == 29) ss.__fp = wb;
          else if (Rn == 31) ss.__sp = wb;
          return;
        }
      }

      // ── 5b. Pre-indexed GPR stores (bits[11:10]=11) ───────────────────────
      // STR Xt, [Xn, #imm9]!   mask 0xFFE00C00, value bits[11:10]=11
      {
        uint32_t m = instr & 0xFFE00C00u;
        if (m == 0xF8000C00u || m == 0xB8000C00u ||
            m == 0x78000C00u || m == 0x38000C00u) {
          int scale  = (instr >> 30) & 0x3;
          int32_t imm9 = (int32_t)((instr >> 12) & 0x1FF);
          if (imm9 & 0x100) imm9 |= ~(int32_t)0x1FF;
          int Rn = (instr >> 5) & 0x1F;
          int Rt = instr & 0x1F;
          uint64_t addr = gpr_base(Rn) + (int64_t)imm9;
          do_gpr_store(addr, Rt, 1 << scale);
          if (Rn < 29)       ss.__x[Rn] = addr;
          else if (Rn == 29) ss.__fp = addr;
          else if (Rn == 31) ss.__sp = addr;
          return;
        }
      }

      // ── 6. STNP / STP GPR pair stores (64-bit: opc=10 V=0) ───────────────
      // STNP: 0xA8000000  STP signed-offset: 0xA9000000
      // STP post-indexed: 0xA8800000  STP pre-indexed: 0xA9800000
      // Stores Rt at [addr], Rt2 at [addr+8]; post/pre-indexed write back Rn
      {
        uint32_t top10 = instr & 0xFFC00000u;
        if (top10 == 0xA8000000u || top10 == 0xA9000000u ||
            top10 == 0xA8800000u || top10 == 0xA9800000u) {
          int32_t imm7 = (int32_t)((instr >> 15) & 0x7F);
          if (imm7 & 0x40) imm7 |= ~(int32_t)0x7F;
          int Rt2 = (instr >> 10) & 0x1F;
          int Rn  = (instr >>  5) & 0x1F;
          int Rt  = instr & 0x1F;
          uint64_t base = gpr_base(Rn);
          // post-indexed: store to [Rn] then Rn += imm7*8
          // pre-indexed:  store to [Rn + imm7*8] and write-back
          uint64_t addr = (top10 == 0xA8800000u) ? base : base + (int64_t)imm7 * 8;
          pthread_jit_write_protect_np(0);
          *(uint64_t*)addr       = gpr(Rt);
          *(uint64_t*)(addr + 8) = gpr(Rt2);
          pthread_jit_write_protect_np(1);
          // write-back for pre/post-indexed
          if (top10 == 0xA8800000u || top10 == 0xA9800000u) {
            uint64_t wb = base + (int64_t)imm7 * 8;
            if (Rn < 29)       ss.__x[Rn] = wb;
            else if (Rn == 29) ss.__fp = wb;
            else if (Rn == 31) ss.__sp = wb;
          }
          uctx->uc_mcontext->__ss.__pc = pc + 4;
          return;
        }
      }

      // ── 6b. STNP / STP W-pair stores (32-bit GPR: opc=00 V=0) ───────────────
      // STNP W: 0x28000000  STP post-indexed: 0x28800000
      // STP signed-offset: 0x29000000  STP pre-indexed: 0x29800000
      // Stores Rt at [addr], Rt2 at [addr+4]
      {
        uint32_t top10 = instr & 0xFFC00000u;
        if (top10 == 0x28000000u || top10 == 0x29000000u ||
            top10 == 0x28800000u || top10 == 0x29800000u) {
          int32_t imm7 = (int32_t)((instr >> 15) & 0x7F);
          if (imm7 & 0x40) imm7 |= ~(int32_t)0x7F;
          int Rt2 = (instr >> 10) & 0x1F;
          int Rn  = (instr >>  5) & 0x1F;
          int Rt  = instr & 0x1F;
          uint64_t base = gpr_base(Rn);
          uint64_t addr = (top10 == 0x28800000u) ? base : base + (int64_t)imm7 * 4;
          pthread_jit_write_protect_np(0);
          *(uint32_t*)addr       = (uint32_t)gpr(Rt);
          *(uint32_t*)(addr + 4) = (uint32_t)gpr(Rt2);
          pthread_jit_write_protect_np(1);
          if (top10 == 0x28800000u || top10 == 0x29800000u) {
            uint64_t wb = base + (int64_t)imm7 * 4;
            if (Rn < 29)       ss.__x[Rn] = wb;
            else if (Rn == 29) ss.__fp = wb;
            else if (Rn == 31) ss.__sp = wb;
          }
          uctx->uc_mcontext->__ss.__pc = pc + 4;
          return;
        }
      }

      // ── 7. STNP / STP Q-pair (128-bit SIMD: opc=10 V=1) ──────────────────
      // STNP Qt: 0xAC000000  STP Qt signed-offset: 0xAD000000  (mask 0xFFC00000)
      // Stores Rt at [addr], Rt2 at [addr+16]
      {
        uint32_t top10 = instr & 0xFFC00000u;
        if (top10 == 0xAC000000u || top10 == 0xAD000000u) {
          int32_t imm7 = (int32_t)((instr >> 15) & 0x7F);
          if (imm7 & 0x40) imm7 |= ~(int32_t)0x7F;
          int Rt2 = (instr >> 10) & 0x1F;
          int Rn  = (instr >>  5) & 0x1F;
          int Rt  = instr & 0x1F;
          uint64_t addr = gpr_base(Rn) + (int64_t)imm7 * 16;
          pthread_jit_write_protect_np(0);
          __uint128_t v1 = ns.__v[Rt], v2 = ns.__v[Rt2];
          __builtin_memcpy((void*)addr,       &v1, 16);
          __builtin_memcpy((void*)(addr + 16), &v2, 16);
          pthread_jit_write_protect_np(1);
          uctx->uc_mcontext->__ss.__pc = pc + 4;
          return;
        }
      }

      // ── 8. STNP / STP D-pair (64-bit SIMD: opc=01 V=1) ──────────────────
      // STNP Dt: 0x6C000000  STP Dt: 0x6D000000  (mask 0xFFC00000)
      {
        uint32_t top10 = instr & 0xFFC00000u;
        if (top10 == 0x6C000000u || top10 == 0x6D000000u) {
          int32_t imm7 = (int32_t)((instr >> 15) & 0x7F);
          if (imm7 & 0x40) imm7 |= ~(int32_t)0x7F;
          int Rt2 = (instr >> 10) & 0x1F;
          int Rn  = (instr >>  5) & 0x1F;
          int Rt  = instr & 0x1F;
          uint64_t addr = gpr_base(Rn) + (int64_t)imm7 * 8;
          pthread_jit_write_protect_np(0);
          uint64_t v1, v2;
          __builtin_memcpy(&v1, &ns.__v[Rt],  8);
          __builtin_memcpy(&v2, &ns.__v[Rt2], 8);
          *(uint64_t*)addr       = v1;
          *(uint64_t*)(addr + 8) = v2;
          pthread_jit_write_protect_np(1);
          uctx->uc_mcontext->__ss.__pc = pc + 4;
          return;
        }
      }

      // ── 9. STNP / STP S-pair (32-bit SIMD: opc=00 V=1) ──────────────────
      // STNP St: 0x2C000000  STP St: 0x2D000000  (mask 0xFFC00000)
      {
        uint32_t top10 = instr & 0xFFC00000u;
        if (top10 == 0x2C000000u || top10 == 0x2D000000u) {
          int32_t imm7 = (int32_t)((instr >> 15) & 0x7F);
          if (imm7 & 0x40) imm7 |= ~(int32_t)0x7F;
          int Rt2 = (instr >> 10) & 0x1F;
          int Rn  = (instr >>  5) & 0x1F;
          int Rt  = instr & 0x1F;
          uint64_t addr = gpr_base(Rn) + (int64_t)imm7 * 4;
          pthread_jit_write_protect_np(0);
          uint32_t v1, v2;
          __builtin_memcpy(&v1, &ns.__v[Rt],  4);
          __builtin_memcpy(&v2, &ns.__v[Rt2], 4);
          *(uint32_t*)addr       = v1;
          *(uint32_t*)(addr + 4) = v2;
          pthread_jit_write_protect_np(1);
          uctx->uc_mcontext->__ss.__pc = pc + 4;
          return;
        }
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
  sigaction(SIGBUS, &sa, nullptr);
  raise(SIGBUS);
}

RuntimeExitStatus exec_runtime(GameLaunchOptions game_options, int argc, const char** argv) {
  // Install SIGILL handler to catch illegal-instruction crashes in GOAL code
#if defined(__APPLE__) && defined(__aarch64__)
  {
    struct sigaction sa_ill{};
    sa_ill.sa_flags = SA_SIGINFO;
    sa_ill.sa_sigaction = [](int, siginfo_t*, void* ctx) {
      if (!ctx) { raise(SIGILL); return; }
      ucontext_t* uctx = (ucontext_t*)ctx;
      auto& ss = uctx->uc_mcontext->__ss;
      {
        char buf[128];
        int n = __builtin_snprintf(buf, sizeof(buf),
          "[EE-CRASH] SIGILL at PC=0x%016llx instr=0x%08x\n",
          (unsigned long long)ss.__pc, *(const uint32_t*)ss.__pc);
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
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, nullptr);
    fprintf(stderr, "[EE-DEBUG] SIGBUS handler installed\n"); fflush(stderr);
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
  munmap(g_ee_main_mem, EE_MAIN_MEM_SIZE);
  Discord_Shutdown();
  return MasterExit;
}
