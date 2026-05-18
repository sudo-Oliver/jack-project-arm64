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
  // On Darwin 25 (macOS 26+): W^X is enforced system-wide. MAP_JIT is the only way to get
  // executable memory. GOAL code executes from MAP_JIT; heap WRITES from GOAL code are
  // emulated by the SIGBUS handler which toggles pthread_jit_write_protect_np per store.
  // C-side writes (init, linker) run with write-protect disabled (pthread_jit_write_protect_np(0)).
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
  // Darwin 25: EE memory is MAP_JIT. Start in write mode for C-side init (memset, linker, etc.).
  // We switch to exec mode just before invoking GOAL code (see call_goal / call_goal_on_stack).
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

    // Fault must be in EE memory (MAP_JIT region) to be a GOAL heap write
    uintptr_t fault_addr = (uintptr_t)info->si_addr;
    uintptr_t ee_base = (uintptr_t)g_ee_main_mem;
    if (fault_addr >= ee_base && fault_addr < ee_base + EE_MAIN_MEM_SIZE) {

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
      // Updates cur_pc by 4 on success, updates context registers for write-back forms.
      // Returns true if the instruction was a recognised store.
      auto exec_one = [&](uint64_t& cur_pc) -> bool {
        uint32_t instr = *(const uint32_t*)cur_pc;

        // ── 1. Register-offset GPR stores (GOAL-generated, LSL extend=011) ──
        {
          uint32_t m = instr & 0xFFE0FC00u;
          int bytes = 0;
          if      (m == 0xF8206800u) bytes = 8;
          else if (m == 0xB8206800u) bytes = 4;
          else if (m == 0x78206800u) bytes = 2;
          else if (m == 0x38206800u) bytes = 1;
          if (bytes) {
            int Rm = (instr >> 16) & 0x1F, Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t addr = gpr_base(Rn) + gpr(Rm);
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

        // ── 1b. UXTW register-offset GPR stores (C-compiler, extend=010) ──
        {
          uint32_t m = instr & 0xFFE0FC00u;
          int bytes = 0;
          if      (m == 0xF8204800u) bytes = 8;
          else if (m == 0xB8204800u) bytes = 4;
          else if (m == 0x78204800u) bytes = 2;
          else if (m == 0x38204800u) bytes = 1;
          if (bytes) {
            int Rm = (instr >> 16) & 0x1F, Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t addr = gpr_base(Rn) + (uint32_t)gpr(Rm);
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

        // ── 1c. SXTW register-offset GPR stores (C-compiler, extend=110) ──
        {
          uint32_t m = instr & 0xFFE0FC00u;
          int bytes = 0;
          if      (m == 0xF820C800u) bytes = 8;
          else if (m == 0xB820C800u) bytes = 4;
          else if (m == 0x7820C800u) bytes = 2;
          else if (m == 0x3820C800u) bytes = 1;
          if (bytes) {
            int Rm = (instr >> 16) & 0x1F, Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t addr = gpr_base(Rn) + (uint64_t)(int64_t)(int32_t)gpr(Rm);
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

        // ── 3. Unsigned-offset GPR stores ──
        {
          uint32_t top10 = instr & 0xFFC00000u;
          if (top10 == 0xF9000000u || top10 == 0xB9000000u ||
              top10 == 0x79000000u || top10 == 0x39000000u) {
            int scale = (instr >> 30) & 0x3;
            uint32_t imm12 = (instr >> 10) & 0xFFF;
            int Rn = (instr >> 5) & 0x1F, Rt = instr & 0x1F;
            uint64_t addr = gpr_base(Rn) + ((uint64_t)imm12 << scale);
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
    }
  }
#endif

  // Unrecognized SIGBUS: print diagnostic and crash
  fprintf(stderr, "[EE-CRASH] SIGBUS at fault addr %p\n", info->si_addr);
  if (ctx && g_ee_main_mem) {
    uintptr_t fault = (uintptr_t)info->si_addr;
    uintptr_t base  = (uintptr_t)g_ee_main_mem;
    fprintf(stderr, "[EE-CRASH] fault %s EE memory\n",
            (fault >= base && fault < base + EE_MAIN_MEM_SIZE) ? "IN" : "OUTSIDE");
#if defined(__aarch64__)
    ucontext_t* uctx = (ucontext_t*)ctx;
    uint64_t pc = uctx->uc_mcontext->__ss.__pc;
    fprintf(stderr, "[EE-CRASH] faulting PC = %p", (void*)pc);
    if (fault >= base && fault < base + EE_MAIN_MEM_SIZE) {
      fprintf(stderr, " (GOAL offset 0x%x)", (uint32_t)(fault - base));
    }
    fprintf(stderr, "\n");
    // Dump the faulting instruction for diagnosis
    fprintf(stderr, "[EE-CRASH] instr at PC: 0x%08x\n", *(const uint32_t*)pc);
#endif
  }
  fflush(stderr);
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
      ucontext_t* uctx = (ucontext_t*)ctx;
      uint64_t pc = uctx->uc_mcontext->__ss.__pc;
      uint64_t sp = uctx->uc_mcontext->__ss.__sp;
      uint64_t x30 = uctx->uc_mcontext->__ss.__lr;
      char buf[256];
      int n = __builtin_snprintf(buf, sizeof(buf),
        "[EE-CRASH] SIGILL at PC=%p SP=%p LR=%p instr=0x%08x\n",
        (void*)pc, (void*)sp, (void*)x30, *(const uint32_t*)pc);
      write(2, buf, n);
      // Also print offset into EE memory if applicable
      if (g_ee_main_mem) {
        uintptr_t base = (uintptr_t)g_ee_main_mem;
        if (pc >= base && pc < base + EE_MAIN_MEM_SIZE) {
          int n2 = __builtin_snprintf(buf, sizeof(buf),
            "[EE-CRASH] PC is GOAL offset 0x%x\n", (uint32_t)(pc - base));
          write(2, buf, n2);
        } else {
          write(2, "[EE-CRASH] PC is in C runtime (not GOAL)\n", 41);
        }
      }
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
