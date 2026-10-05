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

#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>

#ifdef _WIN32
#include <atomic>

// alarm(2) does not exist on Windows. A watchdog thread stands in for it: if the
// alarm is neither cancelled (alarm(0)) nor replaced in time, it ends the
// process with the status SIGALRM's default action would give (128 + 14), so a
// hang is a failure and not a stuck suite there too.
static std::atomic<unsigned> g_alarm_generation{0};
static unsigned alarm(unsigned seconds) {
  unsigned mine = ++g_alarm_generation;
  if (seconds != 0) {
    std::thread([seconds, mine] {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      if (g_alarm_generation.load() == mine) {
        std::fputs("test_native_gate: the alarm fired: a case did not reach a verdict in time\n", stderr);
        std::fflush(stderr);
        std::_Exit(142);
      }
    }).detach();
  }
  return 0;
}
#endif

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
  // Whether the operation must poll under `native`'s name. False for a row
  // whose operation is refused before the native's first poll (the verdict
  // then comes from the refused allocation's own poll).
  bool attributed = true;
};

struct Measured {
  uint64_t fuel = 0;
  size_t peak = 0;
  uint64_t memory_peak = 0;
};

struct Result {
  uint64_t native_polls = 0;  // polls made under the row's native name (0 for a limit row)
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
  if (row.native[0] != '\0') {
    EXPECT_EQ(gltang_execution_native_polls(context.execution, row.native, &r.native_polls), GLTANG_OK)
        << row.native << " is not an id in natives.def";
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
  if (std::getenv("GLTANG_GATE_VERBOSE")) {
    std::printf("  row %s: %s\n", row.native, row.name);
    std::fflush(stdout);
  }
  alarm(tt::heavy_instruments() ? 300 : 60);  // a hang is a failure, not a stuck suite
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
  if (row.native[0] != '\0' && row.attributed) {
    // The operation polled for the native the row names: the poll that ended
    // it (or paced it) is made under that native's name, not under another's.
    // The build runs first and may poll the same native, so it is the
    // difference that counts.
    EXPECT_GT(r.native_polls, base.native_polls) << "the operation made no poll under the name " << row.native;
  }
  // Only a backstop (the alarm above is the hang detector). Under the heavy
  // instruments a row that is allowed 300 seconds took 20.2 of them on the
  // unchanged tree, so the backstop is scaled the way the alarm is.
  EXPECT_LT(r.seconds, tt::heavy_instruments() ? 120.0 : 20.0) << "the wall clock is only a backstop, and this case needed it";
}

/// Under the collector's torture mode every allocation is a collection, under a
/// moving stack every push is a copy, and under Valgrind everything is slow, so
/// the operands there are an order of magnitude smaller. A budget of 300 fuel is 19 KB of copying, and
/// the smaller operands (32 KiB of text, 20,000 elements) are still well past it.
bool instruments_on() {
  return tt::heavy_instruments();
}
const bool kSmall = instruments_on();
const std::string kDoublings = kSmall ? "15" : "20";                       // 32 KiB or 1 MiB of text
const std::string kElements = kSmall ? "20000" : "200000";                // 160 KB or 1.6 MB of elements
const std::string kStr1M = "s = \"x\"; for (i = 0; i < " + kDoublings + "; i += 1) { s = s + s; }";
const std::string kHtml1M = "s = !\"x\"; for (i = 0; i < " + kDoublings + "; i += 1) { s = s + s; }";
const std::string kArr200k = "a = [0] * " + kElements + ";";

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
  // Building a list or a map in an endless loop. The memory budget refuses the growth that would pass it
  // (after a collection) and the loop, which survives a refusal, goes on until its fuel is spent, so the
  // budget is small and the fuel moderate: each refused iteration collects.
  t.push_back({"", "memory: building a list in an endless loop", "a = []; i = 0;", "while (true) { a[i] = i; i += 1; }", 1000000, 256 * 1024, kStopsOrRefuses,
      2 * kMiB, true});
  t.push_back({"", "memory: building a map in an endless loop", "m = {:}; i = 0;", "while (true) { m[\"key\" + i] = i; i += 1; }", 1000000, 256 * 1024, kStopsOrRefuses,
      2 * kMiB, true});
  // Guest stack depth: recursion past the budget is an error value, not a crash.
  t.push_back({"", "guest depth: recursion past the budget", "function f(n) { return f(n + 1); }", "x = f(0); x;", 100000, kUnlimited, FINISHED_ERROR,
      4 * kMiB, false, GLTANG_ERROR_RECURSION_LIMIT});
  // Guest depth with no budget at all: the fuel ends it, with the frames on the guest stack and the C stack untouched.
  Row unlimited_depth = {"", "guest depth: unbounded recursion, ended by fuel", "function f(n) { return f(n + 1); }", "f(0);", 200000, kUnlimited, PAUSED,
      64 * kMiB};
  unlimited_depth.calls = kUnlimited;
  t.push_back(unlimited_depth);
  // Native stack depth: a container nested past the value-depth bound is an error value in copy, equality and print.
  // Building the nest copies it at every level (2,200 levels, millions of allocations), which under the collector's
  // torture mode is a collection each: that row runs in the plain build only (it is in the oracle differential
  // and in the plain suite; nothing about the collector is shown by it that the other rows do not show).
  if (!kSmall) {
    t.push_back({"", "native depth: containers nested past the bound", "a = []; for (i = 0; i < 2200; i += 1) { a = [a]; }", "b = [a]; c = a == a; d = a + a; print(a); print(b.size); c;", 20000000,
        kUnlimited, FINISHED_ERROR | FINISHED_OK, 128 * kMiB});
  }

  // Guest-driven natives, one or more rows each. The golden row first.
  t.push_back({"STRING_CONCAT", "string doubling in an endless loop", "s = \"x\";", "while (true) { s = s + s; }", 1000, 32 * kMiB, kStops, 2 * kMiB});
  t.push_back({"STRING_CONCAT", "concatenating two huge strings", kStr1M, "t = s + s;", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"STRING_SUBSTRING", "slicing the tail of a huge string", kStr1M, "t = s[1:];", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"STRING_SLICE", "reversing a huge string", kStr1M, "t = s[::-1];", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"STRING_SLICE", "taking every other grapheme of a huge string", kStr1M, "t = s[::2];", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"STRING_RETAG", "giving a huge string another encoding", kStr1M, "t = s.html;", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"STRING_RENDER", "encoding a huge string", kHtml1M, "t = s.render;", 300, 16 * kMiB, kStops, 12 * kMiB});
  Row from_host = {"STRING_FROM_UTF8", "reading a huge string a host gave", "x = 1;", "use big; t = big;", 300, 16 * kMiB, kStops, 12 * kMiB};
  from_host.host_string = kSmall ? kMiB / 32 : kMiB;
  // The scan and the copy of a host string are not paced from inside; the whole
  // charge (1 MiB / 64 = 16,384 fuel) is made at once, before any of the work,
  // and the verdict follows it. That charge is this native's slack.
  from_host.fuel_slack = from_host.host_string / 64 + 1500;
  t.push_back(from_host);
  t.push_back({"PRINT", "printing a huge string", kStr1M, "print(s);", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"PRINT", "printing a large container", kArr200k, "print(a);", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"RENDER_TO_STRING", "rendering a large container with as string", kArr200k, "t = a as string;", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"RENDER_TO_STRING", "concatenating a string with a large container", kArr200k, "t = \"x\" + a;", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"ARRAY_GROW", "growing a large array by one past its end", kArr200k, "a[" + kElements + "] = 1;", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"DEEP_COPY", "copying a large container into another", kArr200k, "b = [a];", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"EQUALITY", "comparing two huge equal arrays", "a = [0] * " + kElements + "; b = [0] * " + kElements + ";", "a == b;", 300, 16 * kMiB, kStops, 4 * kMiB});
  t.push_back({"ARRAY_CONCAT", "joining two large arrays", kArr200k, "c = a + a;", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"ARRAY_REPEAT", "repeating an array into 40 MB", "a = [0] * 1000;", "c = a * 5000;", 300, 64 * kMiB, kStops, 48 * kMiB});
  Row refused_repeat = {"ARRAY_REPEAT", "repeating an array into 8 GB under a small memory budget", "a = [0] * 1000;", "c = a * 1000000; c;", 300, 4 * kMiB, FINISHED_ERROR,
      8 * kMiB, true, GLTANG_ERROR_OUT_OF_MEMORY};
  refused_repeat.attributed = false;  // the allocation is refused before the first paced poll
  t.push_back(refused_repeat);
  t.push_back({"ARRAY_SLICE", "reversing a large array", kArr200k, "c = a[::-1];", 300, 16 * kMiB, kStops, 12 * kMiB});
  t.push_back({"ARRAY_CONCAT", "the endless loop that grows an array by joining it to itself", "a = [0] * 100;", "while (true) { a = a + a; }", 1000, 32 * kMiB, kStops, 2 * kMiB});
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
  const std::string head = "GLTANG_NATIVE(";
  while (std::getline(in, line)) {
    // GLTANG_NATIVE(NAME, 1, "text")
    if (line.compare(0, head.size(), head) != 0) {
      continue;
    }
    size_t comma = line.find(',', head.size());
    if (comma == std::string::npos || comma + 3 >= line.size()) {
      continue;
    }
    char flag = line[comma + 2];
    if ((flag != '0' && flag != '1') || line[comma + 3] != ',') {
      continue;
    }
    list.push_back({line.substr(head.size(), comma - head.size()), flag == '1'});
  }
  return list;
}

/// The names that follow `macro(first-argument, ` in a source text.
std::vector<std::string> named_after(const std::string & text, const std::string & macro) {
  std::vector<std::string> names;
  for (size_t at = text.find(macro); at != std::string::npos; at = text.find(macro, at + 1)) {
    size_t comma = text.find(',', at);
    size_t close = text.find(')', at);
    if (comma == std::string::npos || (macro == "GLTANG_PACER(" && close < comma)) {
      continue;
    }
    size_t i = comma + 1;
    while (i < text.size() && text[i] == ' ') {
      ++i;
    }
    size_t j = i;
    while (j < text.size() && (std::isalnum((unsigned char)text[j]) || text[j] == '_')) {
      ++j;
    }
    if (j > i) {
      names.push_back(text.substr(i, j - i));
    }
  }
  return names;
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
  const char * files[] = {"container.c", "errorlist.c", "execution.c", "interp.c", "libvalue.c", "ops.c", "string.c", "template.c", "text.c", "value.c"};
  size_t polls = 0;
  for (const char * f : files) {
    std::string text = slurp(src_dir() + "/" + f);
    ASSERT_FALSE(text.empty()) << f;
    for (const std::string & id : named_after(text, "GLTANG_NATIVE_POLL(")) {
      named.insert(id);
      ++polls;
    }
    for (const std::string & id : named_after(text, "GLTANG_PACER(")) {
      named.insert(id);
      ++polls;
    }
    // A poll that names nobody: the raw call is only the definition (execution.c)
    // and the forwarding inside a pacer.
    std::istringstream lines(text);
    std::string line;
    int number = 0;
    int raw_in_execution = 0;
    while (std::getline(lines, line)) {
      ++number;
      bool raw = line.find("gltang_vm_native_poll(") != std::string::npos;
      bool forwarded = line.find("gltang_vm_native_poll_as(") != std::string::npos;
      raw_in_execution += raw && std::string(f) == "execution.c";
      if (raw && std::string(f) != "execution.c") {
        ADD_FAILURE() << f << ":" << number << ": a poll that names no native: " << line;
      }
      if (forwarded && line.find("pacer->native") == std::string::npos && std::string(f) != "execution.c") {
        ADD_FAILURE() << f << ":" << number << ": a poll that names no native: " << line;
      }
    }
    if (std::string(f) == "execution.c") {
      // The definition of the poll and the one call inside gltang_vm_native_poll_as.
      EXPECT_EQ(raw_in_execution, 2) << "execution.c may hold the raw poll only in its definition and in the naming wrapper";
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
  EXPECT_EQ(t.size(), kSmall ? 26u : 27u);
  for (const Row & row : t) {
    run_row(row);
  }
}

// ---------------------------------------------------------------------------
// The limits that need the host: the wall clock, and a page of children
// ---------------------------------------------------------------------------

namespace {

/// Posts the time request from inside the run, at the Nth poll: the host timer
/// without the scheduler. The request kind it defines stays pending, so that
/// every poll takes the slow path and this handler runs at each of them.
struct Poster {
  GRCORE_Port * port = nullptr;
  uint64_t polls = 0;
  uint64_t at = 0;
  bool posted = false;
};

void poster_handler(GRCORE_Context *, void * value, GRCORE_PollCall *) {
  Poster * p = static_cast<Poster *>(value);
  if (!p->posted && ++p->polls >= p->at) {
    p->posted = true;
    grcore_port_post(p->port, GRCORE_REQUEST_TIME);
  }
}

const GRCORE_Key kPosterKey = GRCORE_KEY_INIT("test timer", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_OBSERVE, nullptr, poster_handler, nullptr, nullptr, nullptr);

struct WallCase {
  const char * name;
  std::string source;
};

std::vector<WallCase> wall_cases() {
  return {
      {"an endless loop of doublings", "s = \"x\"; while (true) { s = s + s; }"},
      {"a long chain of array copies", "a = [0] * 40000; while (true) { b = a + a + a + a; }"},
  };
}

/// Runs a case until the request is posted by `post`, and checks the verdict.
void check_wall_clock_verdict(const WallCase & c, tt::Context & context, GRCORE_Result r, double seconds) {
  EXPECT_TRUE(r == GRCORE_ERR_LIMIT || (r == GRCORE_OK && context.outcome == GRCORE_OUTCOME_PAUSED)) << c.name << ": r=" << r;
  EXPECT_LT(seconds, 20.0) << c.name << ": the verdict came within the backstop";
  EXPECT_LT(context.tracker.peak_bytes, 128 * kMiB) << c.name << ": bounded by the memory budget";
  if (r == GRCORE_OK) {
    ASSERT_GE(grcore_context_pause_key_count(context.context), 1u);
    EXPECT_EQ(grcore_context_pause_key(context.context, 0), grcore_core_key(GRCORE_REQUEST_TIME)) << c.name;
  }
}

}  // namespace

TEST(NativeGateLimits, WallClockTheRequestPostedFromInsideTheRunStopsALongNative) {
  // The time request is posted at the 200th poll, which in the chain of array
  // copies is inside a native. A native cannot pause (AD-21), so the verdict
  // there is an unwind; the endless loop, which polls at every back-edge,
  // pauses. Deterministic: no thread and no sleep.
  for (const WallCase & c : wall_cases()) {
    SCOPED_TRACE(c.name);
    tt::Compiled compiled(c.source);
    ASSERT_TRUE(compiled.ok());
    tt::Config config;
    config.memory_bytes = 64 * kMiB;
    tt::Context context(compiled.program, config);
    ASSERT_TRUE(context.ok());
    Poster poster;
    poster.at = 200;
    ASSERT_EQ(grcore_context_port(context.context, &poster.port), GRCORE_OK);
    GRCORE_RequestKind kind;
    ASSERT_EQ(grcore_context_request_kind(context.context, &kPosterKey, &kind), GRCORE_OK);
    ASSERT_EQ(grcore_context_register(context.context, &kPosterKey, &poster), GRCORE_OK);
    ASSERT_EQ(grcore_port_post(poster.port, kind), GRCORE_OK);
    alarm(60);
    auto started = std::chrono::steady_clock::now();
    GRCORE_Result r = grcore_run(context.context, gltang_execution_entry, context.execution, &context.outcome);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    alarm(0);
    context.has_run = true;
    EXPECT_TRUE(poster.posted);
    grcore_port_release(poster.port);
    check_wall_clock_verdict(c, context, r, seconds);
  }
}

TEST(NativeGateLimits, WallClockTheHostTimerThreadPostsTheRequestAndALongNativeStops) {
  // The same with a real timer thread. It needs the scheduler to run a sleeping
  // thread while another spins, which Valgrind does not do, so it is skipped
  // there; the deterministic test above runs everywhere.
  if (RUNNING_ON_VALGRIND) {
    GTEST_SKIP() << "Valgrind serialises threads and does not wake a sleeping one while another spins";
  }
  for (const WallCase & c : wall_cases()) {
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
    context.has_run = true;
    check_wall_clock_verdict(c, context, r, seconds);
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
