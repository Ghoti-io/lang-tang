/**
 * @file
 *
 * The frame-differential observer (tests/observer.h) over lang-tang: one program
 * is run plain, under GC torture with barrier-verify, on a stack that moves at
 * every push, and with the poll phases shuffled (AD-5), and the four traces of
 * the abstract frames are compared poll by poll. They must be equal, and so
 * must the output, the result and the error list.
 *
 * The instrument is also shown to fail: a planted slot mismatch, a missing poll
 * and a different depth are each reported with the poll index and the frame; an
 * order-dependent DECIDE handler is caught by the shuffled run and an
 * order-independent one is not.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "exec_harness.h"
#include "fuzz/gen.h"
#include "observer.h"
#include "test_helpers.h"

#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>

namespace {

struct Part {
  std::string name;
  std::string source;
  uint64_t fuel = 100000;
  GLTANG_ScopePolicy policy = GLTANG_SCOPE_EMPTY;
  tt::Mode mode = tt::Mode::Script;
};

/// A program, the templates it can call, and how much fuel each leg of the run
/// gets (so that the run pauses and is resumed many times).
struct Case {
  std::string name;
  std::string source;
  tt::Mode mode = tt::Mode::Script;
  std::vector<Part> parts;
  uint64_t step = 400;
  size_t limit = 20000;  ///< The most polls recorded; the count of polls is compared beyond it.
};

struct RunConfig {
  std::string label;
  int torture = 0;
  int verify = 0;
  int moving = 0;
  bool shuffle = false;
  uint64_t seed = 0;
  bool statement_polls = false;  ///< The execution polls at every statement (the debugger's setting).
  long jit = 0;                  ///< The baseline JIT's threshold: 0 is the interpreter alone, 1 compiles every function at its first poll.
};

const RunConfig kPlain = {"plain", 0, 0, 0, false, 0};
const RunConfig kTorture = {"torture+verify", 1, 1, 0, false, 0};
const RunConfig kMoving = {"moving stack", 0, 0, 1, false, 0};
const RunConfig kShuffled = {"phase-shuffled", 0, 0, 0, true, 0x5eed};
// The same four with statement polls on (the setting a debugger needs).
const RunConfig kLinesPlain = {"statement polls, plain", 0, 0, 0, false, 0, true};
const RunConfig kLinesTorture = {"statement polls, torture+verify", 1, 1, 0, false, 0, true};
const RunConfig kLinesMoving = {"statement polls, moving stack", 0, 0, 1, false, 0, true};
const RunConfig kLinesShuffled = {"statement polls, phase-shuffled", 0, 0, 0, true, 0x5eed, true};
#ifdef GLTANG_WITH_JIT  // only the JIT arm's tests read these; clang rejects an unused one
// The same program with every function tiering up at its first poll (story
// 15). The reference for each is the interpreter's trace, and the instruments
// are the ones above: a JIT frame is an interpreter frame, written at the poll,
// so the traces are equal poll for poll.
const RunConfig kJit = {"jit threshold 1", 0, 0, 0, false, 0, false, 1};
const RunConfig kJitTorture = {"jit, torture+verify", 1, 1, 0, false, 0, false, 1};
const RunConfig kJitMoving = {"jit, moving stack", 0, 0, 1, false, 0, false, 1};
// Tier-up is an ACT handler, and the phase shuffle reorders what it must commute with.
const RunConfig kJitShuffled = {"jit, phase-shuffled", 0, 0, 0, true, 0x5eed, false, 1};
const RunConfig kLinesJit = {"statement polls, jit", 0, 0, 0, false, 0, true, 1};
const RunConfig kLinesJitMoving = {"statement polls, jit, moving stack", 0, 0, 1, false, 0, true, 1};
#endif

struct Observed {
  observer::Trace trace;
  std::string raw, rendered, result_kind, result_text, errors;
  size_t pauses = 0;
  GLTANG_JitStats jit = {};
  bool finished = false;
  GRCORE_Result ran = GRCORE_OK;
  std::string describe() const { return raw + "|" + rendered + "|" + result_kind + ":" + result_text + "|" + errors; }
};

std::string canonical_result_text(tt::Context & context) {
  return context.describe();
}

/// Runs a case under a configuration, observing every poll. `extra` may register
/// more handlers on the context before the run starts.
Observed observe(const Case & c, const RunConfig & rc, const std::function<void(GRCORE_Context *)> & extra = nullptr, size_t cap = SIZE_MAX) {
  Observed out;
  tt::Compiled page(c.source, c.mode, "page.tang");
  EXPECT_TRUE(page.ok()) << c.name << ": " << page.error.message;
  if (!page.ok()) {
    return out;
  }
  tt::Config config;
  config.fuel = c.step;
  config.torture = rc.torture;
  config.verify = rc.verify;
  config.moving_stack = rc.moving;
  config.jit_threshold = rc.jit;
  tt::Context context(page.program, config);
  EXPECT_TRUE(context.ok());
  if (!context.ok()) {
    return out;
  }
  EXPECT_EQ(gltang_execution_set_name(context.execution, "page"), GLTANG_OK);
  if (rc.statement_polls) {
    EXPECT_EQ(gltang_execution_set_statement_polls(context.execution, true), GLTANG_OK);
  }
  std::vector<std::unique_ptr<tt::Compiled>> compiled;
  for (const Part & part : c.parts) {
    compiled.push_back(std::make_unique<tt::Compiled>(part.source, part.mode, (part.name + ".tang").c_str()));
    EXPECT_TRUE(compiled.back()->ok()) << part.name << ": " << compiled.back()->error.message;
    EXPECT_EQ(gltang_library_add_template(context.library(), part.name.c_str(), compiled.back()->program, part.fuel, part.policy), GLTANG_OK);
  }
  context.add_native_library();
  observer::Observer obs;
  obs.trace.limit = std::min(c.limit, cap);
  obs.float_bits = &gltang_vm_test_float_bits;  // floats are compared by their bits as well as their text
  EXPECT_EQ(obs.attach(context.context), GRCORE_OK);
  if (extra) {
    extra(context.context);
  }
  if (rc.shuffle) {
    EXPECT_EQ(grcore_context_set_phase_shuffle(context.context, true, rc.seed), GRCORE_OK);
  }
  bool done = context.execute();
  while (!done && context.paused() && out.pauses < 4000) {
    ++out.pauses;
    // Give the run and its innermost open scope more, and go on.
    uint64_t used = grcore_context_fuel_used(context.context);
    grcore_context_set_fuel(context.context, used + c.step);
    uint64_t depth = grcore_context_fuel_scope_depth(context.context);
    if (depth > 0) {
      // A scope that is out (a development PAUSE policy stops at it) is raised;
      // one that still has fuel is left alone, or no scope would ever end a run.
      uint64_t top = grcore_context_fuel_scope_top(context.context);
      uint64_t remaining = 1, scope_used = 0;
      if (grcore_context_fuel_scope_remaining(context.context, top, &remaining) == GRCORE_OK && remaining == 0 &&
          grcore_context_fuel_scope_used(context.context, top, &scope_used) == GRCORE_OK) {
        grcore_context_fuel_scope_set_budget(context.context, top, scope_used + c.step);
      }
    }
    done = context.resume();
  }
  out.ran = context.ran;
  out.finished = done;
  out.raw = context.raw();
  out.rendered = context.rendered();
  out.result_kind = std::to_string((int)context.kind());
  out.result_text = canonical_result_text(context);
  for (size_t i = 0; i < context.error_count(); ++i) {
    auto e = context.error(i);
    out.errors += e.template_name() + ":" + std::to_string(e.e.line) + "[" + e.chain_text() + "]" + std::to_string((int)e.e.how) + ":" + e.message() + ";";
  }
  out.trace = std::move(obs.trace);
  out.jit = context.jit_stats();
  return out;
}

/// Compares two observations; reports the first divergence with its poll and frame.
::testing::AssertionResult same(const Case & c, const Observed & a, const std::string & la, const Observed & b, const std::string & lb) {
  observer::Divergence d;
  if (observer::first_divergence(a.trace, b.trace, &d)) {
    return ::testing::AssertionFailure() << c.name << ": " << la << " against " << lb << ": " << d.str();
  }
  if (a.describe() != b.describe()) {
    return ::testing::AssertionFailure() << c.name << ": " << la << " against " << lb << ": output, result or error list differ:\n  " << a.describe() << "\n  " << b.describe();
  }
  if (a.pauses != b.pauses) {
    return ::testing::AssertionFailure() << c.name << ": " << la << " paused " << a.pauses << " times, " << lb << " " << b.pauses;
  }
  return ::testing::AssertionSuccess();
}

std::vector<std::string> list_files(const std::string & dir) {
  std::vector<std::string> names;
  if (DIR * d = opendir(dir.c_str())) {
    while (dirent * e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() > 5 && n.compare(n.size() - 5, 5, ".tang") == 0) {
        names.push_back(n);
      }
    }
    closedir(d);
  }
  std::sort(names.begin(), names.end());
  return names;
}

/// The corpus files that are small, finish within a modest budget on lang-tang,
/// and are not refused: the first `limit` of them, in name order.
std::vector<Case> corpus_cases(const std::string & sub, tt::Mode mode, size_t limit) {
  std::vector<Case> cases;
  std::string dir = std::string(GLTANG_TEST_DATA) + "/corpus/" + sub;
  for (const std::string & name : list_files(dir)) {
    if (cases.size() >= limit) {
      break;
    }
    if (name.find("reject") != std::string::npos || name.find("runaway") != std::string::npos || name.find("break-continue") != std::string::npos ||
        name.find("heavy") != std::string::npos || name.find("fib") != std::string::npos || name.find("random") != std::string::npos ||
        name.find("tests-first") != std::string::npos || name.find("nested-deep") != std::string::npos || name.find("deeper") != std::string::npos ||
        name.find("trailing-function") != std::string::npos) {
      continue;
    }
    std::string source = read_file(dir + "/" + name);
    if (source.size() > 500) {
      continue;
    }
    tt::Compiled compiled(source, mode, name.c_str());
    if (!compiled.ok()) {
      continue;
    }
    tt::Config config;
    config.fuel = 20000;
    tt::Context probe(compiled.program, config);
    if (!probe.ok() || !probe.execute()) {
      continue;
    }
    Case c;
    c.name = sub + "/" + name;
    c.source = source;
    c.mode = mode;
    c.step = 20;
    cases.push_back(c);
  }
  return cases;
}

const char * kPage = "use sidebar;\nprint(\"<main>\" + sidebar());";
const char * kSidebar = "use nav; print(nav()); print(\"<aside>\");";

/// The handwritten cases: pauses inside calls, nested template calls, scopes of
/// each policy that are exhausted, natives that poll, errors across a boundary.
std::vector<Case> site_cases() {
  std::vector<Case> cases;
  auto add = [&](const std::string & name, const std::string & source, std::vector<Part> parts, uint64_t step = 50) {
    Case c;
    c.name = "site/" + name;
    c.source = source;
    c.parts = std::move(parts);
    c.step = step;
    cases.push_back(c);
  };
  add("empty policy nav loop", kPage, {{"sidebar", kSidebar, 10000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}, {"nav", "while (true) {}", 500, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  add("segments policy", kPage, {{"sidebar", kSidebar, 10000, GLTANG_SCOPE_EMPTY, tt::Mode::Script},
      {"nav", "print(\"a\"); print(\"b\"); while (true) {}", 300, GLTANG_SCOPE_SEGMENTS, tt::Mode::Script}});
  add("pause policy nav", kPage, {{"sidebar", kSidebar, 10000, GLTANG_SCOPE_EMPTY, tt::Mode::Script},
      {"nav", "n = 0; while (n < 150) { n += 1; } print(\"nav\");", 100, GLTANG_SCOPE_PAUSE, tt::Mode::Script}});
  add("function inside a template", kPage, {{"sidebar", kSidebar, 10000, GLTANG_SCOPE_EMPTY, tt::Mode::Script},
      {"nav", "function spin(n) { k = 0; while (k < n) { k += 1; } return k; }\nprint(spin(120));", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  add("three deep",
      "use a; x = 1; print(a());",
      {{"a", "use b; y = 2; print(b());", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script},
       {"b", "use c; z = 3; print(c());", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script},
       {"c", "w = 0; while (w < 90) { w += 1; } print(w);", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  add("template with text and tags",
      "use row; print(\"<ul>\"); for (i = 0; i < 6; i += 1) { print(row()); } print(\"</ul>\");",
      {{"row", "<li><%= \"a&b\" %> <% j = 0; while (j < 5) { j += 1; } %><%= j %></li>", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Template}});
  add("native string building in a scope",
      "use nav; keep = [1, 2, 3]; s = nav(); print(keep.size); print(\"x\");",
      {{"nav", "s = \"x\"; while (true) { s = s + s; }", 700, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  add("error across the boundary",
      "use t; print(\"[\" + t() + \"]\");",
      {{"t", "print(\"a\"); y = 1 / 0; y;", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  add("recursion with pauses",
      "function f(n) { if (n == 0) { return 0; } return 1 + f(n - 1); }\nprint(f(40)); f(30);", {}, 30);
  add("recursion in a template",
      "use t; print(t());",
      {{"t", "function g(n) { if (n <= 0) { return 1; } return n * g(n - 1); }\nprint(g(12));", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}}, 25);
  add("globals and locals", "x = 1; y = \"s\"; function f(a, b) { global x; x = x + a; z = [a, b]; return z; }\nprint(f(2, \"q\")); print(x); f(5, 6);", {}, 20);
  add("many children under the ceiling",
      "use child; for (i = 0; i < 40; i += 1) { child(); } print(\"done\");",
      {{"child", "t = 1;", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}}, 70);
  return cases;
}

/// Generated programs (tests/fuzz/gen.h): many polls, loops, calls, errors.
std::vector<Case> generated_cases(uint64_t seeds) {
  std::vector<Case> cases;
  for (uint64_t seed = 1; seed <= seeds; ++seed) {
    for (gen::Mode mode : {gen::Mode::Script, gen::Mode::Template}) {
      Case c;
      c.name = "generated/" + std::to_string(seed) + (mode == gen::Mode::Script ? "-script" : "-template");
      c.source = gen::generate(seed, mode).source;
      c.mode = mode == gen::Mode::Script ? tt::Mode::Script : tt::Mode::Template;
      c.step = 150;
      c.limit = 150;  // a poll costs a millisecond to record: the first 150 and the count of the rest
      cases.push_back(c);
    }
  }
  return cases;
}

/// Call-heavy programs for the frame differential of compiled calls: chains of
/// compiled frames at every poll (a pause at the bottom of a chain is a chain
/// rebuilt), references and wide argument lists down a chain, a guard that fails
/// three frames down, a callee that is null, a local function, recursion to the
/// depth budget, and generated call graphs (tests/fuzz/gen.h).
std::vector<Case> call_cases() {
  std::vector<Case> cases;
  auto add = [&](const std::string & name, const std::string & source, std::vector<Part> parts, uint64_t step) {
    Case c;
    c.name = "calls/" + name;
    c.source = source;
    c.parts = std::move(parts);
    c.step = step;
    cases.push_back(c);
  };
  add("fib", "function fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }\nprint(fib(9));", {}, 37);
  // fib(9) makes 110 polls, so the limit changes nothing for a correct engine. It bounds a wrong one: a
  // compiled `n < 2` that never holds recurses without end, and every recorded poll holds the whole stack
  // (planted defect 08 took more than 6 GiB here before this).
  cases.back().limit = 150;
  add("a wide callee",
      "function s6(a, b, c, d, e, f) { return a + b + c + d + e + f; }\n"
      "function mid(x) { return s6(x, 1, 2, 3, 4, 5) + s6(1, x, 2, 3, 4, 5); }\n"
      "function top(n) { t = 0; for (i = 0; i < n; i += 1) { t = t + mid(i); } return t; }\n"
      "print(top(12));", {}, 29);
  add("references down a chain",
      "function pass(a, b) { k = 0; while (k < 3) { k = k + 1; } return b; }\n"
      "function wrap(a, b) { r = pass(b, a); s = pass(a, b); return r; }\n"
      "x = wrap([1], [2]); print(x[0]);", {}, 11);
  add("a guard three frames down",
      "function g(n, x) { if (n == 0) { return x + x; } return g(n - 1, x) + 1; }\n"
      "M = 576460752303423487;\nprint(g(2, M)); print(g(2, 3)); print(g(3, M));", {}, 17);
  add("a callee that is null",
      "flag = false;\nif (flag) { function f(x) { return x + 1; } }\n"
      "function call(i) { return f(i); }\nt = 0; for (k = 0; k < 5; k += 1) { v = call(k); t = t + 1; } print(t);", {}, 19);
  add("local functions",
      "function outer(n) { function half(k) { return k - 1; } function inc(k) { return k + 2; } return half(n) + inc(n) + half(inc(n)); }\n"
      "s = 0; for (i = 0; i < 6; i += 1) { s = s + outer(i); } print(s);", {}, 23);
  add("recursion to the depth budget",
      "function f(n) { return f(n + 1); }\nx = f(0); print(x as string); print(\"after\");", {}, 400);
  add("a chain of recursion in a template",
      "use t; print(t());",
      {{"t", "function down(n) { if (n == 0) { i = 0; while (i < 30) { i = i + 1; } return i; } return down(n - 1) + 1; }\nprint(down(12));", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}}, 41);
  add("a template scope that runs out in a chain",
      "use t; print(\"[\" + t() + \"]\"); print(\"after\");",
      {{"t", "function down(n) { if (n == 0) { i = 0; while (true) { i = i + 1; } } return down(n - 1) + 1; }\nprint(down(6));", 500, GLTANG_SCOPE_EMPTY, tt::Mode::Script}}, 60);
  for (uint64_t seed = 1; seed <= 8; ++seed) {
    Case c;
    c.name = "calls/generated " + std::to_string(seed);
    c.source = gen::generate_calls(seed).source;
    c.step = 150;
    c.limit = 150;
    cases.push_back(c);
  }
  return cases;
}

/// Natives called from compiled code (story 9): library calls and member loads in
/// loops and chains, a native that allocates at the bottom of a chain, one that
/// re-enters guest code (a pause inside it is an unwind), one that asks compiled
/// code to leave, a resumable one, and generated call graphs that use the library.
/// A poll inside a native (its answer is paced) shows the frame of the compiled
/// caller at the native's call site, which must be the interpreter's.
std::vector<Case> native_cases() {
  std::vector<Case> cases;
  auto add = [&](const std::string & name, const std::string & source, uint64_t step) {
    Case c;
    c.name = "natives/" + name;
    c.source = source;
    c.step = step;
    cases.push_back(c);
  };
  add("a loop of library calls",
      "function run(n) { use inc; use sum; s = 0; i = 0; while (i < n) { s = sum(inc(s), i, 1); i = i + 1; } return s; }\n"
      "print(run(30));", 19);
  add("a chain with a native at its bottom",
      "function deep(n, a, b) { use alloc_ref; if (n == 0) { r = alloc_ref(a); k = 0; while (k < 3) { k = k + 1; } return r[0]; } r = deep(n - 1, b, a); return r; }\n"
      "a = [1]; b = [2]; x = deep(12, a, b); print(x[0]);", 13);
  add("a native that re-enters",
      "function leaf(x) { use inc; k = 0; while (k < 4) { k = inc(k); } return x + k; }\n"
      "function run(n) { use reenter; s = 0; i = 0; while (i < n) { s = s + reenter(leaf, i); i = i + 1; } return s; }\n"
      "print(run(8)); print(run(4));", 23);
  add("a native that asks compiled code to leave",
      "function run(n) { use deopt; s = 0; i = 0; while (i < n) { s = s + deopt(i); i = i + 1; } return s; }\n"
      "print(run(9)); print(run(5));", 17);
  add("a resumable native",
      "function g() { k = 0; while (k < 4) { k = k + 1; } return k; }\n"
      "function run() { use resumable; return resumable(g); }\n"
      "print(run()); print(run());", 11);
  for (uint64_t seed = 1; seed <= 6; ++seed) {
    Case c;
    c.name = "natives/generated " + std::to_string(seed);
    c.source = gen::generate_calls(seed, true).source;
    c.step = 150;
    c.limit = 150;
    cases.push_back(c);
  }
  return cases;
}

/// Float programs (spec-runtime-float, CAP-5): +-0, +-inf, NaN, subnormals, the
/// largest and smallest normal magnitudes, and integers mixed with floats,
/// inside functions (which tier up) and at the top level. Their float slots and
/// variables are compared by bits, any two NaNs equal. The text of a float is its
/// value rounded to six decimals, so a one-ulp error, a flushed subnormal and
/// a NaN's sign are invisible to it.
std::vector<Case> float_cases() {
  std::vector<Case> cases;
  auto add = [&](const std::string & name, const std::string & source, uint64_t step) {
    Case c;
    c.name = "floats/" + name;
    c.source = source;
    c.step = step;
    c.limit = 400;
    cases.push_back(c);
  };
  add("zeros, infinities and NaN", R"TANG(function mix(a, b) { s = a + b; p = a * b; d = a - b; return s; }
big = "1e308" as float; pinf = big * 10.0; ninf = 0.0 - pinf; nan = pinf - pinf; pz = 0.0; nz = -0.0;
i = 0; while (i < 4) { r1 = mix(pinf, 1.5); r2 = mix(ninf, 2.0); r3 = mix(nz, pz); r4 = mix(nan, 1.0); r5 = mix(nz, nz); lt = nan < 1.0; i += 1; }
print(r1); print(" "); print(r2); print(" "); print(r3); print(" "); print(r4); print(" "); print(r5); print(" "); print(lt); print(" "); print(pinf == ninf); print(nz == pz);
)TANG", 17);
  add("subnormals", R"TANG(function halve(x) { y = x / 2.0; return y; }
tiny = "1e-320" as float; sub = tiny; i = 0; while (i < 4) { sub = halve(sub); twice = sub * 2.0; quarter = sub * 0.25; i += 1; }
least = "5e-324" as float; gone = halve(least); back = least + least;
smallest_normal = "2.2250738585072014e-308" as float; below = halve(smallest_normal); sum = below + below;
print(sub); print(" "); print(gone == 0.0); print(" "); print(back == least * 2.0); print(" "); print(below == 0.0); print(" "); print(sum == smallest_normal);
)TANG", 19);
  add("the largest and smallest magnitudes", R"TANG(function scale(a, b) { c = a * b; return c; }
e200 = "1e-200" as float; big = "1.7976931348623157e308" as float; small = "2.2250738585072014e-308" as float;
i = 0; while (i < 3) { x = scale(big, 0.5); y = scale(small, 0.5); z = scale(big, 2.0); w = big / small; v = small / big; u = scale(e200, e200); i += 1; }
print(x == big / 2.0); print(" "); print(z); print(" "); print(w); print(" "); print(v == 0.0); print(" "); print(y == 0.0); print(" "); print(u == 0.0);
)TANG", 23);
  add("integers mixed with floats", R"TANG(function series(n, x) { acc = 0.0; i = 0; while (i < n) { acc = acc + i * x; acc = acc - i / 2; i += 1; } return acc; }
function widen(k) { f = k + 0.0; g = f * 3; h = g - k; return h; }
r = series(12, 0.1); s = series(5, 1.5);
a = widen(9007199254740993); b = widen(0 - 9007199254740993); c = widen(576460752303423487); d = widen(0 - 576460752303423488); e = widen(7);
m = 7 / 2; n = 7 / 2.0; o = 7.5 / 2; p = 3 * 0.1; q = 0.1 + 0.2; t = (0.1 + 0.2) == 0.3; u = 3 > 2.5; w = 2 == 2.0;
print(r); print(" "); print(s); print(" "); print(a == b); print(" "); print(c); print(" "); print(e); print(" "); print(m); print(" "); print(n); print(" "); print(t); print(u); print(w);
)TANG", 29);
  return cases;
}

std::vector<Case> all_cases() {
  std::vector<Case> cases = corpus_cases("script", tt::Mode::Script, 40);
  for (Case & c : generated_cases(6)) {
    cases.push_back(std::move(c));
  }
  for (Case & c : corpus_cases("template", tt::Mode::Template, 12)) {
    cases.push_back(std::move(c));
  }
  for (Case & c : site_cases()) {
    cases.push_back(std::move(c));
  }
  for (Case & c : call_cases()) {
    cases.push_back(std::move(c));
  }
  for (Case & c : native_cases()) {
    cases.push_back(std::move(c));
  }
  for (Case & c : float_cases()) {
    cases.push_back(std::move(c));
  }
  return cases;
}

}  // namespace

TEST(Observer, TheCaseSetHoldsAtLeastFortyPrograms) {
  std::vector<Case> cases = all_cases();
  EXPECT_GE(cases.size(), 40u);
  size_t with_parts = 0;
  for (const Case & c : cases) {
    with_parts += !c.parts.empty();
  }
  EXPECT_GE(with_parts, 8u) << "template calls and scopes are in the set";
}

TEST(Observer, PlainTortureMovingStackAndShuffledPhasesGiveTheSameFrameTraceOutputResultAndErrors) {
  std::vector<Case> cases = all_cases();
  ASSERT_GE(cases.size(), 40u);
  size_t polls = 0, pauses = 0, deep = 0, scopes_with_variables = 0, polls_with_statements = 0;
#ifdef GLTANG_WITH_JIT
  uint64_t jit_entries = 0, jit_slow_polls = 0, jit_refused_pauses = 0, jit_calls = 0, jit_hook_errors = 0;
#endif
  // Under Valgrind a poll costs fifty milliseconds to record: each program
  // records its first few polls and compares the count of the rest. The other
  // modes record up to the case's limit.
  const size_t cap = RUNNING_ON_VALGRIND ? 8 : SIZE_MAX;
  for (const Case & c : cases) {
    auto started = std::chrono::steady_clock::now();
    Observed plain = observe(c, kPlain, nullptr, cap);
    if (std::getenv("GLTANG_OBSERVER_VERBOSE")) {
      std::printf("  %s: %zu polls, %zu pauses, %.3fs\n", c.name.c_str(), plain.trace.total, plain.pauses,
          std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
      std::fflush(stdout);
    }
    ASSERT_TRUE(plain.finished || plain.pauses > 0 || !plain.raw.empty() || plain.ran != GRCORE_OK) << c.name;
    EXPECT_GT(plain.trace.polls.size(), 0u) << c.name << " recorded no polls: the observer is not seeing the run";
    EXPECT_EQ(plain.trace.polls.size(), std::min(plain.trace.total, std::min(c.limit, cap))) << c.name;
    polls += plain.trace.polls.size();
    pauses += plain.pauses;
    for (const auto & p : plain.trace.polls) {
      deep += p.frames.size() > 2;
      for (const auto & f : p.frames) {
        for (const auto & s : f.scopes) {
          scopes_with_variables += !s.variables.empty();
        }
      }
    }
    Observed torture = observe(c, kTorture, nullptr, cap);
    EXPECT_TRUE(same(c, plain, kPlain.label, torture, kTorture.label));
    Observed moving = observe(c, kMoving, nullptr, cap);
    EXPECT_TRUE(same(c, plain, kPlain.label, moving, kMoving.label));
    Observed shuffled = observe(c, kShuffled, nullptr, cap);
    EXPECT_TRUE(same(c, plain, kPlain.label, shuffled, kShuffled.label));

    // With statement polls on the polls are different ones (the program is
    // paused at statements, so the pauses and the trace are not the plain
    // run's), but the four configurations must agree among themselves, and the
    // output, the result and the error list must be the plain run's.
    Observed lines = observe(c, kLinesPlain, nullptr, cap);
    EXPECT_GE(lines.trace.total, plain.trace.total) << c.name << ": statement polls only add polls";
    polls_with_statements += lines.trace.total;
    if (lines.finished && plain.finished) {
      EXPECT_EQ(lines.describe(), plain.describe()) << c.name << ": statement polls changed what the program did";
    }
    EXPECT_TRUE(same(c, lines, kLinesPlain.label, observe(c, kLinesTorture, nullptr, cap), kLinesTorture.label));
    EXPECT_TRUE(same(c, lines, kLinesPlain.label, observe(c, kLinesMoving, nullptr, cap), kLinesMoving.label));
    EXPECT_TRUE(same(c, lines, kLinesPlain.label, observe(c, kLinesShuffled, nullptr, cap), kLinesShuffled.label));
#ifdef GLTANG_WITH_JIT
    // The frame differential of the baseline JIT: the same program, interpreter
    // against compiled code, at every poll, with and without statement polls
    // (where a compiled LINE polls too), and under the collector's torture and
    // a stack that moves at every push. Same polls, same slots, same header
    // words, same pauses; the output, the result and the error list too.
    Observed jit = observe(c, kJit, nullptr, cap);
    EXPECT_TRUE(same(c, plain, kPlain.label, jit, kJit.label));
    EXPECT_TRUE(same(c, plain, kPlain.label, observe(c, kJitTorture, nullptr, cap), kJitTorture.label));
    EXPECT_TRUE(same(c, plain, kPlain.label, observe(c, kJitMoving, nullptr, cap), kJitMoving.label));
    EXPECT_TRUE(same(c, plain, kPlain.label, observe(c, kJitShuffled, nullptr, cap), kJitShuffled.label));
    Observed lines_jit = observe(c, kLinesJit, nullptr, cap);
    EXPECT_TRUE(same(c, lines, kLinesPlain.label, lines_jit, kLinesJit.label));
    EXPECT_TRUE(same(c, lines, kLinesPlain.label, observe(c, kLinesJitMoving, nullptr, cap), kLinesJitMoving.label));
    jit_entries += jit.jit.entries + lines_jit.jit.entries;
    jit_slow_polls += jit.jit.slow_polls + lines_jit.jit.slow_polls;
    jit_refused_pauses += jit.jit.refused_pauses + lines_jit.jit.refused_pauses;
    jit_calls += jit.jit.calls + lines_jit.jit.calls;
    jit_hook_errors += jit.jit.hook_argument_errors + lines_jit.jit.hook_argument_errors;
#endif
  }
  EXPECT_GT(polls, RUNNING_ON_VALGRIND ? 200u : 1800u);
  EXPECT_GT(pauses, 50u) << "the set includes runs that pause and resume";
  EXPECT_GT(deep, 20u) << "the set includes polls inside nested calls";
  EXPECT_GT(scopes_with_variables, 100u) << "scopes and their variables are recorded";
  EXPECT_GT(polls_with_statements, polls) << "the statement-poll runs have polls the plain ones lack";
#ifdef GLTANG_WITH_JIT
  // The differential is not vacuous: compiled code ran, its polls took the slow
  // path where the observer was watching, and some of them paused the run.
  // Where there is no native backend the JIT runs are the interpreter's, and
  // there is no compiled code to have entered.
  GLTANG_EXPECT_JIT_BACKEND_ON_GATED_TARGET();
  if (jit_backend_present()) {
    EXPECT_GT(jit_entries, 100u) << "the JIT runs entered compiled code";
    EXPECT_GT(jit_slow_polls, 100u) << "polls inside compiled code took the slow path";
    EXPECT_GT(jit_refused_pauses, 5u) << "a pause at a poll inside compiled code is in the set";
    // The frame differential of calls between compiled functions: this is the gate
    // that fails when the run shows none, whatever else it compared.
    EXPECT_GT(jit_calls, 500u) << "the corpus run made calls between compiled functions: chains of compiled frames were compared at the polls";
    EXPECT_EQ(jit_hook_errors, 0u);
  }
#endif
  std::printf("  observer: %zu programs x %d configurations (%d with statement polls), %zu polls, %zu pauses\n", cases.size(),
#ifdef GLTANG_WITH_JIT
      14, 6,
#else
      8, 4,
#endif
      polls, pauses);
}

TEST(Observer, AllInstrumentsTogetherAlsoGiveTheSameTrace) {
  std::vector<Case> cases = site_cases();
  RunConfig all = {"torture+verify+moving+shuffled", 1, 1, 1, true, 99};
  for (const Case & c : cases) {
    Observed plain = observe(c, kPlain);
    Observed everything = observe(c, all);
    EXPECT_TRUE(same(c, plain, kPlain.label, everything, all.label));
  }
}

TEST(Observer, ATraceRecordsDepthIdentityLocationSlotsAndScopes) {
  Case c;
  c.name = "shape";
  c.source = "function f(a) { local = a + 1; k = 0; while (k < 30) { k += 1; } return local; }\nx = f(5); print(x);";
  c.step = 25;
  Observed o = observe(c, kPlain);
  ASSERT_GT(o.trace.polls.size(), 5u);
  // Find a poll inside f.
  const observer::PollRecord * inside = nullptr;
  bool saw_local = false;
  for (const auto & p : o.trace.polls) {
    if (p.frames.size() == 2) {
      inside = inside ? inside : &p;
      for (const auto & v : p.frames[0].scopes[0].variables) {
        saw_local = saw_local || (v.name == "local" && v.text == "6");
      }
    }
  }
  ASSERT_NE(inside, nullptr);
  const observer::FrameRecord & top = inside->frames[0];
  EXPECT_EQ(top.depth, 0u);
  EXPECT_EQ(inside->frames[1].depth, 1u);
  EXPECT_EQ(top.engine, "lang-tang");
  EXPECT_EQ(top.file, "page.tang");
  EXPECT_GT(top.line, 0);
  EXPECT_EQ(top.slots.size(), top.slot_count);
  EXPECT_GE(top.slot_count, 5u);
  EXPECT_EQ(top.slots[0].kind, GRCORE_SLOT_RAW);
  bool saw_value_slot = false;
  for (const auto & s : top.slots) {
    saw_value_slot = saw_value_slot || s.kind == GRCORE_SLOT_VALUE;
  }
  EXPECT_TRUE(saw_value_slot);
  ASSERT_GE(top.scopes.size(), 2u);
  EXPECT_EQ(top.scopes[0].kind, GRCORE_SCOPE_LOCAL);
  EXPECT_EQ(top.scopes[0].name, "f");
  EXPECT_TRUE(saw_local) << "the local `local` reads 6 at some poll inside f";
  EXPECT_EQ(top.scopes.back().kind, GRCORE_SCOPE_GLOBAL);
}

// ---------------------------------------------------------------------------
// The instrument is seen to fail
// ---------------------------------------------------------------------------

namespace {

/// A trace with several frames and varied polls: the nested template case.
Observed nested_trace() {
  Case c;
  for (const Case & k : site_cases()) {
    if (k.name == "site/three deep") {
      c = k;
    }
  }
  return observe(c, kPlain);
}

/// A poll with at least two frames and a slot, where the next poll differs.
size_t pick_poll(const observer::Trace & t) {
  for (size_t p = 5; p + 1 < t.polls.size(); ++p) {
    if (t.polls[p].frames.size() >= 3 && !t.polls[p].frames[1].slots.empty()) {
      observer::Trace a, b;
      a.polls = {t.polls[p]};
      b.polls = {t.polls[p + 1]};
      a.total = b.total = 1;
      observer::Divergence d;
      if (observer::first_divergence(a, b, &d)) {
        return p;
      }
    }
  }
  return 0;
}

}  // namespace

TEST(ObserverFails, AnEqualPairOfTracesIsNotAFalseAlarm) {
  Observed a = nested_trace();
  Observed b = nested_trace();
  observer::Divergence d;
  EXPECT_FALSE(observer::first_divergence(a.trace, b.trace, &d)) << d.str();
  EXPECT_GT(a.trace.polls.size(), 20u);
}

TEST(ObserverFails, APlantedSlotMismatchIsReportedWithThePollAndTheFrame) {
  Observed a = nested_trace();
  Observed b = nested_trace();
  size_t poll = pick_poll(a.trace);
  ASSERT_GT(poll, 0u);
  ASSERT_TRUE(observer::plant_slot_mismatch(&b.trace, poll, 1, 0));
  observer::Divergence d;
  ASSERT_TRUE(observer::first_divergence(a.trace, b.trace, &d));
  EXPECT_EQ(d.poll, poll);
  EXPECT_TRUE(d.has_frame);
  EXPECT_EQ(d.frame, 1u);
  EXPECT_NE(d.what.find("slot 0 text"), std::string::npos) << d.str();
  EXPECT_NE(d.str().find("poll " + std::to_string(poll)), std::string::npos);
  EXPECT_NE(d.str().find("frame 1"), std::string::npos);
  // A VALUE slot, deeper in.
  Observed c = nested_trace();
  size_t value_slot = 0;
  for (size_t i = 0; i < a.trace.polls[poll].frames[0].slots.size(); ++i) {
    if (a.trace.polls[poll].frames[0].slots[i].kind == GRCORE_SLOT_VALUE) {
      value_slot = i;
    }
  }
  ASSERT_TRUE(observer::plant_slot_mismatch(&c.trace, poll, 0, value_slot));
  ASSERT_TRUE(observer::first_divergence(a.trace, c.trace, &d));
  EXPECT_EQ(d.poll, poll);
  EXPECT_EQ(d.frame, 0u);
}

TEST(ObserverFails, AMissingPollIsReportedAtThePollWhereTheTracesPartCompany) {
  Observed a = nested_trace();
  Observed b = nested_trace();
  size_t poll = pick_poll(a.trace);
  ASSERT_GT(poll, 0u);
  ASSERT_TRUE(observer::plant_missing_poll(&b.trace, poll));
  observer::Divergence d;
  ASSERT_TRUE(observer::first_divergence(a.trace, b.trace, &d));
  EXPECT_EQ(d.poll, poll);
  // And a poll missing at the very end: the number of polls differs.
  Observed e = nested_trace();
  ASSERT_TRUE(observer::plant_missing_poll(&e.trace, e.trace.polls.size() - 1));
  ASSERT_TRUE(observer::first_divergence(a.trace, e.trace, &d));
  EXPECT_EQ(d.poll, a.trace.polls.size() - 1);
  EXPECT_NE(d.what.find("number of polls"), std::string::npos) << d.str();
}

TEST(ObserverFails, ADifferentDepthIsReportedWithThePollAndTheFrameThatIsMissing) {
  Observed a = nested_trace();
  Observed b = nested_trace();
  size_t poll = pick_poll(a.trace);
  ASSERT_GT(poll, 0u);
  size_t frames = a.trace.polls[poll].frames.size();
  ASSERT_TRUE(observer::plant_different_depth(&b.trace, poll));
  observer::Divergence d;
  ASSERT_TRUE(observer::first_divergence(a.trace, b.trace, &d));
  EXPECT_EQ(d.poll, poll);
  EXPECT_TRUE(d.has_frame);
  EXPECT_EQ(d.frame, frames - 1);
  EXPECT_NE(d.what.find("depth"), std::string::npos) << d.str();
}

TEST(ObserverFails, AFailedFrameWalkIsADivergenceAndNotAnEmptyRecordThatMatches) {
  Observed a = nested_trace();
  Observed b = nested_trace();
  size_t poll = pick_poll(a.trace);
  ASSERT_GT(poll, 0u);
  // Both runs' walks failed at one poll: empty frame lists on both sides.
  a.trace.polls[poll].captured = b.trace.polls[poll].captured = false;
  a.trace.polls[poll].frames.clear();
  b.trace.polls[poll].frames.clear();
  observer::Divergence d;
  ASSERT_TRUE(observer::first_divergence(a.trace, b.trace, &d));
  EXPECT_EQ(d.poll, poll);
  EXPECT_NE(d.what.find("frame walk failed"), std::string::npos) << d.str();
  EXPECT_TRUE(observer::capture(nullptr, &a.trace.polls[poll].frames) == false);
}

TEST(ObserverFails, AChangedVariableAndALocationAreReportedToo) {
  Observed a = nested_trace();
  Observed b = nested_trace();
  size_t poll = pick_poll(a.trace);
  ASSERT_GT(poll, 0u);
  bool done = false;
  for (auto & scope : b.trace.polls[poll].frames[0].scopes) {
    if (!scope.variables.empty() && !done) {
      scope.variables[0].text += "?";
      done = true;
    }
  }
  ASSERT_TRUE(done);
  observer::Divergence d;
  ASSERT_TRUE(observer::first_divergence(a.trace, b.trace, &d));
  EXPECT_EQ(d.poll, poll);
  EXPECT_EQ(d.frame, 0u);
  EXPECT_NE(d.what.find("variable"), std::string::npos) << d.str();
  Observed c = nested_trace();
  c.trace.polls[poll].frames[2].line += 1;
  ASSERT_TRUE(observer::first_divergence(a.trace, c.trace, &d));
  EXPECT_EQ(d.poll, poll);
  EXPECT_EQ(d.frame, 2u);
  EXPECT_NE(d.what.find("location"), std::string::npos) << d.str();
}

// ---------------------------------------------------------------------------
// Floats are compared by their bits (CAP-5)
// ---------------------------------------------------------------------------

namespace {

constexpr uint64_t kSign = 0x8000000000000000ull;
constexpr uint64_t kExponent = 0x7ff0000000000000ull;
constexpr uint64_t kFraction = 0x000fffffffffffffull;

bool is_nan_float(uint64_t b) { return (b & kExponent) == kExponent && (b & kFraction) != 0; }
bool is_infinity(uint64_t b) { return (b & ~kSign) == kExponent; }
bool is_zero(uint64_t b) { return (b & ~kSign) == 0; }
bool is_subnormal(uint64_t b) { return (b & kExponent) == 0 && (b & kFraction) != 0; }
bool is_ordinary(uint64_t b) { return (b & kExponent) != kExponent && (b & kExponent) != 0; }
bool is_huge(uint64_t b) { return is_ordinary(b) && (b & kExponent) >= 0x7fe0000000000000ull; }
bool is_tiny_normal(uint64_t b) { return is_ordinary(b) && (b & kExponent) <= 0x0010000000000000ull; }

uint64_t one_ulp_up(uint64_t b) { return b + 1u; }
uint64_t flushed_to_zero(uint64_t b) { return b & kSign; }
uint64_t a_nan(uint64_t) { return 0x7ff8000000000000ull; }
uint64_t the_other_nan(uint64_t b) { return (b ^ kSign) ^ 0x1234u; }  // the other sign, another payload

const Case & float_case(const std::vector<Case> & cases, const char * name) {
  for (const Case & c : cases) {
    if (c.name.find(name) != std::string::npos) {
      return c;
    }
  }
  ADD_FAILURE() << "no float case " << name;
  return cases.front();
}

struct FloatSurvey {
  size_t floats = 0, nans = 0, positive_zero = 0, negative_zero = 0, positive_infinity = 0, negative_infinity = 0, subnormals = 0, huge = 0, tiny = 0, ordinary = 0;
  bool negative_nan = false, positive_nan = false;
};

FloatSurvey survey(const observer::Trace & trace) {
  FloatSurvey out;
  auto see = [&](bool is_float, uint64_t b) {
    if (!is_float) {
      return;
    }
    ++out.floats;
    if (is_nan_float(b)) {
      ++out.nans;
      (b & kSign ? out.negative_nan : out.positive_nan) = true;
    }
    else if (is_infinity(b)) {
      ++(b & kSign ? out.negative_infinity : out.positive_infinity);
    }
    else if (is_zero(b)) {
      ++(b & kSign ? out.negative_zero : out.positive_zero);
    }
    else if (is_subnormal(b)) {
      ++out.subnormals;
    }
    else {
      ++out.ordinary;
      out.huge += is_huge(b);
      out.tiny += is_tiny_normal(b);
    }
  };
  for (const auto & p : trace.polls) {
    for (const auto & f : p.frames) {
      for (const auto & s : f.slots) {
        see(s.is_float, s.float_bits);
      }
      for (const auto & sc : f.scopes) {
        for (const auto & v : sc.variables) {
          see(v.is_float, v.float_bits);
        }
      }
    }
  }
  return out;
}

}  // namespace

TEST(Observer, TheFloatProgramsShowEveryKindOfFloatInTheTracesAsBits) {
  FloatSurvey total;
  for (const Case & c : float_cases()) {
    Observed o = observe(c, kPlain);
    ASSERT_TRUE(o.finished) << c.name;
    ASSERT_GT(o.trace.polls.size(), 10u) << c.name;
    FloatSurvey one = survey(o.trace);
    total.floats += one.floats;
    total.nans += one.nans;
    total.positive_zero += one.positive_zero;
    total.negative_zero += one.negative_zero;
    total.positive_infinity += one.positive_infinity;
    total.negative_infinity += one.negative_infinity;
    total.subnormals += one.subnormals;
    total.huge += one.huge;
    total.tiny += one.tiny;
    total.ordinary += one.ordinary;
  }
  EXPECT_GT(total.floats, 200u) << "the traces carry the bits of the floats";
  EXPECT_GT(total.nans, 0u);
  EXPECT_GT(total.positive_zero, 0u);
  EXPECT_GT(total.negative_zero, 0u);
  EXPECT_GT(total.positive_infinity, 0u);
  EXPECT_GT(total.negative_infinity, 0u);
  EXPECT_GT(total.subnormals, 0u);
  EXPECT_GT(total.huge, 0u) << "a value near the largest double";
  EXPECT_GT(total.tiny, 0u) << "a value near the smallest normal double";
  EXPECT_GT(total.ordinary, 0u);
}

#ifdef GLTANG_WITH_JIT
TEST(Observer, TheFloatProgramsGiveTheSameFramesBitForBitInTheInterpreterAndWithTheJit) {
  // Floats still exit to the interpreter (spec-runtime-float, stories 6 and 7
  // compile them), so today this is the interpreter against the interpreter that
  // compiled everything else; the day a float is compiled it is the instrument.
  GLTANG_REQUIRE_JIT_BACKEND();
  uint64_t entries = 0, floats = 0;
  for (const Case & c : float_cases()) {
    Observed plain = observe(c, kPlain);
    Observed jit = observe(c, kJit);
    EXPECT_TRUE(same(c, plain, kPlain.label, jit, kJit.label));
    EXPECT_TRUE(same(c, plain, kPlain.label, observe(c, kJitTorture), kJitTorture.label));
    EXPECT_TRUE(same(c, plain, kPlain.label, observe(c, kJitMoving), kJitMoving.label));
    Observed lines = observe(c, kLinesPlain);
    Observed lines_jit = observe(c, kLinesJit);
    EXPECT_TRUE(same(c, lines, kLinesPlain.label, lines_jit, kLinesJit.label));
    entries += jit.jit.entries + lines_jit.jit.entries;
    floats += survey(jit.trace).floats;
  }
  EXPECT_GT(entries, 20u) << "compiled code ran: the comparison is not vacuous";
  EXPECT_GT(floats, 100u);
}
#endif

TEST(ObserverFails, AOneUlpDifferenceIsSeenByTheBitsAndNotByTheText) {
  std::vector<Case> cases = float_cases();
  const Case & c = float_case(cases, "magnitudes");
  Observed a = observe(c, kPlain);
  Observed control = observe(c, kPlain);
  Observed b = observe(c, kPlain);
  observer::Divergence d;
  EXPECT_FALSE(observer::first_divergence(a.trace, control.trace, &d)) << "the control: " << d.str();
  size_t poll = observer::plant_float_bits(&b.trace, is_ordinary, one_ulp_up);
  ASSERT_NE(poll, SIZE_MAX);
  EXPECT_FALSE(observer::first_divergence(a.trace, b.trace, &d, /*bits=*/false)) << "the text rounds one ulp away: " << d.str();
  ASSERT_TRUE(observer::first_divergence(a.trace, b.trace, &d));
  EXPECT_EQ(d.poll, poll);
  EXPECT_NE(d.what.find("float bits"), std::string::npos) << d.str();
}

TEST(ObserverFails, ASubnormalFlushedToZeroIsSeenByTheBitsAndNotByTheText) {
  std::vector<Case> cases = float_cases();
  const Case & c = float_case(cases, "subnormals");
  Observed a = observe(c, kPlain);
  Observed b = observe(c, kPlain);
  observer::Divergence d;
  size_t poll = observer::plant_float_bits(&b.trace, is_subnormal, flushed_to_zero);
  ASSERT_NE(poll, SIZE_MAX) << "the program holds a subnormal";
  EXPECT_FALSE(observer::first_divergence(a.trace, b.trace, &d, false)) << "a subnormal and zero both print as 0.: " << d.str();
  ASSERT_TRUE(observer::first_divergence(a.trace, b.trace, &d));
  EXPECT_EQ(d.poll, poll);
  EXPECT_NE(d.what.find("float bits"), std::string::npos) << d.str();
}

TEST(ObserverFails, ANaNWhereANumberBelongsIsSeen) {
  std::vector<Case> cases = float_cases();
  const Case & c = float_case(cases, "mixed");
  Observed a = observe(c, kPlain);
  Observed b = observe(c, kPlain);
  observer::Divergence d;
  size_t poll = observer::plant_float_bits(&b.trace, is_ordinary, a_nan);
  ASSERT_NE(poll, SIZE_MAX);
  ASSERT_TRUE(observer::first_divergence(a.trace, b.trace, &d));
  EXPECT_EQ(d.poll, poll);
  EXPECT_NE(d.what.find("float bits"), std::string::npos) << d.str();
  // And the other way: a number where a NaN belongs.
  Observed n = observe(float_case(cases, "infinities"), kPlain);
  Observed o = observe(float_case(cases, "infinities"), kPlain);
  ASSERT_NE(observer::plant_float_bits(&o.trace, is_nan_float, [](uint64_t) -> uint64_t { return 0x3ff0000000000000ull; }), SIZE_MAX);
  ASSERT_TRUE(observer::first_divergence(n.trace, o.trace, &d));
  EXPECT_NE(d.what.find("float bits"), std::string::npos) << d.str();
}

TEST(ObserverFails, AFloatInOnePlaceAndNotInTheOtherIsSeen) {
  std::vector<Case> cases = float_cases();
  Observed a = observe(float_case(cases, "mixed"), kPlain);
  Observed b = observe(float_case(cases, "mixed"), kPlain);
  observer::Divergence d;
  bool done = false;
  for (auto & p : b.trace.polls) {
    for (auto & f : p.frames) {
      for (auto & s : f.slots) {
        if (!done && s.is_float) {
          s.is_float = false;
          done = true;
        }
      }
    }
  }
  ASSERT_TRUE(done);
  ASSERT_TRUE(observer::first_divergence(a.trace, b.trace, &d));
  EXPECT_NE(d.what.find("float-ness"), std::string::npos) << d.str();
}

TEST(Observer, AnyTwoNaNsAgreeWhateverTheirSignAndPayload) {
  std::vector<Case> cases = float_cases();
  const Case & c = float_case(cases, "infinities");
  Observed a = observe(c, kPlain);
  Observed b = observe(c, kPlain);
  observer::Divergence d;
  // Every NaN the trace holds is replaced by one of the other sign and payload.
  size_t replaced = 0;
  while (observer::plant_float_bits(&b.trace, [](uint64_t x) { return is_nan_float(x) && (x & 0xfffu) != 0x234u; }, the_other_nan) != SIZE_MAX) {
    ++replaced;
    ASSERT_LT(replaced, 100000u);
  }
  EXPECT_GT(replaced, 0u);
  FloatSurvey before = survey(a.trace), after = survey(b.trace);
  EXPECT_EQ(before.nans, after.nans);
  EXPECT_FALSE(observer::first_divergence(a.trace, b.trace, &d)) << d.str();
  // Two programs that build a NaN differently (a different sign and payload, read
  // from text) and are otherwise one program: the traces hold different bits and
  // agree.
  auto program = [](const char * text) {
    Case k;
    k.name = "nan";
    k.source = std::string("x = \"") + text + "\" as float; i = 0; while (i < 6) { y = x + 1.0; i += 1; } print(y);";
    k.step = 9;
    return k;
  };
  Observed p = observe(program("nan"), kPlain);
  Observed q = observe(program("-nan(0x1234)"), kPlain);
  uint64_t bits_p = 0, bits_q = 0;
  for (const auto & poll : p.trace.polls) {
    for (const auto & f : poll.frames) {
      for (const auto & sc : f.scopes) {
        for (const auto & v : sc.variables) {
          if (v.name == "x" && v.is_float) {
            bits_p = v.float_bits;
          }
        }
      }
    }
  }
  for (const auto & poll : q.trace.polls) {
    for (const auto & f : poll.frames) {
      for (const auto & sc : f.scopes) {
        for (const auto & v : sc.variables) {
          if (v.name == "x" && v.is_float) {
            bits_q = v.float_bits;
          }
        }
      }
    }
  }
  EXPECT_TRUE(is_nan_float(bits_p));
  EXPECT_TRUE(is_nan_float(bits_q));
  EXPECT_NE(bits_p, bits_q) << "the two programs hold NaNs of different sign and payload";
  EXPECT_FALSE(observer::first_divergence(p.trace, q.trace, &d)) << d.str();
  EXPECT_EQ(p.describe(), q.describe());
}

TEST(ObserverFails, TwoRunsOneUlpApartWhoseTextIsTheSameDiverge) {
  // Not a planted trace: two programs that hold 1 and the double after 1, which
  // both print as "1.". The text channel cannot tell them apart; the bits can.
  auto program = [](const char * text) {
    Case k;
    k.name = "ulp";
    k.source = std::string("x = \"") + text + "\" as float; i = 0; while (i < 6) { y = x + 0.0; i += 1; } print(y);";
    k.step = 9;
    return k;
  };
  Observed a = observe(program("1.0"), kPlain);
  Observed b = observe(program("1.0000000000000002"), kPlain);
  EXPECT_EQ(a.describe(), b.describe()) << "the output and result are the same";
  observer::Divergence d;
  EXPECT_FALSE(observer::first_divergence(a.trace, b.trace, &d, false)) << "the text is the same at every poll: " << d.str();
  ASSERT_TRUE(observer::first_divergence(a.trace, b.trace, &d));
  EXPECT_NE(d.what.find("float bits"), std::string::npos) << d.str();
}

// ---------------------------------------------------------------------------
// Phase shuffle: an order-dependent DECIDE handler is caught, an independent one is not
// ---------------------------------------------------------------------------

namespace {

struct Flag {
  bool a_ran = false;
  uint64_t polls = 0;
};

void decide_a(GRCORE_Context *, void * value, GRCORE_PollCall *) {
  static_cast<Flag *>(value)->a_ran = true;
}

/// Votes pause unless A has already run in this poll: its vote depends on the
/// order the two ran in.
void decide_b_order_dependent(GRCORE_Context *, void * value, GRCORE_PollCall * call) {
  if (!static_cast<Flag *>(value)->a_ran) {
    grcore_pollcall_vote(call, GRCORE_VERDICT_PAUSE);
  }
}

/// Votes pause at every seventh poll, whatever else ran.
void decide_b_independent(GRCORE_Context *, void * value, GRCORE_PollCall * call) {
  Flag * f = static_cast<Flag *>(value);
  if (f->polls % 7 == 3) {
    grcore_pollcall_vote(call, GRCORE_VERDICT_PAUSE);
  }
}

void observe_reset(GRCORE_Context *, void * value, GRCORE_PollCall *) {
  Flag * f = static_cast<Flag *>(value);
  f->a_ran = false;
  ++f->polls;
}

const GRCORE_Key kKeyA = GRCORE_KEY_INIT("order A", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_DECIDE, nullptr, decide_a, nullptr, nullptr, nullptr);
const GRCORE_Key kKeyBDependent = GRCORE_KEY_INIT("order B", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_DECIDE, nullptr, decide_b_order_dependent, nullptr, nullptr, nullptr);
const GRCORE_Key kKeyBIndependent = GRCORE_KEY_INIT("independent B", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_DECIDE, nullptr, decide_b_independent, nullptr, nullptr, nullptr);
const GRCORE_Key kKeyReset = GRCORE_KEY_INIT("reset", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_ACT, nullptr, observe_reset, nullptr, nullptr, nullptr);

Case shuffle_case() {
  Case c;
  c.name = "shuffle";
  c.source = "s = 0; for (i = 0; i < 40; i += 1) { s += i; } print(s); s;";
  c.step = 100000;
  return c;
}

}  // namespace

TEST(PhaseShuffle, AnOrderDependentDecideHandlerIsCaughtByTheShuffledRun) {
  Case c = shuffle_case();
  Flag plain_flag, shuffled_flag;
  auto make = [&](Flag * flag) {
    return [flag](GRCORE_Context * context) {
      ASSERT_EQ(grcore_context_register(context, &kKeyA, flag), GRCORE_OK);
      ASSERT_EQ(grcore_context_register(context, &kKeyBDependent, flag), GRCORE_OK);
      ASSERT_EQ(grcore_context_register(context, &kKeyReset, flag), GRCORE_OK);
    };
  };
  Observed plain = observe(c, kPlain, make(&plain_flag));
  Observed shuffled = observe(c, kShuffled, make(&shuffled_flag));
  EXPECT_EQ(plain.pauses, 0u) << "in registration order A runs before B, which never votes";
  EXPECT_GT(shuffled.pauses, 0u) << "with the order shuffled B sometimes runs first and votes";
  observer::Divergence d;
  ASSERT_TRUE(observer::first_divergence(plain.trace, shuffled.trace, &d)) << "the shuffled run's trace must differ";
  EXPECT_NE(d.what.find("verdict"), std::string::npos) << d.str();
  EXPECT_FALSE(same(c, plain, "plain", shuffled, "shuffled")) << "the comparison used by the four-way test fails on it";
}

TEST(PhaseShuffle, AnOrderIndependentDecideHandlerIsNotCaught) {
  Case c = shuffle_case();
  Flag plain_flag, shuffled_flag;
  auto make = [&](Flag * flag) {
    return [flag](GRCORE_Context * context) {
      ASSERT_EQ(grcore_context_register(context, &kKeyA, flag), GRCORE_OK);
      ASSERT_EQ(grcore_context_register(context, &kKeyBIndependent, flag), GRCORE_OK);
      ASSERT_EQ(grcore_context_register(context, &kKeyReset, flag), GRCORE_OK);
    };
  };
  Observed plain = observe(c, kPlain, make(&plain_flag));
  Observed shuffled = observe(c, kShuffled, make(&shuffled_flag));
  EXPECT_GT(plain.pauses, 0u) << "the independent handler does vote";
  EXPECT_TRUE(same(c, plain, "plain", shuffled, "shuffled"));
}

TEST(PhaseShuffle, TheShuffleReallyReordersTheHandlers) {
  struct Probe {
    std::vector<int> * order;
    int id;
  };
  static const GRCORE_Key probe_key = GRCORE_KEY_INIT("probe", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_OBSERVE, nullptr,
      [](GRCORE_Context *, void * value, GRCORE_PollCall *) {
        Probe * p = static_cast<Probe *>(value);
        p->order->push_back(p->id);
      }, nullptr, nullptr, nullptr);
  Case c = shuffle_case();
  auto run = [&](const RunConfig & rc) {
    std::vector<int> order;
    std::vector<Probe> probes = {{&order, 0}, {&order, 1}, {&order, 2}, {&order, 3}};
    observe(c, rc, [&](GRCORE_Context * context) {
      for (Probe & p : probes) {
        ASSERT_EQ(grcore_context_register(context, &probe_key, &p), GRCORE_OK);
      }
    });
    return order;
  };
  std::vector<int> plain = run(kPlain);
  std::vector<int> shuffled = run(kShuffled);
  ASSERT_GE(plain.size(), 8u);
  bool plain_in_order = true;
  for (size_t i = 0; i < plain.size(); ++i) {
    plain_in_order = plain_in_order && plain[i] == (int)(i % 4);
  }
  EXPECT_TRUE(plain_in_order) << "without the shuffle the handlers run in registration order";
  EXPECT_NE(plain, shuffled) << "with it they do not";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
