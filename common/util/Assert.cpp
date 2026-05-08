#include "Assert.h"

#ifndef NO_ASSERT

#include <cstdio>
#include <cstdlib>
#include <string_view>
#ifdef __APPLE__
#include <execinfo.h>
#endif

#include "common/log/log.h"

void private_assert_failed(const char* expr,
                           const char* file,
                           int line,
                           const char* function,
                           const char* msg) {
#ifdef __APPLE__
  void* bt[64];
  int n = backtrace(bt, 64);
  char** syms = backtrace_symbols(bt, n);
  fprintf(stderr, "Backtrace:\n");
  for (int i = 0; i < n; i++) {
    fprintf(stderr, "  %s\n", syms[i]);
  }
  free(syms);
  fflush(stderr);
#endif
  if (!msg || msg[0] == '\0') {
    std::string log = fmt::format("Assertion failed: '{}'\n\tSource: {}:{}\n\tFunction: {}\n", expr,
                                  file, line, function);
    lg::die("{}", log);
  } else {
    std::string log =
        fmt::format("Assertion failed: '{}'\n\tMessage: {}\n\tSource: {}:{}\n\tFunction: {}\n",
                    expr, msg, file, line, function);
    lg::die("{}", log);
  }
  abort();
}

void private_assert_failed(const char* expr,
                           const char* file,
                           int line,
                           const char* function,
                           const std::string_view& msg) {
  if (msg.empty()) {
    private_assert_failed(expr, file, line, function);
  } else {
    private_assert_failed(expr, file, line, function, msg.data());
  }
}

#endif
