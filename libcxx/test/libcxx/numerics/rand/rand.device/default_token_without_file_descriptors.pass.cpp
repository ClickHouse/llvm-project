//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// REQUIRES: linux
// UNSUPPORTED: no-random-device

// <random>

// class random_device;

// ClickHouse: on Linux, the default token is served by getentropy instead of
// opening /dev/urandom, so a default-constructed random_device works when no
// file descriptor can be allocated. Opening /dev/urandom failed with EMFILE
// here before that change.

#include <random>
#include <cassert>
#include <cerrno>

#include <fcntl.h>
#include <sys/resource.h>

int main(int, char**) {
  // Make every descriptor allocation fail.
  rlimit limit;
  assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
  limit.rlim_cur = 0;
  assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);

  errno = 0;
  assert(open("/dev/urandom", O_RDONLY) == -1);
  assert(errno == EMFILE);

  std::random_device r;
  (void)r();
  assert(r.entropy() > 0);

  std::random_device explicit_default("/dev/urandom");
  (void)explicit_default();

  return 0;
}
