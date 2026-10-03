/**
 * @file
 *
 * The execution corpus run on lang-tang alone: every executable file under
 * tests/corpus/, in its mode, with no ctang. The differential
 * (tests/oracle/test_oracle.cpp) compares these runs with frozen ctang's; this
 * suite is the lang-tang side by itself, so that `make test-torture` runs the
 * whole corpus under the collector's torture and verify modes and a moving
 * stack with AddressSanitizer, where the child ctang is not the subject.
 *
 * Each file runs twice and the two runs must agree to the byte (a program's
 * output must not depend on the run), a file named reject-* must be refused and
 * no other may be (except the few the compiler refuses, listed), and the
 * harness checks, at every destroy, that nothing leaked and that no pointer was
 * stored into a heap object without the barrier.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "exec_harness.h"
#include "oracle/run_lang_tang.h"
#include "test_helpers.h"

#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <set>

namespace {

/// A budget for this suite: enough for every corpus file except the runaways
/// and the heavy loops, which pause, and a pause is a verdict too. Under the
/// collector's torture mode every allocation is a collection and under a moving
/// stack every push is a copy, so the same files get a smaller budget there:
/// the point of that run is the collector and the stack, not how far a loop gets.
bool instruments_on() {
  return tt::heavy_instruments();
}
const uint64_t kFuel = instruments_on() ? 20000 : 2000000;

std::vector<std::string> files_in(const std::string & sub) {
  std::vector<std::string> names;
  std::string dir = std::string(GLTANG_TEST_DATA) + "/corpus/" + sub;
  if (DIR * d = opendir(dir.c_str())) {
    while (dirent * e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() > 5 && n.compare(n.size() - 5, 5, ".tang") == 0) {
        names.push_back(n);
      }
    }
    closedir(d);
  }
  std::sort(names.begin(), names.end());
  return names;
}

/// Under the instruments these build containers thousands deep, copying them as
/// they go (D-017), and take seconds each: with the collector run before every
/// allocation they take ten. They still run in the plain build here and in the
/// oracle differential; nothing about the collector is shown by them that the
/// rest of the suite does not show.
const std::set<std::string> kSlowUnderInstruments = {
    "script/container-nested-deeper-than-the-value-depth-bound.tang", "script/container-nested-deep-print.tang",
    "script/container-nested-just-inside-the-bound.tang"};

/// Parse accepts these and the compiler refuses them, in both engines.
const std::set<std::string> kCompileRefused = {
    "script/assign-to-call.tang", "script/assign-to-index-and-attribute.tang", "script/global.tang", "script/function-duplicate-parameter-name.tang"};

/// Two runs of one program: the same kind of verdict, and for a finished run the same output and result.
bool repeats(const oracle::Verdict & a, const oracle::Verdict & b) {
  if (a.kind != b.kind) {
    return false;
  }
  return a.kind != oracle::Kind::Output || oracle::agree(a, b);
}

bool is_runaway(const std::string & name) {
  return name.find("runaway") != std::string::npos || name == "break-continue.tang";
}

}  // namespace

TEST(ExecCorpus, EveryFileRunsTwiceToTheSameVerdictAndIsRefusedExactlyWhenItsNameSays) {
  size_t ran = 0, refused = 0, paused = 0;
  for (const char * sub : {"script", "template"}) {
    bool script = std::string(sub) == "script";
    for (const std::string & name : files_in(sub)) {
      std::string file = std::string(sub) + "/" + name;
      if (instruments_on() && kSlowUnderInstruments.count(file)) {
        continue;
      }
      std::string source = read_file(std::string(GLTANG_TEST_DATA) + "/corpus/" + file);
      auto started = std::chrono::steady_clock::now();
      oracle::Verdict first = oracle::lang_tang_run(source, script, kFuel);
      if (std::getenv("GLTANG_EXEC_CORPUS_VERBOSE")) {
        std::printf("  %s: %.3fs\n", file.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
      }
      oracle::Verdict second = oracle::lang_tang_run(source, script, kFuel);
      ASSERT_TRUE(repeats(first, second)) << file << " is not repeatable: " << first.str() << " then " << second.str();
      bool named_reject = name.find("reject-") == 0 || kCompileRefused.count(file);
      ASSERT_EQ(first.kind == oracle::Kind::Reject, named_reject) << file << ": " << first.str();
      if (is_runaway(name)) {
        EXPECT_EQ(first.kind, oracle::Kind::Paused) << file << " is a runaway and must pause, not finish";
      }
      ran += first.kind == oracle::Kind::Output;
      refused += first.kind == oracle::Kind::Reject;
      paused += first.kind == oracle::Kind::Paused;
    }
  }
  EXPECT_GE(ran, instruments_on() ? 380u : 400u) << "the execution corpus holds at least 400 programs that run to the end";
  EXPECT_GE(refused, 60u);
  EXPECT_GE(paused, 3u);
  std::printf("  execution corpus on lang-tang alone: %zu ran, %zu refused, %zu paused\n", ran, refused, paused);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
