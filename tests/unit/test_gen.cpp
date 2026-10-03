/**
 * @file
 *
 * The program generator of the differential fuzz run (tests/fuzz/gen.h), and
 * its programs run on lang-tang alone. No ctang is involved, so this suite also
 * runs under the heap's torture and verify modes and a moving stack
 * (`make test-torture`), where the child ctang of the oracle is not the
 * subject: it is the lang-tang side of the generated batch.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "exec_harness.h"
#include "fuzz/gen.h"
#include "oracle/run_lang_tang.h"
#include "test_helpers.h"

#include <set>

namespace {

bool contains(const std::string & s, const char * needle) {
  return s.find(needle) != std::string::npos;
}

/// The seeds run on lang-tang: fewer where each operation costs more.
const uint64_t kSeeds = tt::heavy_instruments() ? 60 : 160;

}  // namespace

TEST(Generator, TheSameSeedGivesTheSameProgramAndDifferentSeedsDiffer) {
  std::set<std::string> distinct;
  for (uint64_t seed = 1; seed <= 60; ++seed) {
    for (gen::Mode mode : {gen::Mode::Script, gen::Mode::Template}) {
      gen::Program a = gen::generate(seed, mode);
      gen::Program b = gen::generate(seed, mode);
      EXPECT_EQ(a.source, b.source) << "seed " << seed;
      EXPECT_EQ(a.seed, seed);
      distinct.insert(a.source);
    }
  }
  EXPECT_EQ(distinct.size(), 120u) << "every seed and mode gives its own program";
}

TEST(Generator, EveryProgramParsesAndCompilesInItsMode) {
  for (uint64_t seed = 1; seed <= 400; ++seed) {
    for (gen::Mode mode : {gen::Mode::Script, gen::Mode::Template}) {
      gen::Program p = gen::generate(seed, mode);
      tt::Compiled compiled(p.source, mode == gen::Mode::Script ? tt::Mode::Script : tt::Mode::Template);
      ASSERT_TRUE(compiled.ok()) << "seed " << seed << (mode == gen::Mode::Script ? " script" : " template") << ": "
          << compiled.error.message << " at " << compiled.error.line << ":" << compiled.error.column << "\n" << p.source;
    }
  }
}

TEST(Generator, ProgramsAreBoundedInSize) {
  size_t biggest = 0;
  for (uint64_t seed = 1; seed <= 400; ++seed) {
    for (gen::Mode mode : {gen::Mode::Script, gen::Mode::Template}) {
      biggest = std::max(biggest, gen::generate(seed, mode).source.size());
    }
  }
  EXPECT_LT(biggest, 40000u);
  EXPECT_GT(biggest, 1000u);
}

TEST(Generator, TheWholeLanguageIsReachedAcrossTheSeeds) {
  std::map<std::string, int> seen;
  const char * const wanted[] = {
      "while (", "do {", "for (l0", " : ", "break;", "continue;", " as int", " as float", " as string", " as bool", "function fa", "function rec",
      "function fs", "use math", "use random", "random.seeded", ".html", ".percent", ".javascript", ".html_attribute", ".render", ".raw", ".length",
      ".byte_length", ".size", "m0.", "m0[", "[:", "::", "&&", "||", "== ", "!= ", " ? ", "(!", "+=", "-=", "*=", "/=", "%=", "!\"", "%\"", "deep(", "loopy(",
      "r0.next_int", "r0.next_float", "math.pi", "} else {", "print("};
  for (uint64_t seed = 1; seed <= 300; ++seed) {
    gen::Program p = gen::generate(seed, gen::Mode::Script);
    for (const char * w : wanted) {
      if (contains(p.source, w)) {
        ++seen[w];
      }
    }
  }
  for (const char * w : wanted) {
    EXPECT_GT(seen[w], 0) << "no script program in 300 seeds contains `" << w << "`";
  }
  int tags = 0, prints = 0, text = 0;
  for (uint64_t seed = 1; seed <= 100; ++seed) {
    gen::Program p = gen::generate(seed, gen::Mode::Template);
    tags += contains(p.source, "<% ");
    prints += contains(p.source, "<%= ");
    text += contains(p.source, "lorem ") || contains(p.source, "<ul>") || contains(p.source, "\n");
  }
  EXPECT_GT(tags, 90);
  EXPECT_GT(prints, 20);
  EXPECT_GT(text, 90);
}

TEST(Generator, TheAvoidedConstructsOfTheRecordedRowsAreNeverGenerated) {
  for (uint64_t seed = 1; seed <= 400; ++seed) {
    for (gen::Mode mode : {gen::Mode::Script, gen::Mode::Template}) {
      gen::Program p = gen::generate(seed, mode);
      const std::string & s = p.source;
      EXPECT_FALSE(contains(s, "global ")) << seed;                  // D-025
      EXPECT_FALSE(contains(s, "random.global")) << seed;            // D-003
      EXPECT_FALSE(contains(s, "random.default")) << seed;           // D-003
      EXPECT_FALSE(contains(s, "use nope")) << seed;                 // D-012
      EXPECT_FALSE(contains(s, "function f(f")) << seed;             // D-024
      // D-026: every `use` precedes the first assignment.
      size_t first_assign = s.find("i0 =");
      size_t last_use = s.rfind("use ");
      if (last_use != std::string::npos) {
        EXPECT_LT(last_use, first_assign) << seed;
      }
      // D-009, D-011, D-023: the program ends in an expression statement, not
      // in a loop, an if, a function declaration or a use.
      std::string tail = s;
      while (!tail.empty() && (tail.back() == '\n' || tail.back() == ' ')) {
        tail.pop_back();
      }
      if (mode == gen::Mode::Template) {
        ASSERT_GE(tail.size(), 3u);
        EXPECT_EQ(tail.substr(tail.size() - 2), "%>") << seed;
        tail = tail.substr(0, tail.size() - 2);
        while (!tail.empty() && tail.back() == ' ') {
          tail.pop_back();
        }
      }
      ASSERT_FALSE(tail.empty());
      EXPECT_EQ(tail.back(), ';') << seed;
      // D-010: each function ends in a return before its closing brace.
      size_t at = 0;
      while ((at = s.find("function ", at)) != std::string::npos) {
        size_t close = s.find("}", s.find("return", at));
        EXPECT_NE(close, std::string::npos) << seed;
        at += 9;
      }
    }
  }
}

TEST(Generator, EveryProgramRunsToTheEndOnLangTangAndRunsTheSameTwice) {
  size_t refused = 0;
  for (uint64_t seed = 1; seed <= kSeeds; ++seed) {
    for (gen::Mode mode : {gen::Mode::Script, gen::Mode::Template}) {
      gen::Program p = gen::generate(seed, mode);
      bool script = mode == gen::Mode::Script;
      oracle::Verdict first = oracle::lang_tang_run(p.source, script);
      oracle::Verdict second = oracle::lang_tang_run(p.source, script);
      ASSERT_EQ(first.kind, oracle::Kind::Output) << "seed " << seed << (script ? " script" : " template") << ": " << first.str() << "\n" << p.source;
      ASSERT_TRUE(first.kind == second.kind && oracle::agree(first, second)) << "seed " << seed << " is not repeatable: " << first.str() << " then " << second.str();
      refused += first.kind == oracle::Kind::Reject;
    }
  }
  EXPECT_EQ(refused, 0u);
}

TEST(Generator, TheGeneratedBatchProducesOutputErrorsAndEveryResultKind) {
  std::set<std::string> kinds;
  size_t with_output = 0, with_error_results = 0;
  for (uint64_t seed = 1; seed <= kSeeds; ++seed) {
    gen::Program p = gen::generate(seed, gen::Mode::Script);
    oracle::Verdict v = oracle::lang_tang_run(p.source, true);
    ASSERT_EQ(v.kind, oracle::Kind::Output);
    kinds.insert(v.result_kind);
    with_output += !v.output.empty();
    with_error_results += v.result_kind == "error";
  }
  EXPECT_GT(with_output, kSeeds / 2) << "most programs print something";
  EXPECT_TRUE(kinds.count("integer") || kinds.count("string") || kinds.count("array"));
  EXPECT_GE(kinds.size(), 3u) << "the results are of several kinds";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
