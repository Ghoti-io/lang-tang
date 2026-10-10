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
 * Generated programs build NaNs on purpose and may print them (D-031: lang-tang
 * prints every NaN as `nan`, ctang prints `nan` or `-nan` by the sign bit). The
 * comparison with ctang is oracle::agree_reading_nan: ctang's `-nan` is read as
 * `nan` in float text and every other byte is exact.
 *
 * A second comparison needs no ctang: the same generated program run by the
 * interpreter and with every function compiled at its first poll (threshold 1),
 * with the frame observer attached, so that every float slot and variable of the
 * first polls is compared by its 64 bits (any two NaNs agree) as well as the
 * output and the result (TiersAgree*). Floats still exit to the interpreter
 * (spec-runtime-float, stories 6 and 7 compile them), so today this is the
 * instrument waiting for them, and it is shown to fail on planted differences.
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
#include <cstring>
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
    const std::function<void(oracle::Verdict &)> & mutate = nullptr, long jit_threshold = -1, uint64_t * compiled_calls = nullptr) {
  gen::Program program = gen::generate(seed, mode);
  bool script = mode == gen::Mode::Script;
  std::string path = dir + "/p-" + std::to_string(seed) + (script ? "-s" : "-t") + ".tang";
  {
    std::ofstream f(path, std::ios::binary);
    f << program.source;
  }
  Result r;
  r.ours = oracle::lang_tang_run(program.source, script, oracle::kDifferentialFuel, jit_threshold, compiled_calls);
  r.theirs = oracle::ctang_run_verdict(runner, script ? "run-script" : "run-template", path, kCtangTimeoutMs);
  std::remove(path.c_str());
  if (mutate) {
    mutate(r.ours);
  }
  if (!oracle::agree_reading_nan(r.ours, r.theirs)) {
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
void campaign(const std::string & runner, uint64_t first, uint64_t count, size_t * agreed, size_t * killed, size_t * paused, long jit_threshold = -1, uint64_t * compiled_calls = nullptr) {
  std::string dir = work_dir(runner);
  size_t shown = 0;
  for (uint64_t k = 0; k < count; ++k) {
    uint64_t seed = first + k / 2;
    gen::Mode mode = k % 2 == 0 ? gen::Mode::Script : gen::Mode::Template;
    Result r = run_one(runner, dir, seed, mode, nullptr, jit_threshold, compiled_calls);
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
  // The same batch with every function compiled at its first poll (calls between
  // compiled functions included).
  size_t agreed_jit = 0, killed_jit = 0, paused_jit = 0;
  uint64_t compiled_calls = 0;
  campaign(runner, 1, 440, &agreed_jit, &killed_jit, &paused_jit, 1, &compiled_calls);
  EXPECT_EQ(agreed_jit, 440u);
  EXPECT_EQ(killed_jit, 0u);
  EXPECT_EQ(paused_jit, 0u);
  std::printf("  fuzz-diff fixed batch compiled at the first poll: 440 programs, %zu agree, %llu calls between compiled functions\n", agreed_jit,
      (unsigned long long)compiled_calls);
  if (const char * why = oracle::no_compiled_calls_reason()) {
    std::printf("  compiled-call check skipped: %s\n", why);
  }
  else {
    EXPECT_GT(compiled_calls, 0u) << "the threshold-1 rerun made no call between compiled functions";
  }
}

TEST(FuzzDiff, CallGraphsAgreeWithCtangPlainAndWithEveryFunctionCompiledAtItsFirstPoll) {
  const char * runner = runner_or_null();
  ASSERT_NE(runner, nullptr);
  std::string dir = work_dir(runner);
  size_t agreed = 0, killed = 0, shown = 0, paused = 0;
  uint64_t compiled_calls = 0;
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
      oracle::Verdict ours = oracle::lang_tang_run(program.source, true, oracle::kDifferentialFuel, threshold, threshold ? &compiled_calls : nullptr);
      paused += ours.kind == oracle::Kind::Paused;
      if (!oracle::agree_reading_nan(ours, theirs)) {
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
  EXPECT_EQ(paused, 0u) << "a generated call graph never runs out of fuel";
  if (const char * why = oracle::no_compiled_calls_reason()) {
    std::printf("  compiled-call check skipped: %s\n", why);
  }
  else {
    EXPECT_GT(compiled_calls, 0u) << "no call graph made a call between compiled functions";
  }
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

// ---------------------------------------------------------------------------
// The interpreter against the JIT, floats by their bits
// ---------------------------------------------------------------------------

namespace {

/// The polls whose frames are recorded for each run; the rest are counted. A poll
/// costs about a millisecond to record, and the output and the result are compared
/// whole.
constexpr size_t kTierPolls = 40;
/// After those, every kTierSampleEvery-th poll, kTierSamples of them at most: a late
/// float is compared by its bits too, at a cost bounded for a long program.
constexpr size_t kTierSampleEvery = 7;
constexpr size_t kTierSamples = 150;

struct Tiers {
  oracle::Verdict plain, jit;
  observer::Trace plain_trace, jit_trace;
  GLTANG_JitStats jit_stats = {};
  std::string source;
  uint64_t seed = 0;
  gen::Mode mode = gen::Mode::Script;
};

Tiers run_tiers(uint64_t seed, gen::Mode mode, uint64_t * compiled_calls = nullptr) {
  Tiers t;
  t.seed = seed;
  t.mode = mode;
  t.source = gen::generate(seed, mode).source;
  bool script = mode == gen::Mode::Script;
  t.plain = oracle::lang_tang_run_traced(t.source, script, 0, &t.plain_trace, kTierPolls, nullptr, kTierSampleEvery, kTierSamples);
  t.jit = oracle::lang_tang_run_traced(t.source, script, 1, &t.jit_trace, kTierPolls, compiled_calls, kTierSampleEvery, kTierSamples, &t.jit_stats);
  return t;
}

/// Empty if the two tiers agree: the verdicts (a NaN's sign is not compared) and
/// the frames at every recorded poll, floats by bits. Else the seed, the mode, the
/// divergence and the whole program.
std::string tiers_difference(const Tiers & t, bool bits = true) {
  oracle::Verdict a = oracle::reading_nan_as_nan(t.plain, true);
  oracle::Verdict b = oracle::reading_nan_as_nan(t.jit, true);
  std::string why;
  observer::Divergence d;
  if (!oracle::agree(a, b)) {
    why = oracle::difference(a, b);
  }
  else if (observer::first_divergence(t.plain_trace, t.jit_trace, &d, bits)) {
    why = "frames: " + d.str();
  }
  if (why.empty()) {
    return why;
  }
  return "TIER DIVERGENCE at seed " + std::to_string(t.seed) + " (" + mode_name(t.mode) + "): " + why +
      "\n--- program (reproduce with: make fuzz-diff FUZZ_DIFF_COUNT=" + (t.mode == gen::Mode::Script ? "1" : "2") + " FUZZ_DIFF_SEED=" + std::to_string(t.seed) + ") ---\n" +
      t.source + "\n--- end of seed " + std::to_string(t.seed) + " ---";
}

/// Runs the seeds [first, first + seeds) in both modes; the number that agree and
/// the number of float slots and variables compared.
void tier_campaign(uint64_t first, uint64_t seeds, size_t * agreed, size_t * programs, size_t * floats, size_t * nans, uint64_t * compiled_calls, uint64_t * entries) {
  size_t shown = 0;
  for (uint64_t seed = first; seed < first + seeds; ++seed) {
    for (gen::Mode mode : {gen::Mode::Script, gen::Mode::Template}) {
      Tiers t = run_tiers(seed, mode, compiled_calls);
      ++*programs;
      *entries += t.jit_stats.entries;
      std::string why = tiers_difference(t);
      if (why.empty()) {
        ++*agreed;
      }
      else if (shown++ < 5) {
        ADD_FAILURE() << why;
      }
      auto count = [&](const observer::Trace & trace) {
        for (const auto & p : trace.polls) {
          for (const auto & f : p.frames) {
            for (const auto & s : f.slots) {
              *floats += s.is_float;
              *nans += s.is_float && observer::detail::is_nan_bits(s.float_bits);
            }
            for (const auto & sc : f.scopes) {
              for (const auto & v : sc.variables) {
                *floats += v.is_float;
                *nans += v.is_float && observer::detail::is_nan_bits(v.float_bits);
              }
            }
          }
        }
      };
      count(t.jit_trace);
    }
  }
}

}  // namespace

TEST(FuzzDiff, TiersAgreeOnGeneratedProgramsWithFloatsComparedByBits) {
  // The same 440 programs as the fixed batch against ctang (seeds 1 to 220, both
  // modes), interpreter against every function compiled at its first poll.
  GLTANG_REQUIRE_JIT_BACKEND();  // else the interpreter would be compared with itself
  size_t agreed = 0, programs = 0, floats = 0, nans = 0;
  uint64_t compiled_calls = 0, entries = 0;
  tier_campaign(1, 220, &agreed, &programs, &floats, &nans, &compiled_calls, &entries);
  EXPECT_EQ(programs, 440u);
  EXPECT_GT(entries, 1000u) << "compiled code ran: the comparison is not vacuous";
  EXPECT_EQ(agreed, programs);
  EXPECT_GT(floats, 2000u) << "the traces carry the bits of the generated programs' floats";
  EXPECT_GT(nans, 20u) << "the generator makes NaNs, and they are in the traces";
  std::printf("  fuzz-diff tiers: %zu programs (seeds 1 to 220, both modes), interpreter against threshold 1, %zu agree; %zu float slots and variables compared by bits, %zu of them NaN\n",
      programs, agreed, floats, nans);
}

TEST(FuzzDiff, TheGeneratorMakesEveryKindOfFloat) {
  // Over a few hundred programs: the sources name NaN, infinities, negative zero,
  // subnormals and the extremes, and some program prints a NaN.
  size_t nan = 0, inf = 0, negative_zero = 0, subnormal = 0, largest = 0, sanf = 0, ulp = 0, printed_nan = 0;
  for (uint64_t seed = 1; seed <= 300; ++seed) {
    std::string template_source = gen::generate(seed, gen::Mode::Template).source;
    std::string source = gen::generate(seed, gen::Mode::Script).source;
    for (const std::string & each : {template_source, source}) {
      std::string bare = each;
      for (const char * cast : {"(\"nan\" as float)", "(\"-nan(0x1234)\" as float)"}) {
        for (size_t at = bare.find(cast); at != std::string::npos; at = bare.find(cast)) {
          bare.erase(at, strlen(cast));
        }
      }
      EXPECT_EQ(bare.find("nan"), std::string::npos) << "seed " << seed << " has a string with nan in it";
    }
    // The comparison with ctang reads every `-nan` of its output as `nan` (oracle.h,
    // read_nan_text), which is exact only while no generated string says "nan": the
    // only "nan" in a program is the text of a cast to float.
    std::string without_casts = source;
    for (const char * cast : {"(\"nan\" as float)", "(\"-nan(0x1234)\" as float)"}) {
      for (size_t at = without_casts.find(cast); at != std::string::npos; at = without_casts.find(cast)) {
        without_casts.erase(at, strlen(cast));
      }
    }
    EXPECT_EQ(without_casts.find("nan"), std::string::npos) << "seed " << seed << " has a string with nan in it";
    nan += source.find("\"nan\" as float") != std::string::npos || source.find("-nan(0x1234)") != std::string::npos;
    inf += source.find("\"inf\" as float") != std::string::npos || source.find("\"-inf\" as float") != std::string::npos;
    negative_zero += source.find("(-0.0)") != std::string::npos;
    subnormal += source.find("5e-324") != std::string::npos || source.find("1e-320") != std::string::npos;
    largest += source.find("1.7976931348623157e308") != std::string::npos;
    sanf += source.find("sanf(") != std::string::npos;
    ulp += source.find("1.0000000000000002") != std::string::npos;
    oracle::Verdict v = oracle::lang_tang_run(source, true);
    printed_nan += v.kind == oracle::Kind::Output && v.output.find("nan") != std::string::npos;
  }
  EXPECT_GT(nan, 30u);
  EXPECT_GT(inf, 30u);
  EXPECT_GT(negative_zero, 10u);
  EXPECT_GT(subnormal, 20u);
  EXPECT_GT(largest, 20u);
  EXPECT_GT(ulp, 10u);
  EXPECT_GT(sanf, 50u) << "a program builds text from a float that may be a NaN";
  EXPECT_GT(printed_nan, 5u) << "a generated program prints a NaN";
}

TEST(FuzzDiff, ThePlantedDifferencesOfTheTierComparisonAreReportedAndItsControlIsNot) {
  // The control: a program that holds a finite float, unaltered.
  auto finite = [](uint64_t b) { return (b & 0x7ff0000000000000ull) != 0x7ff0000000000000ull; };
  Tiers control;
  uint64_t seed = 0;
  for (uint64_t s = 1; s <= 200 && !seed; ++s) {
    Tiers t = run_tiers(s, gen::Mode::Script);
    bool has_finite = false, has_nan = false;
    for (const auto & p : t.jit_trace.polls) {
      for (const auto & f : p.frames) {
        for (const auto & sc : f.scopes) {
          for (const auto & v : sc.variables) {
            has_finite = has_finite || (v.is_float && finite(v.float_bits));
            has_nan = has_nan || (v.is_float && observer::detail::is_nan_bits(v.float_bits));
          }
        }
      }
    }
    if (has_finite && has_nan) {
      seed = s;
      control = std::move(t);
    }
  }
  ASSERT_NE(seed, 0u) << "some program in the first 200 seeds holds a finite float and a NaN";
  EXPECT_EQ(tiers_difference(control), "") << "the control agrees";
  // One ulp: invisible in the text, and reported with the seed, the mode and the program.
  Tiers a = control;
  ASSERT_NE(observer::plant_float_bits(&a.jit_trace, finite, [](uint64_t b) -> uint64_t { return b ^ 1u; }), SIZE_MAX);
  std::string why = tiers_difference(a);
  EXPECT_NE(why.find("float bits"), std::string::npos) << why;
  EXPECT_NE(why.find("seed " + std::to_string(seed)), std::string::npos) << why;
  EXPECT_NE(why.find("--- program"), std::string::npos);
  EXPECT_EQ(tiers_difference(a, /*bits=*/false), "") << "the text alone does not see it";
  // A NaN where a number belongs.
  Tiers b = control;
  ASSERT_NE(observer::plant_float_bits(&b.jit_trace, finite, [](uint64_t) -> uint64_t { return 0x7ff8000000000000ull; }), SIZE_MAX);
  EXPECT_NE(tiers_difference(b).find("float bits"), std::string::npos);
  // A subnormal flushed to zero is the second case of the planted trace tests; here
  // the output is altered too: the verdict side of the comparison fails as well.
  Tiers c = control;
  c.jit.output += "x";
  EXPECT_NE(tiers_difference(c).find("seed"), std::string::npos);
  // Two NaNs agree: every NaN of the trace becomes the NaN of the other sign and a payload
  // (idempotent, and a NaN stays a NaN), and that is no difference.
  Tiers d = control;
  auto not_the_target = [](uint64_t x) { return observer::detail::is_nan_bits(x) && x != 0x7ff8000000005678ull; };
  size_t replaced = 0;
  while (observer::plant_float_bits(&d.jit_trace, not_the_target, [](uint64_t) -> uint64_t { return 0x7ff8000000005678ull; }) != SIZE_MAX) {
    ASSERT_LT(++replaced, 100000u);
  }
  ASSERT_GT(replaced, 0u) << "the control holds a NaN (seed " << seed << ")";
  EXPECT_EQ(tiers_difference(d), "") << "the sign and payload of a NaN are not a difference";
}

TEST(FuzzDiff, TierCampaign) {
  const char * count_text = std::getenv("FUZZ_DIFF_COUNT");
  if (!count_text || !*count_text) {
    GTEST_SKIP() << "set FUZZ_DIFF_COUNT (and FUZZ_DIFF_SEED) to run a campaign: make fuzz-diff FUZZ_DIFF_COUNT=2000 FUZZ_DIFF_SEED=1";
  }
  const char * seed_text = std::getenv("FUZZ_DIFF_SEED");
  uint64_t first = seed_text && *seed_text ? std::strtoull(seed_text, nullptr, 10) : 1;
  uint64_t count = std::strtoull(count_text, nullptr, 10);
  ASSERT_GT(count, 0u);
  // The ctang campaign runs `count` programs, two a seed; so does this.
  GLTANG_REQUIRE_JIT_BACKEND();
  size_t agreed = 0, programs = 0, floats = 0, nans = 0;
  uint64_t compiled_calls = 0, entries = 0;
  tier_campaign(first, (count + 1u) / 2u, &agreed, &programs, &floats, &nans, &compiled_calls, &entries);
  EXPECT_GT(entries, 0u) << "compiled code ran: the comparison is not vacuous";
  std::printf("  fuzz-diff tier campaign: %zu programs from seed %llu, %zu agree, %zu float slots and variables compared by bits, %zu NaN\n", programs,
      (unsigned long long)first, agreed, floats, nans);
  EXPECT_EQ(agreed, programs);
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
