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
  // The divergence rows are all decided (D-001 was the last open one, recorded
  // by story 10). The capability-removed rows (D-032 and after) are open until
  // spec-tang-host-extensibility restores each capability, so the ledger is not
  // closed and ctang does not retire (AD-3) while a capability it offered is
  // missing. What this test guards: nothing but a capability-removed row is
  // ever left open, and the ledger is closed exactly when none is.
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  ASSERT_TRUE(ledger.valid());
  size_t open = 0;
  for (const auto & row : ledger.rows) {
    if (row.status == "open") {
      ++open;
      EXPECT_EQ(row.category, "capability-removed") << row.id << " is open and is not a capability still to be restored";
    }
  }
  EXPECT_EQ(ledger.closed(), open == 0);
}

TEST(Ledger, EveryCapabilityRemovedRowSaysWhatCameBackAndWhere) {
  // spec-tang-host-extensibility story 1: one row per host-visible capability
  // ctang offered and lang-tang lacks. Each names the story that restores it
  // (resolved-by H<n>), has no corpus file (nothing differs in a host-free
  // script), and says in its summary what ctang had and where it is cited.
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  ASSERT_TRUE(ledger.valid());
  size_t rows = 0;
  for (const auto & row : ledger.rows) {
    if (row.category != "capability-removed") {
      continue;
    }
    ++rows;
    EXPECT_TRUE(row.status == "open" || row.status == "fixed") << row.id << " status " << row.status;
    EXPECT_EQ(row.resolved_by.compare(0, 1, "H"), 0) << row.id << " is not resolved by a story of the host-extensibility spec";
    EXPECT_TRUE(row.corpus.empty()) << row.id;
    EXPECT_GT(row.summary.size(), 120u) << row.id << " does not say what ctang had and where it is cited";
    EXPECT_NE(row.summary.find("ctang/"), std::string::npos) << row.id << " cites no ctang file";
  }
  EXPECT_EQ(rows, 25u) << "the inventory of planning/tang-host-extensibility.md section 3 has 25 capabilities to restore";
}

TEST(Ledger, TheDivergencesTheExecutionDifferentialFindsNameTheirFiles) {
  // Story 11: every recorded row whose divergence shows in the output or the
  // result names the corpus files that show it, so it goes stale (and fails the
  // differential) the day the divergence disappears. The rows with no file by
  // nature say why in their summary.
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  ASSERT_TRUE(ledger.valid());
  for (const char * id : {"D-009", "D-010", "D-011", "D-012", "D-013", "D-014", "D-015", "D-017", "D-023", "D-024", "D-025", "D-026", "D-027", "D-028", "D-029", "D-030", "D-031"}) {
    const oracle::Row * row = nullptr;
    for (const auto & r : ledger.rows) {
      if (r.id == id) {
        row = &r;
      }
    }
    ASSERT_NE(row, nullptr) << id;
    EXPECT_EQ(row->status, "recorded") << id;
    EXPECT_FALSE(row->corpus.empty()) << id << " names no corpus file";
  }
  for (const auto & r : ledger.rows) {
    if (r.status == "recorded" && r.corpus.empty()) {
      EXPECT_GT(r.summary.size(), 80u) << r.id << " has no corpus file and must say why in its summary";
    }
  }
}

TEST(Ledger, NoCorpusFileIsNamedByTwoRows) {
  // A file in two rows would be accepted by the first and make the second
  // impossible to see stale.
  oracle::Ledger ledger = oracle::parse_ledger(oracle::read_text_file(kLedger), kCorpus);
  std::map<std::string, std::string> owner;
  for (const auto & r : ledger.rows) {
    for (const auto & f : r.corpus) {
      auto inserted = owner.emplace(f, r.id);
      EXPECT_TRUE(inserted.second) << f << " is named by " << inserted.first->second << " and " << r.id;
    }
  }
}

TEST(Ledger, TheHeaderNoLongerSaysTheOracleIsParseOnly) {
  std::string text = oracle::read_text_file(kLedger);
  EXPECT_EQ(text.find("parse-only"), std::string::npos);
  EXPECT_EQ(text.find("parse only"), std::string::npos);
  EXPECT_NE(text.find("rendered output bytes and the\nfinal result"), std::string::npos) << "the header says what execution compares";
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

TEST(Ledger, PlantedCapabilityRemovedRowIsAcceptedAndKeepsTheLedgerOpen) {
  // The category and the H<n> story reference are what story 1 of the
  // host-extensibility spec added; an open row of that category is a
  // capability not yet restored, and it keeps the ledger open.
  oracle::Ledger ledger = planted("| D-001 | capability-removed | open | - | - | H5 | ctang/x had it and lang-tang does not, restored by story 5 of the spec |\n");
  ASSERT_TRUE(ledger.valid()) << (ledger.errors.empty() ? "" : ledger.errors[0]);
  EXPECT_FALSE(ledger.closed());
  oracle::Ledger done = planted("| D-001 | capability-removed | fixed | - | - | H5, H6 | restored by stories 5 and 6 |\n");
  ASSERT_TRUE(done.valid()) << (done.errors.empty() ? "" : done.errors[0]);
  EXPECT_TRUE(done.closed());
}

TEST(Ledger, PlantedBadHostStoryReferenceFails) {
  EXPECT_TRUE(mentions(planted("| D-001 | capability-removed | open | - | - | Hx | x |\n"), "resolved-by 'Hx'"));
  EXPECT_TRUE(mentions(planted("| D-001 | capability-removed | open | - | - | soon | x |\n"), "resolved-by 'soon'"));
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
