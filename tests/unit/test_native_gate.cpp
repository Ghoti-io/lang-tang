/**
 * @file
 *
 * The native budget gate (CAP-2, CAP-7, AD-21): every limit, and every native
 * the guest can drive with unbounded work, driven separately with adversarial
 * input under a tiny budget, and required to reach a verdict within a bounded
 * amount of work. The outcome is a pause, an unwind (`GRCORE_ERR_LIMIT`) or the
 * program's own error value; never a hang, a crash or a signal (the process
 * survives every case, and an alarm kills it if one hangs).
 *
 * The work a case did is measured by counters this test owns, read after the
 * verdict: the fuel the context charged (`grcore_context_fuel_used`) and the
 * most bytes the group held at once, counted by the allocator and the page
 * provider the group was given (`tt::Tracker`). Each is compared with the
 * budget: the fuel with the budget plus a slack of a few poll chunks, the bytes
 * with the largest allocation the case legitimately makes plus a slack. The
 * wall clock is only a backstop.
 *
 * A case has a build, which makes the operand and is measured first (its fuel
 * and bytes are the baseline, not the work), and an op, which is the adversarial
 * operation and runs on a budget of its own beyond the build.
 *
 * Coverage: the engine's own list of natives is src/vm/natives.def. The gate
 * reads it and the sources and fails if a poll does not name an entry, an entry
 * is named by nothing, or an unbounded entry has no row below, so a new native
 * without a row is a failing build. (That a native polls at all is what its row
 * shows: a native whose poll is removed runs to completion, and its row fails.)
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "exec_harness.h"
#include "test_helpers.h"

#include <chrono>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace {

const uint64_t kUnlimited = GRCORE_UNLIMITED;
const size_t kMiB = 1024u * 1024u;

enum Outcome : unsigned {
  PAUSED = 1,          // run returned OK, paused: the host may raise the budget
  LIMIT = 2,           // run returned GRCORE_ERR_LIMIT: unwound
  FINISHED_ERROR = 4,  // the program ended, holding an error value
  FINISHED_OK = 8,     // the program ended with a value (a case that must not)
};

struct Row {
  const char * native;        // the natives.def entry it covers, or "" for a limit row
  const char * name;
  std::string build;          // makes the operand; its cost is the baseline
  std::string op;             // the adversarial operation
  uint64_t fuel;              // fuel the op may spend beyond the build
  uint64_t memory;            // heap bytes the op may take beyond the build, or kUnlimited
  unsigned allowed;           // the outcomes that are a verdict
  size_t max_extra_bytes;     // the most the group may hold beyond the build's peak
  bool gc = false;            // a collection must have run (memory over budget)
  GLTANG_ErrorKind error = GLTANG_ERROR_KIND_COUNT;  // the error value expected, or any
  uint64_t calls = 512;
  size_t host_string = 0;     // a library string of this many bytes named `big`
  uint64_t fuel_slack = 1500;
};

struct Measured {
  uint64_t fuel = 0;
  size_t peak = 0;
  uint64_t memory_peak = 0;
};

struct Result {
  unsigned outcome = 0;
  GLTANG_ErrorKind error = GLTANG_ERROR_KIND_COUNT;
  Measured m;
  uint64_t collections = 0;
  GRCORE_Result ran = GRCORE_OK;
  double seconds = 0;
};

Result run_source(const std::string & source, const Row & row, uint64_t fuel, uint64_t memory) {
  Result r;
  tt::Compiled compiled(source);
  EXPECT_TRUE(compiled.ok()) << source << ": " << compiled.error.message;
  if (!compiled.ok()) {
    return r;
  }
  tt::Config config;
  config.fuel = fuel;
  config.memory_bytes = memory;
  config.calls = row.calls;
  tt::Context context(compiled.program, config);
  EXPECT_TRUE(context.ok());
  if (!context.ok()) {
    return r;
  }
  if (row.host_string) {
    EXPECT_TRUE(context.add_library("big", tt::Host::string(std::string(row.host_string, 'x'))));
  }
  context.attach();
  auto started = std::chrono::steady_clock::now();
  r.ran = grcore_run(context.context, gltang_execution_entry, context.execution, &context.outcome);
  context.has_run = true;
  r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  if (r.ran == GRCORE_ERR_LIMIT) {
    r.outcome = LIMIT;
  }
  else if (r.ran == GRCORE_OK && context.outcome == GRCORE_OUTCOME_PAUSED) {
    r.outcome = PAUSED;
  }
  else if (r.ran == GRCORE_OK && context.outcome == GRCORE_OUTCOME_FINISHED) {
    r.outcome = context.is_error() ? FINISHED_ERROR : FINISHED_OK;
    if (context.is_error()) {
      r.error = context.error_kind();
    }
  }
  r.m.fuel = grcore_context_fuel_used(context.context);
  r.m.peak = context.tracker.peak_bytes;
  r.m.memory_peak = grcore_context_memory_peak(context.context);
  GRHEAP_Stats stats;
  if (context.heap && grheap_stats(context.heap, &stats) == GRHEAP_OK) {
    r.collections = stats.collections;
  }
  return r;
}

/// A row: the build alone, for the baseline, then the build and the op on a budget.
void run_row(const Row & row) {
  SCOPED_TRACE(std::string(row.native) + ": " + row.name);
  alarm(60);  // a hang is a failure, not a stuck suite
  Result base = run_source(row.build + " 0;", row, kUnlimited, kUnlimited);
  uint64_t memory = row.memory == kUnlimited ? kUnlimited : base.m.memory_peak + row.memory;
  Result r = run_source(row.build + " " + row.op, row, base.m.fuel + row.fuel, memory);
  alarm(0);
  EXPECT_TRUE(r.outcome & row.allowed) << "outcome " << r.outcome << " (1 paused, 2 limit, 4 finished with an error, 8 finished) is not a verdict for this case; run returned " << r.ran;
  EXPECT_LE(r.m.fuel, base.m.fuel + row.fuel + row.fuel_slack)
      << "fuel charged " << r.m.fuel << " against a budget of " << (base.m.fuel + row.fuel) << " (build " << base.m.fuel << " + " << row.fuel << ")";
  EXPECT_LE(r.m.peak, base.m.peak + row.max_extra_bytes)
      << "bytes held " << r.m.peak << " against a build that held " << base.m.peak << " and a bound of " << row.max_extra_bytes << " more";
  if (r.outcome == FINISHED_ERROR && row.error != GLTANG_ERROR_KIND_COUNT) {
    EXPECT_EQ(r.error, row.error);
  }
  if (row.gc) {
    EXPECT_GT(r.collections, 0u) << "memory over budget runs a collection before the verdict";
  }
  EXPECT_LT(r.seconds, 20.0) << "the wall clock is only a backstop, and this case needed it";
}

const char * kStr1M = "s = \"x\"; for (i = 0; i < 20; i += 1) { s = s + s; }";                  // 1 MiB of text
const char * kHtml1M = "s = !\"x\"; for (i = 0; i < 20; i += 1) { s = s + s; }";                 // the same, tagged HTML
const char * kArr200k = "a = [0] * 200000;";                                                      // 1.6 MB of elements

const unsigned kStops = PAUSED | LIMIT;
const unsigned kStopsOrRefuses = PAUSED | LIMIT | FINISHED_ERROR;

/// Every unbounded native of natives.def has at least one row here; a limit
/// row has an empty native. The table is the gate's coverage.
std::vector<Row> rows() {
  std::vector<Row> t;
  // Fuel: a loop the guest never ends.
  t.push_back({"", "fuel: an endless loop", "x = 1;", "while (true) {}", 1000, kUnlimited, PAUSED, 64 * 1024});
  // Memory: a string that doubles forever, with fuel enough that the memory budget is what ends it.
  t.push_back({"", "memory: doubling forever under a small budget", "s = \"xxxxxxxx\";", "while (true) { s = s + s; }", 2000000, 4 * kMiB, kStopsOrRefuses,
      8 * kMiB, true});
  // Guest stack depth: recursion past the budget is an error value, not a crash.
  t.push_back({"", "guest depth: recursion past the budget", "function f(n) { return f(n + 1); }", "x = f(0); x;", 100000, kUnlimited, FINISHED_ERROR,
      4 * kMiB, false, GLTANG_ERROR_RECURSION_LIMIT});
  // Guest depth with no budget at all: the fuel ends it, with the frames on the guest stack and the C stack untouched.
  Row unlimited_depth = {"", "guest depth: unbounded recursion, ended by fuel", "function f(n) { return f(n + 1); }", "f(0);", 200000, kUnlimited, PAUSED,
      64 * kMiB};
  unlimited_depth.calls = kUnlimited;
  t.push_back(unlimited_depth);
  // Native stack depth: a container nested past the value-depth bound is an error value in copy, equality and print.
  t.push_back({"", "native depth: containers nested past the bound", "a = []; for (i = 0; i < 2200; i += 1) { a = [a]; }", "b = [a]; c = a == a; d = a + a; print(a); print(b.size); c;", 20000000,
      kUnlimited, FINISHED_ERROR | FINISHED_OK, 128 * kMiB});

  // Guest-driven natives, one or more rows each. The golden row first.
  t.push_back({"STRING_CONCAT", "string doubling in an endless loop", "s = \"x\";", "while (true) { s = s + s; }", 1000, 32 * kMiB, kStops, 2 * kMiB});
  t.push_back({"STRING_CONCAT", "concatenating two huge strings", kStr1M, "t = s + s;", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"STRING_SUBSTRING", "slicing the tail of a huge string", kStr1M, "t = s[1:];", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"STRING_SLICE", "reversing a huge string", kStr1M, "t = s[::-1];", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"STRING_SLICE", "taking every other grapheme of a huge string", kStr1M, "t = s[::2];", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"STRING_RETAG", "giving a huge string another encoding", kStr1M, "t = s.html;", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"STRING_RENDER", "encoding a huge string", kHtml1M, "t = s.render;", 300, 16 * kMiB, kStops, 12 * kMiB});
  Row from_host = {"STRING_FROM_UTF8", "reading a huge string a host gave", "x = 1;", "use big; t = big;", 300, 16 * kMiB, kStops, 12 * kMiB};
  from_host.host_string = 1 * kMiB;
  // The scan and the copy of a host string are not paced from inside; the whole
  // charge (1 MiB / 64 = 16,384 fuel) is made at once, before any of the work,
  // and the verdict follows it. That charge is this native's slack.
  from_host.fuel_slack = 16384 + 1500;
  t.push_back(from_host);
  t.push_back({"PRINT", "printing a huge string", kStr1M, "print(s);", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"PRINT", "printing a large container", kArr200k, "print(a);", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"RENDER_TO_STRING", "rendering a large container with as string", kArr200k, "t = a as string;", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"RENDER_TO_STRING", "concatenating a string with a large container", kArr200k, "t = \"x\" + a;", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"ARRAY_GROW", "growing a large array by one past its end", kArr200k, "a[200000] = 1;", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"DEEP_COPY", "copying a large container into another", kArr200k, "b = [a];", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"EQUALITY", "comparing two huge equal arrays", "a = [0] * 200000; b = [0] * 200000;", "a == b;", 300, 16 * kMiB, kStops, 4 * kMiB});
  t.push_back({"ARRAY_CONCAT", "joining two large arrays", kArr200k, "c = a + a;", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"ARRAY_REPEAT", "repeating an array into 40 MB", "a = [0] * 1000;", "c = a * 5000;", 300, 64 * kMiB, kStops, 48 * kMiB});
  t.push_back({"ARRAY_REPEAT", "repeating an array into 8 GB under a small memory budget", "a = [0] * 1000;", "c = a * 1000000; c;", 300, 4 * kMiB, FINISHED_ERROR,
      8 * kMiB, true, GLTANG_ERROR_OUT_OF_MEMORY});
  t.push_back({"ARRAY_SLICE", "reversing a large array", kArr200k, "c = a[::-1];", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"ARRAY_SLICE", "the endless loop that grows an array by slices", "a = [0] * 100;", "while (true) { a = a + a; }", 1000, 32 * kMiB, kStops, 2 * kMiB});
  return t;
}

// ---------------------------------------------------------------------------
// The engine's list of natives and the sources that name them
// ---------------------------------------------------------------------------

struct Native {
  std::string id;
  bool unbounded;
};

std::string src_dir() {
  return std::string(GLTANG_TEST_DATA) + "/../src/vm";
}

std::vector<Native> engine_natives() {
  std::ifstream in(src_dir() + "/natives.def");
  std::string line;
  std::vector<Native> list;
  std::regex entry("^GLTANG_NATIVE\\((\\w+), ([01]), \"[^\"]*\"\\)$");
  while (std::getline(in, line)) {
    std::smatch m;
    if (std::regex_match(line, m, entry)) {
      list.push_back({m[1], m[2] == "1"});
    }
  }
  return list;
}

std::string slurp(const std::string & path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// Coverage
// ---------------------------------------------------------------------------

TEST(NativeGateCoverage, TheEnginesListOfNativesIsReadAndHasEntries) {
  std::vector<Native> natives = engine_natives();
  EXPECT_GE(natives.size(), 15u) << "natives.def was not read, or lost its entries";
  size_t unbounded = 0;
  for (const Native & n : natives) {
    unbounded += n.unbounded;
  }
  EXPECT_GE(unbounded, 12u);
}

TEST(NativeGateCoverage, EveryUnboundedNativeInTheEnginesListHasARowInTheTable) {
  std::set<std::string> rowed;
  for (const Row & r : rows()) {
    if (*r.native) {
      rowed.insert(r.native);
    }
  }
  for (const Native & n : engine_natives()) {
    if (n.unbounded) {
      EXPECT_TRUE(rowed.count(n.id)) << "the native " << n.id << " is in src/vm/natives.def and can be driven with unbounded work, and the gate has no row for it";
    }
  }
  std::set<std::string> known;
  for (const Native & n : engine_natives()) {
    known.insert(n.id);
  }
  for (const std::string & id : rowed) {
    EXPECT_TRUE(known.count(id)) << "a row names " << id << ", which natives.def does not list";
  }
}

TEST(NativeGateCoverage, EveryPollInTheSourcesNamesAnEntryAndEveryEntryIsNamed) {
  std::set<std::string> listed;
  for (const Native & n : engine_natives()) {
    listed.insert(n.id);
  }
  std::set<std::string> named;
  std::regex poll("GLTANG_NATIVE_POLL\\(\\s*[^,]+,\\s*(\\w+)\\s*,");
  std::regex pacer("GLTANG_PACER\\(\\s*[^,]+,\\s*(\\w+)\\s*\\)");
  const char * files[] = {"container.c", "errorlist.c", "execution.c", "interp.c", "libvalue.c", "ops.c", "string.c", "template.c", "text.c", "value.c"};
  size_t polls = 0;
  for (const char * f : files) {
    std::string text = slurp(src_dir() + "/" + f);
    ASSERT_FALSE(text.empty()) << f;
    for (std::sregex_iterator it(text.begin(), text.end(), poll), end; it != end; ++it) {
      named.insert((*it)[1]);
      ++polls;
    }
    for (std::sregex_iterator it(text.begin(), text.end(), pacer), end; it != end; ++it) {
      named.insert((*it)[1]);
      ++polls;
    }
    // A poll that names nobody: the raw call is only the definition (execution.c)
    // and the forwarding inside a pacer.
    std::istringstream lines(text);
    std::string line;
    int number = 0;
    while (std::getline(lines, line)) {
      ++number;
      bool raw = line.find("gltang_vm_native_poll(") != std::string::npos;
      bool forwarded = line.find("gltang_vm_native_poll_as(") != std::string::npos;
      if (raw && std::string(f) != "execution.c") {
        ADD_FAILURE() << f << ":" << number << ": a poll that names no native: " << line;
      }
      if (forwarded && line.find("pacer->native") == std::string::npos && std::string(f) != "execution.c") {
        ADD_FAILURE() << f << ":" << number << ": a poll that names no native: " << line;
      }
    }
  }
  EXPECT_GE(polls, 17u);
  for (const std::string & id : named) {
    EXPECT_TRUE(listed.count(id)) << id << " is polled but not in natives.def";
  }
  for (const std::string & id : listed) {
    EXPECT_TRUE(named.count(id)) << id << " is in natives.def and nothing polls for it";
  }
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

TEST(NativeGate, EveryRowReachesAVerdictWithinABoundedAmountOfWork) {
  std::vector<Row> t = rows();
  EXPECT_GE(t.size(), 25u);
  for (const Row & row : t) {
    run_row(row);
  }
}

// ---------------------------------------------------------------------------
// The limits that need the host: the wall clock, and a page of children
// ---------------------------------------------------------------------------

TEST(NativeGateLimits, WallClockTheHostTimerPostsTheRequestAndALongNativeStops) {
  // The host's timer thread posts the time request while a long native is
  // running. A native cannot pause (AD-21), so the verdict is an unwind; the
  // endless loop, which polls at every back-edge, pauses.
  struct Case {
    const char * name;
    std::string source;
  };
  const Case cases[] = {
      {"an endless loop of doublings", "s = \"x\"; while (true) { s = s + s; }"},
      {"a long chain of array copies", "a = [0] * 400000; while (true) { b = a + a + a + a; }"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.name);
    tt::Compiled compiled(c.source);
    ASSERT_TRUE(compiled.ok());
    tt::Config config;
    config.memory_bytes = 64 * kMiB;
    tt::Context context(compiled.program, config);
    ASSERT_TRUE(context.ok());
    GRCORE_Port * port = nullptr;
    ASSERT_EQ(grcore_context_port(context.context, &port), GRCORE_OK);
    std::thread timer([port]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      grcore_port_post(port, GRCORE_REQUEST_TIME);
    });
    alarm(60);
    auto started = std::chrono::steady_clock::now();
    GRCORE_Result r = grcore_run(context.context, gltang_execution_entry, context.execution, &context.outcome);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    alarm(0);
    timer.join();
    grcore_port_release(port);
    EXPECT_TRUE(r == GRCORE_ERR_LIMIT || (r == GRCORE_OK && context.outcome == GRCORE_OUTCOME_PAUSED)) << "r=" << r;
    EXPECT_LT(seconds, 10.0) << "the verdict came within the backstop";
    EXPECT_LT(context.tracker.peak_bytes, 128 * kMiB) << "bounded by the memory budget";
    if (r == GRCORE_OK) {
      ASSERT_GE(grcore_context_pause_key_count(context.context), 1u);
      EXPECT_EQ(grcore_context_pause_key(context.context, 0), grcore_core_key(GRCORE_REQUEST_TIME));
    }
    context.has_run = true;
  }
}

TEST(NativeGateLimits, APageOfTenThousandChildrenIsStoppedByTheRequestBudgetAndNoChildByItsOwn) {
  tt::Compiled page("use child; for (i = 0; i < 10000; i += 1) { child(); } print(\"done\");");
  tt::Compiled child("1;");
  ASSERT_TRUE(page.ok() && child.ok());
  tt::Config config;
  config.fuel = 5000;
  tt::Context context(page.program, config);
  ASSERT_TRUE(context.ok());
  ASSERT_EQ(gltang_library_add_template(context.library(), "child", child.program, 1000000, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  alarm(60);
  EXPECT_FALSE(context.execute());
  ASSERT_TRUE(context.paused()) << "the inclusive request budget pauses the whole run";
  EXPECT_EQ(context.error_count(), 0u) << "no child was stopped by its own scope";
  EXPECT_LT(grcore_context_fuel_used(context.context), 5000u + 1500u);
  ASSERT_EQ(grcore_context_terminate(context.context), GRCORE_OK);
  EXPECT_EQ(grcore_resume(context.context, &context.outcome), GRCORE_ERR_LIMIT);
  alarm(0);
  EXPECT_EQ(context.error_count(), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(context.context), 0u);
}

TEST(NativeGateLimits, ANativeInsideAScopeIsStoppedByTheScopeAndThePageGoesOn) {
  tt::Compiled page("use nav; keep = [1, 2, 3]; s = nav(); print(keep.size); print(\"x\");");
  tt::Compiled nav("s = \"x\"; while (true) { s = s + s; }");
  ASSERT_TRUE(page.ok() && nav.ok());
  tt::Config config;
  config.memory_bytes = 8 * kMiB;
  tt::Context context(page.program, config);
  ASSERT_TRUE(context.ok());
  ASSERT_EQ(gltang_library_add_template(context.library(), "nav", nav.program, 800, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  alarm(60);
  EXPECT_TRUE(context.execute());
  alarm(0);
  EXPECT_EQ(context.raw(), "3x");
  EXPECT_EQ(context.error_count(), 1u);
  EXPECT_LT(context.tracker.peak_bytes, 4 * kMiB) << "the scope's fuel, not the memory budget, ended the copying";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
