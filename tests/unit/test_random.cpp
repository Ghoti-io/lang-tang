// `random` and the seed sequence (CAP-6, AD-25).
//
// Ported from ctang's Random.Random (test-tangLanguageLibrary.cpp), then the
// cases the spec's matrix adds: bit-exactness with std::mt19937_64 word for
// word, one generator per execution, the sequence, and no clock anywhere.

#include "exec_harness.h"

#include <atomic>
#include <random>
#include <set>
#include <thread>

using tt::Compiled;
using tt::Context;

namespace {

uint64_t splitmix64(uint64_t x) {
  // The reference construction, written out independently of the library.
  uint64_t z = (x += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// The numbers the program prints, as a vector.
std::vector<int64_t> integers_of(const std::string & source, GLTANG_SeedSequence * seeds) {
  Compiled compiled(source);
  Context context(compiled.program);
  if (seeds) {
    EXPECT_EQ(gltang_execution_set_seeds(context.execution, seeds), GLTANG_OK);
  }
  EXPECT_TRUE(context.execute());
  std::vector<int64_t> out;
  for (size_t i = 0;; ++i) {
    auto item = context.element(i);
    if (!item.present) {
      break;
    }
    out.push_back(item.integer());
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// ctang: Random.Random
// ---------------------------------------------------------------------------

TEST(Random, Random) {
  {
    // global
    tt::Run run("use random; random.global;");
    EXPECT_EQ(run.context.kind(), GLTANG_KIND_RNG);
  }
  {
    // global is one object: each read is the same generator.
    tt::Run run("use random; a = random.global; b = random.global; [a.next_int != b.next_int, a.next_int != a.next_int];");
    EXPECT_EQ(run.context.describe(), "[true, true]");
  }
  {
    // global, next_int
    tt::Run run("use random; random.global.next_int;");
    EXPECT_TRUE(run.context.is_integer());
  }
  {
    // default
    tt::Run run("use random; random.default;");
    EXPECT_EQ(run.context.kind(), GLTANG_KIND_RNG);
  }
  {
    // default.next_int
    tt::Run run("use random; random.default.next_int;");
    EXPECT_TRUE(run.context.is_integer());
  }
  {
    // Seeded
    tt::Run run("use random; random.seeded(123);");
    EXPECT_EQ(run.context.kind(), GLTANG_KIND_RNG);
  }
  {
    // Seeded, compared with c++ mt19937_64
    tt::Run run("use random; random.seeded(123).next_int;");
    ASSERT_TRUE(run.context.is_integer());
    std::mt19937_64 mt(123);
    EXPECT_EQ((uint64_t)run.context.integer(), mt());
  }
  {
    // Seeded, next_float, compared with c++ mt19937_64
    tt::Run run("use random; random.seeded(123).next_float;");
    ASSERT_TRUE(run.context.is_float());
    std::mt19937_64 mt(123);
    uint64_t word = mt();
    EXPECT_EQ(run.context.number(), (double)(word >> 11) * (1.0 / 9007199254740992.0));
  }
  {
    // Seeded, next_bool, compared with c++ mt19937_64
    tt::Run run("use random; random.seeded(123).next_bool;");
    ASSERT_TRUE(run.context.is_bool());
    std::mt19937_64 mt(123);
    EXPECT_EQ(run.context.boolean(), (mt() & 1) != 0);
  }
}

// ---------------------------------------------------------------------------
// Bit-exact with std::mt19937_64
// ---------------------------------------------------------------------------

TEST(Random, SeededMatchesTheCppEngineWordForWord) {
  for (uint64_t seed : {0ull, 1ull, 5ull, 123ull, 4294967296ull, 123456789012345ull}) {
    // 700 words cross the engine's 312-word state twice.
    std::string source = "use random; r = random.seeded(" + std::to_string(seed) + "); xs = []; for (i = 0; i < 700; i += 1) { xs[i] = r.next_int; } xs;";
    Compiled compiled(source);
    Context context(compiled.program);
    ASSERT_TRUE(context.execute());
    std::mt19937_64 mt(seed);
    for (size_t i = 0; i < 700; ++i) {
      auto item = context.element(i);
      ASSERT_TRUE(item.is_integer()) << i;
      ASSERT_EQ((uint64_t)item.integer(), mt()) << "seed " << seed << " word " << i;
    }
  }
}

TEST(Random, FloatsAndBoolsAreDrawnFromTheSameWords) {
  tt::Run run("use random; r = random.seeded(7); [r.next_float, r.next_bool, r.next_int, r.next_float, r.next_bool];");
  std::mt19937_64 mt(7);
  uint64_t w0 = mt(), w1 = mt(), w2 = mt(), w3 = mt(), w4 = mt();
  EXPECT_EQ(run.context.element(0).number(), (double)(w0 >> 11) * (1.0 / 9007199254740992.0));
  EXPECT_EQ(run.context.element(1).boolean(), (w1 & 1) != 0);
  EXPECT_EQ((uint64_t)run.context.element(2).integer(), w2);
  EXPECT_EQ(run.context.element(3).number(), (double)(w3 >> 11) * (1.0 / 9007199254740992.0));
  EXPECT_EQ(run.context.element(4).boolean(), (w4 & 1) != 0);
}

TEST(Random, ANegativeSeedIsItsTwosComplementAndSetSeedReseedsAndReturnsTheGenerator) {
  tt::Run run("use random; r = random.seeded(0 - 1); a = r.next_int; r.set_seed(5); b = r.next_int; c = random.seeded(5).set_seed(9).next_int; [a, b, c];");
  std::mt19937_64 neg((uint64_t)-1), five(5), nine(9);
  EXPECT_EQ((uint64_t)run.context.element(0).integer(), neg());
  EXPECT_EQ((uint64_t)run.context.element(1).integer(), five());
  EXPECT_EQ((uint64_t)run.context.element(2).integer(), nine());
}

TEST(Random, AGeneratorIsSharedByReferenceAndIsNotACopy) {
  tt::Run run("use random; r = random.seeded(5); s = r; xs = [r]; [r.next_int == random.seeded(5).next_int, s.next_int == random.seeded(5).set_seed(5).next_int, xs[0].next_int != r.next_int];");
  // Reading `next_int` advances the state, so the first is equal (fresh), the
  // second reads the second word of the shared generator, which a fresh one's
  // first word is not.
  EXPECT_EQ(run.context.element(0).boolean(), true);
  EXPECT_EQ(run.context.element(1).boolean(), false);
  EXPECT_EQ(run.context.element(2).boolean(), true);
}

TEST(Random, ArgumentsAreCheckedInCtangsOrder) {
  tt::Run run("use random; r = random.seeded(5); [random.seeded(), random.seeded(1, 2), random.seeded(\"x\"), random.seeded(1.5), r.set_seed(), r.set_seed(1, 2), r.set_seed(\"a\"), r.set_seed(1.5), "
              "random.global.set_seed(\"a\"), random.global.set_seed(), random.global.set_seed(1, 2), random.global.set_seed(1), random.default.set_seed(3)] as string;");
  EXPECT_EQ(run.context.text(),
      "[Error: Argument Count Mismatch, Error: Argument Count Mismatch, Error: Invalid function call, Error: Invalid function call, "
      "Error: Argument Count Mismatch, Error: Argument Count Mismatch, Error: Invalid function call, Error: Invalid function call, "
      "Error: Invalid function call, Error: Argument Count Mismatch, Error: Argument Count Mismatch, "
      "Error: Cannot change the seed of the global random number generator, RNG]");
}

TEST(Random, AGeneratorIsNotSupportedForTheOperatorsAndPrintsNothing) {
  tt::Run run("use random; r = random.seeded(1); print(r); print(random.seeded); [r + 1, r == r, r[0], r.x, r.next_int_range, random.shuffle, r(), r as string, !r, r ? 1 : 2, r];");
  EXPECT_EQ(run.context.raw(), "");
  EXPECT_EQ(run.context.describe(),
      "[Error: Not supported, Error: Not supported, Error: Not supported, Error: Not implemented, Error: Not implemented, "
      "Error: Not implemented, Error: Invalid function call, Error: Not supported, false, 1, RNG]");
}

TEST(Random, TheMethodIsAFunctionValueBoundToItsGenerator) {
  tt::Run run("use random; r = random.seeded(5); f = r.set_seed; g = f(7); [g.next_int == random.seeded(7).next_int, r.next_int == random.seeded(7).set_seed(7).next_int];");
  EXPECT_EQ(run.context.element(0).boolean(), true);
  EXPECT_EQ(run.context.element(1).boolean(), false) << "f(7) reseeded r, which then drew the second word";
}

// ---------------------------------------------------------------------------
// One generator per execution, from the sequence (D-003)
// ---------------------------------------------------------------------------

TEST(Random, TwoExecutionsWithOneMasterSeedGetTheSameSequence) {
  const char * source = "use random; [random.global.next_int, random.global.next_int, random.default.next_int, random.global.next_int];";
  GLTANG_SeedSequence * a = nullptr;
  GLTANG_SeedSequence * b = nullptr;
  ASSERT_EQ(gltang_seeds_create(7, &a), GLTANG_OK);
  ASSERT_EQ(gltang_seeds_create(7, &b), GLTANG_OK);
  auto first = integers_of(source, a);
  auto second = integers_of(source, b);
  ASSERT_EQ(first.size(), 4u);
  EXPECT_EQ(first, second);
  // And it is the sequence's: the global took draw 0, the default draw 1.
  std::mt19937_64 global(splitmix64(7)), dflt(splitmix64(7 + 0x9E3779B97F4A7C15ull));
  EXPECT_EQ((uint64_t)first[0], global());
  EXPECT_EQ((uint64_t)first[1], global());
  EXPECT_EQ((uint64_t)first[2], dflt());
  EXPECT_EQ((uint64_t)first[3], global());
  gltang_seeds_destroy(a);
  gltang_seeds_destroy(b);
}

TEST(Random, TwoExecutionsOfOneSequenceDiffer) {
  const char * source = "use random; [random.global.next_int, random.global.next_int];";
  GLTANG_SeedSequence * seeds = nullptr;
  ASSERT_EQ(gltang_seeds_create(7, &seeds), GLTANG_OK);
  auto first = integers_of(source, seeds);
  auto second = integers_of(source, seeds);
  EXPECT_NE(first, second);
  EXPECT_EQ(gltang_seeds_master(seeds), 7u);
  gltang_seeds_destroy(seeds);
}

TEST(Random, TheOrderOfFirstUseDecidesWhichDrawAGeneratorTakes) {
  GLTANG_SeedSequence * a = nullptr;
  GLTANG_SeedSequence * b = nullptr;
  ASSERT_EQ(gltang_seeds_create(3, &a), GLTANG_OK);
  ASSERT_EQ(gltang_seeds_create(3, &b), GLTANG_OK);
  auto global_first = integers_of("use random; g = random.global.next_int; d = random.default.next_int; [g, d];", a);
  auto default_first = integers_of("use random; d = random.default.next_int; g = random.global.next_int; [g, d];", b);
  ASSERT_EQ(global_first.size(), 2u);
  EXPECT_EQ(global_first[0], default_first[1]) << "the global took draw 0 in the first and draw 1 in the second";
  EXPECT_NE(global_first[0], default_first[0]);
  gltang_seeds_destroy(a);
  gltang_seeds_destroy(b);
}

TEST(Random, WithNoSequenceAnExecutionMakesAPrivateOneFromEntropy) {
  const char * source = "use random; [random.global.next_int, random.default.next_int];";
  auto first = integers_of(source, nullptr);
  auto second = integers_of(source, nullptr);
  ASSERT_EQ(first.size(), 2u);
  EXPECT_NE(first, second) << "two private sequences are two entropy draws; equal numbers are a 2^-128 event";
  EXPECT_NE(first[0], first[1]);
}

TEST(Random, DefaultIsANewGeneratorOnEachAccessAndGlobalIsTheSameOne) {
  tt::Run run("use random; [random.default.next_int != random.default.next_int, random.global.next_int != random.global.next_int];");
  EXPECT_EQ(run.context.describe(), "[true, true]");
  tt::Run second("use random; a = random.default; b = random.default; c = a.next_int; d = b.next_int; [c != d];");
  EXPECT_EQ(second.context.describe(), "[true]");
}

TEST(Random, TwoContextsInOneProcessHaveTheirOwnGlobals) {
  GLTANG_SeedSequence * seeds = nullptr;
  ASSERT_EQ(gltang_seeds_create(11, &seeds), GLTANG_OK);
  Compiled compiled("use random; random.global.next_int;");
  Context one(compiled.program);
  Context two(compiled.program);
  ASSERT_EQ(gltang_execution_set_seeds(one.execution, seeds), GLTANG_OK);
  ASSERT_EQ(gltang_execution_set_seeds(two.execution, seeds), GLTANG_OK);
  ASSERT_TRUE(one.execute());
  ASSERT_TRUE(two.execute());
  EXPECT_EQ((uint64_t)one.integer(), std::mt19937_64(splitmix64(11))());
  EXPECT_EQ((uint64_t)two.integer(), std::mt19937_64(splitmix64(11 + 0x9E3779B97F4A7C15ull))());
  gltang_seeds_destroy(seeds);
}

// ---------------------------------------------------------------------------
// The seed sequence itself
// ---------------------------------------------------------------------------

TEST(Seeds, DrawKIsSplitmix64OfTheMasterPlusKGamma) {
  GLTANG_SeedSequence * seeds = nullptr;
  ASSERT_EQ(gltang_seeds_create(0x123456789abcdefull, &seeds), GLTANG_OK);
  for (uint64_t k = 0; k < 20; ++k) {
    EXPECT_EQ(gltang_seeds_next(seeds), splitmix64(0x123456789abcdefull + k * 0x9E3779B97F4A7C15ull)) << k;
  }
  // splitmix64 of zero is the generator's well-known first output.
  GLTANG_SeedSequence * zero = nullptr;
  ASSERT_EQ(gltang_seeds_create(0, &zero), GLTANG_OK);
  EXPECT_EQ(gltang_seeds_next(zero), 0xE220A8397B1DCDAFull);
  gltang_seeds_destroy(zero);
  gltang_seeds_destroy(seeds);
}

TEST(Seeds, ManyThreadsDrawDistinctSeedsFromOneSequence) {
  GLTANG_SeedSequence * seeds = nullptr;
  ASSERT_EQ(gltang_seeds_create(99, &seeds), GLTANG_OK);
  std::vector<std::vector<uint64_t>> drawn(4);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < 2000; ++i) {
        drawn[t].push_back(gltang_seeds_next(seeds));
      }
    });
  }
  for (auto & t : threads) {
    t.join();
  }
  std::set<uint64_t> all;
  for (auto & d : drawn) {
    all.insert(d.begin(), d.end());
  }
  EXPECT_EQ(all.size(), 8000u) << "every draw is a different counter value";
  // And they are exactly the first 8000 draws of the sequence.
  std::set<uint64_t> expected;
  for (uint64_t k = 0; k < 8000; ++k) {
    expected.insert(splitmix64(99 + k * 0x9E3779B97F4A7C15ull));
  }
  EXPECT_EQ(all, expected);
  gltang_seeds_destroy(seeds);
}

TEST(Seeds, TheRandomSequenceTakesEntropyAndTheReferencesCount) {
  GLTANG_SeedSequence * a = nullptr;
  GLTANG_SeedSequence * b = nullptr;
  ASSERT_EQ(gltang_seeds_create_random(&a), GLTANG_OK);
  ASSERT_EQ(gltang_seeds_create_random(&b), GLTANG_OK);
  EXPECT_NE(gltang_seeds_master(a), gltang_seeds_master(b));
  EXPECT_EQ(gltang_seeds_retain(a), a);
  gltang_seeds_destroy(a);
  gltang_seeds_next(a);  // still alive: one reference left
  gltang_seeds_destroy(a);
  gltang_seeds_destroy(b);
  gltang_seeds_destroy(nullptr);
  EXPECT_EQ(gltang_seeds_next(nullptr), 0u);
  EXPECT_EQ(gltang_seeds_master(nullptr), 0u);
  EXPECT_EQ(gltang_seeds_retain(nullptr), nullptr);
  EXPECT_EQ(gltang_seeds_create(1, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_seeds_create_random(nullptr), GLTANG_ERR_INVALID);
}

TEST(Seeds, AnExecutionKeepsItsSequenceAliveAfterTheHostLetsGo) {
  GLTANG_SeedSequence * seeds = nullptr;
  ASSERT_EQ(gltang_seeds_create(5, &seeds), GLTANG_OK);
  Compiled compiled("use random; random.global.next_int;");
  Context context(compiled.program);
  ASSERT_EQ(gltang_execution_set_seeds(context.execution, seeds), GLTANG_OK);
  gltang_seeds_destroy(seeds);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ((uint64_t)context.integer(), std::mt19937_64(splitmix64(5))());
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
