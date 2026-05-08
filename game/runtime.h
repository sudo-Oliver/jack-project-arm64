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
// On ARM64 Apple (Darwin 25+): GOAL stack lives outside MAP_JIT EE memory.
// This points to the top of a separate PROT_READ|PROT_WRITE stack region.
extern u8* g_goal_jit_stack_top;
extern GameVersion g_game_version;
extern BackgroundWorker g_background_worker;
extern int g_server_port;

RuntimeExitStatus exec_runtime(GameLaunchOptions game_options, int argc, const char** argv);

extern std::thread::id g_main_thread_id;
