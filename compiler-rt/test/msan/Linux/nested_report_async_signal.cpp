// An asynchronous signal delivered while a report is being printed must not
// abort the report. ScopedErrorReportLock treats same-thread reentrancy as a
// nested bug and calls internal__exit() without printing anything, so the
// original report used to be lost with zero frames.
//
// The signal is raised from __sanitizer_on_print, which the runtime calls for
// every line it prints. That happens while the report lock is held and after
// the first line of the report has already been written, so the signal always
// lands inside the report window instead of at a timer-dependent moment.

// RUN: %clangxx_msan -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -O1 %s -o %t && \
// RUN:     not %run %t >%t.out 2>&1
// RUN: FileCheck %s -implicit-check-not="nested bug in the same thread" < %t.out

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static volatile int sink;
static volatile sig_atomic_t armed;
static volatile sig_atomic_t raised;

// Reads uninitialized memory, so the handler itself wants to report.
static void handler(int, siginfo_t *, void *) {
  char *p = (char *)malloc(64);
  if (p[13])
    sink = 1;
  free(p);
}

__attribute__((disable_sanitizer_instrumentation)) static bool
contains(const char *str, const char *sub) {
  for (; *str; ++str) {
    const char *a = str;
    const char *b = sub;
    while (*b && *a == *b) {
      ++a;
      ++b;
    }
    if (!*b)
      return true;
  }
  return false;
}

// Overrides the weak definition in the runtime, so it runs inside the report.
// Fire once, on the report's own first line.
__attribute__((disable_sanitizer_instrumentation)) extern "C" void
__sanitizer_on_print(const char *str) {
  if (armed && !raised && contains(str, "use-of-uninitialized-value")) {
    raised = 1;
    raise(SIGUSR1);
  }
}

__attribute__((noinline)) void report_here() {
  char *q = (char *)malloc(64);
  if (q[7])
    sink = 2;
  free(q);
}

int main() {
  struct sigaction sa = {};
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  if (sigaction(SIGUSR1, &sa, nullptr))
    return 2;

  armed = 1;
  report_here();
  printf("unreachable\n");
  return 0;
}

// The surviving report must be report_here's: nothing raises the signal before
// the runtime starts printing that report.
// CHECK: WARNING: MemorySanitizer: use-of-uninitialized-value
// CHECK: {{^ +#0 .*report_here}}
// CHECK: {{^ +#1 }}
// CHECK: Uninitialized value was created by a heap allocation
// CHECK: SUMMARY: MemorySanitizer: use-of-uninitialized-value{{.*}}report_here
// CHECK-NOT: unreachable
