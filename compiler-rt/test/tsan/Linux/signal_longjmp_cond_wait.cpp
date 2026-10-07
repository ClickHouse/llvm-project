// RUN: %clangxx_tsan -O1 %s -o %t && %run %t 2>&1 | FileCheck %s
// REQUIRES: glibc

// A signal that is already pending when pthread_cond_wait is called must be
// handled before pthread_cleanup_push. If its handler siglongjmps out of the
// cleanup frame, glibc keeps the frame registered and a later pthread_exit
// unwinds into the dead stack.

#include "../test.h"
#include <setjmp.h>
#include <signal.h>
#include <sys/syscall.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static sigjmp_buf env;
static volatile sig_atomic_t in_wait;
static volatile sig_atomic_t handled_in_wait;
static int sent;

static void Handler(int sig) {
  handled_in_wait = in_wait;
  siglongjmp(env, 1);
}

static void *Thread(void *arg) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  if (sigsetjmp(env, 1) == 0) {
    pthread_mutex_lock(&mtx);
    while (!__atomic_load_n(&sent, __ATOMIC_ACQUIRE))
      usleep(1000);
    // The raw syscall is not intercepted, so the signal is delivered on its
    // return, deferred by tsan, and still pending when the wait starts.
    if (syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &set, nullptr, _NSIG / 8)) {
      perror("rt_sigprocmask");
      exit(1);
    }
    in_wait = 1;
    pthread_cond_wait(&cond, &mtx);
    fprintf(stderr, "FAILED: woke up\n");
    exit(1);
  }
  fprintf(stderr, "LONGJMP in_wait=%d\n", (int)handled_in_wait);
  pthread_exit(nullptr);
}

int main() {
  struct sigaction act = {};
  act.sa_handler = Handler;
  if (sigaction(SIGUSR1, &act, nullptr)) {
    perror("sigaction");
    exit(1);
  }
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  // The thread inherits the blocked mask.
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  pthread_t th;
  pthread_create(&th, nullptr, Thread, nullptr);
  pthread_kill(th, SIGUSR1);
  __atomic_store_n(&sent, 1, __ATOMIC_RELEASE);
  pthread_join(th, nullptr);
  fprintf(stderr, "DONE\n");
}

// CHECK-NOT: ThreadSanitizer
// CHECK: LONGJMP in_wait=1
// CHECK-NOT: ThreadSanitizer
// CHECK: DONE
