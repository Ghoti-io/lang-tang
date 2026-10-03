/**
 * @file
 *
 * The oracle differential (AD-16): every corpus file, parsed by lang-tang and
 * by frozen ctang in a child process, compared by the comparison unit and held
 * against the divergence ledger.
 *
 * At this commit both engines can only parse, so the verdict is accept(node
 * count) or reject. That already catches a mis-ported production, a changed
 * precedence and a scanner mode slip, which are the defects a port produces.
 * Execution comparison replaces it later and reuses this driver, the
 * comparison and the ledger rules.
 *
 * The runner is named by GLTANG_ORACLE_RUNNER, which `make test-oracle` sets
 * after it has checked that ctang exists. An unset variable is a failure, not
 * a skip.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "oracle/oracle.h"

#include <cstdlib>

namespace {

const std::string kCorpus = std::string(GLTANG_TEST_DATA) + "/corpus";
const std::string kLedger = std::string(GLTANG_TEST_DATA) + "/../documentation/divergence-ledger.md";

oracle::Verdict lang_tang_verdict(const std::string & source, GLTANG_ParseMode mode) {
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

} // namespace

TEST(Oracle, TheRunnerIsNamed) {
  // Never skipped: make test-oracle sets this only after finding ctang.
  const char * runner = std::getenv("GLTANG_ORACLE_RUNNER");
  ASSERT_NE(runner, nullptr) << "GLTANG_ORACLE_RUNNER is not set; run this through `make test-oracle`";
  ASSERT_TRUE(oracle::file_exists(runner)) << runner;
}

TEST(Oracle, TheLedgerIsValid) {
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  for (const auto & e : ledger.errors) {
    ADD_FAILURE() << e;
  }
}

TEST(Oracle, LangTangAndCtangAgreeOnEveryCorpusFileOrALedgerRowRecordsWhy) {
  const char * runner = std::getenv("GLTANG_ORACLE_RUNNER");
  ASSERT_NE(runner, nullptr) << "GLTANG_ORACLE_RUNNER is not set; run this through `make test-oracle`";
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  ASSERT_TRUE(ledger.valid());

  std::vector<oracle::Entry> entries;
  for (const auto & file : oracle::list_corpus(kCorpus)) {
    bool script = file.compare(0, 7, "script/") == 0;
    std::string path = kCorpus + "/" + file;
    oracle::Entry e;
    e.file = file;
    e.lang_tang = lang_tang_verdict(read_file(path), script ? GLTANG_PARSE_SCRIPT : GLTANG_PARSE_TEMPLATE);
    e.ctang = oracle::ctang_verdict(runner, script ? "script" : "template", path, 10000);
    entries.push_back(e);
  }
  // A differential over a handful of files proves little; the corpus floor is
  // part of the gate.
  ASSERT_GE(entries.size(), 60u);

  oracle::Judgement j = oracle::judge(entries, ledger);
  for (const auto & r : j.recorded) {
    std::printf("  recorded divergence: %s\n", r.c_str());
  }
  for (const auto & f : j.failures) {
    ADD_FAILURE() << f;
  }
  std::printf("  oracle: %zu files, %zu agree, %zu recorded divergences\n", entries.size(), j.agreed, j.recorded.size());
}

TEST(Oracle, TheComparisonSeesAMutatedVerdict) {
  // A differential that has never been seen to disagree may be measuring
  // nothing. Take a real corpus file where both engines accept, change one
  // side's node count, and require the judge to name the file.
  const char * runner = std::getenv("GLTANG_ORACLE_RUNNER");
  ASSERT_NE(runner, nullptr);
  std::string file = "script/if-else.tang";
  std::string path = kCorpus + "/" + file;
  oracle::Verdict ours = lang_tang_verdict(read_file(path), GLTANG_PARSE_SCRIPT);
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
