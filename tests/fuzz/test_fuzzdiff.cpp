/**
 * @file
 *
 * The differential fuzz run (CAP-6, AD-16): programs from the deterministic
 * generator (tests/fuzz/gen.h), each run on lang-tang and on frozen ctang in a
 * child process, and compared exactly as the corpus is (oracle.h). A generated
 * program names no ledger row - the generator steers around every recorded
 * departure - so any difference is a failure, and the failure prints the seed,
 * the mode and the whole program, which is all that is needed to reproduce it:
 *
 *     make fuzz-diff FUZZ_DIFF_COUNT=1 FUZZ_DIFF_SEED=<seed>
 *
 * `make test` runs a fixed batch (the FixedBatch test: fixed seeds, 440
 * programs, both modes). `make fuzz-diff FUZZ_DIFF_COUNT=N FUZZ_DIFF_SEED=S`
 * runs a campaign of N programs from seed S (the Campaign test). A program that
 * ever finds a divergence is minimised by hand into a corpus file and stays
 * there, and the divergence is fixed or recorded.
 *
 * The test that plants a divergence (one side's output altered) requires the
 * failure to name the seed: a fuzz run that has never been seen to fail may be
 * comparing nothing.
 *
 * The runner is named by GLTANG_ORACLE_RUNNER, as for the oracle differential.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "oracle/oracle.h"
#include "oracle/run_lang_tang.h"
#include "fuzz/gen.h"

#include <cstdlib>
#include <functional>
#include <libgen.h>

namespace {

const int kCtangTimeoutMs = 5000;

/// A directory next to the runner for the programs ctang is handed: a child
/// reads a file, not a string, and the build tree is the one place a test may
/// write.
std::string work_dir(const std::string & runner) {
  std::string copy = runner;
  std::string dir = std::string(dirname(copy.data())) + "/fuzzdiff";
  std::string cmd = "mkdir -p '" + dir + "'";
  if (std::system(cmd.c_str()) != 0) {
    throw std::runtime_error("cannot make " + dir);
  }
  return dir;
}

struct Result {
  bool agreed = true;
  std::string message; // why not, naming the seed and the program
  oracle::Verdict ours, theirs;
};

const char * mode_name(gen::Mode mode) {
  return mode == gen::Mode::Script ? "script" : "template";
}

/// One generated program through both engines. `mutate` may alter lang-tang's
/// verdict before the comparison, which is how a planted divergence is made.
Result run_one(const std::string & runner, const std::string & dir, uint64_t seed, gen::Mode mode,
    const std::function<void(oracle::Verdict &)> & mutate = nullptr) {
  gen::Program program = gen::generate(seed, mode);
  bool script = mode == gen::Mode::Script;
  std::string path = dir + "/p-" + std::to_string(seed) + (script ? "-s" : "-t") + ".tang";
  {
    std::ofstream f(path, std::ios::binary);
    f << program.source;
  }
  Result r;
  r.ours = oracle::lang_tang_run(program.source, script);
  r.theirs = oracle::ctang_run_verdict(runner, script ? "run-script" : "run-template", path, kCtangTimeoutMs);
  std::remove(path.c_str());
  if (mutate) {
    mutate(r.ours);
  }
  if (!oracle::agree(r.ours, r.theirs)) {
    r.agreed = false;
    r.message = "DIVERGENCE at seed " + std::to_string(seed) + " (" + mode_name(mode) + "): " + oracle::difference(r.ours, r.theirs) +
        "\n--- program (reproduce with: make fuzz-diff FUZZ_DIFF_COUNT=" + (script ? "1" : "2") + " FUZZ_DIFF_SEED=" + std::to_string(seed) + ") ---\n" +
        program.source + "\n--- end of seed " + std::to_string(seed) + " ---";
  }
  return r;
}

const char * runner_or_null() {
  return std::getenv("GLTANG_ORACLE_RUNNER");
}

/// Runs `count` programs from `first`: program k is seed first + k / 2, script
/// for even k and template for odd k.
void campaign(const std::string & runner, uint64_t first, uint64_t count, size_t * agreed, size_t * killed, size_t * paused) {
  std::string dir = work_dir(runner);
  size_t shown = 0;
  for (uint64_t k = 0; k < count; ++k) {
    uint64_t seed = first + k / 2;
    gen::Mode mode = k % 2 == 0 ? gen::Mode::Script : gen::Mode::Template;
    Result r = run_one(runner, dir, seed, mode);
    *killed += r.theirs.kind == oracle::Kind::Killed;
    *paused += r.ours.kind == oracle::Kind::Paused;
    if (r.agreed) {
      ++*agreed;
    }
    else if (shown++ < 5) {
      ADD_FAILURE() << r.message;
    }
    else {
      ADD_FAILURE() << "DIVERGENCE at seed " << seed << " (" << mode_name(mode) << ") (program not shown)";
    }
  }
}

}  // namespace

TEST(FuzzDiff, TheRunnerIsNamed) {
  ASSERT_NE(runner_or_null(), nullptr) << "GLTANG_ORACLE_RUNNER is not set; run this through `make test-oracle` or `make fuzz-diff`";
}

TEST(FuzzDiff, FixedBatchOfGeneratedProgramsAgreesOnBothEngines) {
  const char * runner = runner_or_null();
  ASSERT_NE(runner, nullptr) << "GLTANG_ORACLE_RUNNER is not set";
  // 220 seeds, both modes: 440 programs, fixed seeds 1 to 220.
  size_t agreed = 0, killed = 0, paused = 0;
  campaign(runner, 1, 440, &agreed, &killed, &paused);
  EXPECT_EQ(agreed, 440u);
  EXPECT_EQ(killed, 0u) << "a generated program never runs away";
  EXPECT_EQ(paused, 0u);
  std::printf("  fuzz-diff fixed batch: 440 programs (seeds 1 to 220, both modes), %zu agree\n", agreed);
}

TEST(FuzzDiff, CallGraphsAgreeWithCtangPlainAndWithEveryFunctionCompiledAtItsFirstPoll) {
  const char * runner = runner_or_null();
  ASSERT_NE(runner, nullptr);
  std::string dir = work_dir(runner);
  size_t agreed = 0, killed = 0, shown = 0;
  // FUZZ_DIFF_CALL_GRAPHS=N runs N programs from FUZZ_DIFF_SEED (the campaign on the EVO).
  const char * count_text = std::getenv("FUZZ_DIFF_CALL_GRAPHS");
  const char * seed_text = std::getenv("FUZZ_DIFF_SEED");
  const uint64_t kCount = count_text && *count_text ? std::strtoull(count_text, nullptr, 10) : 300;
  const uint64_t kFirst = seed_text && *seed_text ? std::strtoull(seed_text, nullptr, 10) : 1;
  for (uint64_t seed = kFirst; seed < kFirst + kCount; ++seed) {
    gen::Program program = gen::generate_calls(seed);
    std::string path = dir + "/c-" + std::to_string(seed) + ".tang";
    {
      std::ofstream f(path, std::ios::binary);
      f << program.source;
    }
    oracle::Verdict theirs = oracle::ctang_run_verdict(runner, "run-script", path, kCtangTimeoutMs);
    std::remove(path.c_str());
    killed += theirs.kind == oracle::Kind::Killed;
    bool ok = true;
    for (long threshold : {0L, 1L}) {
      oracle::Verdict ours = oracle::lang_tang_run(program.source, true, oracle::kDifferentialFuel, threshold);
      if (!oracle::agree(ours, theirs)) {
        ok = false;
        if (shown++ < 3) {
          ADD_FAILURE() << "DIVERGENCE at call-graph seed " << seed << (threshold ? " (compiled at the first poll)" : " (interpreted)") << ": "
                        << oracle::difference(ours, theirs) << "\n--- program ---\n" << program.source << "--- end of seed " << seed << " ---";
        }
      }
    }
    agreed += ok;
  }
  EXPECT_EQ(agreed, kCount);
  EXPECT_EQ(killed, 0u);
  std::printf("  fuzz-diff call graphs: %llu programs (seeds %llu to %llu), interpreted and compiled, %zu agree with ctang\n",
      (unsigned long long)kCount, (unsigned long long)kFirst, (unsigned long long)(kFirst + kCount - 1), agreed);
}

TEST(FuzzDiff, APlantedDivergenceIsReportedWithItsSeedAndProgram) {
  const char * runner = runner_or_null();
  ASSERT_NE(runner, nullptr);
  std::string dir = work_dir(runner);
  // An output byte appended on lang-tang's side only.
  Result out = run_one(runner, dir, 7, gen::Mode::Script, [](oracle::Verdict & v) { v.output += "X"; });
  ASSERT_FALSE(out.agreed) << "a planted divergence must fail";
  EXPECT_NE(out.message.find("seed 7"), std::string::npos) << out.message;
  EXPECT_NE(out.message.find("script"), std::string::npos);
  EXPECT_NE(out.message.find("--- program"), std::string::npos) << "the whole program is printed";
  EXPECT_NE(out.message.find(gen::generate(7, gen::Mode::Script).source), std::string::npos);
  // A result altered, in a template.
  Result res = run_one(runner, dir, 12, gen::Mode::Template, [](oracle::Verdict & v) { v.result_text += "?"; });
  ASSERT_FALSE(res.agreed);
  EXPECT_NE(res.message.find("seed 12"), std::string::npos) << res.message;
  EXPECT_NE(res.message.find("template"), std::string::npos);
  // And lang-tang pausing where ctang finished, and the control: no mutation.
  Result paused = run_one(runner, dir, 7, gen::Mode::Script, [](oracle::Verdict & v) { v = oracle::Verdict::paused(); });
  EXPECT_FALSE(paused.agreed);
  Result control = run_one(runner, dir, 7, gen::Mode::Script);
  EXPECT_TRUE(control.agreed) << control.message;
}

TEST(FuzzDiff, Campaign) {
  const char * count_text = std::getenv("FUZZ_DIFF_COUNT");
  if (!count_text || !*count_text) {
    GTEST_SKIP() << "set FUZZ_DIFF_COUNT (and FUZZ_DIFF_SEED) to run a campaign: make fuzz-diff FUZZ_DIFF_COUNT=2000 FUZZ_DIFF_SEED=1";
  }
  const char * runner = runner_or_null();
  ASSERT_NE(runner, nullptr);
  const char * seed_text = std::getenv("FUZZ_DIFF_SEED");
  uint64_t first = seed_text && *seed_text ? std::strtoull(seed_text, nullptr, 10) : 1;
  uint64_t count = std::strtoull(count_text, nullptr, 10);
  ASSERT_GT(count, 0u);
  size_t agreed = 0, killed = 0, paused = 0;
  campaign(runner, first, count, &agreed, &killed, &paused);
  std::printf("  fuzz-diff campaign: %llu programs from seed %llu, %zu agree, %zu ctang killed, %zu lang-tang paused\n",
      (unsigned long long)count, (unsigned long long)first, agreed, killed, paused);
  EXPECT_EQ(agreed, count);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
