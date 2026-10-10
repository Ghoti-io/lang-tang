/**
 * @file
 *
 * The frame-differential observer (CAP-7, AD-16, AD-18): a header-only
 * instrument that records, at every poll, what the abstract frame walk shows,
 * and compares two such traces poll by poll.
 *
 * It registers an OBSERVE handler with runtime-core, plus a request kind that
 * is posted once and stays pending, so that every poll takes the slow path and
 * the handler runs at each of them (the fast path runs nothing). Per poll it
 * records the verdict and, for each frame, the depth, the poll identity, the
 * location (file and line), the slot count, each slot's kind and inspected
 * text, and each scope's kind and name and each variable's name and inspected
 * text. It reads the frame walk and nothing else: never the engine's own
 * structures, so it is the same instrument for any engine behind the frame
 * protocol (story 15 reuses it for interpreter against JIT, story 12 for a
 * debugger attached against absent) and sees what a debugger or a recorder
 * would see.
 *
 * What it does not record, and why: a slot's raw bits when the slot holds an
 * engine value (its inspected text is recorded instead, because the bits are
 * heap addresses that differ from run to run and under a moving stack); the
 * address of a frame. The raw header words of a frame (function, pc, sp,
 * flags) are recorded as the text the inspector gives them, and they are
 * stable.
 *
 * Floats have a second channel. The inspected text of a float is its value
 * rounded to six decimals with every NaN as nan, so one ulp, a subnormal flushed
 * to zero and the sign and payload of a NaN are invisible in it. An Observer
 * given a `FloatBits` reader (an engine's test-only accessor: the value word in,
 * the double's 64 bits out, false if the value is not a float) also records the
 * bits of every float slot and variable, and the comparison requires them equal,
 * except that any two NaNs are equal: a program cannot observe a NaN's sign or
 * payload, so a tier that produces a different one is not wrong.
 *
 * The comparison reports the first divergence with its poll index and the
 * frame, and says what differs. A trace that is a prefix of the other
 * diverges at the first poll one lacks.
 *
 * Nothing here is specific to lang-tang except that its tests include it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GLTANG_TESTS_OBSERVER_H
#define GHOTI_IO_GLTANG_TESTS_OBSERVER_H

#include <ghoti.io/runtime-core/runtime-core.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace observer {

/// An engine's reader of a float's bits: the value word of a slot or a variable
/// in, the double's 64 bits out; false if the value is not a float.
typedef bool (*FloatBits)(uint64_t value, uint64_t * bits);

struct SlotRecord {
  GRCORE_SlotKind kind = GRCORE_SLOT_RAW;
  std::string text;
  bool is_float = false;   ///< The value is a float and `float_bits` holds its bits (only with a FloatBits reader).
  uint64_t float_bits = 0;
};

struct VariableRecord {
  std::string name;
  GRCORE_SlotKind kind = GRCORE_SLOT_RAW;
  std::string text;
  bool is_float = false;
  uint64_t float_bits = 0;
};

struct ScopeRecord {
  GRCORE_ScopeKind kind = GRCORE_SCOPE_LOCAL;
  std::string name;
  std::vector<VariableRecord> variables;
};

struct FrameRecord {
  size_t depth = 0;
  std::string engine;
  uint64_t function = 0;  ///< The poll identity's function word.
  uint64_t offset = 0;    ///< The poll identity's offset.
  std::string file;
  int line = 0;
  size_t slot_count = 0;
  std::vector<SlotRecord> slots;
  std::vector<ScopeRecord> scopes;
};

struct PollRecord {
  bool captured = true;  ///< False if the frame walk failed: the frames are then not a record of anything.
  GRCORE_Verdict verdict = GRCORE_VERDICT_CONTINUE;
  std::vector<FrameRecord> frames;
};

struct Trace {
  std::vector<PollRecord> polls;
  /// The most polls recorded; later ones are counted and not recorded, in
  /// every run alike.
  size_t limit = 20000;
  size_t total = 0;
  /// After the first `limit` polls, every `sample_every`-th poll is recorded too, up
  /// to `sample_cap` of them (0: none). Two runs of one program poll at the same
  /// indices, so their samples line up; a late difference is then still seen.
  size_t sample_every = 0;
  size_t sample_cap = 0;
  size_t sampled = 0;
};

namespace detail {

inline std::string slot_text(const GRCORE_AbstractFrame & frame, size_t index) {
  char small[256];
  size_t length = 0;
  if (grcore_frame_inspect(&frame, index, small, sizeof(small), &length) != GRCORE_OK) {
    return "<unreadable>";
  }
  if (length < sizeof(small)) {
    return std::string(small, length);
  }
  std::string big(length + 1, '\0');
  size_t again = 0;
  if (grcore_frame_inspect(&frame, index, big.data(), big.size(), &again) != GRCORE_OK) {
    return "<unreadable>";
  }
  big.resize(again < big.size() ? again : big.size() - 1);
  return big;
}

inline std::string variable_text(const GRCORE_Context * context, const GRCORE_AbstractFrame & frame, const GRCORE_Variable & variable) {
  char small[256];
  size_t length = 0;
  if (grcore_engine_inspect(context, frame.engine, variable.kind, variable.value, small, sizeof(small), &length) != GRCORE_OK) {
    return "<unreadable>";
  }
  if (length < sizeof(small)) {
    return std::string(small, length);
  }
  std::string big(length + 1, '\0');
  size_t again = 0;
  if (grcore_engine_inspect(context, frame.engine, variable.kind, variable.value, big.data(), big.size(), &again) != GRCORE_OK) {
    return "<unreadable>";
  }
  big.resize(again < big.size() ? again : big.size() - 1);
  return big;
}

}  // namespace detail

/// Reads the whole stack through the frame walk. Valid where the walk is: at a
/// poll, in a handler, or while a run is paused and the caller holds it.
inline bool capture(const GRCORE_Context * context, std::vector<FrameRecord> * out, FloatBits float_bits = nullptr) {
  GRCORE_FrameWalk walk;
  if (grcore_frame_walk_begin(context, &walk) != GRCORE_OK) {
    return false;
  }
  GRCORE_AbstractFrame frame;
  while (grcore_frame_walk_next(&walk, &frame)) {
    FrameRecord rec;
    rec.depth = frame.depth;
    rec.engine = frame.descriptor && frame.descriptor->name ? frame.descriptor->name : "";
    rec.function = frame.identity.function;
    rec.offset = frame.identity.offset;
    rec.file = frame.location.file ? frame.location.file : "";
    rec.line = frame.location.line;
    rec.slot_count = frame.slot_count;
    for (size_t i = 0; i < frame.slot_count; ++i) {
      SlotRecord slot;
      uint64_t value = 0;
      if (grcore_frame_slot(&frame, i, &slot.kind, &value) != GRCORE_OK) {
        slot.text = "<unreadable>";
      }
      else {
        slot.text = detail::slot_text(frame, i);
        if (float_bits && slot.kind == GRCORE_SLOT_VALUE) {
          slot.is_float = float_bits(value, &slot.float_bits);
        }
      }
      rec.slots.push_back(slot);
    }
    size_t scopes = grcore_frame_scope_count(&frame);
    for (size_t s = 0; s < scopes; ++s) {
      GRCORE_ScopeInfo info;
      if (grcore_frame_scope(&frame, s, &info) != GRCORE_OK) {
        continue;
      }
      ScopeRecord scope;
      scope.kind = info.kind;
      scope.name = info.name ? info.name : "";
      for (size_t v = 0; v < info.variable_count; ++v) {
        GRCORE_Variable variable;
        if (grcore_frame_variable(&frame, s, v, &variable) != GRCORE_OK) {
          continue;
        }
        VariableRecord var;
        var.name = variable.name ? variable.name : "";
        var.kind = variable.kind;
        var.text = detail::variable_text(context, frame, variable);
        if (float_bits && variable.kind == GRCORE_SLOT_VALUE) {
          var.is_float = float_bits(variable.value, &var.float_bits);
        }
        scope.variables.push_back(var);
      }
      rec.scopes.push_back(scope);
    }
    out->push_back(rec);
  }
  return true;
}

/// The handler's state. Attach it to a context before the run starts.
class Observer {
 public:
  Trace trace;
  /// Set before `attach`: records the bits of every float (see the file comment).
  FloatBits float_bits = nullptr;

  Observer() = default;
  Observer(const Observer &) = delete;
  Observer & operator=(const Observer &) = delete;
  ~Observer() { grcore_port_release(port_); }

  /// Registers the handler and posts the request that keeps every poll on the
  /// slow path. The context must be parked and owned by the caller.
  GRCORE_Result attach(GRCORE_Context * context) {
    GRCORE_Result r = grcore_context_request_kind(context, &key(), &kind_);
    if (r == GRCORE_OK) {
      r = grcore_context_port(context, &port_);
    }
    if (r == GRCORE_OK) {
      r = grcore_context_register(context, &key(), this);
    }
    if (r == GRCORE_OK) {
      r = grcore_port_post(port_, kind_);
    }
    return r;
  }

 private:
  GRCORE_Port * port_ = nullptr;
  GRCORE_RequestKind kind_ = 0;

  static void handler(GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
    Observer * self = static_cast<Observer *>(value);
    size_t index = self->trace.total++;
    if (index >= self->trace.limit) {
      Trace & t = self->trace;
      if (!t.sample_every || t.sampled >= t.sample_cap || (index - t.limit) % t.sample_every != 0) {
        return;
      }
      ++t.sampled;
    }
    else if (self->trace.polls.size() >= self->trace.limit) {
      return;
    }
    PollRecord poll;
    poll.verdict = grcore_pollcall_verdict(call);
    poll.captured = capture(context, &poll.frames, self->float_bits);
    self->trace.polls.push_back(std::move(poll));
  }

  static const GRCORE_Key & key() {
    static const GRCORE_Key k = GRCORE_KEY_INIT("frame observer", GRCORE_CARDINALITY_MANY, GRCORE_PHASE_OBSERVE, nullptr, &Observer::handler, nullptr, nullptr, nullptr);
    return k;
  }
};

// ---------------------------------------------------------------------------
// The comparison
// ---------------------------------------------------------------------------

struct Divergence {
  size_t poll = 0;         ///< The index of the first poll that differs.
  bool has_frame = false;
  size_t frame = 0;        ///< Which frame (0 is the innermost).
  std::string what;        ///< What differs.
  std::string left, right; ///< The two values, as text.

  std::string str() const {
    std::string s = "poll " + std::to_string(poll);
    if (has_frame) {
      s += ", frame " + std::to_string(frame);
    }
    s += ": " + what + ": `" + left + "` against `" + right + "`";
    return s;
  }
};

namespace detail {

inline const char * verdict_name(GRCORE_Verdict v) {
  switch (v) {
    case GRCORE_VERDICT_CONTINUE: return "continue";
    case GRCORE_VERDICT_PAUSE: return "pause";
    case GRCORE_VERDICT_UNWIND: return "unwind";
  }
  return "?";
}

inline bool differ(Divergence * d, size_t poll, bool has_frame, size_t frame, const std::string & what, const std::string & a, const std::string & b) {
  d->poll = poll;
  d->has_frame = has_frame;
  d->frame = frame;
  d->what = what;
  d->left = a;
  d->right = b;
  return true;
}

inline std::string where(const FrameRecord & f) {
  return f.file + ":" + std::to_string(f.line);
}

inline bool is_nan_bits(uint64_t bits) {
  return (bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (bits & 0x000fffffffffffffull) != 0;
}

inline std::string hex64(uint64_t bits) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)bits);
  return buf;
}

/// Whether two float readings differ: by bits, with any two NaNs equal. Null
/// when they agree, else what to say.
template <typename R>
inline const char * float_difference(const R & x, const R & y) {
  if (x.is_float != y.is_float) {
    return "float-ness";
  }
  if (x.is_float && x.float_bits != y.float_bits && !(is_nan_bits(x.float_bits) && is_nan_bits(y.float_bits))) {
    return "float bits";
  }
  return nullptr;
}

/// Whether two frames differ, and how. With `bits` false the float channel is
/// not read (a test shows what the text alone misses).
inline bool compare_frames(const FrameRecord & a, const FrameRecord & b, size_t poll, size_t index, Divergence * d, bool bits = true) {
  auto at = [&](const std::string & what, const std::string & x, const std::string & y) {
    return differ(d, poll, true, index, what, x, y);
  };
  if (a.depth != b.depth) {
    return at("depth", std::to_string(a.depth), std::to_string(b.depth));
  }
  if (a.engine != b.engine) {
    return at("engine", a.engine, b.engine);
  }
  if (a.function != b.function || a.offset != b.offset) {
    return at("poll identity", std::to_string(a.function) + "/" + std::to_string(a.offset), std::to_string(b.function) + "/" + std::to_string(b.offset));
  }
  if (a.file != b.file || a.line != b.line) {
    return at("location", where(a), where(b));
  }
  if (a.slot_count != b.slot_count || a.slots.size() != b.slots.size()) {
    return at("slot count", std::to_string(a.slot_count), std::to_string(b.slot_count));
  }
  for (size_t i = 0; i < a.slots.size(); ++i) {
    if (a.slots[i].kind != b.slots[i].kind) {
      return at("slot " + std::to_string(i) + " kind", a.slots[i].kind == GRCORE_SLOT_RAW ? "raw" : "value", b.slots[i].kind == GRCORE_SLOT_RAW ? "raw" : "value");
    }
    if (a.slots[i].text != b.slots[i].text) {
      return at("slot " + std::to_string(i) + " text", a.slots[i].text, b.slots[i].text);
    }
    if (bits) {
      if (const char * what = float_difference(a.slots[i], b.slots[i])) {
        return at("slot " + std::to_string(i) + " " + what, a.slots[i].is_float ? hex64(a.slots[i].float_bits) + " (" + a.slots[i].text + ")" : "not a float",
            b.slots[i].is_float ? hex64(b.slots[i].float_bits) + " (" + b.slots[i].text + ")" : "not a float");
      }
    }
  }
  if (a.scopes.size() != b.scopes.size()) {
    return at("scope count", std::to_string(a.scopes.size()), std::to_string(b.scopes.size()));
  }
  for (size_t s = 0; s < a.scopes.size(); ++s) {
    const ScopeRecord & x = a.scopes[s];
    const ScopeRecord & y = b.scopes[s];
    if (x.kind != y.kind || x.name != y.name) {
      return at("scope " + std::to_string(s), x.name, y.name);
    }
    if (x.variables.size() != y.variables.size()) {
      return at("scope " + x.name + " variable count", std::to_string(x.variables.size()), std::to_string(y.variables.size()));
    }
    for (size_t v = 0; v < x.variables.size(); ++v) {
      if (x.variables[v].name != y.variables[v].name) {
        return at("scope " + x.name + " variable " + std::to_string(v) + " name", x.variables[v].name, y.variables[v].name);
      }
      if (x.variables[v].text != y.variables[v].text) {
        return at("scope " + x.name + " variable `" + x.variables[v].name + "`", x.variables[v].text, y.variables[v].text);
      }
      if (bits) {
        if (const char * what = float_difference(x.variables[v], y.variables[v])) {
          return at("scope " + x.name + " variable `" + x.variables[v].name + "` " + what,
              x.variables[v].is_float ? hex64(x.variables[v].float_bits) + " (" + x.variables[v].text + ")" : "not a float",
              y.variables[v].is_float ? hex64(y.variables[v].float_bits) + " (" + y.variables[v].text + ")" : "not a float");
        }
      }
    }
  }
  return false;
}

}  // namespace detail

/// The first divergence between two traces, poll by poll; true if there is one.
/// Floats are compared by their bits, any two NaNs equal, when the traces carry
/// them; `bits` false compares the text alone, which is how a test shows what the
/// bits channel adds.
inline bool first_divergence(const Trace & a, const Trace & b, Divergence * out, bool bits = true) {
  size_t common = a.polls.size() < b.polls.size() ? a.polls.size() : b.polls.size();
  for (size_t p = 0; p < common; ++p) {
    const PollRecord & x = a.polls[p];
    const PollRecord & y = b.polls[p];
    if (!x.captured || !y.captured) {
      return detail::differ(out, p, false, 0, "frame walk failed", x.captured ? "captured" : "not captured", y.captured ? "captured" : "not captured");
    }
    if (x.verdict != y.verdict) {
      return detail::differ(out, p, false, 0, "verdict", detail::verdict_name(x.verdict), detail::verdict_name(y.verdict));
    }
    if (x.frames.size() != y.frames.size()) {
      // The first frame one stack has and the other lacks is the one named.
      return detail::differ(out, p, true, x.frames.size() < y.frames.size() ? x.frames.size() : y.frames.size(), "depth (frame count)",
          std::to_string(x.frames.size()), std::to_string(y.frames.size()));
    }
    for (size_t f = 0; f < x.frames.size(); ++f) {
      if (detail::compare_frames(x.frames[f], y.frames[f], p, f, out, bits)) {
        return true;
      }
    }
  }
  if (a.polls.size() != b.polls.size() || a.total != b.total) {
    return detail::differ(out, common, false, 0, "number of polls", std::to_string(a.total), std::to_string(b.total));
  }
  return false;
}

// ---------------------------------------------------------------------------
// Planted mismatches, for the tests that show the instrument fails
// ---------------------------------------------------------------------------

/// Alters one slot's text in one poll of one trace. Returns false if there is
/// no such slot.
inline bool plant_slot_mismatch(Trace * trace, size_t poll, size_t frame, size_t slot) {
  if (poll >= trace->polls.size() || frame >= trace->polls[poll].frames.size() || slot >= trace->polls[poll].frames[frame].slots.size()) {
    return false;
  }
  trace->polls[poll].frames[frame].slots[slot].text += "!";
  return true;
}

/// Replaces the bits of the first float slot or variable of a trace whose bits
/// `select` accepts (in the order the comparison reads them: per poll, per frame,
/// slots then variables) with `change(bits)`, and leaves its text alone: what an
/// engine that computes one ulp off, flushes a subnormal or produces a NaN does
/// where the text rounds the difference away. Returns the index of the poll it
/// altered, or SIZE_MAX if the trace has no such float.
inline size_t plant_float_bits(Trace * trace, bool (*select)(uint64_t), uint64_t (*change)(uint64_t)) {
  for (size_t p = 0; p < trace->polls.size(); ++p) {
    for (FrameRecord & f : trace->polls[p].frames) {
      for (SlotRecord & s : f.slots) {
        if (s.is_float && select(s.float_bits)) {
          s.float_bits = change(s.float_bits);
          return p;
        }
      }
      for (ScopeRecord & sc : f.scopes) {
        for (VariableRecord & v : sc.variables) {
          if (v.is_float && select(v.float_bits)) {
            v.float_bits = change(v.float_bits);
            return p;
          }
        }
      }
    }
  }
  return SIZE_MAX;
}

/// Removes one poll from a trace (a poll the other run had and this one missed).
inline bool plant_missing_poll(Trace * trace, size_t poll) {
  if (poll >= trace->polls.size()) {
    return false;
  }
  trace->polls.erase(trace->polls.begin() + (long)poll);
  --trace->total;
  return true;
}

/// Drops the outermost frame of one poll, so the depth differs.
inline bool plant_different_depth(Trace * trace, size_t poll) {
  if (poll >= trace->polls.size() || trace->polls[poll].frames.empty()) {
    return false;
  }
  trace->polls[poll].frames.pop_back();
  return true;
}

}  // namespace observer

#endif
