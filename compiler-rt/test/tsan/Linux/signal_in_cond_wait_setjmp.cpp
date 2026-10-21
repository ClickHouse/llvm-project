// RUN: %clangxx_tsan -O1 %s -o %t && %run %t 2>&1 | FileCheck %s
// REQUIRES: glibc

// pthread_cond_wait must not handle signals synchronously before
// pthread_cleanup_push: its setjmp allocates the thread's first jmp_buf from
// the internal allocator, and a handler that needs the allocator too (here
// via sigsetjmp) would wait for a lock held by the interrupted thread.

#include "../test.h"
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>

static int handled;

static void Handler(int sig) {
  sigjmp_buf env;
  if (sigsetjmp(env, 0))
    return;
  __atomic_store_n(&handled, 1, __ATOMIC_RELAXED);
}

static void *Thread(void *arg) {
  // One signal per thread, timed to arrive while the thread enters its first
  // pthread_cond_timedwait.
  struct sigevent sev;
  memset(&sev, 0, sizeof(sev));
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = SIGUSR1;
  sev._sigev_un._tid = syscall(SYS_gettid);
  timer_t timer;
  if (timer_create(CLOCK_MONOTONIC, &sev, &timer)) {
    perror("timer_create");
    exit(1);
  }
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  its.it_value.tv_nsec = 1 + (uintptr_t)arg;
  if (timer_settime(timer, 0, &its, nullptr)) {
    perror("timer_settime");
    exit(1);
  }

  pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t c = PTHREAD_COND_INITIALIZER;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += 100000;
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000;
  }
  pthread_mutex_lock(&m);
  pthread_cond_timedwait(&c, &m, &ts);
  pthread_mutex_unlock(&m);
  timer_delete(timer);
  return nullptr;
}

int main() {
  // Turn a deadlock into a failure.
  alarm(60);
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = Handler;
  if (sigaction(SIGUSR1, &sa, nullptr)) {
    perror("sigaction");
    exit(1);
  }

  const int kWaves = 20;
  const int kThreads = 128;
  pthread_t th[kThreads];
  for (int w = 0; w < kWaves; w++) {
    for (int i = 0; i < kThreads; i++) {
      uintptr_t delay_ns = (w * kThreads + i) * 7919 % 20000;
      if (pthread_create(&th[i], nullptr, Thread, (void *)delay_ns)) {
        fprintf(stderr, "pthread_create failed\n");
        exit(1);
      }
    }
    for (int i = 0; i < kThreads; i++)
      pthread_join(th[i], nullptr);
  }
  fprintf(stderr, "DONE handled=%d\n",
          __atomic_load_n(&handled, __ATOMIC_RELAXED));
  return 0;
}

// CHECK: DONE handled=1
