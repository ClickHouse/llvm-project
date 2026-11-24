//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <__config>

#if defined(_LIBCPP_USING_WIN32_RANDOM)
// Must be defined before including stdlib.h to enable rand_s().
#  define _CRT_RAND_S
#endif // defined(_LIBCPP_USING_WIN32_RANDOM)

#include <__system_error/throw_system_error.h>
#include <limits>
#include <random>
#include <string>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_LIBCPP_USING_GETENTROPY)
#  include <sys/random.h>
#elif defined(_LIBCPP_USING_DEV_RANDOM)
#  include <fcntl.h>
#  include <unistd.h>
#  if __has_include(<sys/ioctl.h>) && __has_include(<linux/random.h>)
#    include <linux/random.h>
#    include <sys/ioctl.h>
#  endif
#elif defined(_LIBCPP_USING_FUCHSIA_CPRNG)
#  include <zircon/syscalls.h>
#endif

_LIBCPP_BEGIN_NAMESPACE_STD
_LIBCPP_BEGIN_EXPLICIT_ABI_ANNOTATIONS

#if defined(_LIBCPP_USING_GETENTROPY)

random_device::random_device(const string& __token) {
  if (__token != "/dev/urandom")
    std::__throw_system_error(ENOENT, ("random device not supported " + __token).c_str());
}

random_device::~random_device() {}

unsigned random_device::operator()() {
  unsigned r;
  size_t n = sizeof(r);
  int err  = getentropy(&r, n);
  if (err)
    std::__throw_system_error(errno, "random_device getentropy failed");
  return r;
}

#elif defined(_LIBCPP_USING_ARC4_RANDOM)

random_device::random_device(const string&) {}

random_device::~random_device() {}

unsigned random_device::operator()() { return arc4random(); }

#elif defined(_LIBCPP_USING_DEV_RANDOM)

// ClickHouse: on Linux, the default token is served by getentropy (the getrandom system call)
// instead of opening /dev/urandom. It needs no file descriptor, so it works where /dev/urandom
// is not available (a chroot or a minimal container without /dev, before /dev is mounted) and
// when the process is out of file descriptors. `__f_` is -1 in this mode; the layout of
// `random_device` is unchanged. Other tokens are still opened as files.
#  if defined(__linux__)
#    define _LIBCPP_RANDOM_DEVICE_LINUX_GETENTROPY
#  endif

random_device::random_device(const string& __token) {
#  if defined(_LIBCPP_RANDOM_DEVICE_LINUX_GETENTROPY)
  if (__token == "/dev/urandom") {
    __f_ = -1;
    return;
  }
#  endif
  __f_ = open(__token.c_str(), O_RDONLY);
  if (__f_ < 0)
    std::__throw_system_error(errno, ("random_device failed to open " + __token).c_str());
}

random_device::~random_device() {
  if (__f_ >= 0)
    close(__f_);
}

unsigned random_device::operator()() {
  unsigned r;
#  if defined(_LIBCPP_RANDOM_DEVICE_LINUX_GETENTROPY)
  if (__f_ < 0) {
    if (getentropy(&r, sizeof(r)) != 0)
      std::__throw_system_error(errno, "random_device getentropy failed");
    return r;
  }
#  endif
  size_t n = sizeof(r);
  char* p  = reinterpret_cast<char*>(&r);
  while (n > 0) {
    ssize_t s = read(__f_, p, n);
    if (s == 0)
      std::__throw_system_error(ENOMSG, "random_device got EOF");
    if (s == -1) {
      if (errno != EINTR)
        std::__throw_system_error(errno, "random_device got an unexpected error");
      continue;
    }
    n -= static_cast<size_t>(s);
    p += static_cast<size_t>(s);
  }
  return r;
}

#elif defined(_LIBCPP_USING_WIN32_RANDOM)

random_device::random_device(const string& __token) {
  if (__token != "/dev/urandom")
    std::__throw_system_error(ENOENT, ("random device not supported " + __token).c_str());
}

random_device::~random_device() {}

unsigned random_device::operator()() {
  unsigned r;
  errno_t err = rand_s(&r);
  if (err)
    std::__throw_system_error(err, "random_device rand_s failed.");
  return r;
}

#elif defined(_LIBCPP_USING_FUCHSIA_CPRNG)

random_device::random_device(const string& __token) {
  if (__token != "/dev/urandom")
    std::__throw_system_error(ENOENT, ("random device not supported " + __token).c_str());
}

random_device::~random_device() {}

unsigned random_device::operator()() {
  // Implicitly link against the vDSO system call ABI without
  // requiring the final link to specify -lzircon explicitly when
  // statically linking libc++.
#  pragma comment(lib, "zircon")

  // The system call cannot fail.  It returns only when the bits are ready.
  unsigned r;
  _zx_cprng_draw(&r, sizeof(r));
  return r;
}

#else
#  error "Random device not implemented for this architecture"
#endif

double random_device::entropy() const noexcept {
#if defined(_LIBCPP_RANDOM_DEVICE_LINUX_GETENTROPY)
  // getentropy returns data only after the kernel CRNG is initialized.
  if (__f_ < 0)
    return std::numeric_limits<result_type>::digits;
#endif
#if defined(_LIBCPP_USING_DEV_RANDOM) && defined(RNDGETENTCNT)
  int ent;
  if (::ioctl(__f_, RNDGETENTCNT, &ent) < 0)
    return 0;

  if (ent < 0)
    return 0;

  if (ent > std::numeric_limits<result_type>::digits)
    return std::numeric_limits<result_type>::digits;

  return ent;
#elif defined(_LIBCPP_USING_ARC4_RANDOM) || defined(_LIBCPP_USING_FUCHSIA_CPRNG)
  return std::numeric_limits<result_type>::digits;
#else
  return 0;
#endif
}

_LIBCPP_END_EXPLICIT_ABI_ANNOTATIONS
_LIBCPP_END_NAMESPACE_STD
