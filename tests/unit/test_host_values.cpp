// Host-described values (description.h): what a program sees of a value a
// host built without touching its heap, handed over as a library member, by a
// lazy factory, and as a native function's answer.
//
// The rendering tests are differentials: the same data reaches a program once
// as a description and once through the engine's own path (library strings
// bound with `use` and put into containers by the script), and what they print
// must be identical, with the tags in place. A comparison with nothing on the
// other side would pass for the wrong reason, so each also requires the
// encoding to have happened (the rendered text differs from the raw text).
//
// The builder's own rules are in test_description.cpp.

#include "description_harness.h"

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using tt::Compiled;
using tt::Config;
using tt::Context;
using tt::Desc;

namespace {

struct Out {
  bool finished = false;
  GRCORE_Result ran = GRCORE_OK;
  std::string raw;
  std::string rendered;
  std::string described;
  GLTANG_ValueKind kind = GLTANG_KIND_NULL;
  GLTANG_ErrorKind error = GLTANG_ERROR_KIND_COUNT;
  int64_t integer = 0;
  size_t size = 0;
  size_t errors = 0;
  bool fired = false;
  uint64_t fuel = 0;
};

/// Runs `source` with whatever `setup` adds to the context's library.
Out run(const std::string & source, const std::function<void(Context &)> & setup, tt::Mode mode = tt::Mode::Script, const Config & config = Config()) {
  Out out;
  Compiled compiled(source, mode);
  EXPECT_TRUE(compiled.ok()) << source << ": " << compiled.error.message;
  if (!compiled.ok()) {
    return out;
  }
  Context context(compiled.program, config);
  if (!context.ok()) {
    // Only an allocation sweep may fail to make the context at all.
    EXPECT_NE(config.fail_at, 0) << "the context could not be made";
    out.fired = context.tracker.fired;
    return out;
  }
  setup(context);
  out.finished = context.execute();
  out.ran = context.ran;
  out.raw = context.raw();
  out.rendered = context.rendered();
  out.described = context.describe();
  out.kind = context.kind();
  out.size = context.size();
  out.errors = context.error_count();
  if (context.is_error()) {
    out.error = context.error_kind();
  }
  if (context.is_integer()) {
    out.integer = context.integer();
  }
  out.fired = context.tracker.fired;
  out.fuel = grcore_context_fuel_used(context.context);
  return out;
}

Out run(const std::string & source, GLTANG_ValueDescription * value, const char * name = "v", tt::Mode mode = tt::Mode::Script, const Config & config = Config()) {
  return run(source, [&](Context & context) { ASSERT_EQ(gltang_library_add_value(context.library(), name, value), GLTANG_OK); }, mode, config);
}

// ---------------------------------------------------------------------------
// The data of the rendering differentials: rows of cells, each a key, a text and a tag
// ---------------------------------------------------------------------------

struct Cell {
  const char * key;
  const char * text;
  GLTANG_String_Type tag;
};

using Row = std::vector<Cell>;

const std::vector<Row> kRows = {
  {{"name", "Ann <a&b>", GLTANG_UNICODE_STRING_TYPE_HTML},
   {"url", "a b&c=d/\xc3\xa9", GLTANG_UNICODE_STRING_TYPE_PERCENT},
   {"js", "it's \"q\"\n", GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT},
   {"attr", "x\"y'z", GLTANG_UNICODE_STRING_TYPE_HTML_ATTRIBUTE},
   {"note", "<i>ok</i>", GLTANG_UNICODE_STRING_TYPE_TRUSTED}},
  {{"name", "Bob </b> & co", GLTANG_UNICODE_STRING_TYPE_HTML},
   {"url", "q=1 2", GLTANG_UNICODE_STRING_TYPE_PERCENT},
   {"js", "</script>", GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT},
   {"attr", "a<b>c", GLTANG_UNICODE_STRING_TYPE_HTML_ATTRIBUTE},
   {"note", "<b>fine</b>", GLTANG_UNICODE_STRING_TYPE_TRUSTED}},
};

/// The rows as a description: an array of maps of tagged strings.
GLTANG_ValueDescription * rows_description(Desc & d, const std::vector<Row> & rows = kRows) {
  GLTANG_ValueRef array = d.array();
  for (const Row & row : rows) {
    GLTANG_ValueRef map = d.map();
    for (const Cell & cell : row) {
      d.set(map, cell.key, d.string(cell.text, cell.tag));
    }
    d.push(array, map);
  }
  return d.root(array);
}

/// The same rows through the engine's own path: each cell a library string, and
/// a script that puts them into containers.
struct Reference {
  std::string definition;   ///< `use` of every cell, then `rows = [...]`.
  const std::vector<Row> & rows;

  explicit Reference(const std::vector<Row> & r = kRows) : rows(r) {
    std::string uses, literal = "[";
    for (size_t i = 0; i < rows.size(); ++i) {
      literal += i ? ", {" : "{";
      for (size_t c = 0; c < rows[i].size(); ++c) {
        std::string member = "c" + std::to_string(i) + "_" + std::to_string(c);
        uses += "use " + member + "; ";
        literal += std::string(c ? ", " : "") + rows[i][c].key + ": " + member;
      }
      literal += "}";
    }
    literal += "]";
    definition = uses + "rows = " + literal + ";";
  }

  void add_to(Context & context) const {
    for (size_t i = 0; i < rows.size(); ++i) {
      for (size_t c = 0; c < rows[i].size(); ++c) {
        std::string member = "c" + std::to_string(i) + "_" + std::to_string(c);
        EXPECT_EQ(gltang_library_add_string(context.library(), member.c_str(), rows[i][c].text, strlen(rows[i][c].text), rows[i][c].tag), GLTANG_OK);
      }
    }
  }
};

/// A program body that reads `rows` and prints every cell, separated.
const char * kPrintCells =
    "for (r : rows) { print(r[\"name\"]); print(\"|\"); print(r[\"url\"]); print(\"|\"); print(r[\"js\"]); print(\"|\"); print(r[\"attr\"]); print(\"|\"); print(r[\"note\"]); print(\"\\n\"); }";

struct Both {
  Out described;
  Out reference;
};

/// Runs `body` once over `rows` as a description and once over `rows` as the script built them.
Both through_both(const std::string & body, tt::Mode mode = tt::Mode::Script, const std::vector<Row> & rows = kRows, const Config & config = Config()) {
  Both both;
  Desc d;
  gltang_value_description_seal(rows_description(d, rows));
  both.described = run((mode == tt::Mode::Script ? "use rows; " : "<% use rows; %>") + body, d.d, "rows", mode, config);
  Reference reference(rows);
  both.reference = run((mode == tt::Mode::Script ? reference.definition + " " : "<% " + reference.definition + " %>") + body, [&](Context & context) { reference.add_to(context); }, mode, config);
  return both;
}

void expect_identical(const Both & both) {
  ASSERT_TRUE(both.described.finished);
  ASSERT_TRUE(both.reference.finished);
  EXPECT_EQ(both.described.raw, both.reference.raw);
  EXPECT_EQ(both.described.rendered, both.reference.rendered);
  EXPECT_EQ(both.described.described, both.reference.described);
  EXPECT_EQ(both.described.kind, both.reference.kind);
  // The comparison shows something about the tags only if they did something.
  EXPECT_NE(both.described.rendered, both.described.raw) << "nothing was encoded";
}

}  // namespace

// ---------------------------------------------------------------------------
// A library member
// ---------------------------------------------------------------------------

TEST(HostValues, ANestedValueIsReadWithEachLeafInItsOwnEncoding) {
  Both both = through_both(kPrintCells);
  expect_identical(both);
  // And the encodings themselves, not only their agreement with the engine's path.
  EXPECT_NE(both.described.rendered.find("Ann &lt;a&amp;b&gt;"), std::string::npos) << both.described.rendered;
  EXPECT_NE(both.described.rendered.find("<i>ok</i>"), std::string::npos) << "a trusted string is not encoded";
  EXPECT_EQ(both.described.raw.find("&lt;"), std::string::npos) << "the raw output is never encoded";
}

TEST(HostValues, ATemplateIteratesAHostArrayOfMapsAndPrintsEachField) {
  const char * page =
      "<ul><% for (r : rows) { %><li><%= r[\"name\"] %> <a href=\"/p?q=<%= r[\"url\"] %>\" title=\"<%= r[\"attr\"] %>\">go</a> "
      "<script>var s = '<%= r[\"js\"] %>';</script> <%= r[\"note\"] %></li>\n<% } %></ul>";
  Both both = through_both(page, tt::Mode::Template);
  expect_identical(both);
  EXPECT_NE(both.described.rendered.find("<li>Ann &lt;a&amp;b&gt; "), std::string::npos) << both.described.rendered;
  EXPECT_NE(both.described.rendered.find("<i>ok</i></li>"), std::string::npos);
  // The page is built from the markup of its text and the data's own encoding.
  EXPECT_EQ(both.described.rendered.compare(0, 4, "<ul>"), 0);
}

TEST(HostValues, TheTagsSurvivePrintAsStringAndConcatenation) {
  expect_identical(through_both("print(rows);"));
  expect_identical(through_both("print(rows as string);"));
  expect_identical(through_both("print(rows[0] as string);"));
  expect_identical(through_both("print(rows[1][\"name\"] + \"!\" + rows[0][\"name\"]);"));
  expect_identical(through_both("t = \"[\" + rows[0] + \"]\"; print(t);"));
  expect_identical(through_both("s = rows as string; print(s + s);"));
  expect_identical(through_both("copy = rows; print(copy[1]);"));
  expect_identical(through_both("moved = rows + rows; print(moved);"));
}

TEST(HostValues, AMapKeyKeepsItsTag) {
  for (GLTANG_String_Type tag : tt::kAllTags) {
    SCOPED_TRACE(static_cast<int>(tag));
    Desc d;
    GLTANG_ValueRef map = d.map();
    d.set(map, "k<&\"'/ \xc3\xa9", d.integer(1), tag);
    gltang_value_description_seal(d.root(map));
    Out described = run("use v; print(v);", d.d);
    Out reference = run(
        "use k; m = {:}; m[k] = 1; print(m);",
        [&](Context & context) { gltang_library_add_string(context.library(), "k", "k<&\"'/ \xc3\xa9", strlen("k<&\"'/ \xc3\xa9"), tag); });
    ASSERT_TRUE(described.finished);
    ASSERT_TRUE(reference.finished);
    EXPECT_EQ(described.rendered, reference.rendered);
    EXPECT_EQ(described.raw, reference.raw);
    if (tag != GLTANG_UNICODE_STRING_TYPE_TRUSTED) {
      EXPECT_NE(described.rendered, described.raw) << "the key was not encoded";
    }
  }
}

TEST(HostValues, EveryScalarKindArrivesAsItself) {
  Desc d;
  GLTANG_ValueRef a = d.array();
  d.push(a, d.null());
  d.push(a, d.boolean(true));
  d.push(a, d.boolean(false));
  d.push(a, d.integer(0));
  d.push(a, d.integer(-17));
  d.push(a, d.integer((int64_t{1} << 58) - 1));        // the largest inline integer
  d.push(a, d.integer(int64_t{1} << 62));              // boxed
  d.push(a, d.integer(INT64_MIN));
  d.push(a, d.number(2.5));
  d.push(a, d.number(-0.0));
  d.push(a, d.string("h\xc3\xa9llo"));
  d.push(a, d.string("e\xcc\x81"));                    // one grapheme of two code points
  d.push(a, d.string(""));
  gltang_value_description_seal(d.root(a));
  Compiled compiled("use v; v;");
  Context context(compiled.program);
  ASSERT_EQ(gltang_library_add_value(context.library(), "v", d.d), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  ASSERT_TRUE(context.is_array());
  ASSERT_EQ(context.size(), 13u);
  EXPECT_EQ(context.element(0).item.kind, GLTANG_KIND_NULL);
  ASSERT_TRUE(context.element(1).is_bool());
  EXPECT_TRUE(context.element(1).boolean());
  EXPECT_FALSE(context.element(2).boolean());
  EXPECT_EQ(context.element(3).integer(), 0);
  EXPECT_EQ(context.element(4).integer(), -17);
  EXPECT_EQ(context.element(5).integer(), (int64_t{1} << 58) - 1);
  EXPECT_EQ(context.element(6).integer(), int64_t{1} << 62);
  EXPECT_EQ(context.element(7).integer(), INT64_MIN);
  ASSERT_TRUE(context.element(8).is_float());
  EXPECT_EQ(context.element(8).number(), 2.5);
  EXPECT_TRUE(std::signbit(context.element(9).number())) << "negative zero";
  ASSERT_TRUE(context.element(10).is_string());
  EXPECT_EQ(context.element(10).text(), "h\xc3\xa9llo");
  EXPECT_EQ(context.element(10).size(), 5u) << "five graphemes";
  EXPECT_EQ(context.element(11).size(), 1u) << "a letter and its accent are one grapheme";
  EXPECT_EQ(context.element(12).text(), "");
  EXPECT_EQ(context.element(12).size(), 0u);
}

TEST(HostValues, TheRootMayBeAnyKindAndADescriptionWithNoRootIsNull) {
  {
    Desc d;
    Out o = run("use v; v;", d.root(d.string("just text", GLTANG_UNICODE_STRING_TYPE_HTML)));
    EXPECT_EQ(o.kind, GLTANG_KIND_STRING);
    EXPECT_EQ(o.raw, "");
  }
  {
    Desc d;
    Out o = run("use v; v;", d.root(d.integer(42)));
    EXPECT_EQ(o.kind, GLTANG_KIND_INTEGER);
    EXPECT_EQ(o.integer, 42);
  }
  {
    Desc d;
    Out o = run("use v; v;", d.root(d.number(0.25)));
    EXPECT_EQ(o.kind, GLTANG_KIND_FLOAT);
  }
  {
    Desc d;
    Out o = run("use v; v;", d.d);  // never given a root
    EXPECT_EQ(o.kind, GLTANG_KIND_NULL);
  }
  {
    Desc d;
    Out o = run("use v; v;", d.root(d.null()));
    EXPECT_EQ(o.kind, GLTANG_KIND_NULL);
  }
}

TEST(HostValues, EmptyContainersAndNestedEmptyOnes) {
  Desc d;
  GLTANG_ValueRef a = d.array();
  d.push(a, d.array());
  d.push(a, d.map());
  GLTANG_ValueRef inner = d.map();
  d.set(inner, "e", d.array());
  d.push(a, inner);
  Out o = run("use v; print(v); print(v.size);", d.root(a));
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "[[], {}, {\"e\": []}]3");
}

TEST(HostValues, ADuplicateKeyKeepsTheFirstPositionAndTakesTheLastValue) {
  Desc d;
  GLTANG_ValueRef m = d.map();
  d.set(m, "k", d.integer(1));
  d.set(m, "other", d.integer(3));
  d.set(m, "k", d.integer(2));
  d.set(m, "k", d.string("last"));
  d.set(m, "again", d.integer(4));
  Out described = run("use v; print(v);", d.root(m));
  Out reference = run("m = {:}; m[\"k\"] = 1; m[\"other\"] = 3; m[\"k\"] = 2; m[\"k\"] = \"last\"; m[\"again\"] = 4; print(m);", [](Context &) {});
  ASSERT_TRUE(described.finished);
  ASSERT_TRUE(reference.finished);
  EXPECT_EQ(described.raw, reference.raw);
  EXPECT_EQ(described.raw, "{\"k\": last, \"other\": 3, \"again\": 4}");
}

TEST(HostValues, KeysAreMatchedByTheirBytesWhateverTheirTag) {
  Desc d;
  GLTANG_ValueRef m = d.map();
  d.set(m, "k", d.integer(1), GLTANG_UNICODE_STRING_TYPE_HTML);
  d.set(m, "k", d.integer(2), GLTANG_UNICODE_STRING_TYPE_PERCENT);
  Out o = run("use v; print(v); print(v[\"k\"]);", d.root(m));
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "{\"k\": 2}2") << "one entry, found by its bytes, holding the last value";
}

TEST(HostValues, TwoReachesGiveTwoCopiesThatDoNotShareAnything) {
  Desc d;
  GLTANG_ValueRef a = d.array();
  d.push(a, d.integer(1));
  GLTANG_ValueRef m = d.map();
  d.set(m, "n", d.integer(10));
  d.push(a, m);
  gltang_value_description_seal(d.root(a));
  Out o = run(
      "use v; use v as w; v[0] = 99; v[1][\"n\"] = 77; print(w[0]); print(\"|\"); print(w[1][\"n\"]); print(\"|\"); print(v[0]); print(\"|\"); print(v[1][\"n\"]);", d.d);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "1|10|99|77");
}

TEST(HostValues, EachExecutionOfAUseInALoopReachesAFreshCopy) {
  Desc d;
  GLTANG_ValueRef a = d.array();
  d.push(a, d.integer(0));
  Out o = run("for (i = 0; i < 4; i += 1) { use v as c; c[0] = c[0] + 1; print(c[0]); }", d.root(a));
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "1111") << "a cached copy would count 1234";
}

TEST(HostValues, TheDescriptionIsUntouchedByWhatAProgramDoesAndEveryExecutionSeesItWhole) {
  Desc d;
  GLTANG_ValueRef a = d.array();
  d.push(a, d.integer(1));
  d.push(a, d.integer(2));
  gltang_value_description_seal(d.root(a));
  GLTANG_Library * root = nullptr;
  ASSERT_EQ(gltang_library_create(nullptr, &root), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_value(root, "v", d.d), GLTANG_OK);
  Compiled compiled("use v; v[0] = 100; v[2] = 3; v;");
  for (int run_number = 0; run_number < 3; ++run_number) {
    Context context(compiled.program);
    ASSERT_EQ(gltang_execution_set_libraries(context.execution, root), GLTANG_OK);
    context.attached = true;
    ASSERT_TRUE(context.execute());
    EXPECT_EQ(context.describe(), "[100, 2, 3]") << run_number;
  }
  EXPECT_EQ(gltang_value_description_nodes(d.d), 3u);
  gltang_library_release(root);
}

TEST(HostValues, APathIsReadThroughTheMemberAndAMissingStepIsTheEnginesOwnAnswer) {
  Desc d;
  GLTANG_ValueRef m = d.map();
  GLTANG_ValueRef inner = d.map();
  d.set(inner, "b", d.integer(7));
  d.set(m, "a", inner);
  d.set(m, "list", d.push(d.array(), d.string("x")));
  Out o = run("use v.a.b as x; use v.list as l; print(x); print(l[0]);", d.root(m), "v");
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "7x");
  // The same path over a map the script built, to see that a described map and a built one answer a missing step alike.
  Out missing = run("use v.nope as n; n;", d.d, "v");
  Out built = run("v = {a: 1}; v.nope;", [](Context &) {});
  ASSERT_TRUE(missing.finished);
  ASSERT_TRUE(built.finished);
  EXPECT_EQ(missing.kind, built.kind);
  EXPECT_EQ(missing.error, built.error);
}

TEST(HostValues, AMemberAndANestedLibraryBothHoldDescriptions) {
  Desc d;
  GLTANG_ValueRef a = d.array();
  d.push(a, d.integer(5));
  gltang_value_description_seal(d.root(a));
  Out o = run(
      "use app.rows as rows; use app.rows as other; rows[0] = 1; print(rows[0]); print(other[0]);",
      [&](Context & context) {
        GLTANG_Library * app = nullptr;
        ASSERT_EQ(gltang_library_create("app", &app), GLTANG_OK);
        ASSERT_EQ(gltang_library_add_value(app, "rows", d.d), GLTANG_OK);
        ASSERT_EQ(gltang_library_add_library(context.library(), app), GLTANG_OK);
        gltang_library_release(app);
      });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "15");
}

// ---------------------------------------------------------------------------
// A lazy factory
// ---------------------------------------------------------------------------

namespace {

struct Factory {
  int calls = 0;
  bool available = true;
  bool give_root = true;
  bool seen_wrong_description = false;
};

/// Builds `[calls]` and checks that what it was given is empty, unsealed and not keepable.
bool counting_factory(void * user, GLTANG_ValueDescription * out) {
  Factory * f = static_cast<Factory *>(user);
  ++f->calls;
  if (gltang_value_description_sealed(out) || gltang_value_description_nodes(out) != 0 || gltang_value_description_retain(out) != nullptr) {
    f->seen_wrong_description = true;
  }
  if (!f->available) {
    return false;
  }
  if (!f->give_root) {
    return true;
  }
  GLTANG_ValueRef array = {0, 0}, n = {0, 0};
  EXPECT_EQ(gltang_value_array(out, &array), GLTANG_OK);
  EXPECT_EQ(gltang_value_integer(out, f->calls, &n), GLTANG_OK);
  EXPECT_EQ(gltang_value_array_push(out, array, n), GLTANG_OK);
  EXPECT_EQ(gltang_value_set_root(out, array), GLTANG_OK);
  return true;
}

}  // namespace

TEST(ValueFactory, IsNeverCalledAtRegistrationOrByAProgramThatDoesNotReachIt) {
  Factory f;
  {
    Compiled compiled("use other; if (false) { use lazy; } other;");
    Context context(compiled.program);
    ASSERT_EQ(gltang_library_add_value_factory(context.library(), "lazy", counting_factory, &f), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_integer(context.library(), "other", 3), GLTANG_OK);
    EXPECT_EQ(f.calls, 0) << "not at registration";
    ASSERT_TRUE(context.execute());
  }
  EXPECT_EQ(f.calls, 0);
}

TEST(ValueFactory, IsCalledOncePerExecutedUseAndEachReachIsAFreshValue) {
  Factory f;
  Out o = run("use lazy; use lazy as again; if (false) { use lazy as never; } lazy[0] = 100; print(lazy); print(again);",
      [&](Context & context) { ASSERT_EQ(gltang_library_add_value_factory(context.library(), "lazy", counting_factory, &f), GLTANG_OK); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(f.calls, 2);
  EXPECT_EQ(o.raw, "[100][2]");
  EXPECT_FALSE(f.seen_wrong_description) << "the description a factory is given is empty, unsealed and cannot be kept";
}

TEST(ValueFactory, AReachInALoopCallsItEachTime) {
  Factory f;
  Out o = run("for (i = 0; i < 5; i += 1) { use lazy as x; print(x[0]); }",
      [&](Context & context) { ASSERT_EQ(gltang_library_add_value_factory(context.library(), "lazy", counting_factory, &f), GLTANG_OK); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(f.calls, 5);
  EXPECT_EQ(o.raw, "12345");
}

TEST(ValueFactory, DecliningBindsNullAsTheScalarFactoryDoes) {
  Factory f;
  f.available = false;
  Out o = run("use lazy; print(lazy == null); lazy;",
      [&](Context & context) { ASSERT_EQ(gltang_library_add_value_factory(context.library(), "lazy", counting_factory, &f), GLTANG_OK); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.kind, GLTANG_KIND_NULL);
  EXPECT_EQ(f.calls, 1);
}

TEST(ValueFactory, ADescriptionLeftWithoutARootIsNull) {
  Factory f;
  f.give_root = false;
  Out o = run("use lazy; lazy;",
      [&](Context & context) { ASSERT_EQ(gltang_library_add_value_factory(context.library(), "lazy", counting_factory, &f), GLTANG_OK); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.kind, GLTANG_KIND_NULL);
}

TEST(ValueFactory, BuildsTaggedNestedValuesAndTheyAreReadWithTheirTags) {
  struct Build {
    static bool menu(void *, GLTANG_ValueDescription * out) {
      GLTANG_ValueRef items = {0, 0}, item = {0, 0}, label = {0, 0}, url = {0, 0};
      EXPECT_EQ(gltang_value_array(out, &items), GLTANG_OK);
      for (const char * text : {"Home <b>", "Tom & Jerry"}) {
        EXPECT_EQ(gltang_value_map(out, &item), GLTANG_OK);
        EXPECT_EQ(gltang_value_string(out, text, strlen(text), GLTANG_UNICODE_STRING_TYPE_HTML, &label), GLTANG_OK);
        EXPECT_EQ(gltang_value_string(out, "/a b", 4, GLTANG_UNICODE_STRING_TYPE_PERCENT, &url), GLTANG_OK);
        EXPECT_EQ(gltang_value_map_set(out, item, "label", 5, GLTANG_UNICODE_STRING_TYPE_TRUSTED, label), GLTANG_OK);
        EXPECT_EQ(gltang_value_map_set(out, item, "url", 3, GLTANG_UNICODE_STRING_TYPE_TRUSTED, url), GLTANG_OK);
        EXPECT_EQ(gltang_value_array_push(out, items, item), GLTANG_OK);
      }
      EXPECT_EQ(gltang_value_set_root(out, items), GLTANG_OK);
      return true;
    }
  };
  Out o = run("<ul><% use menu; for (m : menu) { %><li><a href=\"<%= m[\"url\"] %>\"><%= m[\"label\"] %></a></li><% } %></ul>",
      [](Context & context) { ASSERT_EQ(gltang_library_add_value_factory(context.library(), "menu", Build::menu, nullptr), GLTANG_OK); }, tt::Mode::Template);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.rendered, "<ul><li><a href=\"%2Fa+b\">Home &lt;b&gt;</a></li><li><a href=\"%2Fa+b\">Tom &amp; Jerry</a></li></ul>");
  EXPECT_EQ(o.raw, "<ul><li><a href=\"/a b\">Home <b></a></li><li><a href=\"/a b\">Tom & Jerry</a></li></ul>");
}

TEST(ValueFactory, WhatItBuildsIsChargedToTheContextsMemory) {
  struct Hog {
    int accepted = 0;
    GLTANG_Result refused = GLTANG_OK;
    size_t nodes_after = 0;
    static bool build(void * user, GLTANG_ValueDescription * out) {
      Hog * hog = static_cast<Hog *>(user);
      const std::string chunk(64 * 1024, 'x');
      for (int i = 0; i < 100 && hog->refused == GLTANG_OK; ++i) {
        GLTANG_ValueRef s = {0, 0};
        GLTANG_Result r = gltang_value_string(out, chunk.data(), chunk.size(), GLTANG_UNICODE_STRING_TYPE_TRUSTED, &s);
        if (r == GLTANG_OK) {
          ++hog->accepted;
        }
        else {
          hog->refused = r;
        }
      }
      hog->nodes_after = gltang_value_description_nodes(out);
      return false;  // nothing wanted: the memory is what is being measured
    }
  };
  Hog hog;
  Config config;
  config.memory_bytes = 3u * 1024 * 1024;
  Out o = run("use lazy; 1;", [&](Context & context) { ASSERT_EQ(gltang_library_add_value_factory(context.library(), "lazy", Hog::build, &hog), GLTANG_OK); }, tt::Mode::Script, config);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(hog.refused, GLTANG_ERR_OOM) << "6.4 MiB of strings under a 3 MiB budget must be refused by the context's allocator, not by the description's own 16 MiB limit";
  EXPECT_GT(hog.accepted, 0);
  EXPECT_LT(hog.accepted, 60) << "refused well before the description's own limit would have been";
  EXPECT_EQ(hog.nodes_after, static_cast<size_t>(hog.accepted)) << "the refused string left nothing behind";
}

// ---------------------------------------------------------------------------
// A native function's answer
// ---------------------------------------------------------------------------

namespace {

GLTANG_ValueDescription * g_kept = nullptr;

/// Builds `[argc, "a<b" (html)]` in the call's own description.
bool native_builds(GLTANG_NativeCall * call, void *) {
  GLTANG_ValueDescription * d = nullptr;
  EXPECT_EQ(gltang_call_description(call, &d), GLTANG_OK);
  GLTANG_ValueDescription * second = nullptr;
  EXPECT_EQ(gltang_call_description(call, &second), GLTANG_ERR_INVALID) << "one description to a call";
  EXPECT_EQ(second, nullptr);
  EXPECT_EQ(gltang_value_description_retain(d), nullptr) << "a call's description cannot be kept";
  {
    // Nor can it be handed to a library, which would keep it past the call.
    GLTANG_Library * library = nullptr;
    EXPECT_EQ(gltang_library_create(nullptr, &library), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_value(library, "x", d), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_library_count(library), 0u);
    gltang_library_release(library);
  }
  GLTANG_ValueRef a = {0, 0}, n = {0, 0}, s = {0, 0};
  EXPECT_EQ(gltang_value_array(d, &a), GLTANG_OK);
  EXPECT_EQ(gltang_value_integer(d, static_cast<int64_t>(gltang_call_count(call)), &n), GLTANG_OK);
  EXPECT_EQ(gltang_value_string(d, "a<b", 3, GLTANG_UNICODE_STRING_TYPE_HTML, &s), GLTANG_OK);
  EXPECT_EQ(gltang_value_array_push(d, a, n), GLTANG_OK);
  EXPECT_EQ(gltang_value_array_push(d, a, s), GLTANG_OK);
  EXPECT_EQ(gltang_value_set_root(d, a), GLTANG_OK);
  gltang_call_return_value(call, d);
  return true;
}

bool native_keeps(GLTANG_NativeCall * call, void *) {
  gltang_call_return_value(call, g_kept);
  return true;
}

/// The answers replace one another: the last one is the call's value.
bool native_replaces(GLTANG_NativeCall * call, void *) {
  GLTANG_ValueDescription * d = nullptr;
  EXPECT_EQ(gltang_call_description(call, &d), GLTANG_OK);
  GLTANG_ValueRef n = {0, 0};
  EXPECT_EQ(gltang_value_integer(d, 11, &n), GLTANG_OK);
  EXPECT_EQ(gltang_value_set_root(d, n), GLTANG_OK);
  switch (gltang_call_integer(call, 0)) {
    case 0:  // a value, then an integer: the integer
      gltang_call_return_value(call, d);
      gltang_call_return_integer(call, 22);
      break;
    case 1:  // an integer, then a value: the value
      gltang_call_return_integer(call, 22);
      gltang_call_return_value(call, d);
      break;
    case 2:  // a value, then the host's own sealed one: that one
      gltang_call_return_value(call, d);
      gltang_call_return_value(call, g_kept);
      break;
    case 3:  // a value, then nothing: null
      gltang_call_return_value(call, d);
      gltang_call_return_null(call);
      break;
    default:  // NULL is no description
      gltang_call_return_integer(call, 5);
      gltang_call_return_value(call, nullptr);
      break;
  }
  return true;
}

/// Builds, answers, and then reports failure: the error wins.
bool native_answers_and_fails(GLTANG_NativeCall * call, void *) {
  GLTANG_ValueDescription * d = nullptr;
  EXPECT_EQ(gltang_call_description(call, &d), GLTANG_OK);
  GLTANG_ValueRef n = {0, 0};
  EXPECT_EQ(gltang_value_integer(d, 1, &n), GLTANG_OK);
  EXPECT_EQ(gltang_value_set_root(d, n), GLTANG_OK);
  gltang_call_return_value(call, d);
  return false;
}

/// Builds and never answers: the description goes with the call, and nothing leaks.
bool native_builds_and_forgets(GLTANG_NativeCall * call, void *) {
  GLTANG_ValueDescription * d = nullptr;
  EXPECT_EQ(gltang_call_description(call, &d), GLTANG_OK);
  const std::string text(5000, 'q');
  for (int i = 0; i < 20; ++i) {
    GLTANG_ValueRef s = {0, 0};
    EXPECT_EQ(gltang_value_string(d, text.data(), text.size(), GLTANG_UNICODE_STRING_TYPE_TRUSTED, &s), GLTANG_OK);
  }
  gltang_call_return_integer(call, 9);
  return true;
}

void add_native(Context & context, const char * name, GLTANG_NativeFn fn) {
  ASSERT_EQ(gltang_library_add_native(context.library(), name, fn, nullptr), GLTANG_OK);
}

}  // namespace

TEST(NativeAnswer, ADescriptionBuiltInTheCallIsTheCallsValueWithItsTags) {
  Out o = run("use f; print(f(1, 2, 3)[0]); print(\"|\"); print(f()[1]);", [](Context & c) { add_native(c, "f", native_builds); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "3|a<b");
  EXPECT_EQ(o.rendered, "3|a&lt;b") << "the string kept its tag through the answer";
}

TEST(NativeAnswer, ASealedDescriptionTheHostKeepsIsAnsweredAgainAndAgainByManyContexts) {
  Desc d;
  g_kept = rows_description(d);
  gltang_value_description_seal(g_kept);
  for (int context_number = 0; context_number < 3; ++context_number) {
    Out o = run("use f; a = f(); b = f(); a[0][\"name\"] = \"changed\"; print(a[0][\"name\"]); print(\"|\"); print(b[0][\"name\"]);", [](Context & c) { add_native(c, "f", native_keeps); });
    ASSERT_TRUE(o.finished) << context_number;
    EXPECT_EQ(o.raw, "changed|Ann <a&b>") << "two answers of one description are two copies, " << context_number;
    EXPECT_EQ(o.rendered, "changed|Ann &lt;a&amp;b&gt;");
  }
  g_kept = nullptr;
}

TEST(NativeAnswer, TheLastAnswerIsTheValueAndNullIsNoDescription) {
  Desc d;
  GLTANG_ValueRef a = d.array();
  d.push(a, d.integer(33));
  g_kept = d.root(a);
  Out o = run("use f; print(f(0)); print(\"|\"); print(f(1)); print(\"|\"); print(f(2)); print(\"|\"); print(f(3) == null); print(\"|\"); print(f(4));",
      [](Context & c) { add_native(c, "f", native_replaces); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "22|11|[33]|true|") << "an answer of NULL leaves the earlier one, which was an integer";
  g_kept = nullptr;
}

TEST(NativeAnswer, ReportingFailureAfterAnAnswerIsTheHostFailedError) {
  Out o = run("use f; f();", [](Context & c) { add_native(c, "f", native_answers_and_fails); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.kind, GLTANG_KIND_ERROR);
  EXPECT_EQ(o.error, GLTANG_ERROR_HOST_FAILED);
}

TEST(NativeAnswer, ADescriptionTheNativeBuiltAndNeverAnsweredIsFreedWithTheCall) {
  // The context's destructor requires that every block it handed out came back.
  Out o = run("use f; for (i = 0; i < 30; i += 1) { f(); } 1;", [](Context & c) { add_native(c, "f", native_builds_and_forgets); });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 1);
}

TEST(NativeAnswer, AnAnswerTooLargeForTheMemoryBudgetIsOutOfMemoryAndTheRunGoesOn) {
  GLTANG_ValueLimits wide = tt::limits(300000, 0, 0);
  Desc d(&wide);
  GLTANG_ValueRef a = d.array();
  for (int i = 0; i < 100000; ++i) {
    d.push(a, d.integer(i));
  }
  g_kept = d.root(a);
  Config config;
  config.memory_bytes = 512 * 1024;
  Out o = run("use f; x = f(); 7;", [](Context & c) { add_native(c, "f", native_keeps); }, tt::Mode::Script, config);
  EXPECT_TRUE(o.finished) << "an allocation that fails is a value, not the end of the run";
  EXPECT_EQ(o.integer, 7);
  Out e = run("use f; f();", [](Context & c) { add_native(c, "f", native_keeps); }, tt::Mode::Script, config);
  ASSERT_TRUE(e.finished);
  EXPECT_EQ(e.kind, GLTANG_KIND_ERROR);
  EXPECT_EQ(e.error, GLTANG_ERROR_OUT_OF_MEMORY);
  g_kept = nullptr;
}

TEST(NativeAnswer, ANativeCalledFromACompiledFunctionAnswersADescription) {
  Config config;
  config.jit_threshold = 1;  // every function compiles at its first poll
  Desc d;
  GLTANG_ValueRef a = d.array();
  d.push(a, d.integer(4));
  d.push(a, d.string("s<", GLTANG_UNICODE_STRING_TYPE_HTML));
  g_kept = d.root(a);
  Out o = run("function g() { use f; t = 0; for (i = 0; i < 300; i += 1) { t = t + f()[0]; } return t; } g();", [](Context & c) { add_native(c, "f", native_keeps); }, tt::Mode::Script, config);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 1200);
  Out p = run("function g() { use f; return f()[1]; } print(g()); print(g());", [](Context & c) { add_native(c, "f", native_keeps); }, tt::Mode::Script, config);
  ASSERT_TRUE(p.finished);
  EXPECT_EQ(p.rendered, "s&lt;s&lt;");
  g_kept = nullptr;
}

TEST(NativeAnswer, AnAnswerThatRunsTheBudgetOutUnwindsTheRunWhetherTheCallIsCompiledOrNot) {
  GLTANG_ValueLimits wide = tt::limits(300000, 0, 0);
  Desc d(&wide);
  GLTANG_ValueRef a = d.array();
  for (int i = 0; i < 60000; ++i) {
    d.push(a, d.integer(i));
  }
  g_kept = d.root(a);
  for (long threshold : {0l, 1l}) {
    SCOPED_TRACE(threshold);
    Config config;
    config.jit_threshold = threshold;
    Out baseline = run("function g() { use f; return 1; } g();", [](Context & c) { add_native(c, "f", native_keeps); }, tt::Mode::Script, config);
    ASSERT_TRUE(baseline.finished);
    config.fuel = baseline.fuel + 400;
    Out o = run("function g() { use f; return f(); } g();", [](Context & c) { add_native(c, "f", native_keeps); }, tt::Mode::Script, config);
    EXPECT_FALSE(o.finished);
    EXPECT_EQ(o.ran, GRCORE_ERR_LIMIT) << "the answer's copy is paced, so the budget stops it";
  }
  g_kept = nullptr;
}

TEST(HostValues, AFunctionThatReachesAMemberIsCompiledAndSeesAFreshCopyEachTime) {
  Config config;
  config.jit_threshold = 1;
  Desc d;
  GLTANG_ValueRef a = d.array();
  d.push(a, d.integer(1));
  Out o = run("function g() { use v; v[0] = v[0] + 1; return v[0]; } t = 0; for (i = 0; i < 100; i += 1) { t = t + g(); } t;", d.root(a), "v", tt::Mode::Script, config);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 200) << "each call reaches a new copy: a cached one would give 5,150";
}

TEST(HostValues, CompiledFunctionsReachADescriptionByAPathAndAFactoryByName) {
  Config config;
  config.jit_threshold = 1;
  Desc d;
  GLTANG_ValueRef a = d.array();
  d.push(a, d.integer(3));
  gltang_value_description_seal(d.root(a));
  Factory f;
  Out o = run(
      "function g() { use app.rows as r; use lazy; r[0] = r[0] + lazy[0]; return r[0]; } t = 0; for (i = 0; i < 50; i += 1) { t = t + g(); } t;",
      [&](Context & context) {
        GLTANG_Library * app = nullptr;
        ASSERT_EQ(gltang_library_create("app", &app), GLTANG_OK);
        ASSERT_EQ(gltang_library_add_value(app, "rows", d.d), GLTANG_OK);
        ASSERT_EQ(gltang_library_add_library(context.library(), app), GLTANG_OK);
        gltang_library_release(app);
        ASSERT_EQ(gltang_library_add_value_factory(context.library(), "lazy", counting_factory, &f), GLTANG_OK);
      },
      tt::Mode::Script, config);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(f.calls, 50) << "the factory is reached once for each call, compiled or not";
  // Each call reaches a fresh [3] and adds the factory's n-th answer (n = 1..50): 3 + n.
  EXPECT_EQ(o.integer, 50 * 3 + 50 * 51 / 2);
}

TEST(Materialize, ADescriptionIsReadByTheErrorListAndTheOutputLikeAnyOtherValue) {
  // Nothing in the engine's readers knows where a value came from.
  Desc d;
  GLTANG_ValueRef a = d.array();
  d.push(a, d.string("p<q", GLTANG_UNICODE_STRING_TYPE_HTML));
  d.push(a, d.number(2.5));
  gltang_value_description_seal(d.root(a));
  Out o = run("use v; v;", d.d);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[p<q, 2.5]");
  EXPECT_EQ(o.size, 2u);
  EXPECT_EQ(o.errors, 0u);
}

TEST(Materialize, AFailedMakingIsTheEnginesOwnErrorValueWithTheEnginesOwnMessage) {
  Desc d;
  gltang_vm_test_description_limits(d.d, 100000, 16u * 1024 * 1024, 1000000);
  GLTANG_ValueRef root = d.array();
  GLTANG_ValueRef previous = root;
  for (int k = 1; k < 2100; ++k) {
    GLTANG_ValueRef next = d.array();
    d.push(previous, next);
    previous = next;
  }
  d.root(root);
  Out deep = run("use v; v;", d.d);
  Out reference = run("function f(n) { return f(n + 1); } f(0);", [](Context &) {}, tt::Mode::Script, [] { Config c; c.calls = 50; return c; }());
  ASSERT_TRUE(deep.finished);
  ASSERT_TRUE(reference.finished);
  ASSERT_EQ(reference.error, GLTANG_ERROR_RECURSION_LIMIT);
  EXPECT_EQ(deep.described, reference.described) << "the same error, the same words";
}

// ---------------------------------------------------------------------------
// Limits, and a description that has somehow passed them
// ---------------------------------------------------------------------------

TEST(Materialize, ADescriptionDeeperThanTheEngineNestsIsRecursionLimitExceeded) {
  // The builder refuses this. The test hook lifts its limit so that the
  // materializer meets a description it would never be given, and must refuse
  // it itself.
  Desc d;
  gltang_vm_test_description_limits(d.d, 100000, 16u * 1024 * 1024, 1000000);
  GLTANG_ValueRef root = d.array();
  GLTANG_ValueRef previous = root;
  for (int k = 1; k < 2100; ++k) {
    GLTANG_ValueRef next = d.array();
    d.push(previous, next);
    previous = next;
  }
  d.root(root);
  Out o = run("use v; x = 5; v;", d.d);
  ASSERT_TRUE(o.finished) << "a refusal is a value";
  EXPECT_EQ(o.kind, GLTANG_KIND_ERROR);
  EXPECT_EQ(o.error, GLTANG_ERROR_RECURSION_LIMIT);
}

TEST(Materialize, TheEnginesDeepestNestingIsStillMade) {
  Desc d;
  gltang_vm_test_description_limits(d.d, 100000, 16u * 1024 * 1024, 1000000);
  GLTANG_ValueRef root = d.array();
  GLTANG_ValueRef previous = root;
  for (int k = 1; k < static_cast<int>(GLTANG_VALUE_MAX_DEPTH); ++k) {
    GLTANG_ValueRef next = d.array();
    d.push(previous, next);
    previous = next;
  }
  d.push(previous, d.integer(1));
  d.root(root);
  Out o = run("use v; v;", d.d);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.kind, GLTANG_KIND_ARRAY) << "2,048 nested arrays are the most the engine nests";
}

TEST(Materialize, ADescriptionPastItsOwnNodeLimitIsRefusedAgain) {
  Desc d;
  GLTANG_ValueRef a = d.array();
  for (int i = 0; i < 10; ++i) {
    d.push(a, d.integer(i));
  }
  d.root(a);
  Out fine = run("use v; v;", d.d);
  EXPECT_EQ(fine.kind, GLTANG_KIND_ARRAY);
  gltang_vm_test_description_limits(d.d, 5, 16u * 1024 * 1024, 64);
  Out o = run("use v; x = 5; v;", d.d);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.kind, GLTANG_KIND_ERROR);
  EXPECT_EQ(o.error, GLTANG_ERROR_OUT_OF_MEMORY);
}

TEST(Materialize, ADescriptionPastItsOwnByteLimitIsRefusedAgain) {
  Desc d;
  GLTANG_ValueRef a = d.array();
  d.push(a, d.string(std::string(1000, 'z')));
  d.root(a);
  Out fine = run("use v; v;", d.d);
  EXPECT_EQ(fine.kind, GLTANG_KIND_ARRAY);
  gltang_vm_test_description_limits(d.d, 1000, 500, 64);
  Out o = run("use v; v;", d.d);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.kind, GLTANG_KIND_ERROR);
  EXPECT_EQ(o.error, GLTANG_ERROR_OUT_OF_MEMORY);
}

TEST(Materialize, ADescriptionPastTheCeilingsIsRefusedWhateverItClaims) {
  // The ceilings are the engine's, and a description cannot raise them: the
  // test lowers them to a few nodes and a few bytes, and the description (which
  // claims, through the hook, that it may hold any number) is refused.
  struct Restore {
    ~Restore() { gltang_vm_test_materialize_ceilings(GLTANG_VALUE_MAX_NODES, GLTANG_VALUE_MAX_BYTES); }
  } restore;
  Desc d;
  gltang_vm_test_description_limits(d.d, SIZE_MAX, SIZE_MAX, GLTANG_VALUE_MAX_DEPTH);
  GLTANG_ValueRef a = d.array();
  for (int i = 0; i < 10; ++i) {
    d.push(a, d.string("0123456789"));
  }
  d.root(a);
  EXPECT_EQ(run("use v; v;", d.d).kind, GLTANG_KIND_ARRAY) << "the ceilings as they are allow it";
  gltang_vm_test_materialize_ceilings(5, GLTANG_VALUE_MAX_BYTES);
  Out nodes = run("use v; x = 1; v;", d.d);
  ASSERT_TRUE(nodes.finished);
  EXPECT_EQ(nodes.kind, GLTANG_KIND_ERROR);
  EXPECT_EQ(nodes.error, GLTANG_ERROR_OUT_OF_MEMORY);
  gltang_vm_test_materialize_ceilings(GLTANG_VALUE_MAX_NODES, 100);
  Out bytes = run("use v; x = 1; v;", d.d);
  ASSERT_TRUE(bytes.finished);
  EXPECT_EQ(bytes.kind, GLTANG_KIND_ERROR);
  EXPECT_EQ(bytes.error, GLTANG_ERROR_OUT_OF_MEMORY);
}

TEST(Materialize, ALongCopyIsChargedFuelAndTheBudgetStopsIt) {
  GLTANG_ValueLimits wide = tt::limits(300000, 0, 0);
  Desc d(&wide);
  GLTANG_ValueRef a = d.array();
  for (int i = 0; i < 60000; ++i) {
    d.push(a, d.integer(i));
  }
  gltang_value_description_seal(d.root(a));
  Out unlimited = run("use v; 1;", d.d);
  ASSERT_TRUE(unlimited.finished);
  Out baseline = run("use w; 1;", [](Context & c) { ASSERT_EQ(gltang_library_add_integer(c.library(), "w", 1), GLTANG_OK); });
  EXPECT_GT(unlimited.fuel, baseline.fuel + 700) << "60,000 elements of 8 bytes at 64 bytes a unit of fuel is 7,500; none was charged";
  Config config;
  config.fuel = baseline.fuel + 400;
  Out stopped = run("use v; 1;", d.d, "v", tt::Mode::Script, config);
  EXPECT_FALSE(stopped.finished);
  EXPECT_EQ(stopped.ran, GRCORE_ERR_LIMIT) << "the run unwinds, as a long string copy does";
}

TEST(Materialize, ACopyTooLargeForTheMemoryBudgetIsOutOfMemoryAndTheRunGoesOn) {
  GLTANG_ValueLimits wide = tt::limits(300000, 0, 0);
  Desc d(&wide);
  GLTANG_ValueRef a = d.array();
  for (int i = 0; i < 100000; ++i) {
    d.push(a, d.integer(i));
  }
  gltang_value_description_seal(d.root(a));
  Config config;
  config.memory_bytes = 512 * 1024;
  Out o = run("use v; x = 3; x;", d.d, "v", tt::Mode::Script, config);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 3);
  Out e = run("use v; v;", d.d, "v", tt::Mode::Script, config);
  ASSERT_TRUE(e.finished);
  EXPECT_EQ(e.error, GLTANG_ERROR_OUT_OF_MEMORY);
}

// ---------------------------------------------------------------------------
// Allocation failure, collection at every allocation, threads
// ---------------------------------------------------------------------------

namespace {

GLTANG_ValueDescription * g_sweep_kept = nullptr;

bool sweep_kept(GLTANG_NativeCall * call, void *) {
  gltang_call_return_value(call, g_sweep_kept);
  return true;
}

bool sweep_builds(GLTANG_NativeCall * call, void *) {
  GLTANG_ValueDescription * d = nullptr;
  if (gltang_call_description(call, &d) != GLTANG_OK) {
    gltang_call_return_error(call, GLTANG_ERROR_HOST_FAILED);
    return true;
  }
  GLTANG_ValueRef a = {0, 0}, s = {0, 0}, n = {0, 0};
  if (gltang_value_array(d, &a) != GLTANG_OK || gltang_value_string(d, "built <in> call", 15, GLTANG_UNICODE_STRING_TYPE_HTML, &s) != GLTANG_OK ||
      gltang_value_integer(d, 1ll << 61, &n) != GLTANG_OK || gltang_value_array_push(d, a, s) != GLTANG_OK || gltang_value_array_push(d, a, n) != GLTANG_OK ||
      gltang_value_set_root(d, a) != GLTANG_OK) {
    gltang_call_return_error(call, GLTANG_ERROR_OUT_OF_MEMORY);
    return true;
  }
  gltang_call_return_value(call, d);
  return true;
}

bool sweep_factory(void *, GLTANG_ValueDescription * out) {
  GLTANG_ValueRef m = {0, 0}, v = {0, 0};
  if (gltang_value_map(out, &m) != GLTANG_OK || gltang_value_string(out, "lazy & fresh", 12, GLTANG_UNICODE_STRING_TYPE_HTML, &v) != GLTANG_OK ||
      gltang_value_map_set(out, m, "k", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, v) != GLTANG_OK || gltang_value_set_root(out, m) != GLTANG_OK) {
    return false;
  }
  return true;
}

}  // namespace

TEST(HostValuesAllocationFailure, EachAllocationOfAReachAFactoryAndAnAnswerFailedInTurnIsAnErrorValueAndLeaksNothing) {
  Desc d;
  g_sweep_kept = rows_description(d);
  gltang_value_description_seal(g_sweep_kept);
  Desc big;
  GLTANG_ValueRef a = big.array();
  big.push(a, big.string("a sealed <value>", GLTANG_UNICODE_STRING_TYPE_HTML));
  big.push(a, big.number(1.5));
  big.push(a, big.integer(int64_t{1} << 60));
  gltang_value_description_seal(big.root(a));
  const std::string source = "use rows; use big; use lazy; use kept; use built; print(rows[0][\"name\"]); print(big[0]); print(big[1]); print(lazy[\"k\"]); print(kept()[1][\"url\"]); print(built()[0]); print(\"|\"); print(big[2]);";
  auto setup = [&](Context & c) {
    ASSERT_EQ(gltang_library_add_value(c.library(), "rows", g_sweep_kept), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_value(c.library(), "big", big.d), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_value_factory(c.library(), "lazy", sweep_factory, nullptr), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(c.library(), "kept", sweep_kept, nullptr), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(c.library(), "built", sweep_builds, nullptr), GLTANG_OK);
  };
  Out baseline = run(source, setup);
  ASSERT_TRUE(baseline.finished);
  ASSERT_FALSE(baseline.fired);
  ASSERT_EQ(baseline.raw, "Ann <a&b>a sealed <value>1.5lazy & freshq=1 2built <in> call|1152921504606846976");
  bool reached_undisturbed_run = false;
  int degraded = 0;
  for (long n = 1; n <= 20000; ++n) {
    Config config;
    config.fail_at = n;
    Out o = run(source, setup, tt::Mode::Script, config);
    if (!o.fired) {
      EXPECT_EQ(o.raw, baseline.raw) << "n=" << n;
      reached_undisturbed_run = true;
      break;
    }
    degraded += o.raw != baseline.raw || !o.finished;
  }
  EXPECT_TRUE(reached_undisturbed_run) << "the sweep never reached a run with no failed allocation";
  EXPECT_GT(degraded, 20) << "failing allocations barely changed an answer, so the sweep reached little";
  g_sweep_kept = nullptr;
}

TEST(HostValues, TheTagsAreReadRightWhenEveryAllocationCollectsAndAMovedObjectIsFound) {
  Config config;
  config.torture = 1;
  config.verify = 1;
  expect_identical(through_both(kPrintCells, tt::Mode::Script, kRows, config));
  config.moving_stack = 1;
  expect_identical(through_both("print(rows); print(rows as string + rows[0][\"note\"]);", tt::Mode::Script, kRows, config));
  Config relocating;
  relocating.torture = 1;
  relocating.verify = 1;
#ifdef GLTANG_TEST_HAS_RELOCATE
  relocating.relocate = 1;
#endif
  expect_identical(through_both(kPrintCells, tt::Mode::Script, kRows, relocating));
}

TEST(HostValues, NumbersAndStringsInAMapSurviveTheMakingOfTheirKeysWhenEveryAllocationCollects) {
  // A float, a boxed integer and a string are heap objects, held by nothing but
  // the materializer while the key of their entry is made.
  Desc d;
  GLTANG_ValueRef m = d.map();
  d.set(m, "f", d.number(1.5));
  d.set(m, "big", d.integer(int64_t{1} << 61));
  d.set(m, "s", d.string("a <b>", GLTANG_UNICODE_STRING_TYPE_HTML));
  GLTANG_ValueRef inner = d.array();
  d.push(inner, d.number(1.25));
  d.push(inner, d.string("x&y", GLTANG_UNICODE_STRING_TYPE_HTML));
  d.set(m, "list", inner);
  gltang_value_description_seal(d.root(m));
  Config config;
  config.torture = 1;
  config.verify = 1;
  const std::string body = "print(v[\"f\"]); print(v[\"big\"]); print(v[\"s\"]); print(v[\"list\"]);";
  Out described = run("use v; " + body, d.d, "v", tt::Mode::Script, config);
  Out reference = run("v = {f: 1.5, big: 2305843009213693952, s: !\"a <b>\", list: [1.25, !\"x&y\"]}; " + body, [](Context &) {}, tt::Mode::Script, config);
  ASSERT_TRUE(described.finished);
  ASSERT_TRUE(reference.finished);
  EXPECT_EQ(described.raw, reference.raw);
  EXPECT_EQ(described.rendered, reference.rendered);
  EXPECT_EQ(described.raw, "1.52305843009213693952a <b>[1.25, x&y]");
  EXPECT_NE(described.rendered, described.raw);
}

TEST(HostValues, OneFrozenDescriptionIsReadByManyContextsOnManyThreadsAtOnce) {
  Desc d;
  gltang_value_description_seal(rows_description(d));
  GLTANG_Library * root = nullptr;
  ASSERT_EQ(gltang_library_create(nullptr, &root), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_value(root, "rows", d.d), GLTANG_OK);
  const std::string source = std::string("use rows; for (n = 0; n < 40; n += 1) { use rows as r; r[0][\"name\"] = \"x\"; } ") + kPrintCells;
  Compiled compiled(source);
  ASSERT_TRUE(compiled.ok());
  std::string expected;
  {
    Context context(compiled.program);
    ASSERT_EQ(gltang_execution_set_libraries(context.execution, root), GLTANG_OK);
    context.attached = true;
    ASSERT_TRUE(context.execute());
    expected = context.raw();
  }
  ASSERT_NE(expected.find("Ann <a&b>"), std::string::npos) << "the description was read whole by the reference run";
  std::atomic<int> good{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&]() {
      Context context(compiled.program);
      if (context.ok() && gltang_execution_set_libraries(context.execution, root) == GLTANG_OK) {
        context.attached = true;
        if (context.execute() && context.raw() == expected) {
          ++good;
        }
      }
    });
  }
  for (auto & t : threads) {
    t.join();
  }
  gltang_library_release(root);
  EXPECT_EQ(good.load(), 4);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
