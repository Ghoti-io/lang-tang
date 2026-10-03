/**
 * @file
 *
 * The divergence ledger: the real one is valid and seeded, and the validator
 * is shown to fail on a planted ledger for each way a row can be malformed.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "oracle/oracle.h"

namespace {

const std::string kCorpus = std::string(GLTANG_TEST_DATA) + "/corpus";
const std::string kLedger = std::string(GLTANG_TEST_DATA) + "/../documentation/divergence-ledger.md";

const char * kHeader =
    "| id | category | status | ref | corpus | resolved-by | summary |\n"
    "| --- | --- | --- | --- | --- | --- | --- |\n";

bool mentions(const oracle::Ledger & ledger, const std::string & needle) {
  for (const auto & e : ledger.errors) {
    if (e.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

oracle::Ledger planted(const std::string & rows) {
  return oracle::parse_ledger(std::string(kHeader) + rows, kCorpus);
}

} // namespace

TEST(Ledger, TheRealLedgerIsValid) {
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  for (const auto & e : ledger.errors) {
    ADD_FAILURE() << e;
  }
  EXPECT_FALSE(ledger.rows.empty());
}

TEST(Ledger, EverySectionThirteenItemThatIsNotFixedHasARow) {
  // Language reference section 13: items 9 ("not a defect, described
  // wrongly") and 13 ("still open") are the two whose status is not Fixed.
  // Item 9 was `open` until the host API (story 10): the error list is what
  // closes it, so its row is `recorded` (D-001, resolved by 10). Item 13
  // (dates) was closed by the compiler story, which adds no syntax: its row is
  // `fixed`. The test requires that each row exists and has not been dropped.
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  for (const char * ref : {"9", "13"}) {
    std::string wanted = std::string(ref) == "9" ? "recorded" : "fixed";
    bool found = false;
    for (const auto & r : ledger.rows) {
      if (r.ref == ref && r.category == "ctang-defect" && r.status == wanted) {
        found = true;
      }
    }
    EXPECT_TRUE(found) << "no " << wanted << " ctang-defect row for section 13 item " << ref;
  }
}

TEST(Ledger, TheRecordedDeparturesAreSeeded) {
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  int rng = 0, limit = 0, reporting = 0;
  for (const auto & r : ledger.rows) {
    if (r.status != "recorded") {
      continue;
    }
    rng += r.category == "rng";
    limit += r.category == "limit";
    reporting += r.category == "error-reporting";
  }
  EXPECT_GE(rng, 1);
  EXPECT_GE(limit, 2); // budgets; and the pause and unwind outcomes
  EXPECT_GE(reporting, 1);
}

TEST(Ledger, IsClosedExactlyWhenNoRowIsOpen) {
  // After the host API (story 10) section 13.9, the last open row, is a
  // recorded departure: the error is listed. The execution differential
  // (story 11) may open rows again, and then this reads false.
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  ASSERT_TRUE(ledger.valid());
  size_t open = 0;
  for (const auto & row : ledger.rows) {
    open += row.status == "open";
  }
  EXPECT_EQ(ledger.closed(), open == 0);
  EXPECT_EQ(open, 0u) << "D-001 was the last open row, and story 10 recorded it";
}

TEST(Ledger, ClosedMeansNoRowIsOpen) {
  oracle::Ledger open = planted("| D-001 | limit | open | - | - | 9 | still open |\n");
  ASSERT_TRUE(open.valid());
  EXPECT_FALSE(open.closed());
  oracle::Ledger done = planted(
      "| D-001 | limit | recorded | - | - | 9 | a departure |\n"
      "| D-002 | ctang-defect | fixed | 9 | - | 11 | repaired |\n");
  ASSERT_TRUE(done.valid());
  EXPECT_TRUE(done.closed());
}

TEST(Ledger, PlantedWellFormedRowPasses) {
  oracle::Ledger ledger = planted("| D-001 | ctang-defect | recorded | 13 | script/reject-date-now.tang, script/if.tang | 9 | fine |\n");
  EXPECT_TRUE(ledger.valid()) << (ledger.errors.empty() ? "" : ledger.errors[0]);
  ASSERT_EQ(ledger.rows.size(), 1u);
  EXPECT_EQ(ledger.rows[0].corpus.size(), 2u);
}

TEST(Ledger, PlantedDuplicateIdFails) {
  oracle::Ledger ledger = planted(
      "| D-001 | limit | open | - | - | 9 | one |\n"
      "| D-001 | rng | recorded | - | - | 10 | two |\n");
  EXPECT_FALSE(ledger.valid());
  EXPECT_TRUE(mentions(ledger, "D-001 is not unique"));
}

TEST(Ledger, PlantedBadCategoryFails) {
  oracle::Ledger ledger = planted("| D-001 | whim | open | - | - | 9 | x |\n");
  EXPECT_TRUE(mentions(ledger, "category 'whim'"));
}

TEST(Ledger, PlantedBadStatusFails) {
  oracle::Ledger ledger = planted("| D-001 | limit | pending | - | - | 9 | x |\n");
  EXPECT_TRUE(mentions(ledger, "status 'pending'"));
}

TEST(Ledger, PlantedMissingCorpusFileFails) {
  oracle::Ledger ledger = planted("| D-001 | limit | recorded | - | script/no-such-file.tang | 9 | x |\n");
  EXPECT_TRUE(mentions(ledger, "script/no-such-file.tang"));
}

TEST(Ledger, PlantedBadIdFails) {
  oracle::Ledger ledger = planted("| X-1 | limit | open | - | - | 9 | x |\n");
  EXPECT_TRUE(mentions(ledger, "id 'X-1'"));
}

TEST(Ledger, PlantedBadRefFails) {
  oracle::Ledger ledger = planted("| D-001 | limit | open | 13.9 | - | 9 | x |\n");
  EXPECT_TRUE(mentions(ledger, "ref '13.9'"));
}

TEST(Ledger, PlantedBadResolvedByFails) {
  oracle::Ledger ledger = planted("| D-001 | limit | open | - | - | soon | x |\n");
  EXPECT_TRUE(mentions(ledger, "resolved-by 'soon'"));
}

TEST(Ledger, PlantedWrongCellCountFails) {
  oracle::Ledger ledger = planted("| D-001 | limit | open | - | 9 | x |\n");
  EXPECT_TRUE(mentions(ledger, "6 cells, not 7"));
}

TEST(Ledger, PlantedEmptySummaryFails) {
  oracle::Ledger ledger = planted("| D-001 | limit | open | - | - | 9 |  |\n");
  EXPECT_TRUE(mentions(ledger, "summary is empty"));
}

TEST(Ledger, PlantedMalformedRowIsReportedNamingItsLine) {
  oracle::Ledger ledger = planted(
      "| D-001 | limit | open | - | - | 9 | fine |\n"
      "| D-002 | bogus | open | - | - | 9 | broken |\n");
  ASSERT_FALSE(ledger.valid());
  EXPECT_TRUE(mentions(ledger, "line 4:"));
}

TEST(Ledger, PlantedNoTableFails) {
  oracle::Ledger ledger = oracle::parse_ledger("# Ledger\n\nNothing tabular here.\n", kCorpus);
  EXPECT_TRUE(mentions(ledger, "no table"));
}

TEST(Ledger, PlantedZeroRowsFails) {
  oracle::Ledger ledger = planted("");
  EXPECT_TRUE(mentions(ledger, "no rows"));
}

TEST(Ledger, PlantedWrongHeaderFails) {
  oracle::Ledger ledger = oracle::parse_ledger("| id | kind | status | ref | corpus | resolved-by | summary |\n| - | - | - | - | - | - | - |\n| D-001 | limit | open | - | - | 9 | x |\n", kCorpus);
  EXPECT_TRUE(mentions(ledger, "header row"));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
