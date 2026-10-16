// The handle API under pressure (handle.h): the `@may_gc` / `@no_gc` labels
// checked against a run that collects at every allocation, and the ways a
// callback fails (the context's memory, the fuel, an allocation refused at each
// point) leaving an error value, a run that goes on, and nothing leaked.

#include "description_harness.h"

#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using tt::Compiled;
using tt::Config;
using tt::Context;

namespace {

using Hook = std::function<bool(GLTANG_NativeCall *)>;

bool run_hook(GLTANG_NativeCall * call, void * user) {
  return (*static_cast<Hook *>(user))(call);
}

// ---------------------------------------------------------------------------
// The labels
// ---------------------------------------------------------------------------

struct Labelled {
  std::string name;
  bool no_gc;
};

std::string slurp(const std::string & path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/// Every `GLTANG_API` function of handle.h and the label of its doc block.
std::vector<Labelled> labelled_functions() {
  std::string text = slurp(std::string(GLTANG_TEST_DATA) + "/../include/ghoti.io/lang-tang/handle.h");
  std::vector<Labelled> found;
  size_t at = 0;
  while ((at = text.find("GLTANG_API ", at)) != std::string::npos) {
    size_t open = text.find('(', at);
    size_t name_end = open;
    size_t name_begin = text.rfind(' ', name_end - 1);
    std::string name = text.substr(name_begin + 1, name_end - name_begin - 1);
    while (!name.empty() && name[0] == '*') {
      name.erase(0, 1);
    }
    // The doc block is the last comment before the declaration.
    size_t close = text.rfind("*/", at);
    size_t start = text.rfind("/**", close);
    std::string doc = text.substr(start, close - start);
    bool may = doc.find("@may_gc") != std::string::npos;
    bool none = doc.find("@no_gc") != std::string::npos;
    EXPECT_NE(may, none) << name << " must carry exactly one of @may_gc and @no_gc";
    found.push_back({name, none});
    at = open;
  }
  return found;
}

}  // namespace

TEST(HandleLabels, EveryFunctionCarriesExactlyOneLabel) {
  std::vector<Labelled> fns = labelled_functions();
  EXPECT_GE(fns.size(), 34u) << "the header was not read";
  std::set<std::string> names;
  for (const Labelled & f : fns) {
    EXPECT_TRUE(names.insert(f.name).second) << f.name << " declared twice";
  }
}

TEST(HandleLabels, ANoGcFunctionNeverCollectsUnderTortureAndAMayGcOneThatAllocatesDoes) {
  std::vector<Labelled> fns = labelled_functions();
  // What each function is called with: a string, an array and a map the program
  // gave, and handles to them made before anything is measured.
  Config config;
  config.torture = 1;
  config.verify = 1;
  Compiled compiled("use probe; probe(\"abc\", [1, 2], {k: 1}); 1;");
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  struct Case {
    bool allocates;   // a `@may_gc` function that must show a collection
    std::function<void(GLTANG_NativeCall *, GLTANG_Handle s, GLTANG_Handle a, GLTANG_Handle m)> body;
  };
  GLTANG_Persistent persistent = {0, 0};
  GLTANG_Library * library = nullptr;
  ASSERT_EQ(gltang_library_create("lib", &library), GLTANG_OK);
  std::map<std::string, Case> table;
#define CASE(fn, allocates, ...) table[#fn] = {allocates, [&](GLTANG_NativeCall * call, GLTANG_Handle s, GLTANG_Handle a, GLTANG_Handle m) { (void)s; (void)a; (void)m; __VA_ARGS__; }}
  GLTANG_Handle out;
  size_t length;
  bool found;
  char buffer[16];
  GLTANG_StringSegment segment;
  segment.size = sizeof(segment);
  CASE(gltang_call_arg, false, (void)gltang_call_arg(call, 1, &out));
  CASE(gltang_call_bound, false, (void)gltang_call_bound(call, &out));
  CASE(gltang_call_host_context, false, (void)gltang_call_host_context(call));
  CASE(gltang_call_return_handle, false, gltang_call_return_handle(call, s));
  CASE(gltang_handle_push_frame, false, (void)gltang_handle_push_frame(call));
  CASE(gltang_handle_pop_frame, false, (void)gltang_handle_pop_frame(call));
  CASE(gltang_handle_valid, false, (void)gltang_handle_valid(call, s));
  CASE(gltang_handle_kind, false, (void)gltang_handle_kind(call, a));
  CASE(gltang_handle_bool, false, (void)gltang_handle_bool(call, s));
  CASE(gltang_handle_integer, false, (void)gltang_handle_integer(call, s));
  CASE(gltang_handle_float, false, (void)gltang_handle_float(call, s));
  CASE(gltang_handle_text, false, (void)gltang_handle_text(call, s, &length));
  CASE(gltang_handle_copy_text, false, (void)gltang_handle_copy_text(call, s, buffer, sizeof buffer));
  CASE(gltang_handle_string_graphemes, false, (void)gltang_handle_string_graphemes(call, s));
  CASE(gltang_handle_string_segments, false, (void)gltang_handle_string_segments(call, s));
  CASE(gltang_handle_string_segment, false, (void)gltang_handle_string_segment(call, s, 0, &segment));
  CASE(gltang_handle_array_length, false, (void)gltang_handle_array_length(call, a));
  CASE(gltang_handle_array_get, false, (void)gltang_handle_array_get(call, a, 0, &out));
  CASE(gltang_handle_map_count, false, (void)gltang_handle_map_count(call, m));
  CASE(gltang_handle_map_get, false, (void)gltang_handle_map_get(call, m, "k", 1, &found, &out));
  CASE(gltang_handle_map_key_at, false, (void)gltang_handle_map_key_at(call, m, 0, &out));
  CASE(gltang_handle_map_value_at, false, (void)gltang_handle_map_value_at(call, m, 0, &out));
  CASE(gltang_handle_make_null, false, (void)gltang_handle_make_null(call, &out));
  CASE(gltang_handle_make_bool, false, (void)gltang_handle_make_bool(call, true, &out));
  CASE(gltang_handle_make_integer, true, (void)gltang_handle_make_integer(call, int64_t{1} << 61, &out));
  CASE(gltang_handle_make_float, true, (void)gltang_handle_make_float(call, 1.5, &out));
  CASE(gltang_handle_make_string, true, (void)gltang_handle_make_string(call, "xyz", 3, GLTANG_UNICODE_STRING_TYPE_HTML, &out));
  CASE(gltang_handle_make_concat, true, (void)gltang_handle_make_concat(call, s, s, &out));
  CASE(gltang_handle_make_array, true, (void)gltang_handle_make_array(call, 4, &out));
  CASE(gltang_handle_array_push, true, (void)gltang_handle_array_push(call, a, s));
  CASE(gltang_handle_make_map, true, (void)gltang_handle_make_map(call, 4, &out));
  CASE(gltang_handle_map_set, true, (void)gltang_handle_map_set(call, m, "new", 3, GLTANG_UNICODE_STRING_TYPE_TRUSTED, s));
  CASE(gltang_handle_make_library, true, (void)gltang_handle_make_library(call, library, &out));
  CASE(gltang_handle_make_native, true, (void)gltang_handle_make_native(call, run_hook, nullptr, a, &out));
  CASE(gltang_handle_promote, false, (void)gltang_handle_promote(call, s, &persistent));
  CASE(gltang_persistent_get, false, (void)gltang_persistent_get(call, persistent, &out));
  CASE(gltang_persistent_release, false, (void)gltang_persistent_release(call, persistent));
#undef CASE

  std::set<std::string> measured;
  size_t increased = 0;
  bool ran = false;
  Hook hook = [&](GLTANG_NativeCall * call) {
    ran = true;
    GLTANG_Handle s, a, m;
    EXPECT_EQ(gltang_call_arg(call, 0, &s), GLTANG_OK);
    EXPECT_EQ(gltang_call_arg(call, 1, &a), GLTANG_OK);
    EXPECT_EQ(gltang_call_arg(call, 2, &m), GLTANG_OK);
    // gltang_persistent_get and _release need a persistent to exist; the promote case makes it.
    EXPECT_EQ(gltang_handle_promote(call, s, &persistent), GLTANG_OK);
    for (const Labelled & f : fns) {
      auto it = table.find(f.name);
      EXPECT_NE(it, table.end()) << f.name << " has no row in the gate's table, so its label was never checked";
      if (it == table.end()) {
        continue;
      }
      measured.insert(f.name);
      GRHEAP_Stats before, after;
      EXPECT_EQ(grheap_stats(context.heap, &before), GRHEAP_OK);
      it->second.body(call, s, a, m);
      EXPECT_EQ(grheap_stats(context.heap, &after), GRHEAP_OK);
      if (f.no_gc) {
        EXPECT_EQ(after.collections, before.collections) << f.name << " is labelled @no_gc and collected";
        EXPECT_FALSE(it->second.allocates) << f.name << " is in the table as allocating but labelled @no_gc";
      }
      else if (it->second.allocates) {
        EXPECT_GT(after.collections, before.collections) << f.name << " is labelled @may_gc and allocates, but the torture run saw no collection (the gate would measure nothing)";
        increased += after.collections > before.collections;
      }
      // A table row restores what the next one expects.
      if (f.name == "gltang_persistent_release") {
        EXPECT_EQ(gltang_handle_promote(call, s, &persistent), GLTANG_OK);
      }
    }
    gltang_call_return_null(call);
    return true;
  };
  ASSERT_EQ(gltang_library_add_native(context.library(), "probe", run_hook, &hook), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  gltang_library_release(library);
  EXPECT_TRUE(ran);
  EXPECT_EQ(measured.size(), fns.size());
  EXPECT_GE(increased, 10u) << "the torture run must have collected for the allocating constructors";
}

// ---------------------------------------------------------------------------
// Failure
// ---------------------------------------------------------------------------

namespace {

/// Builds an array of `n` rows of maps with tagged strings, answers it.
bool build_rows(GLTANG_NativeCall * call, void * user) {
  const int n = *static_cast<int *>(user);
  GLTANG_Handle rows;
  if (gltang_handle_make_array(call, 0, &rows) != GLTANG_OK) {
    return true;
  }
  for (int i = 0; i < n; ++i) {
    // The pieces of one row end with it: a frame for each row keeps the handles few.
    GLTANG_Handle row, name, num;
    std::string text = "row <" + std::to_string(i) + ">";
    if (gltang_handle_push_frame(call) != GLTANG_OK || gltang_handle_make_map(call, 0, &row) != GLTANG_OK || gltang_handle_make_string(call, text.data(), text.size(), GLTANG_UNICODE_STRING_TYPE_HTML, &name) != GLTANG_OK ||
        gltang_handle_make_integer(call, (int64_t{1} << 60) + i, &num) != GLTANG_OK || gltang_handle_map_set(call, row, "name", 4, GLTANG_UNICODE_STRING_TYPE_TRUSTED, name) != GLTANG_OK ||
        gltang_handle_map_set(call, row, "num", 3, GLTANG_UNICODE_STRING_TYPE_TRUSTED, num) != GLTANG_OK || gltang_handle_array_push(call, rows, row) != GLTANG_OK ||
        gltang_handle_pop_frame(call) != GLTANG_OK) {
      return true;   // the callback's value is Out of memory (or the run is unwinding)
    }
  }
  gltang_call_return_handle(call, rows);
  return true;
}

struct Result {
  bool finished = false;
  GRCORE_Result ran = GRCORE_OK;
  std::string raw;
  bool fired = false;
};

Result run(const std::string & source, int rows, const Config & config, size_t scope_limit = 0) {
  Result r;
  Compiled compiled(source);
  EXPECT_TRUE(compiled.ok());
  Context context(compiled.program, config);
  if (!context.ok()) {
    EXPECT_NE(config.fail_at, 0);
    r.fired = context.tracker.fired;
    return r;
  }
  if (scope_limit) {
    EXPECT_EQ(gltang_execution_set_handle_limits(context.execution, scope_limit, 0), GLTANG_OK);
  }
  int n = rows;
  EXPECT_EQ(gltang_library_add_native(context.library(), "rows", build_rows, &n), GLTANG_OK);
  r.finished = context.execute();
  r.ran = context.ran;
  r.raw = context.raw();
  (void)context.describe();
  (void)context.rendered();
  r.fired = context.tracker.fired;
  return r;
}

}  // namespace

TEST(HandleFailure, AnAllocationRefusedAtEachPointOfABuildIsAnErrorValueAndLeaksNothing) {
  const std::string source = "use rows; r = rows(); print(r[0][\"name\"]); print(\"|\"); print(r[1][\"num\"]);";
  // Under the instruments that make every allocation slow (torture collects at each one) the build is smaller:
  // the sweep runs a whole program for each allocation it fails.
  const int rows = tt::heavy_instruments() ? 2 : 8;
  Result baseline = run(source, rows, Config());
  ASSERT_TRUE(baseline.finished);
  ASSERT_EQ(baseline.raw, "row <0>|1152921504606846977");
  bool reached = false;
  int degraded = 0;
  for (long n = 1; n <= 20000; ++n) {
    Config config;
    config.fail_at = n;
    Result r = run(source, rows, config);
    if (!r.fired) {
      EXPECT_EQ(r.raw, baseline.raw) << "n=" << n;
      reached = true;
      break;
    }
    degraded += r.raw != baseline.raw || !r.finished;
  }
  EXPECT_TRUE(reached) << "the sweep never reached a run with no failed allocation";
  EXPECT_GT(degraded, tt::heavy_instruments() ? 3 : 10) << "failing allocations barely changed an answer, so the sweep reached little";
}

/// A build is a tenth the size where every allocation is slow.
static int scaled(int rows) {
  return tt::heavy_instruments() ? rows / 10 : rows;
}

TEST(HandleFailure, ABuildBiggerThanTheMemoryBudgetIsOutOfMemoryAndTheRunGoesOn) {
  Config config;
  config.memory_bytes = 256 * 1024;
  // The memory budget's verdict on a build it cannot hold is either the callback's
  // out-of-memory value with the run going on, or the run's end (the native gate's
  // "stops or refuses"); a crash, a leak or a half-built answer is neither.
  Result r = run("use rows; r = rows(); x = 5; x;", scaled(5000), config);
  EXPECT_TRUE(r.finished || r.ran == GRCORE_ERR_LIMIT);
  Compiled compiled("use rows; rows();");
  Context context(compiled.program, config);
  int n = scaled(5000);
  ASSERT_EQ(gltang_library_add_native(context.library(), "rows", build_rows, &n), GLTANG_OK);
  const bool finished = context.execute();
  if (finished) {
    if (context.is_error()) {
      EXPECT_EQ(context.error_kind(), GLTANG_ERROR_OUT_OF_MEMORY);
    }
    else {
      EXPECT_TRUE(tt::heavy_instruments()) << "5,000 rows do not fit 256 KiB, so only the smaller build of the slow arms can succeed";
    }
  }
  else {
    EXPECT_EQ(context.ran, GRCORE_ERR_LIMIT);
  }
}

TEST(HandleFailure, ABuildThatRunsTheFuelOutUnwindsTheRun) {
  Result base = run("use rows; rows();", 5, Config());
  ASSERT_TRUE(base.finished);
  Config unlimited;
  Compiled compiled("use rows; rows();");
  Context probe(compiled.program, unlimited);
  int n = scaled(20000);
  ASSERT_EQ(gltang_library_add_native(probe.library(), "rows", build_rows, &n), GLTANG_OK);
  ASSERT_TRUE(probe.execute());
  const uint64_t used = grcore_context_fuel_used(probe.context);
  ASSERT_GT(used, 200u) << "building the rows is charged";
  Config small;
  small.fuel = used / 10;
  Result r = run("use rows; rows();", scaled(20000), small);
  EXPECT_FALSE(r.finished);
  EXPECT_EQ(r.ran, GRCORE_ERR_LIMIT) << "a constructor's poll ordered the run to stop";
}

TEST(HandleFailure, AScopeLimitFarBelowTheBuildIsTheCallbacksOutOfMemoryAtEveryPoint) {
  for (size_t limit : {1u, 2u, 3u, 5u, 8u}) {
    SCOPED_TRACE(limit);
    Compiled compiled("use rows; x = rows(); y = 4; y;");
    Context context(compiled.program);
    ASSERT_EQ(gltang_execution_set_handle_limits(context.execution, limit, 0), GLTANG_OK);
    int n = 10;
    ASSERT_EQ(gltang_library_add_native(context.library(), "rows", build_rows, &n), GLTANG_OK);
    ASSERT_TRUE(context.execute());
    EXPECT_EQ(context.integer(), 4);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
