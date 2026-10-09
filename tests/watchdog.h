/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2024-2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Lang-tang.
 *
 * Ghoti.io Lang-tang is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Lang-tang is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef GHOTI_IO_GLTANG_TEST_WATCHDOG_H
#define GHOTI_IO_GLTANG_TEST_WATCHDOG_H

// A per-test watchdog, as test_native_gate.cpp's alarm is: a program that
// compiled recursion or a pause protocol can hang is killed by the alarm the
// moment its test overruns, so a hang is a failure that names its test and not a
// suite that never ends. Install it in main() with `install_watchdog(seconds)`.

#include <gtest/gtest.h>

#if defined(__has_include) && __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#else
#define RUNNING_ON_VALGRIND 0
#endif

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <atomic>
#include <chrono>
#include <thread>
#else
#include <unistd.h>
#endif

namespace tt {

#ifdef _WIN32
// alarm(2) does not exist on Windows: a thread stands in for it, and ends the
// process with the status SIGALRM's default action would give (128 + 14).
inline std::atomic<unsigned> & watchdog_generation() {
  static std::atomic<unsigned> g{0};
  return g;
}
inline void watchdog_arm(unsigned seconds) {
  unsigned mine = ++watchdog_generation();
  if (seconds != 0) {
    std::thread([seconds, mine] {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      if (watchdog_generation().load() == mine) {
        std::fputs("watchdog: a test did not finish in time\n", stderr);
        std::fflush(stderr);
        std::_Exit(142);
      }
    }).detach();
  }
}
#else
inline void watchdog_arm(unsigned seconds) {
  alarm(seconds);
}
#endif

class WatchdogListener : public ::testing::EmptyTestEventListener {
 public:
  explicit WatchdogListener(unsigned seconds) : seconds_(seconds) {}
  void OnTestStart(const ::testing::TestInfo & info) override {
    std::fprintf(stderr, "[ watchdog ] %s.%s armed for %u s\n", info.test_suite_name(), info.name(), seconds_);
    watchdog_arm(seconds_);
  }
  void OnTestEnd(const ::testing::TestInfo &) override {
    watchdog_arm(0);
  }

 private:
  unsigned seconds_;
};

inline unsigned heavy_seconds_scale() {
  const char * torture = std::getenv("GRHEAP_TORTURE");
  const char * moving = std::getenv("GLTANG_TEST_MOVING_STACK");
  unsigned scale = 1;
  if (torture && *torture && *torture != '0') {
    scale *= 10;
  }
  if (moving && *moving && *moving != '0') {
    scale *= 4;
  }
  if (RUNNING_ON_VALGRIND) {
    scale *= 10;
  }
  return scale;
}

/// Arms an alarm at the start of every test and cancels it at the end. Torture,
/// a moving stack and Valgrind each cost an order of magnitude, so the limit
/// scales: it is a backstop for a hang, not a measure of speed.
inline void install_watchdog(unsigned seconds) {
  if (heavy_seconds_scale() > 1) {
    seconds *= heavy_seconds_scale();
  }
  ::testing::UnitTest::GetInstance()->listeners().Append(new WatchdogListener(seconds));
}

}  // namespace tt

#endif
