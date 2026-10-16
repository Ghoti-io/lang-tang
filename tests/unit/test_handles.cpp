// Handles (handle.h): how a native function or a factory touches the program's
// values. The scope and its serial, the readers (a string's segments with their
// tags), the constructors, persistent handles, natives bound to a value, and a
// factory that answers a library. Each is driven by a program, because what a
// handle is for is a value the program built and will use.
//
// The collector is the thing under test as much as the API: a handle held across
// a constructor must survive a collection at every allocation (the torture arm),
// and a heap that moves objects (the relocation arm) must find it where it went.
// Tests that depend on it say so, and force the arm on inside the test, so that a
// plain `make test` runs it too.

#include "description_harness.h"

#include <ghoti.io/cutil/memory.h>

#include <functional>
#include <map>
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

/// A native whose body is a lambda, kept alive by the test.
struct Native {
  Hook hook;
  int calls = 0;
};

void add(Context & context, const char * name, Native & native) {
  Hook inner = native.hook;
  native.hook = [&native, inner](GLTANG_NativeCall * call) {
    ++native.calls;
    return inner(call);
  };
  ASSERT_EQ(gltang_library_add_native(context.library(), name, run_hook, &native.hook), GLTANG_OK);
}

struct Out {
  bool finished = false;
  std::string raw;
  std::string rendered;
  std::string described;
  GLTANG_ValueKind kind = GLTANG_KIND_NULL;
  GLTANG_ErrorKind error = GLTANG_ERROR_KIND_COUNT;
  int64_t integer = 0;
  GRCORE_Result ran = GRCORE_OK;
};

Out run(const std::string & source, const std::function<void(Context &)> & setup, const Config & config = Config(), tt::Mode mode = tt::Mode::Script) {
  Out out;
  Compiled compiled(source, mode);
  EXPECT_TRUE(compiled.ok()) << source << ": " << compiled.error.message;
  if (!compiled.ok()) {
    return out;
  }
  Context context(compiled.program, config);
  EXPECT_TRUE(context.ok());
  if (!context.ok()) {
    return out;
  }
  setup(context);
  out.finished = context.execute();
  out.ran = context.ran;
  out.raw = context.raw();
  out.rendered = context.rendered();
  out.described = context.describe();
  out.kind = context.kind();
  if (context.is_error()) {
    out.error = context.error_kind();
  }
  if (context.is_integer()) {
    out.integer = context.integer();
  }
  return out;
}

Config forced_torture() {
  Config c;
  c.torture = 1;
  c.verify = 1;
  return c;
}

GLTANG_Handle none() {
  return GLTANG_Handle{0, 0};
}

}  // namespace

// ---------------------------------------------------------------------------
// Scopes
// ---------------------------------------------------------------------------

TEST(Handles, AnArgumentIsReadThroughAHandleWhateverItsKind) {
  Native n;
  std::vector<std::string> seen;
  n.hook = [&](GLTANG_NativeCall * call) {
    for (size_t i = 0; i < gltang_call_count(call); ++i) {
      GLTANG_Handle h;
      EXPECT_EQ(gltang_call_arg(call, i, &h), GLTANG_OK);
      EXPECT_TRUE(gltang_handle_valid(call, h));
      switch (gltang_handle_kind(call, h)) {
        case GLTANG_KIND_NULL: seen.push_back("null"); break;
        case GLTANG_KIND_BOOL: seen.push_back(gltang_handle_bool(call, h) ? "true" : "false"); break;
        case GLTANG_KIND_INTEGER: seen.push_back("int:" + std::to_string(gltang_handle_integer(call, h))); break;
        case GLTANG_KIND_FLOAT: seen.push_back("float:" + std::to_string(gltang_handle_float(call, h))); break;
        case GLTANG_KIND_STRING: {
          size_t length = 0;
          const char * t = gltang_handle_text(call, h, &length);
          seen.push_back("str:" + std::string(t, length));
          break;
        }
        case GLTANG_KIND_ARRAY: seen.push_back("array:" + std::to_string(gltang_handle_array_length(call, h))); break;
        case GLTANG_KIND_MAP: seen.push_back("map:" + std::to_string(gltang_handle_map_count(call, h))); break;
        case GLTANG_KIND_LIBRARY: seen.push_back("library"); break;
        case GLTANG_KIND_FUNCTION: seen.push_back("function"); break;
        default: seen.push_back("other"); break;
      }
    }
    gltang_call_return_null(call);
    return true;
  };
  Out o = run("use f; use math; f(null, true, 7, 1.5, \"s\", [1, 2], {a: 1}, math, f);", [&](Context & c) { add(c, "f", n); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(seen, (std::vector<std::string>{"null", "true", "int:7", "float:1.500000", "str:s", "array:2", "map:1", "library", "function"}));
}

TEST(Handles, ABadIndexOrANullArgumentIsRefused) {
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle h = none();
    EXPECT_EQ(gltang_call_arg(call, 5, &h), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_call_arg(call, 0, nullptr), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_call_arg(nullptr, 0, &h), GLTANG_ERR_INVALID);
    EXPECT_FALSE(gltang_handle_valid(call, h)) << "a zero handle is never valid";
    EXPECT_FALSE(gltang_handle_valid(nullptr, h));
    EXPECT_EQ(gltang_handle_kind(call, h), GLTANG_KIND_NULL);
    EXPECT_EQ(gltang_handle_integer(call, h), 0);
    EXPECT_EQ(gltang_handle_text(call, h, nullptr), nullptr);
    gltang_call_return_integer(call, 1);
    return true;
  };
  Out o = run("use f; f(1);", [&](Context & c) { add(c, "f", n); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 1);
}

TEST(Handles, AHandleEndsWithItsCallbackAndAnotherCallbackCannotUseIt) {
  static GLTANG_Handle kept;
  kept = none();
  Native first, second;
  first.hook = [&](GLTANG_NativeCall * call) {
    EXPECT_EQ(gltang_handle_make_integer(call, 41, &kept), GLTANG_OK);
    EXPECT_EQ(gltang_handle_integer(call, kept), 41);
    gltang_call_return_null(call);
    return true;
  };
  second.hook = [&](GLTANG_NativeCall * call) {
    // The same numbers, from the callback that has returned: refused, and nothing read.
    EXPECT_FALSE(gltang_handle_valid(call, kept));
    EXPECT_EQ(gltang_handle_integer(call, kept), 0);
    EXPECT_EQ(gltang_handle_kind(call, kept), GLTANG_KIND_NULL);
    GLTANG_Handle mine;
    EXPECT_EQ(gltang_handle_make_integer(call, 99, &mine), GLTANG_OK);
    EXPECT_EQ(mine.index, kept.index) << "the slot is the same, the scope is not";
    EXPECT_NE(mine.scope, kept.scope) << "a serial is never reused";
    EXPECT_FALSE(gltang_handle_valid(call, kept));
    EXPECT_EQ(gltang_handle_integer(call, mine), 99);
    GLTANG_Handle forged = {mine.scope, 40};
    EXPECT_FALSE(gltang_handle_valid(call, forged)) << "past the handles made";
    gltang_call_return_handle(call, kept);  // answering with a stale handle is an error, not a read
    return true;
  };
  Out o = run("use a; use b; a(); b();", [&](Context & c) { add(c, "a", first); add(c, "b", second); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.kind, GLTANG_KIND_ERROR);
  EXPECT_EQ(o.error, GLTANG_ERROR_HOST_FAILED);
}

TEST(Handles, TheScopeLimitMakesTheCallbacksValueOutOfMemoryAndTheRunGoesOn) {
  Native n;
  size_t made = 0;
  GLTANG_Result last = GLTANG_OK;
  n.hook = [&](GLTANG_NativeCall * call) {
    for (int i = 0; i < 10 && last == GLTANG_OK; ++i) {
      GLTANG_Handle h;
      last = gltang_handle_make_integer(call, i, &h);
      made += last == GLTANG_OK;
    }
    gltang_call_return_integer(call, 5);  // an answer the limit overrides
    return true;
  };
  Compiled compiled("use f; x = f(); y = 3; x;");
  Context context(compiled.program);
  ASSERT_EQ(gltang_execution_set_handle_limits(context.execution, 4, 0), GLTANG_OK);
  add(context, "f", n);
  ASSERT_TRUE(context.execute()) << "an allocation that fails is a value, not the end of the run";
  EXPECT_EQ(made, 4u);
  EXPECT_EQ(last, GLTANG_ERR_LIMIT);
  ASSERT_TRUE(context.is_error());
  EXPECT_EQ(context.error_kind(), GLTANG_ERROR_OUT_OF_MEMORY);
}

TEST(Handles, AFrameKeepsTheHandlesOfOneStepAndLetsTheOuterOnesBeUsedInside) {
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle rows, inner, element;
    EXPECT_EQ(gltang_handle_pop_frame(call), GLTANG_ERR_INVALID) << "no frame of its own is open";
    EXPECT_EQ(gltang_handle_make_array(call, 0, &rows), GLTANG_OK);
    GLTANG_Handle stale = none();
    for (int i = 0; i < 100; ++i) {
      EXPECT_EQ(gltang_handle_push_frame(call), GLTANG_OK);
      EXPECT_EQ(gltang_handle_make_integer(call, i, &element), GLTANG_OK);
      EXPECT_TRUE(gltang_handle_valid(call, rows)) << "an outer handle is valid inside";
      EXPECT_EQ(gltang_handle_array_push(call, rows, element), GLTANG_OK);
      if (i == 0) {
        stale = element;
      }
      EXPECT_EQ(gltang_handle_make_array(call, 0, &inner), GLTANG_OK);
      EXPECT_EQ(gltang_handle_pop_frame(call), GLTANG_OK);
      EXPECT_FALSE(gltang_handle_valid(call, element)) << "the frame's handles end with it";
      EXPECT_FALSE(gltang_handle_valid(call, inner));
    }
    EXPECT_FALSE(gltang_handle_valid(call, stale));
    EXPECT_EQ(gltang_handle_array_length(call, rows), 100u);
    EXPECT_EQ(gltang_handle_pop_frame(call), GLTANG_ERR_INVALID) << "the scope itself is not a frame to pop";
    gltang_call_return_handle(call, rows);
    return true;
  };
  Compiled compiled("use f; r = f(); r[99];");
  Context context(compiled.program);
  ASSERT_EQ(gltang_execution_set_handle_limits(context.execution, 8, 0), GLTANG_OK) << "far fewer than the 200 handles made";
  add(context, "f", n);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.integer(), 99);
}

TEST(Handles, TheLimitsAreCheckedWhenSet) {
  Compiled compiled("1;");
  Context context(compiled.program);
  EXPECT_EQ(gltang_execution_set_handle_limits(nullptr, 1, 1), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_set_handle_limits(context.execution, (size_t{1} << 20) + 1, 0), GLTANG_ERR_LIMIT);
  EXPECT_EQ(gltang_execution_set_handle_limits(context.execution, 0, (size_t{1} << 20) + 1), GLTANG_ERR_LIMIT);
  EXPECT_EQ(gltang_execution_set_handle_limits(context.execution, 0, 0), GLTANG_OK) << "zero is the default";
}

TEST(Handles, ACallbackThatUsesOnlyTheScalarCallsOpensNoScope) {
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    gltang_call_return_integer(call, gltang_call_integer(call, 0) + 1);
    return true;
  };
  Out o = run("use f; s = 0; for (i = 0; i < 100; i += 1) { s += f(i); } s;", [&](Context & c) { add(c, "f", n); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 5050);
}

// ---------------------------------------------------------------------------
// Readers: a string keeps its tags
// ---------------------------------------------------------------------------

TEST(Handles, AStringIsReadWithTheEncodingOfEachSegment) {
  Native n;
  struct Seg {
    size_t offset, length;
    GLTANG_String_Type tag;
  };
  std::vector<Seg> segments;
  size_t graphemes = 0, bytes = 0;
  std::string copy;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle h;
    EXPECT_EQ(gltang_call_arg(call, 0, &h), GLTANG_OK);
    graphemes = gltang_handle_string_graphemes(call, h);
    const char * t = gltang_handle_text(call, h, &bytes);
    EXPECT_NE(t, nullptr);
    char buffer[64];
    size_t need = gltang_handle_copy_text(call, h, buffer, sizeof buffer);
    copy.assign(buffer, need);
    EXPECT_EQ(gltang_handle_copy_text(call, h, nullptr, 0), need) << "snprintf style";
    for (size_t i = 0; i < gltang_handle_string_segments(call, h); ++i) {
      GLTANG_StringSegment seg;
      seg.size = sizeof(seg);
      EXPECT_TRUE(gltang_handle_string_segment(call, h, i, &seg));
      segments.push_back({seg.offset, seg.length, seg.encoding});
    }
    GLTANG_StringSegment wrong;
    wrong.size = 1;
    EXPECT_FALSE(gltang_handle_string_segment(call, h, 0, &wrong)) << "the size prefix is checked";
    GLTANG_StringSegment out;
    out.size = sizeof(out);
    EXPECT_FALSE(gltang_handle_string_segment(call, h, 99, &out));
    gltang_call_return_null(call);
    return true;
  };
  // "caf\xc3\xa9 " trusted, "<b>&" html, then " ok" percent: three segments, one of them with a multibyte grapheme before the tags change.
  Out o = run("use f; use t; use h; use p; f(t + h + p);", [&](Context & c) {
    add(c, "f", n);
    ASSERT_EQ(gltang_library_add_string(c.library(), "t", "caf\xc3\xa9 ", 6, GLTANG_UNICODE_STRING_TYPE_TRUSTED), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_string(c.library(), "h", "<b>&", 4, GLTANG_UNICODE_STRING_TYPE_HTML), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_string(c.library(), "p", " ok", 3, GLTANG_UNICODE_STRING_TYPE_PERCENT), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(copy, "caf\xc3\xa9 <b>& ok");
  EXPECT_EQ(bytes, 13u);
  EXPECT_EQ(graphemes, 12u) << "the e-acute is one grapheme of two bytes";
  ASSERT_EQ(segments.size(), 3u);
  EXPECT_EQ(segments[0].offset, 0u);
  EXPECT_EQ(segments[0].length, 6u);
  EXPECT_EQ(segments[0].tag, GLTANG_UNICODE_STRING_TYPE_TRUSTED);
  EXPECT_EQ(segments[1].offset, 6u);
  EXPECT_EQ(segments[1].length, 4u);
  EXPECT_EQ(segments[1].tag, GLTANG_UNICODE_STRING_TYPE_HTML);
  EXPECT_EQ(segments[2].offset, 10u);
  EXPECT_EQ(segments[2].length, 3u);
  EXPECT_EQ(segments[2].tag, GLTANG_UNICODE_STRING_TYPE_PERCENT);
}

TEST(Handles, AnArrayOfMapsIsWalkedWithTheTagsOfEveryString) {
  Native n;
  std::string walk;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle rows;
    EXPECT_EQ(gltang_call_arg(call, 0, &rows), GLTANG_OK);
    for (size_t i = 0; i < gltang_handle_array_length(call, rows); ++i) {
      GLTANG_Handle row;
      EXPECT_EQ(gltang_handle_array_get(call, rows, i, &row), GLTANG_OK);
      for (size_t m = 0; m < gltang_handle_map_count(call, row); ++m) {
        GLTANG_Handle key, value;
        EXPECT_EQ(gltang_handle_map_key_at(call, row, m, &key), GLTANG_OK);
        EXPECT_EQ(gltang_handle_map_value_at(call, row, m, &value), GLTANG_OK);
        size_t kl = 0;
        const char * k = gltang_handle_text(call, key, &kl);
        GLTANG_StringSegment seg;
        seg.size = sizeof(seg);
        EXPECT_TRUE(gltang_handle_string_segment(call, value, 0, &seg));
        size_t vl = 0;
        const char * v = gltang_handle_text(call, value, &vl);
        walk += std::string(k, kl) + "=" + std::string(v, vl) + "/" + std::to_string(static_cast<int>(seg.encoding)) + ";";
      }
      GLTANG_Handle found;
      bool present = true;
      EXPECT_EQ(gltang_handle_map_get(call, row, "name", 4, &present, &found), GLTANG_OK);
      EXPECT_TRUE(present);
      EXPECT_EQ(gltang_handle_map_get(call, row, "nope", 4, &present, &found), GLTANG_OK);
      EXPECT_FALSE(present);
    }
    gltang_call_return_null(call);
    return true;
  };
  Out o = run("use f; use a; use b; f([{name: a, url: b}, {name: b, url: a}]);", [&](Context & c) {
    add(c, "f", n);
    ASSERT_EQ(gltang_library_add_string(c.library(), "a", "x<", 2, GLTANG_UNICODE_STRING_TYPE_HTML), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_string(c.library(), "b", "y z", 3, GLTANG_UNICODE_STRING_TYPE_PERCENT), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(walk, "name=x</1;url=y z/3;name=y z/3;url=x</1;");
}

// ---------------------------------------------------------------------------
// Constructors and the answer
// ---------------------------------------------------------------------------

TEST(Handles, ANativeAnswersAContainerItBuiltAndTheProgramPrintsEachLeafPerItsTag) {
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle rows, row, label, url, count, ratio, flag, nothing;
    EXPECT_EQ(gltang_handle_make_array(call, 0, &rows), GLTANG_OK);
    for (const char * text : {"Home <b>", "Tom & Jerry"}) {
      EXPECT_EQ(gltang_handle_make_map(call, 0, &row), GLTANG_OK);
      EXPECT_EQ(gltang_handle_make_string(call, text, strlen(text), GLTANG_UNICODE_STRING_TYPE_HTML, &label), GLTANG_OK);
      EXPECT_EQ(gltang_handle_make_string(call, "/a b", 4, GLTANG_UNICODE_STRING_TYPE_PERCENT, &url), GLTANG_OK);
      EXPECT_EQ(gltang_handle_map_set(call, row, "label", 5, GLTANG_UNICODE_STRING_TYPE_TRUSTED, label), GLTANG_OK);
      EXPECT_EQ(gltang_handle_map_set(call, row, "url", 3, GLTANG_UNICODE_STRING_TYPE_TRUSTED, url), GLTANG_OK);
      EXPECT_EQ(gltang_handle_array_push(call, rows, row), GLTANG_OK);
    }
    EXPECT_EQ(gltang_handle_make_integer(call, int64_t{1} << 61, &count), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_float(call, 0.25, &ratio), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_bool(call, true, &flag), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_null(call, &nothing), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_push(call, rows, count), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_push(call, rows, ratio), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_push(call, rows, flag), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_push(call, rows, nothing), GLTANG_OK);
    gltang_call_return_handle(call, rows);
    return true;
  };
  Out o = run("use menu; r = menu(); print(r[0][\"label\"]); print(\"|\"); print(r[1][\"url\"]); print(\"|\"); print(r[2]); print(\"|\"); print(r[3]); print(\"|\"); print(r[4]); print(\"|\"); print(r[5] == null);",
      [&](Context & c) { add(c, "menu", n); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "Home <b>|/a b|2305843009213693952|0.25|true|true");
  EXPECT_EQ(o.rendered, "Home &lt;b&gt;|%2Fa+b|2305843009213693952|0.25|true|true");
}

TEST(Handles, AContainerStoredIntoAContainerIsACopyAsInTheLanguage) {
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle outer, inner, one, seen;
    EXPECT_EQ(gltang_handle_make_array(call, 0, &outer), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_array(call, 0, &inner), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_integer(call, 1, &one), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_push(call, inner, one), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_push(call, outer, inner), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_push(call, outer, inner), GLTANG_OK);   // a second copy
    EXPECT_EQ(gltang_handle_array_push(call, inner, one), GLTANG_OK);     // after both pushes: the copies do not see it
    EXPECT_EQ(gltang_handle_array_get(call, outer, 0, &seen), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_length(call, seen), 1u);
    EXPECT_EQ(gltang_handle_array_length(call, inner), 2u);
    gltang_call_return_handle(call, outer);
    return true;
  };
  Out o = run("use f; x = f(); x[0][1] = 7; print(x);", [&](Context & c) { add(c, "f", n); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "[[1, 7], [1]]") << "the two pushed copies are independent of each other";
}

TEST(Handles, ARefusalOfBadInputChangesNothingAndNeverCrashes) {
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle h, arr, map, num;
    EXPECT_EQ(gltang_handle_make_string(call, "\xff", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &h), GLTANG_ERR_INVALID) << "not UTF-8";
    EXPECT_EQ(gltang_handle_make_string(call, "x", 1, static_cast<GLTANG_String_Type>(77), &h), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_handle_make_string(call, nullptr, 3, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &h), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_handle_make_string(call, "", 0, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &h), GLTANG_OK) << "the empty string is a string";
    EXPECT_EQ(gltang_handle_make_array(call, 0, &arr), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_map(call, 0, &map), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_integer(call, 1, &num), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_push(call, map, num), GLTANG_ERR_INVALID) << "push is for arrays";
    EXPECT_EQ(gltang_handle_map_set(call, arr, "k", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, num), GLTANG_ERR_INVALID) << "set is for maps";
    EXPECT_EQ(gltang_handle_map_set(call, map, "\xff", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, num), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_handle_array_push(call, arr, none()), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_handle_array_get(call, arr, 0, &h), GLTANG_ERR_INVALID) << "out of range";
    EXPECT_EQ(gltang_handle_array_length(call, num), 0u);
    EXPECT_EQ(gltang_handle_map_count(call, arr), 0u);
    EXPECT_EQ(gltang_handle_make_concat(call, num, num, &h), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_handle_make_library(call, nullptr, &h), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_handle_make_native(call, nullptr, nullptr, none(), &h), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_handle_array_length(call, arr), 0u);
    gltang_call_return_integer(call, 3);
    return true;
  };
  Out o = run("use f; f();", [&](Context & c) { add(c, "f", n); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 3) << "no refusal made the call out of memory";
}

TEST(Handles, ConcatenationKeepsTheTagsOfBothHalves) {
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle a, b, joined;
    EXPECT_EQ(gltang_handle_make_string(call, "<i>", 3, GLTANG_UNICODE_STRING_TYPE_HTML, &a), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_string(call, "<b>", 3, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &b), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_concat(call, a, b, &joined), GLTANG_OK);
    EXPECT_EQ(gltang_handle_string_segments(call, joined), 2u);
    gltang_call_return_handle(call, joined);
    return true;
  };
  Out o = run("use f; print(f());", [&](Context & c) { add(c, "f", n); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "<i><b>");
  EXPECT_EQ(o.rendered, "&lt;i&gt;<b>");
}

TEST(Handles, ALaterAnswerReplacesAnEarlierOneAndAHandleAnswerSurvivesTheScopeClosing) {
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle h;
    EXPECT_EQ(gltang_handle_make_integer(call, 8, &h), GLTANG_OK);
    gltang_call_return_integer(call, 1);
    gltang_call_return_handle(call, h);
    gltang_call_return_float(call, 2.0);
    gltang_call_return_handle(call, h);
    return true;
  };
  Out o = run("use f; f() + f();", [&](Context & c) { add(c, "f", n); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 16);
}

// ---------------------------------------------------------------------------
// Handles across collections
// ---------------------------------------------------------------------------

TEST(Handles, EveryHandleSurvivesACollectionAtEveryAllocationOfABuild) {
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle rows;
    EXPECT_EQ(gltang_handle_make_array(call, 0, &rows), GLTANG_OK);
    for (int i = 0; i < 30; ++i) {
      GLTANG_Handle row, name, value, big;
      EXPECT_EQ(gltang_handle_make_map(call, 0, &row), GLTANG_OK);
      std::string text = "row <" + std::to_string(i) + ">";
      EXPECT_EQ(gltang_handle_make_string(call, text.data(), text.size(), GLTANG_UNICODE_STRING_TYPE_HTML, &name), GLTANG_OK);
      EXPECT_EQ(gltang_handle_make_float(call, i + 0.5, &value), GLTANG_OK);
      EXPECT_EQ(gltang_handle_make_integer(call, (int64_t{1} << 60) + i, &big), GLTANG_OK);
      EXPECT_EQ(gltang_handle_map_set(call, row, "name", 4, GLTANG_UNICODE_STRING_TYPE_TRUSTED, name), GLTANG_OK);
      EXPECT_EQ(gltang_handle_map_set(call, row, "value", 5, GLTANG_UNICODE_STRING_TYPE_TRUSTED, value), GLTANG_OK);
      EXPECT_EQ(gltang_handle_map_set(call, row, "big", 3, GLTANG_UNICODE_STRING_TYPE_TRUSTED, big), GLTANG_OK);
      EXPECT_EQ(gltang_handle_array_push(call, rows, row), GLTANG_OK);
    }
    // The first handles are still good after thirty rounds of allocation.
    EXPECT_EQ(gltang_handle_array_length(call, rows), 30u);
    gltang_call_return_handle(call, rows);
    return true;
  };
  Config config = forced_torture();
  Out o = run("use f; r = f(); s = 0; for (x : r) { s += x[\"big\"] % 1000 + x[\"value\"]; } print(r[29][\"name\"]); print(\"|\"); print(s);", [&](Context & c) { add(c, "f", n); }, config);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.rendered.rfind("row &lt;29&gt;|", 0), 0u) << o.rendered;
  EXPECT_NE(o.raw.find("row <29>|"), std::string::npos);
  config.moving_stack = 1;
  Out p = run("use f; r = f(); print(r[0][\"name\"]);", [&](Context & c) { add(c, "f", n); }, config);
  ASSERT_TRUE(p.finished);
  EXPECT_EQ(p.raw, "row <0>");
}

TEST(Handles, ACallbackThatGetsAHandleToAnArgumentReadsItAgainAfterEveryAllocationItMakes) {
  Native n;
  std::string got;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle arg, copy, filler;
    EXPECT_EQ(gltang_call_arg(call, 0, &arg), GLTANG_OK);
    for (int i = 0; i < 20; ++i) {
      EXPECT_EQ(gltang_handle_make_string(call, "filler filler filler", 20, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &filler), GLTANG_OK);
    }
    size_t length = 0;
    const char * t = gltang_handle_text(call, arg, &length);
    got.assign(t, length);
    EXPECT_EQ(gltang_handle_make_concat(call, arg, arg, &copy), GLTANG_OK);
    t = gltang_handle_text(call, copy, &length);
    got += "|" + std::string(t, length);
    gltang_call_return_null(call);
    return true;
  };
  Out o = run("use f; f(\"abc\" + \"def\");", [&](Context & c) { add(c, "f", n); }, forced_torture());
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(got, "abcdef|abcdefabcdef");
}

// ---------------------------------------------------------------------------
// Persistent handles
// ---------------------------------------------------------------------------

TEST(Persistent, APromotedValueLivesAcrossCallbacksCollectionsAndRelease) {
  static GLTANG_Persistent kept;
  kept = GLTANG_Persistent{0, 0};
  Native save, use, drop;
  save.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle h, e;
    EXPECT_EQ(gltang_handle_make_array(call, 0, &h), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_string(call, "kept <1>", 8, GLTANG_UNICODE_STRING_TYPE_HTML, &e), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_push(call, h, e), GLTANG_OK);
    EXPECT_EQ(gltang_handle_promote(call, h, &kept), GLTANG_OK);
    gltang_call_return_null(call);
    return true;
  };
  use.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle h, e;
    EXPECT_EQ(gltang_persistent_get(call, kept, &h), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_length(call, h), 1u);
    EXPECT_EQ(gltang_handle_array_get(call, h, 0, &e), GLTANG_OK);
    GLTANG_StringSegment seg;
    seg.size = sizeof(seg);
    EXPECT_TRUE(gltang_handle_string_segment(call, e, 0, &seg));
    EXPECT_EQ(seg.encoding, GLTANG_UNICODE_STRING_TYPE_HTML);
    gltang_call_return_handle(call, e);
    return true;
  };
  drop.hook = [&](GLTANG_NativeCall * call) {
    EXPECT_EQ(gltang_persistent_release(call, kept), GLTANG_OK);
    EXPECT_EQ(gltang_persistent_release(call, kept), GLTANG_ERR_INVALID) << "released twice";
    GLTANG_Handle h;
    EXPECT_EQ(gltang_persistent_get(call, kept, &h), GLTANG_ERR_INVALID) << "a tombstone";
    gltang_call_return_null(call);
    return true;
  };
  Out o = run(
      "use save; use use_it; use drop; save(); junk = \"\"; for (i = 0; i < 200; i += 1) { junk = junk + \"garbage \" + i; } print(use_it()); print(\"|\"); print(use_it()); drop();",
      [&](Context & c) { add(c, "save", save); add(c, "use_it", use); add(c, "drop", drop); }, forced_torture());
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "kept <1>|kept <1>");
  EXPECT_EQ(o.rendered, "kept &lt;1&gt;|kept &lt;1&gt;");
  EXPECT_EQ(save.calls, 1);
}

TEST(Persistent, TheLimitIsEnforcedAndAReleasedSlotIsReusedWithANewGeneration) {
  Native n;
  std::vector<GLTANG_Persistent> kept;
  GLTANG_Result over = GLTANG_OK;
  GLTANG_Persistent stale = {0, 0}, reused = {0, 0};
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle h;
    EXPECT_EQ(gltang_handle_make_integer(call, 5, &h), GLTANG_OK);
    for (int i = 0; i < 4; ++i) {
      GLTANG_Persistent p;
      EXPECT_EQ(gltang_handle_promote(call, h, &p), GLTANG_OK);
      kept.push_back(p);
    }
    GLTANG_Persistent p;
    over = gltang_handle_promote(call, h, &p);
    stale = kept[1];
    EXPECT_EQ(gltang_persistent_release(call, kept[1]), GLTANG_OK);
    EXPECT_EQ(gltang_handle_promote(call, h, &reused), GLTANG_OK);
    EXPECT_EQ(reused.index, stale.index) << "the slot is reused";
    EXPECT_NE(reused.generation, stale.generation) << "and the old handle is a tombstone";
    GLTANG_Handle g;
    EXPECT_EQ(gltang_persistent_get(call, stale, &g), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_persistent_get(call, reused, &g), GLTANG_OK);
    GLTANG_Persistent forged = {99, 1};
    EXPECT_EQ(gltang_persistent_get(call, forged, &g), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_persistent_release(call, forged), GLTANG_ERR_INVALID);
    gltang_call_return_null(call);
    return true;
  };
  Compiled compiled("use f; f();");
  Context context(compiled.program);
  ASSERT_EQ(gltang_execution_set_handle_limits(context.execution, 0, 4), GLTANG_OK);
  add(context, "f", n);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(over, GLTANG_ERR_LIMIT);
  // The execution is destroyed with four persistents live: the context's
  // destructor requires every block to have come back.
}

TEST(Persistent, ASnapshotIsRefusedWhileOneIsHeld) {
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle h;
    GLTANG_Persistent p;
    EXPECT_EQ(gltang_handle_make_integer(call, 1, &h), GLTANG_OK);
    EXPECT_EQ(gltang_handle_promote(call, h, &p), GLTANG_OK);
    gltang_call_return_null(call);
    return true;
  };
  Compiled compiled("use f; f();");
  Context context(compiled.program);
  add(context, "f", n);
  ASSERT_TRUE(context.execute());
  GLTANG_Snapshot * snapshot = nullptr;
  EXPECT_EQ(gltang_snapshot_take(context.execution, &snapshot), GLTANG_ERR_UNSUPPORTED);
  EXPECT_EQ(snapshot, nullptr);
}

// ---------------------------------------------------------------------------
// Natives bound to a value, libraries, factories
// ---------------------------------------------------------------------------

TEST(BoundNative, EachObjectsMethodReadsItsOwnValue) {
  Native name_of, make;
  name_of.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle bound;
    EXPECT_EQ(gltang_call_bound(call, &bound), GLTANG_OK);
    bool found = false;
    GLTANG_Handle value;
    EXPECT_EQ(gltang_handle_map_get(call, bound, "name", 4, &found, &value), GLTANG_OK);
    EXPECT_TRUE(found);
    gltang_call_return_handle(call, value);
    return true;
  };
  // `account(name)` answers a map {name, label: <a native bound to a copy of the map>}.
  make.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle arg, obj, fn;
    EXPECT_EQ(gltang_call_arg(call, 0, &arg), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_map(call, 0, &obj), GLTANG_OK);
    EXPECT_EQ(gltang_handle_map_set(call, obj, "name", 4, GLTANG_UNICODE_STRING_TYPE_TRUSTED, arg), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_native(call, run_hook, &name_of.hook, obj, &fn), GLTANG_OK);
    EXPECT_EQ(gltang_handle_map_set(call, obj, "label", 5, GLTANG_UNICODE_STRING_TYPE_TRUSTED, fn), GLTANG_OK);
    gltang_call_return_handle(call, obj);
    return true;
  };
  Out o = run(
      "use account; a = account(\"ann\"); b = account(\"bob\"); print(a[\"label\"]()); print(\"|\"); print(b[\"label\"]()); print(\"|\"); f = a[\"label\"]; print(f());",
      [&](Context & c) { add(c, "account", make); }, forced_torture());
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "ann|bob|ann");
}

TEST(BoundNative, ANativeBoundToNothingSeesNullAndOneBoundToAContainerSeesACopy) {
  Native make;
  GLTANG_ValueKind kind_of_bound = GLTANG_KIND_ERROR;
  Native probe;
  probe.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle bound;
    EXPECT_EQ(gltang_call_bound(call, &bound), GLTANG_OK);
    kind_of_bound = gltang_handle_kind(call, bound);
    gltang_call_return_handle(call, bound);
    return true;
  };
  make.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle arr, one, fn;
    EXPECT_EQ(gltang_handle_make_array(call, 0, &arr), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_integer(call, 1, &one), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_push(call, arr, one), GLTANG_OK);
    EXPECT_EQ(gltang_handle_make_native(call, run_hook, &probe.hook, arr, &fn), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_push(call, arr, one), GLTANG_OK);   // after binding: the native's value does not change
    gltang_call_return_handle(call, fn);
    return true;
  };
  Out o = run("use make; use plain; f = make(); print(f()); print(\"|\"); print(plain());", [&](Context & c) {
    add(c, "make", make);
    ASSERT_EQ(gltang_library_add_native(c.library(), "plain", run_hook, &probe.hook), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "[1]|");
  EXPECT_EQ(kind_of_bound, GLTANG_KIND_NULL) << "the last call was the plain native, bound to nothing";
}

TEST(BoundNative, ASnapshotIsRefusedWhileTheExecutionOwnsOne) {
  Native make;
  make.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle fn;
    EXPECT_EQ(gltang_handle_make_native(call, run_hook, nullptr, none(), &fn), GLTANG_OK);
    gltang_call_return_handle(call, fn);
    return true;
  };
  Compiled compiled("use make; f = make(); 1;");
  Context context(compiled.program);
  add(context, "make", make);
  ASSERT_TRUE(context.execute());
  GLTANG_Snapshot * snapshot = nullptr;
  EXPECT_EQ(gltang_snapshot_take(context.execution, &snapshot), GLTANG_ERR_UNSUPPORTED);
}

namespace {

/// A library `pages` holding a factory `recent` that answers a library of two titles.
bool recent_factory(GLTANG_NativeCall * call, void * user) {
  int * calls = static_cast<int *>(user);
  ++*calls;
  GLTANG_Library * recent = nullptr;
  if (gltang_library_create("recent", &recent) != GLTANG_OK || gltang_library_add_string(recent, "first", "One <1>", 7, GLTANG_UNICODE_STRING_TYPE_HTML) != GLTANG_OK
      || gltang_library_add_integer(recent, "count", *calls) != GLTANG_OK) {
    return false;
  }
  GLTANG_Handle h;
  GLTANG_Result r = gltang_handle_make_library(call, recent, &h);
  gltang_library_release(recent);   // the execution keeps its own reference
  if (r != GLTANG_OK) {
    return false;
  }
  gltang_call_return_handle(call, h);
  return true;
}

}  // namespace

TEST(HandleFactory, AFactoryAnswersALibraryAndUseReadsItsMembers) {
  int calls = 0;
  Out o = run("use pages.recent as r; use pages.recent as again; print(r.first); print(r.count); print(again.count); r;", [&](Context & c) {
    GLTANG_Library * pages = nullptr;
    ASSERT_EQ(gltang_library_create("pages", &pages), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_handle_factory(pages, "recent", recent_factory, &calls), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_library(c.library(), pages), GLTANG_OK);
    gltang_library_release(pages);
  }, forced_torture());
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(calls, 2) << "one call for each use that reaches it";
  EXPECT_EQ(o.raw, "One <1>12");
  EXPECT_EQ(o.rendered, "One &lt;1&gt;12");
  EXPECT_EQ(o.kind, GLTANG_KIND_LIBRARY);
}

TEST(HandleFactory, TheLibrariesACallbackMadeAreFreedWithTheExecution) {
  // The libraries are cutil's memory, not the context's, so the context's own leak
  // check does not see them: count cutil's allocations and frees across the run.
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    for (int i = 0; i < 3; ++i) {
      GLTANG_Library * lib = nullptr;
      EXPECT_EQ(gltang_library_create("made", &lib), GLTANG_OK);
      EXPECT_EQ(gltang_library_add_integer(lib, "v", i), GLTANG_OK);
      GLTANG_Handle h;
      EXPECT_EQ(gltang_handle_make_library(call, lib, &h), GLTANG_OK);
      gltang_library_release(lib);   // the host lets go; only the execution holds it
    }
    gltang_call_return_null(call);
    return true;
  };
  const size_t live_before = gcu_memory_alloc_count - gcu_memory_free_count;
  {
    Compiled compiled("use f; f(); 1;");
    Context context(compiled.program);
    add(context, "f", n);
    ASSERT_TRUE(context.execute());
  }
  EXPECT_EQ(gcu_memory_alloc_count - gcu_memory_free_count, live_before) << "a library the execution held was not released";
}

TEST(HandleFactory, IsNeverCalledAtRegistrationOrWhenNothingReachesIt) {
  int calls = 0;
  Out o = run("use other; if (false) { use lazy; } other;", [&](Context & c) {
    ASSERT_EQ(gltang_library_add_handle_factory(c.library(), "lazy", recent_factory, &calls), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_integer(c.library(), "other", 3), GLTANG_OK);
    EXPECT_EQ(calls, 0);
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(calls, 0);
}

TEST(HandleFactory, AnAnswerOfAnyKindAndFalseIsNull) {
  struct F {
    static bool container(GLTANG_NativeCall * call, void *) {
      GLTANG_Handle m, v;
      if (gltang_handle_make_map(call, 0, &m) != GLTANG_OK || gltang_handle_make_string(call, "<hi>", 4, GLTANG_UNICODE_STRING_TYPE_HTML, &v) != GLTANG_OK ||
          gltang_handle_map_set(call, m, "greeting", 8, GLTANG_UNICODE_STRING_TYPE_TRUSTED, v) != GLTANG_OK) {
        return false;
      }
      gltang_call_return_handle(call, m);
      return true;
    }
    static bool declines(GLTANG_NativeCall * call, void *) {
      gltang_call_return_integer(call, 9);   // an answer is ignored when it says it is not available
      return false;
    }
    static bool scalar(GLTANG_NativeCall * call, void *) {
      gltang_call_return_integer(call, 4);
      return true;
    }
    static bool silent(GLTANG_NativeCall *, void *) {
      return true;
    }
  };
  Out o = run("use c; use d; use s; use n; print(c[\"greeting\"]); print(d == null); print(s); print(n == null);", [&](Context & ctx) {
    ASSERT_EQ(gltang_library_add_handle_factory(ctx.library(), "c", F::container, nullptr), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_handle_factory(ctx.library(), "d", F::declines, nullptr), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_handle_factory(ctx.library(), "s", F::scalar, nullptr), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_handle_factory(ctx.library(), "n", F::silent, nullptr), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "<hi>true4true");
  EXPECT_EQ(o.rendered, "&lt;hi&gt;true4true");
}

TEST(HandleFactory, ANativeAnswersALibraryAndAHostThatReleasesItsReferenceAtOnceIsSafe) {
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Library * lib = nullptr;
    EXPECT_EQ(gltang_library_create("made", &lib), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_integer(lib, "v", 77), GLTANG_OK);
    GLTANG_Handle h;
    EXPECT_EQ(gltang_handle_make_library(call, lib, &h), GLTANG_OK);
    GLTANG_Handle again;
    EXPECT_EQ(gltang_handle_make_library(call, lib, &again), GLTANG_OK) << "the same library twice is held once";
    EXPECT_TRUE(gltang_library_sealed(lib));
    gltang_library_release(lib);
    gltang_call_return_handle(call, h);
    return true;
  };
  Out o = run("use f; l = f(); junk = \"\"; for (i = 0; i < 100; i += 1) { junk = junk + \"x\"; } print(l.v); l.v;", [&](Context & c) { add(c, "f", n); }, forced_torture());
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "77");
  EXPECT_EQ(o.integer, 77);
}

// ---------------------------------------------------------------------------
// Compiled code reaches the same API
// ---------------------------------------------------------------------------

TEST(Handles, ANativeCalledFromACompiledFunctionUsesHandlesAndFactoriesAreReachedThere) {
  Native n;
  n.hook = [&](GLTANG_NativeCall * call) {
    GLTANG_Handle a, v;
    EXPECT_EQ(gltang_call_arg(call, 0, &a), GLTANG_OK);
    EXPECT_EQ(gltang_handle_array_get(call, a, 0, &v), GLTANG_OK);
    GLTANG_Handle made;
    EXPECT_EQ(gltang_handle_make_integer(call, gltang_handle_integer(call, v) + 1, &made), GLTANG_OK);
    gltang_call_return_handle(call, made);
    return true;
  };
  int calls = 0;
  Config config;
  config.jit_threshold = 1;
  Out o = run("function g() { use f; use pages.recent as r; t = 0; for (i = 0; i < 100; i += 1) { t = t + f([i]); } return t + r.count; } g();", [&](Context & c) {
    add(c, "f", n);
    GLTANG_Library * pages = nullptr;
    ASSERT_EQ(gltang_library_create("pages", &pages), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_handle_factory(pages, "recent", recent_factory, &calls), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_library(c.library(), pages), GLTANG_OK);
    gltang_library_release(pages);
  }, config);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 5050 + 1);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
