/**
 * @file
 *
 * lang-tang's side of the execution differential: a program run with the
 * default libraries and a generous fuel budget, and its output and result
 * reduced to the same verdict the oracle runner prints for ctang.
 *
 * The canonical result rule is the one in oracle_ctang.c, applied to
 * lang-tang's result accessors: a kind and a canonical text, so an error is
 * its kind and message and never the file and line it came from, and a
 * container is a count (an array also lists its elements one level deep).
 *
 * Nothing here includes ctang. It is shared by the oracle differential, the
 * differential fuzz run, and the tests that run the same programs on lang-tang
 * alone (so the lang-tang side is also exercised under GC torture, where the
 * child ctang is not the subject).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GLTANG_TESTS_ORACLE_RUN_LANG_TANG_H
#define GHOTI_IO_GLTANG_TESTS_ORACLE_RUN_LANG_TANG_H

#include "exec_harness.h"
#include "oracle/oracle.h"
#include "test_helpers.h"

#include <cstdio>

namespace oracle {

/// The fuel a differential run is given. Generous: a program that spends it is
/// a runaway, and lang-tang pauses where ctang would be killed (AD-16).
constexpr uint64_t kDifferentialFuel = 200000000;

inline const char * kind_text(GLTANG_ValueKind kind) {
  switch (kind) {
    case GLTANG_KIND_NULL: return "null";
    case GLTANG_KIND_BOOL: return "bool";
    case GLTANG_KIND_INTEGER: return "integer";
    case GLTANG_KIND_FLOAT: return "float";
    case GLTANG_KIND_STRING: return "string";
    case GLTANG_KIND_ARRAY: return "array";
    case GLTANG_KIND_MAP: return "map";
    case GLTANG_KIND_FUNCTION: return "function";
    case GLTANG_KIND_ERROR: return "error";
    case GLTANG_KIND_LIBRARY: return "library";
    case GLTANG_KIND_RNG: return "rng";
  }
  return "other";
}

inline std::string hex_of(const std::string & bytes) {
  std::string out;
  char buf[4];
  for (unsigned char c : bytes) {
    std::snprintf(buf, sizeof(buf), "%02x", c);
    out += buf;
  }
  return out;
}

inline std::string float_text(double d) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", d);
  return buf;
}

/// The canonical text of a scalar item, or the count of a container.
inline std::string item_text(const GLTANG_ResultItem & item) {
  switch (item.kind) {
    case GLTANG_KIND_BOOL: return item.boolean ? "true" : "false";
    case GLTANG_KIND_INTEGER: return std::to_string(item.integer);
    case GLTANG_KIND_FLOAT: return float_text(item.number);
    case GLTANG_KIND_STRING: return item.text ? std::string(item.text, item.length) : std::string();
    case GLTANG_KIND_ERROR: return gltang_error_kind_message(item.error);
    case GLTANG_KIND_ARRAY:
    case GLTANG_KIND_MAP: return std::to_string(item.size);
    default: return "";
  }
}

/// The result of a finished execution as (kind, canonical text).
inline void canonical_result(const GLTANG_Execution * execution, std::string * kind, std::string * text) {
  GLTANG_ValueKind k = gltang_execution_result_kind(execution);
  *kind = kind_text(k);
  switch (k) {
    case GLTANG_KIND_BOOL: *text = gltang_execution_result_bool(execution) ? "true" : "false"; break;
    case GLTANG_KIND_INTEGER: *text = std::to_string(gltang_execution_result_integer(execution)); break;
    case GLTANG_KIND_FLOAT: *text = float_text(gltang_execution_result_float(execution)); break;
    case GLTANG_KIND_STRING: {
      size_t length = 0;
      const char * t = gltang_execution_result_text(execution, &length);
      *text = t ? std::string(t, length) : std::string();
      break;
    }
    case GLTANG_KIND_ERROR: {
      GLTANG_ErrorKind ek = GLTANG_ERROR_KIND_COUNT;
      gltang_execution_result_error(execution, &ek, nullptr);
      *text = gltang_error_kind_message(ek);
      break;
    }
    case GLTANG_KIND_MAP: *text = std::to_string(gltang_execution_result_size(execution)); break;
    case GLTANG_KIND_ARRAY: {
      size_t n = gltang_execution_result_size(execution);
      *text = std::to_string(n) + "[";
      for (size_t i = 0; i < n; ++i) {
        GLTANG_ResultItem item;
        if (!gltang_execution_result_element(execution, i, &item)) {
          *text += "?";
          continue;
        }
        *text += std::string(i ? ";" : "") + kind_text(item.kind) + ":" + hex_of(item_text(item));
      }
      *text += "]";
      break;
    }
    default: text->clear(); break;
  }
}

/// How a lang-tang run came out, beyond the verdict, for the tests that look
/// at more than the comparison (a torture run checks the heap too).
struct LangTangRun {
  Verdict verdict = Verdict::reject();
  std::string error; ///< Why a harness-level failure (not a verdict) happened.
};

/// Why a threshold-1 rerun cannot be required to have made compiled calls, or
/// nullptr when it must: a build without the JIT, or a target without a native
/// backend, compiles nothing, and the rerun is then only the interpreter again.
/// (The context's native stack budget is the harness's default, 1 MiB, never
/// unlimited, which would compile no call either.)
inline const char * no_compiled_calls_reason() {
#ifdef GLTANG_WITH_JIT
  return ::jit_backend_present() ? nullptr : "this target has no native code backend";
#else
  return "built with JIT=no";
#endif
}

/// Compiles and runs `source` on lang-tang, as the corpus and the generator do.
/// `compiled_calls`, when given, is increased by the calls between compiled functions the run made. `fuel` is the budget and `jit_threshold` the tier-up threshold (-1: the
/// environment's, then the library's, 0: off, 1: every function at its first
/// poll); a run that spends the fuel, or is unwound with the limit, is `paused`. A program that does not compile is `reject`.
inline Verdict lang_tang_run(const std::string & source, bool script, uint64_t fuel = kDifferentialFuel, long jit_threshold = -1,
    uint64_t * compiled_calls = nullptr) {
  tt::Compiled compiled(source, script ? tt::Mode::Script : tt::Mode::Template, "program.tang");
  if (compiled.result == GLTANG_ERR_FORMAT) {
    return Verdict::reject();
  }
  if (!compiled.ok()) {
    // Any other refusal of a program is not a verdict: a harness failure.
    throw std::runtime_error(std::string("lang-tang answered ") + gltang_result_string(compiled.result) + " to a compile");
  }
  tt::Config config;
  config.fuel = fuel;
  config.jit_threshold = jit_threshold;  // -1: the environment's, then the library's
  tt::Context context(compiled.program, config);
  if (!context.ok()) {
    throw std::runtime_error("lang-tang could not make a context");
  }
  context.attach();
  GRCORE_Result r = grcore_run(context.context, gltang_execution_entry, context.execution, &context.outcome);
  context.has_run = true;
  if (compiled_calls) {
    *compiled_calls += context.jit_stats().calls;
  }
  if (r == GRCORE_ERR_LIMIT || (r == GRCORE_OK && context.outcome == GRCORE_OUTCOME_PAUSED)) {
    return Verdict::paused();
  }
  if (r != GRCORE_OK) {
    throw std::runtime_error("lang-tang's run ended with core result " + std::to_string((int)r));
  }
  std::string kind, text;
  canonical_result(context.execution, &kind, &text);
  return Verdict::ran(context.rendered(), kind, text);
}

}  // namespace oracle

#endif
