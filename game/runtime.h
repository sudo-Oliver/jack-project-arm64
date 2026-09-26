#pragma once

/*!
 * @file runtime.h
 * Setup and launcher for the runtime.
 */

#include <thread>

#include "common/common_types.h"
#include "common/versions/versions.h"

#include "game/common/game_common_types.h"
#include "game/kernel/common/kboot.h"
#include "system/background_worker.h"

extern u8* g_ee_main_mem;
// ARM64 Darwin: set true whenever link_and_exec writes JIT code without flushing icache.
// call_goal / call_goal_on_stack check this and flush only when needed.
// On ARM64 Apple (Darwin 25+): GOAL stack lives outside MAP_JIT EE memory.
// This points to the top of a separate PROT_READ|PROT_WRITE stack region.
extern u8* g_goal_jit_stack_top;
// Name of the GOAL module currently executing its top-level (updated by jak1_finish).
// Used by crash handlers. Safe to read async-signal-safely (written before GOAL runs).
extern const char* g_current_goal_module;
extern GameVersion g_game_version;
extern BackgroundWorker g_background_worker;
extern int g_server_port;

RuntimeExitStatus exec_runtime(GameLaunchOptions game_options, int argc, const char** argv);

extern std::thread::id g_main_thread_id;
