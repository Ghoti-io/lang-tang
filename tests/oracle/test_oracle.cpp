/**
 * @file
 *
 * The oracle differential (AD-16): every corpus file, parsed and then run by
 * lang-tang and by frozen ctang in a child process, compared by the comparison
 * unit and held against the divergence ledger.
 *
 * Two comparisons, in this order:
 *
 *  - the parse differential: accept(node count) or reject, which catches a
 *    mis-ported production, a changed precedence or a scanner mode slip;
 *  - the execution differential: the rendered output bytes and the final result
 *    (a kind and a canonical text, the same rule on both sides), with the
 *    verdicts reject (does not compile), killed (ctang only), and paused
 *    (lang-tang only). A run that disagrees and is named by no `recorded` row
 *    fails, naming the file; a `recorded` row naming a file that agrees is
 *    stale and fails.
 *
 * The runner is named by GLTANG_ORACLE_RUNNER, which `make test-oracle` sets
 * after it has checked that ctang exists. An unset variable is a failure, not
 * a skip.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "oracle/oracle.h"
#include "oracle/run_lang_tang.h"

#include <cstdlib>

namespace {

const std::string kCorpus = std::string(GLTANG_TEST_DATA) + "/corpus";
const std::string kLedger = std::string(GLTANG_TEST_DATA) + "/../documentation/divergence-ledger.md";

/// How long ctang may take on one program before the harness kills it. A
/// runaway is one such program (`while (true) {}`); every other corpus file
/// finishes in milliseconds.
const int kCtangTimeoutMs = 2500;

/// The files the execution differential does not run, each with its reason.
/// A file that diverges is not here: that is a ledger row (the constraint is
/// that a divergence is fixed or recorded, never hidden in a skip list).
struct Skip {
  const char * file;
  const char * reason;
};
const Skip kSkipList[] = {
    {"", "sentinel: the table is otherwise empty, and a zero-size array is not C++"},
    // Nothing is skipped. The corpus has no file that needs a host extra (a
    // native function, a template registration or a host value cannot be
    // given to ctang by a file), and the refused (reject-*) files are run
    // as well: both engines must refuse to compile them, which is a
    // comparison too. The list exists so that excluding a file is a
    // reviewed edit of this table with a reason, never an edit elsewhere.
};

/// Files the parser accepts and the compiler refuses, in both engines. They
/// are run like the others and must be refused by both; anything else that is
/// refused by both must be named reject-*, so that a typo in a new program
/// cannot turn into a second refusal that counts as agreement.
const Skip kCompileRefused[] = {
    {"script/assign-to-call.tang", "assignment to a call is `Cannot assign to this expression` at compile time"},
    {"script/assign-to-index-and-attribute.tang", "the corpus file mixes a valid target with an invalid one"},
    {"script/global.tang", "`global` at top level is a compile error (reference 5.11)"},
    {"script/function-duplicate-parameter-name.tang", "parameter names must be distinct (reference 7)"},
};

bool compile_refused(const std::string & file) {
  for (const Skip & s : kCompileRefused) {
    if (file == s.file) {
      return true;
    }
  }
  return false;
}

bool skipped(const std::string & file) {
  for (const Skip & s : kSkipList) {
    if (*s.file && file == s.file) {
      return true;
    }
  }
  return false;
}

oracle::Verdict lang_tang_parse_verdict(const std::string & source, GLTANG_ParseMode mode) {
  GLTANG_Tree * tree = nullptr;
  GLTANG_Result result = gltang_parse(source.c_str(), mode, nullptr, &tree);
  oracle::Verdict v = oracle::Verdict::reject();
  if (result == GLTANG_OK) {
    v = oracle::Verdict::accept(gltang_tree_node_count(tree));
    gltang_tree_destroy(tree);
  }
  else if (result != GLTANG_ERR_FORMAT) {
    throw std::runtime_error(std::string("lang-tang answered ") + gltang_result_string(result));
  }
  return v;
}

const char * runner_or_fail() {
  const char * runner = std::getenv("GLTANG_ORACLE_RUNNER");
  return runner;
}

}  // namespace

TEST(Oracle, TheRunnerIsNamed) {
  // Never skipped: make test-oracle sets this only after finding ctang.
  const char * runner = runner_or_fail();
  ASSERT_NE(runner, nullptr) << "GLTANG_ORACLE_RUNNER is not set; run this through `make test-oracle`";
  ASSERT_TRUE(oracle::file_exists(runner)) << runner;
}

TEST(Oracle, TheLedgerIsValid) {
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  for (const auto & e : ledger.errors) {
    ADD_FAILURE() << e;
  }
}

TEST(Oracle, LangTangAndCtangParseEveryCorpusFileTheSameWayOrALedgerRowRecordsWhy) {
  const char * runner = runner_or_fail();
  ASSERT_NE(runner, nullptr) << "GLTANG_ORACLE_RUNNER is not set; run this through `make test-oracle`";
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  ASSERT_TRUE(ledger.valid());

  std::vector<oracle::Entry> entries;
  for (const auto & file : oracle::list_corpus(kCorpus)) {
    bool script = file.compare(0, 7, "script/") == 0;
    std::string path = kCorpus + "/" + file;
    oracle::Entry e;
    e.file = file;
    e.lang_tang = lang_tang_parse_verdict(read_file(path), script ? GLTANG_PARSE_SCRIPT : GLTANG_PARSE_TEMPLATE);
    e.ctang = oracle::ctang_verdict(runner, script ? "script" : "template", path, 10000);
    entries.push_back(e);
  }
  // A differential over a handful of files proves little; the corpus floor is
  // part of the gate.
  ASSERT_GE(entries.size(), 60u);

  // Staleness is the execution differential's to judge: a row names the files
  // that diverge in execution, and those parse the same.
  oracle::Judgement j = oracle::judge(entries, ledger, /*check_stale=*/false);
  for (const auto & f : j.failures) {
    ADD_FAILURE() << f;
  }
  std::printf("  parse oracle: %zu files, %zu agree, %zu recorded divergences\n", entries.size(), j.agreed, j.recorded.size());
}

TEST(Oracle, LangTangAndCtangRunEveryCorpusFileToTheSameOutputAndResultOrALedgerRowRecordsWhy) {
  const char * runner = runner_or_fail();
  ASSERT_NE(runner, nullptr) << "GLTANG_ORACLE_RUNNER is not set; run this through `make test-oracle`";
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  ASSERT_TRUE(ledger.valid());

  std::vector<oracle::Entry> entries;
  size_t executed = 0, refused = 0, paused = 0, killed = 0;
  for (const auto & file : oracle::list_corpus(kCorpus)) {
    if (skipped(file)) {
      continue;
    }
    bool script = file.compare(0, 7, "script/") == 0;
    std::string path = kCorpus + "/" + file;
    oracle::Entry e;
    e.file = file;
    e.lang_tang = oracle::lang_tang_run(read_file(path), script);
    e.ctang = oracle::ctang_run_verdict(runner, script ? "run-script" : "run-template", path, kCtangTimeoutMs);
    executed += e.lang_tang.kind == oracle::Kind::Output && e.ctang.kind == oracle::Kind::Output;
    refused += e.lang_tang.kind == oracle::Kind::Reject && e.ctang.kind == oracle::Kind::Reject;
    paused += e.lang_tang.kind == oracle::Kind::Paused;
    killed += e.ctang.kind == oracle::Kind::Killed;
    if (e.lang_tang.kind == oracle::Kind::Paused || e.ctang.kind == oracle::Kind::Killed) {
      std::printf("  runaway or crash: %s: lang-tang %s, ctang %s\n", file.c_str(), e.lang_tang.str().c_str(), e.ctang.str().c_str());
    }
    // A file is refused by both engines exactly when its name says so (the
    // corpus convention of tests/unit/test_corpus.cpp): a typo in a new
    // program must not turn it into a second refusal that counts as agreement.
    bool named_reject = file.find("/reject-") != std::string::npos || compile_refused(file);
    EXPECT_EQ(e.lang_tang.kind == oracle::Kind::Reject, named_reject) << file << ": lang-tang " << e.lang_tang.str();
    // ctang alone may refuse a file that lang-tang runs, but only under a
    // `recorded` row (D-026), which names it.
    EXPECT_EQ(e.ctang.kind == oracle::Kind::Reject, named_reject || (e.ctang.kind == oracle::Kind::Reject && ledger.recorded_row_for(file) != nullptr))
        << file << ": ctang " << e.ctang.str();
    entries.push_back(e);
  }
  // The execution corpus floor (story 11): at least 200 files that both
  // engines ran to the end.
  EXPECT_GE(executed, 200u) << "the execution corpus must hold at least 200 programs both engines run";

  oracle::Judgement j = oracle::judge(entries, ledger);
  if (std::getenv("GLTANG_ORACLE_VERBOSE")) {
    for (const auto & r : j.recorded) {
      std::printf("  recorded divergence: %s\n", r.c_str());
    }
  }
  for (const auto & f : j.failures) {
    ADD_FAILURE() << f;
  }

  // A row's files must be run by this differential; otherwise it could never
  // be seen to go stale.
  std::set<std::string> ran;
  for (const auto & e : entries) {
    ran.insert(e.file);
  }
  for (const auto & row : ledger.rows) {
    if (row.status != "recorded") {
      continue;
    }
    for (const auto & f : row.corpus) {
      EXPECT_TRUE(ran.count(f)) << row.id << " names " << f << ", which the execution differential does not run";
    }
  }
  std::printf("  execution oracle: %zu files, %zu run by both, %zu refused by both, %zu agree, %zu recorded divergences, %zu lang-tang paused, %zu ctang killed\n",
      entries.size(), executed, refused, j.agreed, j.recorded.size(), paused, killed);
}

TEST(Oracle, TheComparisonSeesAMutatedParseVerdict) {
  // A differential that has never been seen to disagree may be measuring
  // nothing. Take a real corpus file where both engines accept, change one
  // side's node count, and require the judge to name the file.
  const char * runner = runner_or_fail();
  ASSERT_NE(runner, nullptr);
  std::string file = "script/if-else.tang";
  std::string path = kCorpus + "/" + file;
  oracle::Verdict ours = lang_tang_parse_verdict(read_file(path), GLTANG_PARSE_SCRIPT);
  oracle::Verdict theirs = oracle::ctang_verdict(runner, "script", path, 10000);
  ASSERT_EQ(ours.kind, oracle::Kind::Accept);
  ASSERT_TRUE(oracle::agree(ours, theirs)) << ours.str() << " vs " << theirs.str();
  oracle::Verdict mutated = ours;
  mutated.nodes += 1;
  oracle::Ledger empty = oracle::parse_ledger(
      "| id | category | status | ref | corpus | resolved-by | summary |\n| - | - | - | - | - | - | - |\n"
      "| D-001 | limit | open | - | - | 9 | x |\n", kCorpus);
  oracle::Judgement j = oracle::judge({{file, mutated, theirs}}, empty);
  ASSERT_FALSE(j.ok());
  EXPECT_NE(j.failures[0].find(file), std::string::npos);
}

TEST(Oracle, TheExecutionComparisonSeesAMutatedOutputAndAMutatedResult) {
  // The same control for execution: a real program both engines run, one
  // side's output changed by a byte, then its result changed. The judge must
  // fail and name the file each time.
  const char * runner = runner_or_fail();
  ASSERT_NE(runner, nullptr);
  std::string file = "script/fib.tang";
  std::string path = kCorpus + "/" + file;
  oracle::Verdict ours = oracle::lang_tang_run(read_file(path), true);
  oracle::Verdict theirs = oracle::ctang_run_verdict(runner, "run-script", path, kCtangTimeoutMs);
  ASSERT_EQ(ours.kind, oracle::Kind::Output) << ours.str();
  ASSERT_TRUE(oracle::agree(ours, theirs)) << ours.str() << " vs " << theirs.str();
  ASSERT_FALSE(ours.output.empty()) << "the control file prints nothing, so a changed output proves nothing";

  oracle::Ledger none = oracle::parse_ledger(
      "| id | category | status | ref | corpus | resolved-by | summary |\n| --- | --- | --- | --- | --- | --- | --- |\n"
      "| D-001 | limit | recorded | - | - | 9 | x |\n", kCorpus);
  oracle::Verdict bad_output = ours;
  bad_output.output.back() ^= 1;
  oracle::Judgement j1 = oracle::judge({{file, bad_output, theirs}}, none);
  ASSERT_FALSE(j1.ok());
  EXPECT_NE(j1.failures[0].find(file), std::string::npos);

  oracle::Verdict bad_result = ours;
  bad_result.result_kind = "integer";
  bad_result.result_text = "7";
  oracle::Judgement j2 = oracle::judge({{file, bad_result, theirs}}, none);
  ASSERT_FALSE(j2.ok());
  EXPECT_NE(j2.failures[0].find(file), std::string::npos);
}

TEST(Oracle, ARunawayIsKilledOnTheCtangSideAndPausedOnTheLangTangSideAndThatAgrees) {
  const char * runner = runner_or_fail();
  ASSERT_NE(runner, nullptr);
  std::string path = kCorpus + "/script/runaway-while-true.tang";
  oracle::Verdict ours = oracle::lang_tang_run(read_file(path), true, 200000);
  oracle::Verdict theirs = oracle::ctang_run_verdict(runner, "run-script", path, 1000);
  EXPECT_EQ(ours.kind, oracle::Kind::Paused);
  EXPECT_EQ(theirs.kind, oracle::Kind::Killed);
  EXPECT_TRUE(oracle::agree(ours, theirs));
  // And the two ways it can go wrong: lang-tang finishing what ctang could not
  // is a divergence, and ctang finishing what lang-tang paused on is too.
  EXPECT_FALSE(oracle::agree(oracle::Verdict::ran("", "null", ""), theirs));
  EXPECT_FALSE(oracle::agree(ours, oracle::Verdict::ran("", "null", "")));
}

TEST(Oracle, AnUnreadableRunnerReplyIsAHarnessFailureAndNeverAVerdict) {
  // The runner that exits 0 and prints nothing (planted case 7 patches the
  // real runner into exactly this): the driver throws, so no comparison can
  // read the silence as agreement.
  const char * runner = runner_or_fail();
  ASSERT_NE(runner, nullptr);
  std::string path = kCorpus + "/script/fib.tang";
  EXPECT_THROW(oracle::ctang_run_verdict("/bin/true", "run-script", path, 5000), std::runtime_error);
  EXPECT_THROW(oracle::ctang_verdict("/bin/true", "script", path, 5000), std::runtime_error);
  // The real runner, asked something it does not know, exits with a usage code.
  EXPECT_THROW(oracle::ctang_run_verdict(runner, "no-such-mode", path, 5000), std::runtime_error);
  // And a file it cannot read.
  EXPECT_THROW(oracle::ctang_run_verdict(runner, "run-script", kCorpus + "/script/no-such-file.tang", 5000), std::runtime_error);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
