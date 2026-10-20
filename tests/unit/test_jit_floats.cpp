/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2024-2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Lang-tang.
 *
 * Ghoti.io Lang-tang is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Lang-tang is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Floats carried raw in compiled code (spec-runtime-float story 6, AD-27).
//
// A float constant is held as a raw double in compiled code, in a local or an
// operand slot, and is boxed only when a frame is rebuilt, from boxes made ahead
// of time. These tests say what that means in numbers: the heap's allocation
// count over two loop lengths (the slope), the values a rebuild boxes (each live
// raw value once, and nothing allocated or collected across the rebuild), the
// conversions an unwind makes (none), the reservation and the pool across
// returns, deopts and nested entries, the refusals, and the engine's `convert`
// and `reverse` over hand-made sites, bit for bit. Each one can fail, and has
// been seen to: tests/planted/35 to 43.

#include "exec_harness.h"
#include "jit_harness.h"
#include "observer.h"
#include "test_helpers.h"
#include <ghoti.io/lang-tang/bytecode.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <ghoti.io/runtime-core/runtime-core.h>

using namespace jt;

#ifdef GLTANG_WITH_JIT

namespace {

uint64_t bits_of(double d) {
  uint64_t u;
  std::memcpy(&u, &d, sizeof u);
  return u;
}

/// `expect_same`, and the run was not blinded: no function of the program was declined at the compile (a loop-free function
/// whose every path ends at a designed exit is), because a declined function runs in the interpreter and proves nothing about
/// compiled code. The tests that mean to decline use `expect_same` itself.
void expect_same_compiled(const Scenario & sc, Outcome * interpreter = nullptr, Outcome * jit = nullptr) {
  Outcome a, b;
  expect_same(sc, interpreter ? interpreter : &a, jit ? jit : &b);
  const Outcome & seen = jit ? *jit : b;
  EXPECT_EQ(seen.stats.functions_declined_expected, 0u) << "a function of the program was declined, so the interpreter ran it";
}

/// A run the test drives itself, so that the execution can be asked questions
/// before and after: one program, a threshold, the instruments, and (optionally) a
/// script that runs at every poll.
struct Driver {
  Compiled page;
  std::unique_ptr<Context> ctx;
  Script script;
  bool finished = false;
  bool scripted = false;

  Driver(const std::string & source, long threshold, bool with_script, int torture = 0, int verify = 0, long fail_at = 0, bool natives = false)
      : page(source, Mode::Script, "floats.tang"), scripted(with_script) {
    EXPECT_TRUE(page.ok()) << page.error.message;
    if (!page.ok()) {
      return;
    }
    Config config;
    config.torture = torture;
    config.verify = verify;
    config.moving_stack = 0;
    config.jit_threshold = threshold;
    config.fail_at = fail_at;
    ctx = std::make_unique<Context>(page.program, config);
    if (ctx->ok() && natives) {
      ctx->add_native_library();
    }
    if (ctx->ok()) {
      EXPECT_TRUE(gltang_execution_set_name(ctx->execution, "page") == GLTANG_OK || fail_at != 0);
    }
    if (ctx->ok() && with_script) {
      EXPECT_TRUE(script.attach(ctx->context));
    }
  }

  bool ok() const { return ctx && ctx->ok(); }
  bool run() {
    finished = ctx->execute();
    return finished;
  }
  GRHEAP_Stats heap() const {
    GRHEAP_Stats s;
    std::memset(&s, 0, sizeof s);
    (void)grheap_stats(ctx->heap, &s);
    return s;
  }
  GLTANG_JitStats stats() const { return ctx->jit_stats(); }
  struct Boxes {
    uint64_t pool = 0, capacity = 0, extensions = 0;
  };
  Boxes boxes() const {
    Boxes b;
    (void)gltang_vm_jit_test_boxes(ctx->execution, &b.pool, &b.capacity, &b.extensions);
    return b;
  }
};

/// What a handler sees at a poll.
struct Seen {
  uint64_t function = 0;  ///< The function index of the poll.
  uint64_t allocations = 0, collections = 0;
  uint64_t pool = 0, capacity = 0, extensions = 0;
  uint64_t boxed = 0;
};

std::function<void(Script &, GRCORE_Context *, uint64_t)> recorder(Driver * d, std::vector<Seen> * out, std::function<void(Script &, uint64_t)> also = nullptr) {
  return [d, out, also](Script & script, GRCORE_Context * context, uint64_t n) {
    Seen s;
    GRCORE_PollIdentity id = {0, 0};
    (void)grcore_context_poll_identity(context, &id);
    s.function = id.function & 0xFFFFFFFFu;
    GRHEAP_Stats h = d->heap();
    s.allocations = h.allocations;
    s.collections = h.collections;
    Driver::Boxes b = d->boxes();
    s.pool = b.pool;
    s.capacity = b.capacity;
    s.extensions = b.extensions;
    s.boxed = d->stats().values_boxed;
    out->push_back(s);
    if (also) {
      also(script, n);
    }
  };
}

/// `k` float locals, then a loop of `n` polls, then the integer count.
std::string locals_source(int k, const std::string & n, const std::string & tail = "return i;") {
  std::string body;
  for (int j = 0; j < k; ++j) {
    body += "a" + std::to_string(j) + " = " + std::to_string(j + 1) + ".5; ";
  }
  return "function f(n) { " + body + "i = 0; while (i < n) { i = i + 1; } " + tail + " }\nprint(f(" + n + "));\n";
}

/// The bits of every float the frames of the paused context hold, by walking them
/// the way the frame differential does.
std::multiset<uint64_t> float_bits_in_frames(Context & context) {
  std::vector<observer::FrameRecord> frames;
  EXPECT_TRUE(observer::capture(context.context, &frames, &gltang_vm_test_float_bits, &gltang_vm_test_float_text));
  std::multiset<uint64_t> bits;
  for (const auto & f : frames) {
    for (const auto & s : f.slots) {
      if (s.is_float) {
        bits.insert(s.float_bits);
      }
    }
  }
  return bits;
}

}  // namespace

// ---------------------------------------------------------------------------
// The allocation slope
// ---------------------------------------------------------------------------

TEST(JitFloats, ACompiledLoopThatCarriesAFloatAllocatesNothingPerIterationAndBoxesOnlyAtTheExit) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const std::string program = "function f(n) { x = 1.5; i = 0; while (i < n) { i = i + 1; } print(x); return i; }\nprint(f(N));\n";
  auto with = [&](const char * n) {
    Scenario sc;
    sc.source = program;
    sc.source.replace(sc.source.find("N"), 1, n);
    return sc;
  };
  Outcome plain_short, jit_short, plain_long, jit_long;
  expect_same_compiled(with("200"), &plain_short, &jit_short);
  expect_same_compiled(with("5000"), &plain_long, &jit_long);
  ASSERT_FALSE(HasFailure());
  EXPECT_GE(jit_long.stats.entries, 1u);
  EXPECT_EQ(jit_long.stats.slow_polls, 0u) << "nothing was pending: every poll was the fast path";
  EXPECT_EQ(jit_long.heap_allocations, jit_short.heap_allocations) << "the allocation count has slope zero in the loop length";
  EXPECT_EQ(plain_long.heap_allocations, plain_short.heap_allocations) << "and so does the interpreter's, on this loop (test_jit_arith.cpp re-runs it on arithmetic)";
  // The one exit, at `print`, boxed what was live: the local and the operand, once each.
  EXPECT_EQ(jit_short.stats.values_boxed, 2u);
  EXPECT_EQ(jit_long.stats.values_boxed, 2u) << "the same however long the loop ran";
  EXPECT_EQ(jit_long.stats.conversion_errors, 0u);
  EXPECT_EQ(jit_long.stats.compile_failures, 0u);
  EXPECT_EQ(jit_long.raw, "1.55000");
  std::printf("  float carry slope: compiled allocations %llu at 200 iterations and %llu at 5000; interpreted %llu and %llu\n",
      static_cast<unsigned long long>(jit_short.heap_allocations), static_cast<unsigned long long>(jit_long.heap_allocations),
      static_cast<unsigned long long>(plain_short.heap_allocations), static_cast<unsigned long long>(plain_long.heap_allocations));
  EXPECT_EQ(plain_long.stats.values_boxed, 0u) << "the interpreter boxes nothing";
}

TEST(JitFloats, TheSlopeIsStillZeroWithAHandlerPendingAtEveryPoll) {
  GLTANG_REQUIRE_JIT_BACKEND();
  uint64_t allocations[2] = {0, 0}, boxed[2] = {0, 0}, slow[2] = {0, 0};
  const char * lengths[2] = {"100", "2000"};
  for (int which = 0; which < 2; ++which) {
    Scenario sc;
    sc.source = std::string("function f(n) { x = 1.5; i = 0; while (i < n) { i = i + 1; } print(x); return i; }\nprint(f(") + lengths[which] + "));\n";
    sc.script = true;  // a request stays pending, so every poll takes the slow path
    Outcome plain, jit;
    expect_same_compiled(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    allocations[which] = jit.heap_allocations;
    boxed[which] = jit.stats.values_boxed;
    slow[which] = jit.stats.slow_polls;
  }
  EXPECT_GT(slow[1], 1900u) << "every iteration's poll took the slow path";
  EXPECT_EQ(allocations[0], allocations[1]) << "a slow poll that continues converts and allocates nothing";
  EXPECT_EQ(boxed[0], 2u);
  EXPECT_EQ(boxed[1], 2u) << "the exit at print boxed its two live values, and no poll boxed anything";
}

TEST(JitFloats, TheSlopeIsStillZeroUnderTortureAndVerify) {
  GLTANG_REQUIRE_JIT_BACKEND();
  uint64_t allocations[2] = {0, 0};
  const char * lengths[2] = {"60", "360"};
  for (int which = 0; which < 2; ++which) {
    Scenario sc;
    sc.source = std::string("function f(n) { x = 1.5; i = 0; while (i < n) { i = i + 1; } print(x); return i; }\nprint(f(") + lengths[which] + "));\n";
    sc.torture = 1;
    sc.verify = 1;
    sc.script = true;
    Outcome plain, jit;
    expect_same_compiled(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    allocations[which] = jit.heap_allocations;
    EXPECT_EQ(jit.stats.values_boxed, 2u);
    EXPECT_EQ(jit.stats.conversion_errors, 0u);
    EXPECT_EQ(jit.heap_invalid_pointers, 0u) << "the collector never read a raw word as a value";
  }
  EXPECT_EQ(allocations[0], allocations[1]);
}

TEST(JitFloats, AFloatConstantIsCompiledInAFunctionWithALoopAndALoopFreeOneThatOnlyBoxesItIsDeclined) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // With a loop the function compiles, is entered, and the constant is an immediate that its return boxes.
  {
    Driver d("function f() { k = 0; while (k < 1) { k = k + 1; } return 1.5; }\nprint(f()); print(f()); print(f());\n", 1, false);
    ASSERT_TRUE(d.ok());
    ASSERT_TRUE(d.run());
    EXPECT_EQ(d.ctx->raw(), "1.51.51.5");
    GLTANG_JitStats st = d.stats();
    EXPECT_GE(st.functions_compiled, 1u);
    EXPECT_EQ(st.compile_failures, 0u);
    EXPECT_GE(st.entries, 1u);
    EXPECT_GE(st.values_boxed, 1u) << "the RET with a raw operand is an exit, with the operand boxed by the rebuild";
    EXPECT_GE(st.exits_expected, 3u) << "and a designed one";
    EXPECT_EQ(st.functions_declined_expected, 0u);
  }
  // Without one every path ends at that return: nothing could repay the compile.
  {
    Driver d("function f() { return 1.5; }\nprint(f()); print(f()); print(f());\n", 1, false);
    ASSERT_TRUE(d.ok());
    ASSERT_TRUE(d.run());
    EXPECT_EQ(d.ctx->raw(), "1.51.51.5");
    GLTANG_JitStats st = d.stats();
    EXPECT_EQ(st.functions_declined_expected, 1u);
    EXPECT_EQ(st.compile_failures, 0u);
    EXPECT_EQ(st.values_boxed, 0u) << "it was never entered";
  }
}

// ---------------------------------------------------------------------------
// A rebuild boxes each live raw value once and allocates nothing
// ---------------------------------------------------------------------------

namespace {

/// Pauses at a poll inside the loop of `locals_source(k)` and reads what the rebuild did.
void pause_with_live_floats(int k, int torture, int verify) {
  Driver d(locals_source(k, "1000"), 1, true, torture, verify);
  ASSERT_TRUE(d.ok());
  std::vector<Seen> seen;
  d.script.on_poll = recorder(&d, &seen, [](Script & script, uint64_t n) {
    if (n == 30) {
      script.post(GRCORE_REQUEST_INTERRUPT);
    }
  });
  EXPECT_FALSE(d.run());
  ASSERT_TRUE(d.ctx->paused());
  ASSERT_FALSE(seen.empty());
  GLTANG_JitStats st = d.stats();
  EXPECT_EQ(st.refused_pauses, 1u) << "the pause was taken inside compiled code";
  EXPECT_EQ(st.values_boxed, static_cast<uint64_t>(k)) << k << " live raw values: " << k << " boxes, each once";
  EXPECT_EQ(st.rebuild_failures, 0u);
  EXPECT_EQ(st.conversion_errors, 0u);
  GRHEAP_Stats h = d.heap();
  if (!d.ctx->torture_on) {
    // Across the rebuild (from the last handler call at the pausing poll to the
    // paused context here) the allocation count and the collection count are unchanged.
    EXPECT_EQ(h.allocations, seen.back().allocations) << "the rebuild allocated";
    EXPECT_EQ(h.collections, seen.back().collections) << "the rebuild collected";
  }
  EXPECT_EQ(h.invalid_pointers, 0u);
  // The guest frame holds k floats with the same bits.
  std::multiset<uint64_t> expected;
  for (int j = 0; j < k; ++j) {
    expected.insert(bits_of(j + 1.5));
  }
  EXPECT_EQ(float_bits_in_frames(*d.ctx), expected);
  // And the interpreter finishes with the interpreter's output.
  EXPECT_TRUE(d.ctx->resume());
  EXPECT_EQ(d.ctx->raw(), "1000");
  EXPECT_EQ(grheap_collect(d.ctx->heap), GRHEAP_OK);
  EXPECT_EQ(d.heap().invalid_pointers, 0u);
}

}  // namespace

TEST(JitFloats, APauseAtAPollBoxesEveryLiveRawValueOnceAndAllocatesAndCollectsNothingAcrossTheRebuild) {
  GLTANG_REQUIRE_JIT_BACKEND();
  for (int k : {1, 2, 5, 16}) {
    pause_with_live_floats(k, 0, 0);
    if (HasFailure()) {
      ADD_FAILURE() << "with " << k << " live raw values";
      return;
    }
  }
}

TEST(JitFloats, ARebuildUnderTortureAndVerifyBoxesFromBoxesThatSurvivedEveryCollection) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // Every allocation collects, so the pool's boxes live only if the collector
  // sees them; the floats the rebuild hands the interpreter must have the right
  // bits, and no root slot may name a freed object.
  for (int k : {1, 2, 5, 16}) {
    pause_with_live_floats(k, 1, 1);
    if (HasFailure()) {
      ADD_FAILURE() << "with " << k << " live raw values";
      return;
    }
  }
}

TEST(JitFloats, ARawValueInAnOperandSlotIsBoxedByTheRebuildToo) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // `1.5 + h()`: the constant waits in an operand slot of the caller while the
  // callee polls, and a pause there rebuilds both frames.
  Driver d(
      "function h(n) { i = 0; while (i < n) { i = i + 1; } return i; }\n"
      "function g(n) { k = 0; while (k < 2) { k = k + 1; } r = 1.5 + h(n); return r; }\n"
      "print(g(500));\n",
      1, true);
  ASSERT_TRUE(d.ok());
  std::vector<Seen> seen;
  d.script.on_poll = recorder(&d, &seen, [](Script & script, uint64_t n) {
    if (n == 40) {
      script.post(GRCORE_REQUEST_INTERRUPT);
    }
  });
  EXPECT_FALSE(d.run());
  ASSERT_TRUE(d.ctx->paused());
  EXPECT_EQ(d.stats().values_boxed, 1u) << "the operand slot's raw 1.5, once";
  EXPECT_GE(d.stats().calls, 1u) << "the callee was called from compiled code";
  EXPECT_EQ(float_bits_in_frames(*d.ctx), std::multiset<uint64_t>{bits_of(1.5)});
  EXPECT_TRUE(d.ctx->resume());
  EXPECT_EQ(d.ctx->raw(), "501.5");
}

TEST(JitFloats, ARawOperandSlotIsReportedAsRawBitsLikeARawLocalAndItsInspectTextIsTheBitsNotTheFloat) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Driver d(
      "function h(n) { i = 0; while (i < n) { i = i + 1; } return i; }\n"
      "function g(n) { k = 0; while (k < 2) { k = k + 1; } r = 1.5 + h(n); return r; }\n"
      "print(g(40));\n",
      1, true);
  ASSERT_TRUE(d.ok());
  int found = 0, checks = 0;
  d.script.on_poll = [&](Script &, GRCORE_Context * context, uint64_t n) {
    if (n < 6 || n > 9) {
      return;
    }
    GRCORE_FrameWalk walk;
    ASSERT_EQ(grcore_frame_walk_begin(context, &walk), GRCORE_OK);
    GRCORE_AbstractFrame frame;
    while (grcore_frame_walk_next(&walk, &frame)) {
      for (size_t i = 0; i < frame.slot_count; ++i) {
        GRCORE_SlotKind kind;
        uint64_t value;
        GRCORE_Representation representation;
        ASSERT_EQ(grcore_frame_slot_tagged(&frame, i, &kind, &value, &representation), GRCORE_OK);
        if (representation != GRCORE_REPR_F64) {
          continue;
        }
        ++found;
        // g has three locals after the four header words, so a slot from index 7 is an operand slot.
        EXPECT_GE(i, 7u) << "the raw 1.5 is in an operand slot";
        EXPECT_EQ(kind, GRCORE_SLOT_RAW) << "never a value";
        EXPECT_EQ(value, bits_of(1.5));
        char text[64];
        size_t length = 0;
        ASSERT_EQ(grcore_frame_inspect(&frame, i, text, sizeof text, &length), GRCORE_OK);
        EXPECT_EQ(std::string(text, length), std::to_string(bits_of(1.5))) << "inspect shows the bits of a raw slot, not the float (a gap in runtime-core, see design.md)";
      }
    }
    ++checks;
  };
  ASSERT_TRUE(d.run());
  EXPECT_GT(checks, 0);
  EXPECT_EQ(found, checks) << "one raw operand slot at each of those polls";
}

TEST(JitFloats, SlowPollsThatContinueConvertNothingAndTheFramesAgreeWithTheInterpretersBitForBit) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Driver d(locals_source(3, "400"), 1, true);
  ASSERT_TRUE(d.ok());
  std::vector<Seen> seen;
  d.script.on_poll = recorder(&d, &seen);
  ASSERT_TRUE(d.run());
  ASSERT_GT(seen.size(), 300u);
  for (size_t i = 0; i < seen.size(); ++i) {
    EXPECT_EQ(seen[i].boxed, 0u) << "no poll that continued converted anything";
    if (i >= 10) {
      EXPECT_EQ(seen[i].allocations, seen[10].allocations) << "and none allocated (the pool was made at the entry, before the loop)";
    }
  }
  EXPECT_EQ(d.stats().values_boxed, 0u) << "the function returned an integer: nothing was ever boxed";
  EXPECT_GE(d.stats().slow_polls, 300u);
  EXPECT_EQ(d.ctx->raw(), "400");
}

// ---------------------------------------------------------------------------
// An unwind converts nothing
// ---------------------------------------------------------------------------

TEST(JitFloats, AnUnwindWithFloatsLiveConvertsNothingAllocatesNothingAndLeavesThePoolAndTheReservationAsTheyWere) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Driver d(locals_source(4, "100000"), 1, true);
  ASSERT_TRUE(d.ok());
  std::vector<Seen> seen;
  d.script.on_poll = recorder(&d, &seen, [](Script & script, uint64_t n) {
    if (n == 25) {
      script.post(GRCORE_REQUEST_TERMINATE);
    }
  });
  EXPECT_FALSE(d.run());
  ASSERT_FALSE(seen.empty());
  GLTANG_JitStats st = d.stats();
  EXPECT_EQ(st.refused_unwinds, 1u) << "the run ended by an unwind at a compiled poll";
  EXPECT_EQ(st.values_boxed, 0u) << "an unwind converts no frame: no compiled frame survives it";
  EXPECT_EQ(st.conversion_errors, 0u);
  EXPECT_EQ(st.rebuild_failures, 0u);
  EXPECT_EQ(d.heap().allocations, seen.back().allocations) << "and allocates nothing";
  EXPECT_EQ(seen.back().pool, 4u) << "the pool held its boxes up to the last poll...";
  EXPECT_EQ(seen.back().capacity, 4u);
  EXPECT_EQ(d.boxes().capacity, 0u) << "...and the reservation is given back";
  EXPECT_EQ(d.boxes().extensions, 0u);
  EXPECT_EQ(d.heap().invalid_pointers, 0u);
}

TEST(JitFloats, AFuelScopeThatRunsOutInsideCompiledCodeWithFloatsLiveUnwindsLikeTheInterpreterAndConvertsNothing) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = "use t; print(\"[\" + t() + \"]\"); print(\"after\");";
  Part t;
  t.name = "t";
  t.source = "function spin(n) { x = 1.5; y = 2.5; k = 0; while (k < n) { k += 1; } return k; }\nprint(spin(100000));";
  t.fuel = 600;
  t.policy = GLTANG_SCOPE_EMPTY;
  sc.parts.push_back(t);
  Outcome plain, jit;
  expect_same_compiled(sc, &plain, &jit);
  EXPECT_GE(jit.stats.refused_unwinds, 1u);
  EXPECT_EQ(jit.stats.values_boxed, 0u) << "the scope's unwind boxed nothing";
  EXPECT_EQ(jit.stats.conversion_errors, 0u);
  EXPECT_EQ(jit.heap_allocations, plain.heap_allocations) << "and allocated nothing the interpreter did not";
}

// ---------------------------------------------------------------------------
// Everything that reads a raw operand leaves compiled code
// ---------------------------------------------------------------------------

TEST(JitFloats, EveryInstructionThatReadsARawOperandIsAnExitWithTheOperandInTheFrameStateAndTheInterpreterReExecutesIt) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const char * consumers[] = {
      "print(x);",
      // x + 1, 1 + x, -x, x < 2 and x == 1.5 were consumers here until story 7 compiled them: they are in
      // test_jit_arith.cpp now. What stays an exit is what is not an operation: % (a float is "not
      // supported"), the casts to string and to bool, truthiness.
      "y = x % 2; print(y);",
      "y = 2 % x; print(y);",
      "y = x as string; print(y);",
      "y = x as bool; print(y);",
      "if (x) { i = i + 1; } print(i);",
      "y = !x; print(y);",
      "y = x && 1; print(y);",
      "y = x || 1; print(y);",
      "g = x; print(g);",
      "h(x); print(1);",
      "y = h(2, x); print(y);",
      "y = x(1); print(y);",
      "y = x.name; print(y);",
      "a = [x, 2]; print(a[0]);",
      "return x;",
  };
  for (const char * consumer : consumers) {
    Scenario sc;
    sc.source = std::string("g = 0;\nfunction h(a, b) { return a; }\n") +
        "function f(n) { global g; x = 1.5; i = 0; while (i < n) { i = i + 1; } " + consumer + " return i; }\n"
        "print(f(5)); print(f(0));\n";
    Outcome plain, jit;
    expect_same_compiled(sc, &plain, &jit);
    EXPECT_GE(jit.stats.deopts, 1u) << consumer << ": the consumer is an unconditional exit";
    EXPECT_GE(jit.stats.values_boxed, 1u) << consumer << ": the operand was boxed by the rebuild";
    EXPECT_EQ(jit.stats.compile_failures, 0u) << consumer;
    EXPECT_EQ(jit.stats.conversion_errors, 0u) << consumer;
    EXPECT_EQ(jit.stats.rebuild_failures, 0u) << consumer;
    if (HasFailure()) {
      ADD_FAILURE() << "with the consumer: " << consumer;
      return;
    }
  }
}

TEST(JitFloats, ARawValueMovedThroughLocalsAndDupsReachesItsConsumerWithTheSameBits) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source =
      "function f(n) { a = b = c = 0.1; d = a; e = 0.0; i = 0; while (i < n) { i = i + 1; e = d; } print(a); print(b); print(c); print(d); return i; }\n"
      "print(f(30)); print(f(0));\n";
  sc.script = true;
  Outcome plain, jit;
  expect_same_compiled(sc, &plain, &jit);
  EXPECT_GE(jit.stats.values_boxed, 4u);
  EXPECT_EQ(jit.stats.conversion_errors, 0u);
}

// ---------------------------------------------------------------------------
// A merge of unequal kinds is not compiled raw
// ---------------------------------------------------------------------------

TEST(JitFloats, AMergeOfUnequalKindsIsCompiledWithNoRawSlotAndAMergeOfEqualKindsStaysRaw) {
  GLTANG_REQUIRE_JIT_BACKEND();
  struct Case {
    const char * name;
    const char * source;
    bool raw;
  };
  const Case cases[] = {
      {"a local first assigned a float inside a loop",
       "function f(n) { i = 0; while (i < n) { x = 1.5; i = i + 1; } print(x); return i; }\nprint(f(3)); print(f(0));\n", false},
      {"one arm float and the other an integer",
       "function f(c, n) { if (c) { x = 1.5; } else { x = 2; } print(x); return n; }\nprint(f(true, 1)); print(f(false, 1));\n", false},
      {"a float before the loop and a float in it",
       "function f(n) { acc = 0.0; i = 0; while (i < n) { acc = 1.5; i = i + 1; } print(acc); return i; }\nprint(f(3)); print(f(0));\n", true},
      {"a float kept across both arms",
       "function f(c, n) { x = 2.5; if (c) { i = 1; } else { i = 2; } j = 0; while (j < 1) { j = j + 1; } print(x); return i + n; }\nprint(f(true, 1)); print(f(false, 1));\n", true},
  };
  for (const Case & c : cases) {
    Scenario sc;
    sc.source = c.source;
    Outcome plain, jit;
    expect_same_compiled(sc, &plain, &jit);
    Driver d(c.source, 1, false);
    ASSERT_TRUE(d.ok());
    ASSERT_TRUE(d.run());
    uint64_t f64 = 0, other = 0, max_site = 0, recorded = 0;
    ASSERT_EQ(gltang_vm_jit_test_converting(d.ctx->execution, &f64, &other, &max_site, &recorded), 1);
    EXPECT_EQ(other, 0u) << c.name;
    EXPECT_EQ(max_site, recorded) << c.name << ": the payload records the most converting locations at one site";
    if (c.raw) {
      EXPECT_GT(f64, 0u) << c.name << ": the kinds agree, so the float stays raw";
      EXPECT_GT(recorded, 0u) << c.name;
    }
    else {
      EXPECT_EQ(f64, 0u) << c.name << ": the kinds disagree, so no slot is raw anywhere in the function";
      EXPECT_EQ(recorded, 0u) << c.name;
    }
    EXPECT_EQ(d.ctx->raw(), jit.raw) << c.name;
    EXPECT_EQ(d.stats().compile_failures, 0u) << c.name;
    if (HasFailure()) {
      ADD_FAILURE() << "with: " << c.name;
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Chains, the reservation and the pool
// ---------------------------------------------------------------------------

namespace {

// Each top level here starts with kTopLevelLeaves (jit_harness.h): the functions are entered
// by the interpreter, whose entry polls see the reservation at zero, as they were before a top
// level ran to its end compiled (spec-template-compiled story 2).
const std::string kChain = std::string(kTopLevelLeaves) +
    // SMALL_MIN, and `m - 1` fails its guard on it (`m + 1` of SMALL_MAX did until
    // spec-template-compiled story 3 made a sum outside the range the interpreter's `+`,
    // called from compiled code with no exit).
    "M = (0 - 576460752303423487) - 1;\n"
    "function callee(n, m) { y = 2.5; i = 0; while (i < n) { i = i + 1; } z = m - 1; return i; }\n"
    "function caller(n, m) { x = 1.5; r = callee(n, m); s = 0; j = 0; while (j < 3) { j = j + 1; } return r; }\n"
    "print(caller(3, 1)); print(caller(3, M));\n";

}  // namespace

TEST(JitFloats, AGuardThatFailsAtTheBottomOfAChainBoxesEachFramesValuesOnceAndTheReservationBalances) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario ref;
  ref.source = kChain;
  Outcome plain, jit;
  expect_same_compiled(ref, &plain, &jit);
  Driver d(kChain, 1, true);
  ASSERT_TRUE(d.ok());
  std::vector<Seen> seen;
  d.script.on_poll = recorder(&d, &seen);
  ASSERT_TRUE(d.run());
  EXPECT_EQ(d.ctx->raw(), plain.raw);
  GLTANG_JitStats st = d.stats();
  EXPECT_GE(st.calls, 1u);
  EXPECT_GE(st.deepest_chain, 1u);
  EXPECT_EQ(st.values_boxed, 2u) << "the caller's x and the callee's y, once each, when the guard failed at the callee's `m - 1`";
  EXPECT_EQ(st.reservation_peak, 2u) << "the entry's extension and the callee's, summed";
  EXPECT_EQ(st.rebuild_failures, 0u);
  EXPECT_EQ(st.conversion_errors, 0u);
  // The capacity at the caller's polls is the caller's own, however many calls have returned
  // (index 2 is `caller`, 1 is `callee`), and at the callee's it is the sum.
  std::set<uint64_t> in_caller, in_callee;
  for (const Seen & s : seen) {
    (s.function == 2 ? in_caller : in_callee).insert(s.capacity);
  }
  // (The interpreter's own entry polls, before it enters compiled code, see zero.)
  EXPECT_EQ(in_caller, (std::set<uint64_t>{0, 1})) << "a returned call gave its extension back";
  EXPECT_EQ(in_callee, (std::set<uint64_t>{0, 2}));
  // After the deopt and the end of the run the reservation is at its base.
  EXPECT_EQ(d.boxes().capacity, 0u);
  EXPECT_EQ(d.boxes().extensions, 0u);
  EXPECT_EQ(d.boxes().pool, 0u) << "control is back with the host: the pool is let go";
  EXPECT_EQ(d.heap().invalid_pointers, 0u);
}

TEST(JitFloats, ManyCompiledCallsThatReturnLeaveTheReservationAtTheCallersAndTheWarmPoolAllocatesNothingPerCall) {
  GLTANG_REQUIRE_JIT_BACKEND();
  auto source = [](int calls) {
    return "function leaf(a) { y = 2.5; z = 3.5; return a + 1; }\n"
           "function mid(n) { x = 1.5; s = 0; i = 0; while (i < n) { s = s + leaf(i); i = i + 1; } return s; }\n"
           + std::string(kTopLevelLeaves) + "print(mid(" + std::to_string(calls) + "));\n";  // mid entered by the interpreter
  };
  uint64_t allocations[2] = {0, 0};
  const int counts[2] = {50, 800};
  for (int which = 0; which < 2; ++which) {
    Driver d(source(counts[which]), 1, true);
    ASSERT_TRUE(d.ok());
    std::vector<Seen> seen;
    d.script.on_poll = recorder(&d, &seen);
    ASSERT_TRUE(d.run());
    EXPECT_GE(d.stats().calls, static_cast<uint64_t>(counts[which]));
    std::set<uint64_t> in_mid;
    for (const Seen & s : seen) {
      if (s.function == 2) {
        in_mid.insert(s.capacity);
      }
    }
    EXPECT_EQ(in_mid, (std::set<uint64_t>{0, 1})) << "mid holds one raw float; each returned call to leaf gave back its two";
    EXPECT_EQ(d.stats().values_boxed, 0u);
    EXPECT_EQ(d.stats().reservation_peak, 3u);
    allocations[which] = d.heap().allocations;
  }
  EXPECT_EQ(allocations[0], allocations[1]) << "a retract keeps the boxes: calls after the first allocate nothing";
}

TEST(JitFloats, ANestedEntryThroughANativeExtendsTheReservationAndGivesItBack) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const std::string source =
      "function leaf(a) { y = 2.5; i = 0; while (i < 3) { i = i + 1; } return a + 1; }\n"
      "function run() { use reenter; x = 1.5; s = 0; i = 0; while (i < 6) { s = s + reenter(leaf, i); i = i + 1; } return s; }\n" +
      std::string(kTopLevelLeaves) + "print(run());\n";  // run entered by the interpreter
  Scenario sc;
  sc.source = source;
  sc.before = [](Context & c) { c.add_native_library(); };
  Outcome plain, jit;
  expect_same_compiled(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  Driver d(source, 1, true, 0, 0, 0, true);
  ASSERT_TRUE(d.ok());
  std::vector<Seen> seen;
  d.script.on_poll = recorder(&d, &seen);
  ASSERT_TRUE(d.run());
  EXPECT_EQ(d.ctx->raw(), plain.raw);
  std::set<uint64_t> in_run, in_leaf;
  for (const Seen & s : seen) {
    if (s.function == 2) {
      in_run.insert(s.capacity);
    }
    else if (s.function == 1) {
      in_leaf.insert(s.capacity);
    }
  }
  EXPECT_EQ(in_run, (std::set<uint64_t>{0, 1})) << "back in the outer run after each nested one, the reservation is the outer's own";
  EXPECT_EQ(in_leaf, (std::set<uint64_t>{1, 2})) << "inside the nested entry it is the outer's (the nested run's entry poll, made by the interpreter) and then the outer's and the nested entry's";
  EXPECT_EQ(d.boxes().capacity, 0u);
  EXPECT_EQ(d.boxes().extensions, 0u);
  EXPECT_EQ(d.stats().conversion_errors, 0u);
}

TEST(JitFloats, ACompiledCallWhoseCalleeHoldsNoFloatDoesNoFloatBookkeepingAtAll) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The regression guard of the call path: a push and a pop of a callee with no raw
  // float must cost one load and one compare, so across fib(15), every call of which is a
  // compiled call, nothing is looked up, recorded or made. Counted, never timed.
  Scenario sc;
  sc.source = "function fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } print(fib(15));";
  Outcome plain, jit;
  expect_same_compiled(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "610");
  EXPECT_GE(jit.stats.calls, 1000u) << "the calls were compiled calls";
  EXPECT_EQ(jit.stats.extension_lookups, 0u) << "no callee was looked up";
  EXPECT_EQ(jit.stats.extension_records, 0u) << "no extension was recorded";
  EXPECT_EQ(jit.stats.box_pool_made + jit.stats.reservation_peak + jit.stats.values_boxed, 0u);
  // The control: a callee that holds a float is looked up, and its extension is recorded.
  Scenario floats;
  floats.source = "function leaf(a) { y = 2.5; return a + 1; }\nfunction mid(n) { s = 0; i = 0; while (i < n) { s = s + leaf(i); i = i + 1; } return s; }\nprint(mid(30));\n";
  Outcome fplain, fjit;
  expect_same_compiled(floats, &fplain, &fjit);
  EXPECT_GT(fjit.stats.extension_lookups, 0u);
  EXPECT_GT(fjit.stats.extension_records, 0u);
}

TEST(JitFloats, AFloatFreeProgramExtendsByZeroAndMakesNoBox) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Driver d(
      "function leaf(a) { return a + 1; }\nfunction run(n) { s = 0; i = 0; while (i < n) { s = s + leaf(i); i = i + 1; } return s; }\nprint(run(300));\n",
      1, true);
  ASSERT_TRUE(d.ok());
  std::vector<Seen> seen;
  d.script.on_poll = recorder(&d, &seen);
  ASSERT_TRUE(d.run());
  GLTANG_JitStats st = d.stats();
  EXPECT_GE(st.calls, 300u);
  EXPECT_EQ(st.box_pool_made, 0u);
  EXPECT_EQ(st.values_boxed, 0u);
  EXPECT_EQ(st.reservation_peak, 0u);
  EXPECT_EQ(st.reservation_refusals + st.box_pool_refusals + st.conversion_errors, 0u);
  for (const Seen & s : seen) {
    EXPECT_EQ(s.capacity, 0u);
    EXPECT_EQ(s.pool, 0u);
  }
  uint64_t f64 = 1, other = 1, max_site = 1, recorded = 1;
  ASSERT_EQ(gltang_vm_jit_test_converting(d.ctx->execution, &f64, &other, &max_site, &recorded), 1);
  EXPECT_EQ(f64 + other + max_site + recorded, 0u) << "no converting location in the emitted code";
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

namespace {

const std::string kRefusal =
    "function leaf(a) { y = 2.5; return a + 1; }\n"
    "function mid(n) { x = 1.5; s = 0; i = 0; while (i < n) { s = s + leaf(i); i = i + 1; } return s; }\n" +
    std::string(kTopLevelLeaves) + "print(mid(5)); print(mid(5)); print(mid(5));\n";  // as kChain

}  // namespace

TEST(JitFloats, ARefusedExtensionAtAnEntryMeansTheFunctionIsNotEnteredThisTimeAndTheOutputIsTheInterpreters) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = kRefusal;
  Outcome plain, control;
  expect_same_compiled(sc, &plain, &control);
  sc.before = [](Context & c) { gltang_vm_set_float_test_switches_unchecked(c.execution, 1, 0); };
  Outcome refused = run(sc, 1);
  ASSERT_TRUE(refused.created);
  EXPECT_EQ(refused.key(), plain.key());
  EXPECT_EQ(refused.stats.reservation_refusals, 1u);
  EXPECT_LT(refused.stats.calls, control.stats.calls) << "the first call of mid was left to the interpreter, so its calls of leaf were not between compiled functions";
  EXPECT_EQ(refused.stats.conversion_errors, 0u);
  EXPECT_EQ(refused.stats.hook_argument_errors, 0u);
}

TEST(JitFloats, ARefusedExtensionOrTopUpAtAPushIsAnExitAtTheCallTheInterpreterMakesWithNothingHalfPushed) {
  GLTANG_REQUIRE_JIT_BACKEND();
  for (int pool_refusal = 0; pool_refusal < 2; ++pool_refusal) {
    Scenario sc;
    sc.source = kRefusal;
    Outcome plain = run(sc, 0);
    // The first nonzero extension is `mid`'s entry; the second is `leaf`'s push.
    sc.before = [pool_refusal](Context & c) {
      gltang_vm_set_float_test_switches_unchecked(c.execution, pool_refusal ? 0 : 2, pool_refusal ? 2 : 0);
    };
    Outcome refused = run(sc, 1);
    ASSERT_TRUE(refused.created);
    EXPECT_EQ(refused.key(), plain.key()) << (pool_refusal ? "a pool refusal" : "a reservation refusal");
    EXPECT_EQ(pool_refusal ? refused.stats.box_pool_refusals : refused.stats.reservation_refusals, 1u);
    EXPECT_GE(refused.stats.call_exits_push_refused, 1u) << "the call site exited and the interpreter made the call";
    EXPECT_EQ(refused.stats.rebuild_failures, 0u);
    EXPECT_EQ(refused.stats.hook_argument_errors, 0u);
    EXPECT_EQ(refused.native_depth, 0u);
  }
}

TEST(JitFloats, AnAllocationThatFailsAnywhereInARunWithFloatsNeverBreaksAFrameOrTheOutputAndLeavesTheReservationAtZero) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario plain_sc;
  plain_sc.source = kChain;
  Outcome plain = run(plain_sc, 0);
  Driver clean(kChain, 1, false);
  ASSERT_TRUE(clean.ok());
  ASSERT_TRUE(clean.run());
  const long total = clean.ctx->tracker.calls;
  ASSERT_GT(total, 50);
  long finished = 0, entry_refused = 0, push_refused = 0, rebuilt_after_failure = 0;
  for (long n = 1; n <= total; n += (total > 400 ? total / 400 : 1)) {
    Driver d(kChain, 1, false, 0, 0, n);
    if (!d.ok()) {
      continue;  // the context could not even be made: nothing to run
    }
    const bool done = d.run();
    GLTANG_JitStats st = d.stats();
    EXPECT_EQ(st.conversion_errors, 0u) << "failing allocation " << n;
    EXPECT_EQ(st.hook_argument_errors, 0u) << "failing allocation " << n;
    EXPECT_EQ(st.rebuild_failures, 0u) << "failing allocation " << n << ": a refused extension is an exit, never a failed rebuild";
    EXPECT_EQ(d.heap().invalid_pointers, 0u) << "failing allocation " << n;
    // Whatever failed, control is back with the host: nothing is extended and no box is held.
    Driver::Boxes b = d.boxes();
    EXPECT_EQ(b.capacity, 0u) << "failing allocation " << n;
    EXPECT_EQ(b.extensions, 0u) << "failing allocation " << n;
    EXPECT_EQ(b.pool, 0u) << "failing allocation " << n;
    finished += done;
    // An allocation the guest makes that fails is an error value (and not always one in
    // the error list), so the output of this sweep is not compared: the sweep over the
    // refusals alone, below, compares it.
    (void)done;
    const uint64_t refusals = st.reservation_refusals + st.box_pool_refusals;
    push_refused += refusals > 0 && st.call_exits_push_refused > 0;
    entry_refused += refusals > 0 && st.call_exits_push_refused == 0;
    rebuilt_after_failure += d.ctx->tracker.fired && st.values_boxed > 0;
  }
  EXPECT_GT(finished, 0);
  EXPECT_GT(entry_refused, 0) << "a failure was seen at an entry";
  EXPECT_GT(push_refused, 0) << "a failure was seen at a push";
  EXPECT_GT(rebuilt_after_failure, 0) << "a failure was seen in a run that went on to rebuild frames";
}

TEST(JitFloats, RefusingEveryExtensionAndEveryTopUpInTurnGivesTheInterpretersOutputEachTime) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario plain_sc;
  plain_sc.source = kChain;
  Outcome plain = run(plain_sc, 0);
  Driver clean(kChain, 1, false);
  ASSERT_TRUE(clean.ok());
  ASSERT_TRUE(clean.run());
  EXPECT_EQ(clean.ctx->raw(), plain.raw);
  const uint64_t extensions = clean.stats().reservation_peak;  // at least the entries and pushes there were
  ASSERT_GT(extensions, 0u);
  long refused_entries = 0, refused_pushes = 0;
  for (int pool = 0; pool < 2; ++pool) {
    for (uint64_t nth = 1; nth <= 8; ++nth) {
      Driver d(kChain, 1, false);
      ASSERT_TRUE(d.ok());
      gltang_vm_set_float_test_switches_unchecked(d.ctx->execution, pool ? 0 : nth, pool ? nth : 0);
      ASSERT_TRUE(d.run()) << "refusing " << (pool ? "top-up " : "extension ") << nth;
      EXPECT_EQ(d.ctx->raw(), plain.raw) << "refusing " << (pool ? "top-up " : "extension ") << nth;
      GLTANG_JitStats st = d.stats();
      EXPECT_EQ(st.rebuild_failures + st.conversion_errors + st.hook_argument_errors, 0u);
      EXPECT_EQ(d.boxes().capacity + d.boxes().extensions + d.boxes().pool, 0u);
      refused_entries += st.reservation_refusals + st.box_pool_refusals > 0 && st.call_exits_push_refused == 0;
      refused_pushes += st.call_exits_push_refused > 0;
    }
  }
  EXPECT_GT(refused_entries, 0);
  EXPECT_GT(refused_pushes, 0);
}

// ---------------------------------------------------------------------------
// The descriptor: convert and reverse, bit for bit
// ---------------------------------------------------------------------------

namespace {

const uint64_t kInterestingBits[] = {
    0x0000000000000000ull,  // +0
    0x8000000000000000ull,  // -0
    0x0000000000000001ull,  // the smallest subnormal
    0x000fffffffffffffull,  // the largest subnormal
    0x8000000000000001ull,  // a negative subnormal
    0x0010000000000000ull,  // the smallest normal
    0x7fefffffffffffffull,  // the largest finite
    0xffefffffffffffffull,
    0x7ff0000000000000ull,  // +inf
    0xfff0000000000000ull,  // -inf
    0x7ff8000000000000ull,  // the default quiet NaN
    0xfff8000000000000ull,  // the same with its sign set
    0x7ff8000000001234ull,  // a quiet NaN with a payload
    0xfff8000000005678ull,
    0x7ff0000000000001ull,  // a signalling NaN
    0x3ff0000000000000ull,  // 1
    0x400921fb54442d18ull,  // pi
};

GRCORE_CodeLocation f64_constant(uint64_t bits) {
  GRCORE_CodeLocation l = {};
  l.kind = GRCORE_LOC_CONSTANT;
  l.slot_kind = GRCORE_SLOT_RAW;
  l.value = static_cast<int64_t>(bits);
  l.representation = GRCORE_REPR_F64;
  return l;
}

GRCORE_CodeLocation frame_slot(int64_t offset, GRCORE_Representation representation) {
  GRCORE_CodeLocation l = {};
  l.kind = GRCORE_LOC_FRAME_SLOT;
  l.slot_kind = GRCORE_SLOT_RAW;
  l.value = offset;
  l.representation = representation;
  return l;
}

GRCORE_CodeSite site_of(const std::vector<GRCORE_CodeLocation> & locations) {
  GRCORE_CodeSite s = {};
  s.kind = GRCORE_SITE_GUARD;
  s.frame_state = locations.data();
  s.frame_state_count = locations.size();
  return s;
}

/// A finished execution, parked and owned by this thread, with a compiled float function behind it.
struct Parked {
  Driver d;
  Parked() : d("function f(n) { x = 1.5; i = 0; while (i < n) { i = i + 1; } return i; }\nprint(f(10));\n", 1, false) {
    EXPECT_TRUE(d.ok());
    EXPECT_TRUE(d.run());
  }
  GLTANG_Execution * exec() { return d.ctx->execution; }
  GRCORE_Context * context() { return d.ctx->context; }
  GRCORE_EngineId engine() { return gltang_vm_test_engine_id(d.ctx->execution); }
};

}  // namespace

TEST(JitFloats, ConvertAndReverseKeepEveryBitOfTheDoubleBothWays) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Parked p;
  const size_t count = sizeof(kInterestingBits) / sizeof(kInterestingBits[0]);
  ASSERT_TRUE(gltang_vm_jit_test_extend(p.exec(), count));
  std::vector<GRCORE_CodeLocation> locations;
  for (uint64_t b : kInterestingBits) {
    locations.push_back(f64_constant(b));
  }
  GRCORE_CodeSite site = site_of(locations);
  std::vector<uint64_t> slots(count, 0xdeadbeefull);
  uint64_t frame_base = 0;
  const uint64_t boxed_before = p.d.stats().values_boxed;
  const uint64_t allocations = p.d.heap().allocations;
  const uint64_t collections = p.d.heap().collections;
  ASSERT_EQ(grcore_deopt_rebuild(p.context(), p.engine(), &site, &frame_base, gltang_vm_jit_test_reservation(p.exec()), slots.data(), count), GRCORE_OK);
  EXPECT_EQ(p.d.heap().allocations, allocations) << "a conversion draws on the pool and allocates nothing";
  EXPECT_EQ(p.d.heap().collections, collections);
  EXPECT_EQ(p.d.stats().values_boxed, boxed_before + count);
  std::set<uint64_t> distinct;
  for (size_t i = 0; i < count; ++i) {
    uint64_t bits = ~kInterestingBits[i];
    ASSERT_TRUE(gltang_vm_test_float_bits(slots[i], &bits)) << i;
    EXPECT_EQ(bits, kInterestingBits[i]) << "convert, value " << i;
    distinct.insert(slots[i]);
  }
  EXPECT_EQ(distinct.size(), count) << "one box for each, never shared";
  // And back, through a checked write-back: the bits survive exactly.
  std::vector<GRCORE_CodeLocation> back;
  for (size_t i = 0; i < count; ++i) {
    back.push_back(frame_slot(static_cast<int64_t>(8 * i), GRCORE_REPR_F64));
  }
  GRCORE_CodeSite back_site = site_of(back);
  std::vector<uint64_t> frame(count, 0xAAAAAAAAAAAAAAAAull);
  GRCORE_DeoptOutcome outcome = GRCORE_DEOPT_EXIT_AT_SITE;
  size_t misfit = 0;
  ASSERT_EQ(grcore_deopt_write_back_checked(p.context(), p.engine(), &back_site, frame.data(), slots.data(), count, &outcome, &misfit), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_DEOPT_WRITTEN);
  EXPECT_EQ(misfit, SIZE_MAX);
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(frame[i], kInterestingBits[i]) << "reverse, value " << i;
  }
  gltang_vm_jit_test_retract_all(p.exec());
}

TEST(JitFloats, ReverseFitsExactlyAFloatAndAFailedWriteBackExitsAtTheSiteWritingNothingForEveryOtherValue) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Driver d(
      "function k() { return 1; }\n"
      "function f(n) { i3 = 3; ibig = 576460752303423488; s = \"text\"; t = true; z = null; arr = [1, 2]; err = 1 / 0; fn = k; fl = 3.0; i = 0;\n"
      "  while (i < n) { i = i + 1; } return i; }\n"
      "print(f(100));\n",
      0, true);  // the interpreter: its frames hold boxed values
  ASSERT_TRUE(d.ok());
  d.script.on_poll = [](Script & script, GRCORE_Context *, uint64_t n) {
    if (n == 12) {
      script.post(GRCORE_REQUEST_INTERRUPT);
    }
  };
  EXPECT_FALSE(d.run());
  ASSERT_TRUE(d.ctx->paused());
  std::vector<std::pair<std::string, uint64_t>> words;
  {
    GRCORE_FrameWalk walk;
    ASSERT_EQ(grcore_frame_walk_begin(d.ctx->context, &walk), GRCORE_OK);
    GRCORE_AbstractFrame frame;
    ASSERT_TRUE(grcore_frame_walk_next(&walk, &frame));
    for (size_t s = 0; s < grcore_frame_scope_count(&frame); ++s) {
      GRCORE_ScopeInfo info;
      if (grcore_frame_scope(&frame, s, &info) != GRCORE_OK || info.kind != GRCORE_SCOPE_LOCAL) {
        continue;
      }
      for (size_t i = 0; i < info.variable_count; ++i) {
        GRCORE_Variable variable;
        if (grcore_frame_variable(&frame, s, i, &variable) == GRCORE_OK && variable.name) {
          words.emplace_back(variable.name, variable.value);
        }
      }
    }
  }
  auto word = [&](const char * name) {
    for (const auto & w : words) {
      if (w.first == name) {
        return w.second;
      }
    }
    ADD_FAILURE() << "no local named " << name;
    return uint64_t{0};
  };
  const GRCORE_EngineId engine = gltang_vm_test_engine_id(d.ctx->execution);
  auto offer = [&](uint64_t value, GRCORE_Representation representation, GRCORE_DeoptOutcome * outcome, uint64_t * frame_word) {
    std::vector<GRCORE_CodeLocation> locations = {frame_slot(0, representation)};
    GRCORE_CodeSite site = site_of(locations);
    *frame_word = 0xAAAAAAAAAAAAAAAAull;
    size_t misfit = 0;
    EXPECT_EQ(grcore_deopt_write_back_checked(d.ctx->context, engine, &site, frame_word, &value, 1, outcome, &misfit), GRCORE_OK);
    EXPECT_EQ(misfit, *outcome == GRCORE_DEOPT_WRITTEN ? SIZE_MAX : size_t{0});
  };
  GRCORE_DeoptOutcome outcome;
  uint64_t frame_word = 0;
  // A float fits: the 3. does, and its bits arrive exactly.
  offer(word("fl"), GRCORE_REPR_F64, &outcome, &frame_word);
  EXPECT_EQ(outcome, GRCORE_DEOPT_WRITTEN);
  EXPECT_EQ(frame_word, bits_of(3.0));
  // Nothing else does, and the frame is as it was.
  const char * misfits[] = {"i3", "ibig", "s", "t", "z", "arr", "err", "fn"};
  for (const char * name : misfits) {
    offer(word(name), GRCORE_REPR_F64, &outcome, &frame_word);
    EXPECT_EQ(outcome, GRCORE_DEOPT_EXIT_AT_SITE) << name << " is not a float";
    EXPECT_EQ(frame_word, 0xAAAAAAAAAAAAAAAAull) << name << ": nothing was written";
  }
  // The word zero is not a float either, nor is a tagged word that names no object.
  offer(0, GRCORE_REPR_F64, &outcome, &frame_word);
  EXPECT_EQ(outcome, GRCORE_DEOPT_EXIT_AT_SITE);
  // False for the representations this engine does not have, and counted.
  const uint64_t errors_before = d.stats().conversion_errors;
  for (GRCORE_Representation r : {GRCORE_REPR_I32, GRCORE_REPR_I64, GRCORE_REPR_F32}) {
    offer(word("fl"), r, &outcome, &frame_word);
    EXPECT_EQ(outcome, GRCORE_DEOPT_EXIT_AT_SITE) << "representation " << r;
    EXPECT_EQ(frame_word, 0xAAAAAAAAAAAAAAAAull);
  }
  EXPECT_EQ(d.stats().conversion_errors, errors_before + 3u);
  // All or nothing: a float and a misfit in one site write neither.
  {
    std::vector<GRCORE_CodeLocation> locations = {frame_slot(0, GRCORE_REPR_F64), frame_slot(8, GRCORE_REPR_F64)};
    GRCORE_CodeSite site = site_of(locations);
    uint64_t frame[2] = {0xAAAAAAAAAAAAAAAAull, 0xAAAAAAAAAAAAAAAAull};
    uint64_t values[2] = {word("fl"), word("i3")};
    size_t misfit = 0;
    ASSERT_EQ(grcore_deopt_write_back_checked(d.ctx->context, engine, &site, frame, values, 2, &outcome, &misfit), GRCORE_OK);
    EXPECT_EQ(outcome, GRCORE_DEOPT_EXIT_AT_SITE);
    EXPECT_EQ(misfit, 1u);
    EXPECT_EQ(frame[0], 0xAAAAAAAAAAAAAAAAull) << "the float that did fit was not written either";
    EXPECT_EQ(frame[1], 0xAAAAAAAAAAAAAAAAull);
  }
  EXPECT_TRUE(d.ctx->resume());
}

TEST(JitFloats, ConvertForARepresentationItDoesNotHaveWritesNullAndCountsAnInternalError) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Parked p;
  ASSERT_TRUE(gltang_vm_jit_test_extend(p.exec(), 3));
  GRCORE_CodeLocation i64 = f64_constant(7);
  i64.representation = GRCORE_REPR_I64;
  GRCORE_CodeLocation i32 = i64;
  i32.representation = GRCORE_REPR_I32;
  GRCORE_CodeLocation f32 = i64;
  f32.representation = GRCORE_REPR_F32;
  std::vector<GRCORE_CodeLocation> locations = {i64, i32, f32};
  GRCORE_CodeSite site = site_of(locations);
  uint64_t slots[3] = {1, 1, 1};
  uint64_t frame_base = 0;
  const uint64_t pool = p.d.boxes().pool;
  ASSERT_EQ(grcore_deopt_rebuild(p.context(), p.engine(), &site, &frame_base, gltang_vm_jit_test_reservation(p.exec()), slots, 3), GRCORE_OK);
  for (uint64_t s : slots) {
    EXPECT_EQ(s, 0u) << "null, never a raw word";
  }
  EXPECT_EQ(p.d.stats().conversion_errors, 3u);
  EXPECT_EQ(p.d.boxes().pool, pool) << "no box was taken";
  gltang_vm_jit_test_retract_all(p.exec());
}

TEST(JitFloats, TheDescriptorBindsCodeWithFloatLocationsAndADescriptorWithoutTheConversionsDoesNot) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Parked p;
  // Ours binds: the program's functions were compiled with raw float locations, and registered.
  EXPECT_GE(p.d.stats().functions_compiled, 1u);
  EXPECT_EQ(p.d.stats().compile_failures, 0u);
  uint64_t f64 = 0, other = 0, max_site = 0, recorded = 0;
  ASSERT_EQ(gltang_vm_jit_test_converting(p.exec(), &f64, &other, &max_site, &recorded), 1);
  EXPECT_GT(f64, 0u);
  // A hand-made table with an F64 location.
  std::vector<GRCORE_CodeLocation> locations = {frame_slot(-8, GRCORE_REPR_F64)};
  GRCORE_CodeSite site = site_of(locations);
  site.code_offset = 4;
  GRCORE_CodeMeta meta = {};
  meta.version = GRCORE_CODEMETA_FORMAT_VERSION;
  meta.frame_bytes = 16;
  meta.code_bytes = 16;
  meta.site_count = 1;
  meta.sites = &site;
  const char * reason = nullptr;
  EXPECT_EQ(grcore_deopt_bind(p.context(), p.engine(), &meta, &reason), GRCORE_OK) << (reason ? reason : "");
  // The control: a descriptor with no `convert` or `reverse` refuses the same table.
  static const GRCORE_EngineDescriptor none = GRCORE_ENGINE_DESCRIPTOR_INIT(
      "no conversions", nullptr, nullptr, nullptr, {nullptr, nullptr, nullptr}, {0, 0, 0}, nullptr, nullptr, nullptr, nullptr);
  GRCORE_EngineId other_engine = 0;
  ASSERT_EQ(grcore_engine_register(p.context(), &none, &other_engine), GRCORE_OK);
  reason = nullptr;
  EXPECT_EQ(grcore_deopt_bind(p.context(), other_engine, &meta, &reason), GRCORE_ERR_UNSUPPORTED);
  EXPECT_NE(reason, nullptr);
}

// ---------------------------------------------------------------------------
// What the code and the frames say about a raw float
// ---------------------------------------------------------------------------

TEST(JitFloats, TheEmittedMetadataOfAFloatProgramHasOnlyRawF64ConvertingLocationsAndNoDerivedPointer) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Driver d(
      "function f(a, b) { x = 1.5; s = 0; i = 0; while (i < a) { s = s + g(i, b); i = i + 1; } print(x); return s; }\n"
      "function g(x, y) { z = 2.5; if (x < y) { return x; } return y; }\nprint(f(10, 5)); print(f(10, 7));",
      1, false);
  ASSERT_TRUE(d.ok());
  ASSERT_TRUE(d.run());
  uint64_t sites = 0, derived = 0, converting = 0;
  ASSERT_EQ(gltang_vm_jit_test_metadata(d.ctx->execution, &sites, &derived, &converting), 1);
  EXPECT_GT(sites, 10u);
  EXPECT_EQ(derived, 0u);
  EXPECT_GT(converting, 0u) << "the float twin of the float-free control, which has none";
  uint64_t f64 = 0, other = 0, max_site = 0, recorded = 0;
  ASSERT_EQ(gltang_vm_jit_test_converting(d.ctx->execution, &f64, &other, &max_site, &recorded), 1);
  EXPECT_EQ(f64, converting) << "every converting location is a raw F64 on a raw slot";
  EXPECT_EQ(other, 0u);
  EXPECT_GE(max_site, 1u);
  EXPECT_EQ(max_site, recorded) << "the payload records the most at any one site";
  EXPECT_EQ(d.stats().compile_failures, 0u);
}

TEST(JitFloats, ARawLocalIsReportedAsRawBitsInTheAbstractFrameAndAsARawVariableNeverAsAValue) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Driver d("function f(n) { x = 1.5; i = 0; while (i < n) { i = i + 1; } return i; }\nprint(f(200));\n", 1, true);
  ASSERT_TRUE(d.ok());
  bool saw_slot = false, saw_variable = false;
  int checks = 0;
  d.script.on_poll = [&](Script &, GRCORE_Context * context, uint64_t n) {
    if (n < 5 || n > 8) {
      return;
    }
    GRCORE_FrameWalk walk;
    ASSERT_EQ(grcore_frame_walk_begin(context, &walk), GRCORE_OK);
    GRCORE_AbstractFrame frame;
    ASSERT_TRUE(grcore_frame_walk_next(&walk, &frame));
    ++checks;
    for (size_t i = 0; i < frame.slot_count; ++i) {
      GRCORE_SlotKind kind;
      uint64_t value;
      GRCORE_Representation representation;
      ASSERT_EQ(grcore_frame_slot_tagged(&frame, i, &kind, &value, &representation), GRCORE_OK);
      if (representation == GRCORE_REPR_F64) {
        EXPECT_EQ(kind, GRCORE_SLOT_RAW) << "a raw double is never a value";
        EXPECT_EQ(value, bits_of(1.5));
        saw_slot = true;
      }
    }
    for (size_t s = 0; s < grcore_frame_scope_count(&frame); ++s) {
      GRCORE_ScopeInfo info;
      ASSERT_EQ(grcore_frame_scope(&frame, s, &info), GRCORE_OK);
      for (size_t v = 0; v < info.variable_count; ++v) {
        GRCORE_Variable variable;
        ASSERT_EQ(grcore_frame_variable(&frame, s, v, &variable), GRCORE_OK);
        if (variable.name && std::string(variable.name) == "x") {
          EXPECT_EQ(variable.kind, GRCORE_SLOT_RAW) << "the local x, held raw, is reported as raw bits";
          EXPECT_EQ(variable.value, bits_of(1.5));
          saw_variable = true;
        }
      }
    }
  };
  ASSERT_TRUE(d.run());
  EXPECT_GT(checks, 0);
  EXPECT_TRUE(saw_slot);
  EXPECT_TRUE(saw_variable);
}

// ---------------------------------------------------------------------------
// The pool lives as long as a run does
// ---------------------------------------------------------------------------

TEST(JitFloats, ThePoolIsLetGoWhenControlReturnsToTheHostSoAPausedRunSnapshotsAndRestores) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const std::string source =
      "function leaf(a) { y = 2.5; return a + 1; }\n"
      "function mid(n) { x = 1.5; s = 0; i = 0; while (i < n) { s = s + leaf(i); i = i + 1; } return s; }\n"
      "print(mid(200)); print(mid(300));\n";
  Scenario ref;
  ref.source = source;
  Outcome plain = run(ref, 0);
  Driver d(source, 1, true);
  ASSERT_TRUE(d.ok());
  std::vector<Seen> seen;
  d.script.on_poll = recorder(&d, &seen, [](Script & script, uint64_t n) {
    if (n == 120) {
      script.post(GRCORE_REQUEST_INTERRUPT);
    }
  });
  EXPECT_FALSE(d.run());
  ASSERT_TRUE(d.ctx->paused());
  ASSERT_FALSE(seen.empty());
  EXPECT_GT(seen.back().pool, 0u) << "at the pausing poll the pool held boxes, warm from the calls before it";
  EXPECT_GT(d.stats().box_pool_made, 0u);
  // With control at the host: nothing is extended and no box is held.
  Driver::Boxes b = d.boxes();
  EXPECT_EQ(b.pool, 0u);
  EXPECT_EQ(b.capacity, 0u) << "a pause rebuilt every compiled frame and gave every extension back";
  EXPECT_EQ(b.extensions, 0u);
  // So the heap image has the root slots a fresh destination reports, and the restore succeeds.
  GLTANG_Snapshot * snapshot = nullptr;
  ASSERT_EQ(gltang_snapshot_take(d.ctx->execution, &snapshot), GLTANG_OK);
  Config config;
  config.jit_threshold = 1;
  Context destination(d.page.program, config);
  ASSERT_TRUE(destination.ok());
  EXPECT_EQ(gltang_snapshot_restore(destination.execution, snapshot), GLTANG_OK) << "a pool still rooted would have put extra slots in the image";
  gltang_snapshot_release(snapshot);
  EXPECT_TRUE(destination.resume());
  EXPECT_EQ(destination.raw(), plain.raw);
  EXPECT_TRUE(d.ctx->resume());
  EXPECT_EQ(d.ctx->raw(), plain.raw);
}

TEST(JitFloats, AFailedStackPushAfterASuccessfulExtensionGivesTheExtensionBack) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = kRefusal;
  Outcome plain = run(sc, 0);
  Driver d(kRefusal, 1, false);
  ASSERT_TRUE(d.ok());
  gltang_vm_set_stack_push_failure_unchecked(d.ctx->execution, 1);
  ASSERT_TRUE(d.run());
  EXPECT_EQ(d.ctx->raw(), plain.raw) << "the call exited and the interpreter made it";
  uint64_t capacity = 99, extensions = 99, count = 0;
  gltang_vm_jit_test_failed_push(d.ctx->execution, &capacity, &extensions, &count);
  EXPECT_EQ(count, 1u) << "the refusal happened";
  EXPECT_EQ(capacity, 1u) << "right after it, the reservation held the caller's own extension and not the callee's";
  EXPECT_EQ(extensions, 1u);
  EXPECT_GE(d.stats().call_exits_push_refused, 1u);
  EXPECT_EQ(d.boxes().capacity, 0u);
  EXPECT_EQ(d.boxes().extensions, 0u);
}

TEST(JitFloats, AFailedRebuildLeavesNoRawWordInAGuestFrameWhereACollectionCanReadIt) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The injected failure of the deopt hook's rebuild, under torture and verify: the
  // run ends unwound; the seam checks the guest frames the failed rebuild left (a
  // collection would not: a guest frame paired with a compiled one is not scanned), and
  // none may hold a raw double in a value slot.
  // The first exit of the run is the top level's, at the declaration of f. The seam is
  // switched on at f's third poll, so the exit it refuses is f's own, at the `print`,
  // with the local x and the operand raw.
  Driver d("function f(n) { x = 1.5; i = 0; while (i < n) { i = i + 1; } print(x); return i; }\nr = f(5);\n", 1, true, 1, 1);
  ASSERT_TRUE(d.ok());
  int in_f = 0;
  d.script.on_poll = [&](Script &, GRCORE_Context * context, uint64_t) {
    GRCORE_PollIdentity id = {0, 0};
    (void)grcore_context_poll_identity(context, &id);
    if ((id.function & 0xFFFFFFFFu) == 1u && ++in_f == 3) {
      d.ctx->apply_jit_switches(false, true, true);
    }
  };
  EXPECT_FALSE(d.run());
  GLTANG_JitStats st = d.stats();
  EXPECT_EQ(st.rebuild_failures, 1u);
  EXPECT_EQ(st.values_boxed, 0u) << "nothing was converted";
  EXPECT_EQ(d.heap().invalid_pointers, 0u) << "and no raw word was where the collector reads values";
  EXPECT_EQ(gltang_vm_jit_test_stray_words(d.ctx->execution), 0u) << "nor in any value slot of a guest frame, which the unwind is about to pop";
  EXPECT_EQ(d.boxes().capacity, 0u);
  EXPECT_EQ(d.boxes().extensions, 0u);
}

// ---------------------------------------------------------------------------
// The compiler's tables
// ---------------------------------------------------------------------------

TEST(JitFloats, EveryOpcodeIsInBothOfTheCompilersKindTablesOrIsNotInline) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The instructions that read no stack operand, or only move one (reads_raw says no),
  // and those that flow on with a known effect on the kinds (apply_kinds says yes). Story 7 adds the
  // operations that compute with a raw operand: they are not exits either (reads_raw says no), and
  // they have a known effect on the kinds (a float result stays raw). `%` and a cast to bool (the hook
  // asks with cast type 2) are exits with a raw operand, which is what the policy shows for them.
  // The output operations (spec-template-compiled story 1) flow on: `PRINT_CONST` and `CLEAR_RESULT`
  // read no operand, and `PRINT` of a raw float is the designed exit (its text is the interpreter's).
  // Story 2: `HALT` reads no operand (it returns); `STORE_GLOBAL`, `SET_RESULT` and `DISCARD` flow on,
  // and with a raw operand each is the designed box exit, as `PRINT` is.
  // Story 3: `INDEX` and `SLICE` flow on (calls of the interpreter's own operations), and with a raw
  // operand each is the designed box exit. `ADD` stays in the first set: the policy's state names no
  // string constant beside the raw operands, and `"x" + f` is the one `+` that is a box exit.
  // Story 4: `ARRAY`, `MAP` and `ADOPT` flow on (a literal's allocation and fill, the copy of a stored
  // container), and with a raw element or operand each is the designed box exit.
  const std::set<unsigned> go_on_with_a_raw_operand = {GLTANG_OP_POLL, GLTANG_OP_LINE, GLTANG_OP_POP, GLTANG_OP_DUP, GLTANG_OP_NULL, GLTANG_OP_TRUE, GLTANG_OP_FALSE,
      GLTANG_OP_CONST, GLTANG_OP_LOAD_LOCAL, GLTANG_OP_STORE_LOCAL, GLTANG_OP_LOAD_GLOBAL, GLTANG_OP_FUNC, GLTANG_OP_USE, GLTANG_OP_JMP,
      GLTANG_OP_PRINT_CONST, GLTANG_OP_CLEAR_RESULT, GLTANG_OP_HALT, GLTANG_OP_NEG, GLTANG_OP_ADD, GLTANG_OP_SUB, GLTANG_OP_MUL, GLTANG_OP_DIV, GLTANG_OP_LT, GLTANG_OP_LE, GLTANG_OP_GT, GLTANG_OP_GE,
      GLTANG_OP_EQ, GLTANG_OP_NE};
  const std::set<unsigned> flows = {GLTANG_OP_POLL, GLTANG_OP_LINE, GLTANG_OP_POP, GLTANG_OP_DUP, GLTANG_OP_NULL, GLTANG_OP_TRUE, GLTANG_OP_FALSE,
      GLTANG_OP_CONST, GLTANG_OP_LOAD_LOCAL, GLTANG_OP_STORE_LOCAL, GLTANG_OP_LOAD_GLOBAL, GLTANG_OP_FUNC, GLTANG_OP_USE, GLTANG_OP_JMP,
      GLTANG_OP_NEG, GLTANG_OP_NOT, GLTANG_OP_ATTR, GLTANG_OP_ADD, GLTANG_OP_SUB, GLTANG_OP_MUL, GLTANG_OP_DIV, GLTANG_OP_MOD, GLTANG_OP_LT,
      GLTANG_OP_LE, GLTANG_OP_GT, GLTANG_OP_GE, GLTANG_OP_EQ, GLTANG_OP_NE, GLTANG_OP_CAST, GLTANG_OP_CALL, GLTANG_OP_JMP_FALSE, GLTANG_OP_JMP_TRUE,
      GLTANG_OP_AND, GLTANG_OP_OR, GLTANG_OP_RET, GLTANG_OP_PRINT, GLTANG_OP_PRINT_CONST, GLTANG_OP_CLEAR_RESULT,
      GLTANG_OP_HALT, GLTANG_OP_STORE_GLOBAL, GLTANG_OP_SET_RESULT, GLTANG_OP_DISCARD, GLTANG_OP_INDEX, GLTANG_OP_SLICE,
      GLTANG_OP_ARRAY, GLTANG_OP_MAP, GLTANG_OP_ADOPT};
  for (unsigned op = 0; op < static_cast<unsigned>(GLTANG_OP_COUNT); ++op) {
    unsigned policy = gltang_vm_jit_test_opcode_policy(op);
    const bool reads = (policy & 1u) != 0;
    const bool known = (policy & 2u) != 0;
    EXPECT_EQ(reads, go_on_with_a_raw_operand.count(op) == 0) << "opcode " << op << ": with every operand raw, only a mover or an operation that computes with one may go on without an exit";
    EXPECT_EQ(known, flows.count(op) != 0) << "opcode " << op << ": a stack effect on the kinds is known for exactly the instructions that flow on";
  }
}

TEST(JitFloats, AFunctionWhoseBeginningIsAFloatAndWhoseMergeConflictsIsCompiledEnteredAndHasNoRawSlot) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // Every statement begins with a LINE, so "the first operation leaves compiled code at
  // once" (the decline) cannot be reached from source with a float constant: the function
  // is compiled and entered whether or not its merge conflicts, and then holds no raw slot.
  const char * source =
      "function f(c, n) { x = 1.5; if (c) { y = 1.5; } else { y = 2; } print(y); return n; }\n"
      "print(f(true, 1)); print(f(false, 2));\n";
  Scenario sc;
  sc.source = source;
  Outcome plain, jit;
  expect_same_compiled(sc, &plain, &jit);
  EXPECT_GE(jit.stats.functions_compiled, 1u);
  EXPECT_GE(jit.stats.entries, 1u) << "not declined";
  Driver d(source, 1, false);
  ASSERT_TRUE(d.ok());
  ASSERT_TRUE(d.run());
  uint64_t f64 = 1, other = 1, max_site = 1, recorded = 1;
  ASSERT_EQ(gltang_vm_jit_test_converting(d.ctx->execution, &f64, &other, &max_site, &recorded), 1);
  EXPECT_EQ(f64 + other + max_site + recorded, 0u) << "the conflict turned the float constants into exits";
  EXPECT_EQ(d.stats().values_boxed, 0u) << "so nothing in compiled code held a float";
}

// ---------------------------------------------------------------------------
// The push and pop hooks, taken one at a time
// ---------------------------------------------------------------------------

namespace {

/// A finished run whose function `f` (token program 0, function 1) holds a raw float, so its record says so.
struct HookRig {
  Driver d;
  uint64_t token = uint64_t{1};
  uint64_t args[2] = {(uint64_t{3} << 4) | 1u, 0};
  HookRig() : d("function f(n) { x = 1.5; i = 0; while (i < n) { i = i + 1; } return i; }\nprint(f(3));\n", 1, false) {
    EXPECT_TRUE(d.ok());
    EXPECT_TRUE(d.run());
    EXPECT_GE(d.stats().functions_compiled, 1u);
  }
  GLTANG_Execution * exec() { return d.ctx->execution; }
  int push() { return gltang_vm_jit_test_hook(exec(), exec(), 0, token, args, 2, 1); }
  void pop() { EXPECT_EQ(gltang_vm_jit_test_hook(exec(), exec(), 1, 0, nullptr, 0, 1), 0); }
};

}  // namespace

TEST(JitFloats, ARecordThatBelongsToAnotherFunctionIsNotGivenBackByAPopAndIsCounted) {
  GLTANG_REQUIRE_JIT_BACKEND();
  HookRig r;
  ASSERT_EQ(r.push(), 0);
  EXPECT_EQ(r.d.boxes().extensions, 1u) << "the callee holds a raw float: its push made a record";
  // The frame the pop will take off is another function's (the top level's): the record at this
  // depth is not its own.
  GRCORE_Stack * stack = grcore_context_stack(r.d.ctx->context);
  grcore_stack_slots(stack, grcore_stack_top(stack))[0] = 0;
  r.pop();
  EXPECT_EQ(r.d.stats().extension_mismatches, 1u);
  EXPECT_EQ(r.d.boxes().extensions, 1u) << "so the record stays, to be given back with the entry's";
  gltang_vm_jit_test_retract_all(r.exec());
  EXPECT_EQ(r.d.boxes().capacity + r.d.boxes().extensions, 0u);
  // The control: the same push and pop with its own frame gives back and counts nothing.
  ASSERT_EQ(r.push(), 0);
  r.pop();
  EXPECT_EQ(r.d.stats().extension_mismatches, 1u);
  EXPECT_EQ(r.d.boxes().capacity + r.d.boxes().extensions, 0u);
}

TEST(JitFloats, AFunctionMarkedNeverOrDiscardedMakesNoExtensionWhenPushedAgain) {
  GLTANG_REQUIRE_JIT_BACKEND();
  HookRig r;
  ASSERT_EQ(r.push(), 0);
  EXPECT_EQ(r.d.boxes().extensions, 1u) << "the control: while it is compiled, a push extends";
  r.pop();
  const uint64_t lookups = r.d.stats().extension_lookups;
  ASSERT_EQ(gltang_vm_jit_test_mark_never(r.exec(), r.token), 1);
  ASSERT_EQ(r.push(), 0);
  EXPECT_EQ(r.d.stats().extension_lookups, lookups + 1u) << "the push looked the record up...";
  EXPECT_EQ(r.d.boxes().extensions, 0u) << "...and found no raw floats in it";
  EXPECT_EQ(r.d.boxes().capacity, 0u);
  r.pop();
  // And the real discard: a float function whose integer subtraction fails its guard on every call (a real failure,
  // counted) is let go at the eighth. (A subtraction: a sum outside the range is the interpreter's `+` called from
  // compiled code since spec-template-compiled story 3, with no exit.)
  Driver d(
      "M = (0 - 576460752303423487) - 1;\nfunction f(n) { x = 1.5; return n - 1; }\n"
      "for (k = 0; k < 12; k += 1) { f(M); }\n",
      1, false);
  ASSERT_TRUE(d.ok());
  ASSERT_TRUE(d.run());
  ASSERT_GE(d.stats().functions_discarded, 1u);
  const uint64_t args[2] = {(uint64_t{3} << 4) | 1u, 0};
  ASSERT_EQ(gltang_vm_jit_test_hook(d.ctx->execution, d.ctx->execution, 0, uint64_t{1}, args, 2, 1), 0);
  EXPECT_EQ(d.boxes().extensions, 0u) << "a discarded function's record holds no count";
  EXPECT_EQ(gltang_vm_jit_test_hook(d.ctx->execution, d.ctx->execution, 1, 0, nullptr, 0, 1), 0);
}

// ---------------------------------------------------------------------------
// The flag that starts the bookkeeping turns on while calls are open
// ---------------------------------------------------------------------------

TEST(JitFloats, TheFirstFloatFunctionCompiledMidChainLeavesTheEarlierPushesWithNoRecordAndEveryPopGivesBackOnlyItsOwn) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // outer and mid hold no float; deep does, and is compiled at its first call, from compiled
  // code, while outer's entry and mid's push are open: the first nonzero extension is
  // deep's, made after two calls that looked nothing up.
  const std::string source =
      "function deep(n) { y = 2.5; i = 0; while (i < 3) { i = i + 1; } return n + 1; }\n"
      "function mid(n) { i = 0; while (i < 2) { i = i + 1; } return deep(n) + 1; }\n"
      "function outer(n) { s = 0; i = 0; while (i < n) { s = s + mid(i); i = i + 1; } return s; }\n"
      "print(outer(5));\n";
  Scenario sc;
  sc.source = source;
  Outcome plain, jit;
  expect_same_compiled(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  Driver d(source, 1, true);
  ASSERT_TRUE(d.ok());
  std::vector<Seen> seen;
  d.script.on_poll = recorder(&d, &seen);
  ASSERT_TRUE(d.run());
  EXPECT_EQ(d.ctx->raw(), plain.raw);
  GLTANG_JitStats st = d.stats();
  EXPECT_GE(st.calls, 10u);
  EXPECT_GT(st.extension_lookups, 0u) << "the flag turned on";
  EXPECT_LT(st.extension_lookups, st.calls) << "after at least one call that looked nothing up";
  EXPECT_EQ(st.extension_mismatches, 0u);
  EXPECT_EQ(st.extension_records, 5u) << "one for each of deep's five calls, and none for outer's entry or mid's pushes";
  std::set<uint64_t> in_outer, in_mid, in_deep;
  for (const Seen & x : seen) {
    if (x.function == 0) {
      continue;  // the top level's own poll
    }
    (x.function == 3 ? in_outer : x.function == 2 ? in_mid : in_deep).insert(x.capacity);
  }
  EXPECT_EQ(in_outer, std::set<uint64_t>{0}) << "every pop gave back only its own: the float-free ones had nothing to give";
  EXPECT_EQ(in_mid, std::set<uint64_t>{0});
  EXPECT_EQ(in_deep, std::set<uint64_t>{1}) << "deep's own extension, and no one else's";
  EXPECT_EQ(d.boxes().capacity + d.boxes().extensions + d.boxes().pool, 0u);
}

TEST(JitFloats, AnEntryExtensionIsNotTakenByThePopOfAFloatFreeCallAtTheSameDepth) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // outer holds a float, so its entry records an extension at level 0; mid holds none, and its
  // push, at level 1, is below deep's, at level 2. Each pop sees the entry's record at the top of
  // the records (before deep's, after it is given back) and must leave it alone.
  const std::string source =
      "function deep(n) { y = 2.5; i = 0; while (i < 3) { i = i + 1; } return n + 1; }\n"
      "function mid(n) { i = 0; while (i < 2) { i = i + 1; } return deep(n) + 1; }\n"
      "function outer(n) { x = 1.5; s = 0; i = 0; while (i < n) { s = s + mid(i); i = i + 1; } return s; }\n" +
      std::string(kTopLevelLeaves) + "print(outer(5));\n";  // outer entered by the interpreter
  Scenario sc;
  sc.source = source;
  Outcome plain, jit;
  expect_same_compiled(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  Driver d(source, 1, true);
  ASSERT_TRUE(d.ok());
  std::vector<Seen> seen;
  d.script.on_poll = recorder(&d, &seen);
  ASSERT_TRUE(d.run());
  EXPECT_EQ(d.ctx->raw(), plain.raw);
  EXPECT_EQ(d.stats().extension_mismatches, 0u);
  std::set<uint64_t> in_outer, in_mid, in_deep;
  for (const Seen & x : seen) {
    if (x.function == 0) {
      continue;  // the top level's own poll
    }
    (x.function == 3 ? in_outer : x.function == 2 ? in_mid : in_deep).insert(x.capacity);
  }
  EXPECT_EQ(in_outer, (std::set<uint64_t>{0, 1})) << "(0 is the interpreter's entry poll) the entry's own extension stays through every call and pop below it";
  EXPECT_EQ(in_mid, (std::set<uint64_t>{1})) << "a float-free push added nothing and its pop took nothing";
  EXPECT_EQ(in_deep, std::set<uint64_t>{2}) << "the entry's and deep's";
  EXPECT_EQ(d.boxes().capacity + d.boxes().extensions + d.boxes().pool, 0u);
}

TEST(JitFloats, AFunctionDiscardedWhileItsRecordIsOpenStillTakesItsDesignedExitAsDesigned) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The function is let go (as a nested run's eighth exit would) at a poll inside its loop, while the run that entered it
  // is still open; its exit at the print is then taken from code whose function has no code any more. The exit is
  // classified from the code that ran: it is designed, and counts toward nothing.
  // The top level leaves at once (kTopLevelLeaves): that is its one counted exit, and f is entered by the interpreter.
  const std::string source = "function f(n) { x = n * 0.5; i = 0; while (i < 60) { i = i + 1; } print(x); return 0; }\n" + std::string(kTopLevelLeaves) + "f(3);\n";
  Scenario ref;
  ref.source = source;
  Outcome plain = run(ref, 0);
  Driver d(source, 1, true);
  ASSERT_TRUE(d.ok());
  int in_f = 0, discarded = 0;
  d.script.on_poll = [&](Script &, GRCORE_Context * context, uint64_t) {
    GRCORE_PollIdentity id = {0, 0};
    (void)grcore_context_poll_identity(context, &id);
    if ((id.function & 0xFFFFFFFFu) == 1u && ++in_f == 5) {
      discarded += gltang_vm_jit_test_discard(d.ctx->execution, 1u);
    }
  };
  ASSERT_TRUE(d.run());
  EXPECT_EQ(discarded, 1) << "the function was discarded while its record was open";
  EXPECT_EQ(d.ctx->raw(), plain.raw);
  GLTANG_JitStats st = d.stats();
  EXPECT_EQ(st.exits_expected, 1u) << "the exit at the print was found designed in the code that ran";
  EXPECT_EQ(st.exits_counted, 1u) << "and only the main program's own exit counted";
  EXPECT_EQ(st.rebuild_failures, 0u);
}

#else  // JIT=no

TEST(JitFloats, WithoutTheJitNothingIsBoxedAheadAndTheStatisticsAreZero) {
  Compiled page("function f(n) { x = 1.5; i = 0; while (i < n) { i = i + 1; } print(x); return i; }\nprint(f(100));\n", Mode::Script, "nojit.tang");
  ASSERT_TRUE(page.ok());
  Context context(page.program);
  ASSERT_TRUE(context.ok());
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "1.5100");
  GLTANG_JitStats st = context.jit_stats();
  EXPECT_EQ(st.values_boxed + st.box_pool_made + st.reservation_refusals + st.box_pool_refusals + st.conversion_errors + st.reservation_peak, 0u);
  EXPECT_EQ(st.entries + st.deopts + st.functions_compiled, 0u);
}

#endif

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
