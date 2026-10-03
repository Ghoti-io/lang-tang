/**
 * @file
 *
 * The comparison unit, the judge that holds it against the ledger, and the
 * child-process driver, each driven by planted cases. No ctang is involved:
 * these are the rules of the differential, shown to fail where they should
 * (the differential itself is tests/oracle/test_oracle.cpp).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "oracle/oracle.h"

using oracle::Verdict;

namespace {

const std::string kCorpus = std::string(GLTANG_TEST_DATA) + "/corpus";

const char * kHeader =
    "| id | category | status | ref | corpus | resolved-by | summary |\n"
    "| --- | --- | --- | --- | --- | --- | --- |\n";

oracle::Ledger ledger_of(const std::string & rows) {
  oracle::Ledger l = oracle::parse_ledger(std::string(kHeader) + rows, kCorpus);
  for (const auto & e : l.errors) {
    ADD_FAILURE() << "the planted ledger is itself malformed: " << e;
  }
  return l;
}

bool any_mentions(const std::vector<std::string> & lines, const std::string & needle) {
  for (const auto & l : lines) {
    if (l.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace

TEST(Comparison, EqualVerdictsAgree) {
  EXPECT_TRUE(oracle::agree(Verdict::accept(5), Verdict::accept(5)));
  EXPECT_TRUE(oracle::agree(Verdict::reject(), Verdict::reject()));
}

TEST(Comparison, AcceptWithDifferentNodeCountsDisagree) {
  EXPECT_FALSE(oracle::agree(Verdict::accept(5), Verdict::accept(6)));
}

TEST(Comparison, AcceptAndRejectDisagreeEitherWayRound) {
  EXPECT_FALSE(oracle::agree(Verdict::accept(1), Verdict::reject()));
  EXPECT_FALSE(oracle::agree(Verdict::reject(), Verdict::accept(1)));
}

TEST(Comparison, KilledAgreesOnlyWithPaused) {
  EXPECT_TRUE(oracle::agree(Verdict::paused(), Verdict::killed()));
  EXPECT_FALSE(oracle::agree(Verdict::reject(), Verdict::killed()));
  EXPECT_FALSE(oracle::agree(Verdict::accept(3), Verdict::killed()));
  EXPECT_FALSE(oracle::agree(Verdict::killed(), Verdict::killed())); // lang-tang is never killed
}

TEST(Comparison, PausedAgreesWithNothingElse) {
  EXPECT_FALSE(oracle::agree(Verdict::paused(), Verdict::reject()));
  EXPECT_FALSE(oracle::agree(Verdict::paused(), Verdict::accept(2)));
}

TEST(Judge, AllAgreeingPasses) {
  oracle::Ledger ledger = ledger_of("| D-001 | limit | open | - | - | 9 | x |\n");
  auto j = oracle::judge({{"script/if.tang", Verdict::accept(4), Verdict::accept(4)}}, ledger);
  EXPECT_TRUE(j.ok());
  EXPECT_EQ(j.agreed, 1u);
}

TEST(Judge, APlantedUnrecordedDivergenceFailsNamingTheFile) {
  oracle::Ledger ledger = ledger_of("| D-001 | limit | open | - | - | 9 | x |\n");
  auto j = oracle::judge({{"script/if.tang", Verdict::accept(4), Verdict::reject()}}, ledger);
  ASSERT_FALSE(j.ok());
  EXPECT_TRUE(any_mentions(j.failures, "script/if.tang"));
  EXPECT_TRUE(any_mentions(j.failures, "unrecorded divergence"));
}

TEST(Judge, AnOpenRowDoesNotExcuseADivergence) {
  // Only `recorded` accepts a divergence: open means nobody has decided yet.
  oracle::Ledger ledger = ledger_of("| D-001 | ctang-defect | open | 9 | script/if.tang | 11 | undecided |\n");
  auto j = oracle::judge({{"script/if.tang", Verdict::accept(4), Verdict::reject()}}, ledger);
  EXPECT_FALSE(j.ok());
}

TEST(Judge, ARecordedRowAcceptsTheDivergenceItNames) {
  oracle::Ledger ledger = ledger_of("| D-001 | ctang-defect | recorded | 9 | script/if.tang | 11 | a deliberate departure |\n");
  auto j = oracle::judge({{"script/if.tang", Verdict::accept(4), Verdict::reject()}}, ledger);
  EXPECT_TRUE(j.ok());
  ASSERT_EQ(j.recorded.size(), 1u);
  EXPECT_NE(j.recorded[0].find("D-001"), std::string::npos);
}

TEST(Judge, ARecordedRowForAnotherFileDoesNotExcuseThisOne) {
  oracle::Ledger ledger = ledger_of("| D-001 | ctang-defect | recorded | 9 | script/while.tang | 11 | other |\n");
  auto j = oracle::judge({{"script/if.tang", Verdict::accept(4), Verdict::reject()}}, ledger);
  ASSERT_FALSE(j.ok());
  EXPECT_TRUE(any_mentions(j.failures, "script/if.tang"));
}

TEST(Judge, APlantedStaleRowFailsNamingTheRow) {
  oracle::Ledger ledger = ledger_of("| D-007 | ctang-defect | recorded | 9 | script/if.tang | 11 | no longer true |\n");
  auto j = oracle::judge({{"script/if.tang", Verdict::accept(4), Verdict::accept(4)}}, ledger);
  ASSERT_FALSE(j.ok());
  EXPECT_TRUE(any_mentions(j.failures, "stale ledger row D-007"));
  EXPECT_TRUE(any_mentions(j.failures, "script/if.tang"));
}

TEST(Judge, KilledCtangAgainstRejectIsADivergenceNotACrash) {
  oracle::Ledger ledger = ledger_of("| D-001 | limit | open | - | - | 9 | x |\n");
  auto j = oracle::judge({{"script/if.tang", Verdict::reject(), Verdict::killed()}}, ledger);
  ASSERT_FALSE(j.ok());
  EXPECT_TRUE(any_mentions(j.failures, "ctang killed"));
}

TEST(Judge, KilledCtangAgainstPausedAgrees) {
  oracle::Ledger ledger = ledger_of("| D-001 | limit | open | - | - | 9 | x |\n");
  auto j = oracle::judge({{"script/loop.tang", Verdict::paused(), Verdict::killed()}}, ledger);
  EXPECT_TRUE(j.ok());
}

TEST(Driver, ReadsAChildsOutputAndExit) {
  auto r = oracle::run_child({"/bin/sh", "-c", "printf 'ok 7\\n'"}, 5000);
  EXPECT_FALSE(r.timed_out);
  EXPECT_FALSE(r.signaled);
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.output, "ok 7\n");
}

TEST(Driver, AHungChildIsKilledAtTheWallClockAndIsAVerdict) {
  // A runner that sleeps: the stand-in for a ctang that hangs.
  auto r = oracle::run_child({"/bin/sh", "-c", "sleep 30"}, 300);
  EXPECT_TRUE(r.timed_out);
  EXPECT_LT(r.seconds, 10.0);
  EXPECT_EQ(oracle::ctang_verdict("/bin/sh", "-c", "sleep 30", 300).kind, oracle::Kind::Killed);
}

TEST(Driver, ACrashingChildIsAVerdictToo) {
  EXPECT_EQ(oracle::ctang_verdict("/bin/sh", "-c", "kill -SEGV $$", 5000).kind, oracle::Kind::Killed);
  auto r = oracle::run_child({"/bin/sh", "-c", "kill -ABRT $$"}, 5000);
  EXPECT_TRUE(r.signaled);
  EXPECT_EQ(r.signal_number, SIGABRT);
}

TEST(Driver, ACrashDisagreesWithARejection) {
  Verdict ct = oracle::ctang_verdict("/bin/sh", "-c", "kill -SEGV $$", 5000);
  EXPECT_FALSE(oracle::agree(Verdict::reject(), ct));
  EXPECT_TRUE(oracle::agree(Verdict::paused(), ct));
}

TEST(Driver, ABrokenRunnerThrowsRatherThanPassingAsAVerdict) {
  // Exit 3 is "cannot read the file": a broken harness, never a verdict.
  EXPECT_THROW(oracle::ctang_verdict("/bin/sh", "-c", "exit 3", 5000), std::runtime_error);
  EXPECT_THROW(oracle::ctang_verdict("/bin/sh", "-c", "echo gibberish", 5000), std::runtime_error);
  EXPECT_THROW(oracle::ctang_verdict("/no/such/runner", "script", "x", 5000), std::runtime_error);
}

TEST(Driver, ParsesBothVerdictLines) {
  EXPECT_EQ(oracle::ctang_verdict("/bin/sh", "-c", "echo error", 5000).kind, oracle::Kind::Reject);
  Verdict v = oracle::ctang_verdict("/bin/sh", "-c", "echo 'ok 12'", 5000);
  EXPECT_EQ(v.kind, oracle::Kind::Accept);
  EXPECT_EQ(v.nodes, 12u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
