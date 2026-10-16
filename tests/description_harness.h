// The host side of a described value, for the tests: a description that
// releases itself, and a builder that makes the nodes of a tree in a line each.
//
// A refusal is a test failure here (EXPECT_EQ), because a test that builds a
// tree means to build it; a test of a refusal calls the API directly.

#ifndef GLTANG_TESTS_DESCRIPTION_HARNESS_H
#define GLTANG_TESTS_DESCRIPTION_HARNESS_H

#include "exec_harness.h"

#include <string>
#include <vector>

namespace tt {

struct Desc {
  GLTANG_ValueDescription * d = nullptr;
  explicit Desc(const GLTANG_ValueLimits * limits = nullptr) {
    EXPECT_EQ(gltang_value_description_create(limits, &d), GLTANG_OK);
  }
  ~Desc() { gltang_value_description_release(d); }
  Desc(const Desc &) = delete;
  Desc & operator=(const Desc &) = delete;
  operator GLTANG_ValueDescription *() const { return d; }

  GLTANG_ValueRef null() {
    GLTANG_ValueRef r = {0, 0};
    EXPECT_EQ(gltang_value_null(d, &r), GLTANG_OK);
    return r;
  }
  GLTANG_ValueRef boolean(bool b) {
    GLTANG_ValueRef r = {0, 0};
    EXPECT_EQ(gltang_value_bool(d, b, &r), GLTANG_OK);
    return r;
  }
  GLTANG_ValueRef number(double n) {
    GLTANG_ValueRef r = {0, 0};
    EXPECT_EQ(gltang_value_float(d, n, &r), GLTANG_OK);
    return r;
  }
  GLTANG_ValueRef integer(int64_t n) {
    GLTANG_ValueRef r = {0, 0};
    EXPECT_EQ(gltang_value_integer(d, n, &r), GLTANG_OK);
    return r;
  }
  GLTANG_ValueRef array() {
    GLTANG_ValueRef r = {0, 0};
    EXPECT_EQ(gltang_value_array(d, &r), GLTANG_OK);
    return r;
  }
  GLTANG_ValueRef map() {
    GLTANG_ValueRef r = {0, 0};
    EXPECT_EQ(gltang_value_map(d, &r), GLTANG_OK);
    return r;
  }
  GLTANG_ValueRef string(const std::string & s, GLTANG_String_Type t = GLTANG_UNICODE_STRING_TYPE_TRUSTED) {
    GLTANG_ValueRef r = {0, 0};
    EXPECT_EQ(gltang_value_string(d, s.data(), s.size(), t, &r), GLTANG_OK);
    return r;
  }
  /// Appends `child` to `array`; returns `array` so calls chain.
  GLTANG_ValueRef push(GLTANG_ValueRef array, GLTANG_ValueRef child) {
    EXPECT_EQ(gltang_value_array_push(d, array, child), GLTANG_OK);
    return array;
  }
  /// Sets `key` in `map`; returns `map` so calls chain.
  GLTANG_ValueRef set(GLTANG_ValueRef map, const std::string & key, GLTANG_ValueRef child, GLTANG_String_Type key_tag = GLTANG_UNICODE_STRING_TYPE_TRUSTED) {
    EXPECT_EQ(gltang_value_map_set(d, map, key.data(), key.size(), key_tag, child), GLTANG_OK);
    return map;
  }
  /// Makes `root` the value, and returns the description for handing over.
  GLTANG_ValueDescription * root(GLTANG_ValueRef r) {
    EXPECT_EQ(gltang_value_set_root(d, r), GLTANG_OK);
    return d;
  }
};

/// All five segment tags, for the tests that go through each.
inline const GLTANG_String_Type kAllTags[] = {
  GLTANG_UNICODE_STRING_TYPE_TRUSTED, GLTANG_UNICODE_STRING_TYPE_HTML, GLTANG_UNICODE_STRING_TYPE_HTML_ATTRIBUTE,
  GLTANG_UNICODE_STRING_TYPE_PERCENT, GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT};

inline GLTANG_ValueLimits limits(size_t nodes, size_t bytes, size_t depth) {
  GLTANG_ValueLimits l;
  l.size = sizeof(l);
  l.max_nodes = nodes;
  l.max_bytes = bytes;
  l.max_depth = depth;
  return l;
}


}  // namespace tt

#endif
