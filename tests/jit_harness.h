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

#ifndef GHOTI_IO_GLTANG_TEST_JIT_HARNESS_H
#define GHOTI_IO_GLTANG_TEST_JIT_HARNESS_H

// The scenario harness of the JIT tests: one program, run with the JIT off and
// with every function tiering up at its first poll, and everything observable
// about the two runs compared. Shared by test_jit.cpp (the baseline) and
// test_jit_calls.cpp (calls between compiled functions).

#include "../src/vm/test_hooks.h"
#include "exec_harness.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ghoti.io/runtime-core/runtime-core.h>

extern "C" void gltang_vm_set_statement_polls_unchecked(GLTANG_Execution * execution, bool enabled);
extern "C" int gltang_vm_jit_test_forged_install(GLTANG_Execution * execution, uint32_t code_fn, uint32_t slot_fn, int mode, uintptr_t * before, uintptr_t * after);
extern "C" int gltang_vm_jit_test_metadata(GLTANG_Execution * execution, uint64_t * sites, uint64_t * derived, uint64_t * converting);
extern "C" int gltang_vm_jit_test_hook(GLTANG_Execution * execution, int hook, uint64_t token, const uint64_t * args, uint64_t count);

namespace jt {

using tt::Compiled;
using tt::Config;
using tt::Context;
using tt::Mode;

// ---------------------------------------------------------------------------
// A poll handler a test scripts: it runs at every poll (a request stays pending
// so that every poll takes the slow path, as the observer's does), counts them,
// and records the identity of each.
// ---------------------------------------------------------------------------

struct Script {
  std::function<void(Script &, GRCORE_Context *, uint64_t)> on_poll;
  uint64_t polls = 0;
  std::vector<GRCORE_PollIdentity> identities;
  GRCORE_Port * port = nullptr;
  GRCORE_RequestKind kind = 0;

  Script() = default;
  Script(const Script &) = delete;
  Script & operator=(const Script &) = delete;
  ~Script() { grcore_port_release(port); }

  static const GRCORE_Key & key() {
    static const GRCORE_Key k = GRCORE_KEY_INIT("jit test script", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_ACT, nullptr, &Script::handler, nullptr, nullptr, nullptr);
    return k;
  }

  bool attach(GRCORE_Context * context) {
    if (grcore_context_request_kind(context, &key(), &kind) != GRCORE_OK || grcore_context_port(context, &port) != GRCORE_OK ||
        grcore_context_register(context, &key(), this) != GRCORE_OK) {
      return false;
    }
    return grcore_port_post(port, kind) == GRCORE_OK;
  }

  /// Posts a request of a core kind from inside a poll.
  void post(GRCORE_RequestKind k) { EXPECT_EQ(grcore_port_post(port, k), GRCORE_OK); }

 private:
  static void handler(GRCORE_Context * context, void * value, GRCORE_PollCall *) {
    Script * self = static_cast<Script *>(value);
    GRCORE_PollIdentity id = {0, 0};
    if (grcore_context_poll_identity(context, &id) == GRCORE_OK) {
      self->identities.push_back(id);
    }
    uint64_t n = self->polls++;
    if (self->on_poll) {
      self->on_poll(*self, context, n);
    }
  }
};

struct Part {
  std::string name, source;
  uint64_t fuel = 100000;
  GLTANG_ScopePolicy policy = GLTANG_SCOPE_EMPTY;
  Mode mode = Mode::Script;
};

/// What to run and how.
struct Scenario {
  std::string source;
  Mode mode = Mode::Script;
  std::vector<Part> parts;
  uint64_t fuel = GRCORE_UNLIMITED;      ///< The context's budget.
  uint64_t step = 0;                     ///< Nonzero: on a pause raise the budget by this and resume.
  bool statement_polls = false;
  int torture = -1, verify = -1, moving = -1;
  uint64_t memory_bytes = GRCORE_UNLIMITED;
  uint64_t memory_reserve = GRCORE_DEFAULT_MEMORY_RESERVE;
  uint64_t native_depth = GRCORE_UNLIMITED;
  uint64_t native_stack_bytes = uint64_t{1} << 20;  ///< The byte budget compiled calls need; GRCORE_UNLIMITED compiles none.
  uint64_t calls = 512;                  ///< The guest-depth budget (ctang's max_call_depth).
  bool calls_off = false;                ///< Compile no call site (the control of a measurement).
  bool fail_rebuild = false;             ///< The deopt hook's rebuild fails (the injected failure).
  bool gc_at_push = false;               ///< Collect in the push hook even without torture (so no other collection runs between it and the first use of a frame).
  bool natives_off = false;              ///< Compile no library call or member load (as a backend that refuses natives would).
  bool gc_at_native = false;             ///< Collect once a native's record is open even without torture.
  bool fail_protect = false;
  bool resume = false;                   ///< On a pause with `step` 0, resume without raising the budget (an interrupt).
  bool script = false;                   ///< Attach a Script, with `on_poll` below.
  std::function<void(Script &, GRCORE_Context *, uint64_t)> on_poll;
  std::function<void(Context &)> before;  ///< Called after the context is made and before the run.
  size_t max_pauses = 4000;
};

/// Everything observable about a run.
struct Outcome {
  bool created = false;
  bool finished = false;
  GRCORE_Result ran = GRCORE_OK;
  std::string raw, rendered, result, errors;
  std::vector<std::string> pauses;       ///< For each pause: where, identity and fuel used.
  uint64_t fuel = 0;                     ///< Fuel used at the end.
  uint64_t polls = 0;
  std::vector<GRCORE_PollIdentity> identities;
  GLTANG_JitStats stats = {};
  uint64_t native_depth = 0;
  uint64_t memory_peak = 0;
  uint64_t stack_moves = 0;              ///< How many times the guest stack's buffer moved (it starts small and grows).
  uint64_t retired_peak = 0;             ///< The most references the context's retired-code list held at once.
  uint64_t registered = 0;               ///< Compiled ranges registered at the end of the run.
  uint64_t natives_called = 0;           ///< Natives entered through the shared wrapper, from either tier.
  std::string key() const {
    std::string s = raw + "|" + rendered + "|" + result + "|" + errors + "|" + std::to_string(ran) + "|" + std::to_string(finished) + "|fuel " + std::to_string(fuel);
    for (const auto & p : pauses) {
      s += "|" + p;
    }
    return s;
  }
};

// run() and expect_same() serve the JIT arm only; the JIT=no arm has no test that
// calls them, and -Wunused-function would report them there.
#ifdef GLTANG_WITH_JIT
inline Outcome run(const Scenario & sc, long threshold) {
  Outcome out;
  Compiled page(sc.source, sc.mode, "jit.tang");
  EXPECT_TRUE(page.ok()) << page.error.message;
  if (!page.ok()) {
    return out;
  }
  Config config;
  config.fuel = sc.fuel;
  config.torture = sc.torture;
  config.verify = sc.verify;
  config.moving_stack = sc.moving;
  config.memory_bytes = sc.memory_bytes;
  config.memory_reserve = sc.memory_reserve;
  config.native_depth = sc.native_depth;
  config.native_stack_bytes = sc.native_stack_bytes;
  config.calls = sc.calls;
  config.jit_threshold = threshold;
  Context context(page.program, config);
  EXPECT_TRUE(context.ok());
  if (!context.ok()) {
    return out;
  }
  out.created = true;
  context.tracker.fail_protect = sc.fail_protect;
  context.apply_jit_switches(sc.calls_off, sc.fail_rebuild, sc.gc_at_push);
  context.apply_native_switches(sc.natives_off, sc.gc_at_native);
  EXPECT_EQ(gltang_execution_set_name(context.execution, "page"), GLTANG_OK);
  if (sc.statement_polls) {
    EXPECT_EQ(gltang_execution_set_statement_polls(context.execution, true), GLTANG_OK);
  }
  std::vector<std::unique_ptr<Compiled>> compiled;
  for (const Part & part : sc.parts) {
    compiled.push_back(std::make_unique<Compiled>(part.source, part.mode, (part.name + ".tang").c_str()));
    EXPECT_TRUE(compiled.back()->ok()) << part.name << ": " << compiled.back()->error.message;
    EXPECT_EQ(gltang_library_add_template(context.library(), part.name.c_str(), compiled.back()->program, part.fuel, part.policy), GLTANG_OK);
  }
  Script script;
  script.on_poll = sc.on_poll;
  if (sc.script) {
    EXPECT_TRUE(script.attach(context.context));
  }
  if (sc.before) {
    sc.before(context);
  }
  bool done = context.execute();
  while (!done && context.paused() && out.pauses.size() < sc.max_pauses) {
    GRCORE_Location where = grcore_context_pause_location(context.context);
    GRCORE_PollIdentity id = {0, 0};
    (void)grcore_context_poll_identity(context.context, &id);
    out.pauses.push_back(std::string(where.file ? where.file : "?") + ":" + std::to_string(where.line) + "@" + std::to_string(id.function) + "/" +
        std::to_string(id.offset) + " fuel " + std::to_string(grcore_context_fuel_used(context.context)));
    if (sc.step == 0) {
      if (sc.resume) {
        done = context.resume();
        continue;
      }
      break;
    }
    uint64_t used = grcore_context_fuel_used(context.context);
    grcore_context_set_fuel(context.context, used + sc.step);
    uint64_t depth = grcore_context_fuel_scope_depth(context.context);
    if (depth > 0) {
      uint64_t top = grcore_context_fuel_scope_top(context.context);
      uint64_t remaining = 1, scope_used = 0;
      if (grcore_context_fuel_scope_remaining(context.context, top, &remaining) == GRCORE_OK && remaining == 0 &&
          grcore_context_fuel_scope_used(context.context, top, &scope_used) == GRCORE_OK) {
        grcore_context_fuel_scope_set_budget(context.context, top, scope_used + sc.step);
      }
    }
    done = context.resume();
  }
  out.finished = done;
  out.ran = context.ran;
  out.raw = context.raw();
  out.rendered = context.rendered();
  out.result = context.describe();
  for (size_t i = 0; i < context.error_count(); ++i) {
    auto e = context.error(i);
    out.errors += e.template_name() + ":" + std::to_string(e.e.line) + "[" + e.chain_text() + "]" + std::to_string((int)e.e.how) + ":" + e.message() + ";";
  }
  out.fuel = grcore_context_fuel_used(context.context);
  out.polls = script.polls;
  out.identities = script.identities;
  out.stats = context.jit_stats();
  out.native_depth = grcore_context_depth(context.context, GRCORE_DEPTH_NATIVE);
  out.memory_peak = grcore_context_memory_peak(context.context);
  out.stack_moves = grcore_stack_move_count(grcore_context_stack(context.context));
  out.retired_peak = grcore_code_retired_peak(context.context);
  out.registered = grcore_code_registered_count(context.context);
  out.natives_called = gltang_vm_test_natives_called(context.execution);
  return out;
}

/// The program is run with the JIT off and with every function tiering up at
/// its first poll; the two must be the same run in every observable way.
inline void expect_same(const Scenario & sc, Outcome * interpreter = nullptr, Outcome * jit = nullptr) {
  Outcome a = run(sc, 0);
  Outcome b = run(sc, 1);
  ASSERT_TRUE(a.created);
  ASSERT_TRUE(b.created);
  EXPECT_EQ(a.key(), b.key());
  EXPECT_EQ(a.polls, b.polls);
  EXPECT_EQ(a.natives_called, b.natives_called) << "every native, compiled or an exit, is entered through the one wrapper, once";
  ASSERT_EQ(a.identities.size(), b.identities.size());
  for (size_t i = 0; i < a.identities.size(); ++i) {
    ASSERT_TRUE(a.identities[i].function == b.identities[i].function && a.identities[i].offset == b.identities[i].offset)
        << "poll " << i << ": " << a.identities[i].function << "/" << a.identities[i].offset << " against " << b.identities[i].function << "/"
        << b.identities[i].offset;
  }
  if (interpreter) {
    *interpreter = a;
  }
  if (jit) {
    *jit = b;
  }
}
#endif


}  // namespace jt

#endif
