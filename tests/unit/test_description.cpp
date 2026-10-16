// Descriptions (description.h): what a host builds before any execution
// exists. The builder's rules, each seen from the outside: the node kinds, the
// one-parent rule and the cycle it prevents, the limits (the node, byte and
// depth caps, each refusing before anything changes), the UTF-8 check made once
// when a string or key is added, sealing, and the reference count.
//
// What the engine makes of a description is in test_host_values.cpp.

#include "description_harness.h"

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

using tt::Desc;
using tt::limits;

// ---------------------------------------------------------------------------
// Creating
// ---------------------------------------------------------------------------

TEST(Description, AnEmptyOneHasNoNodesAndTheDefaultsAreTheDocumentedOnes) {
  Desc d;
  EXPECT_EQ(gltang_value_description_nodes(d), 0u);
  EXPECT_EQ(gltang_value_description_bytes(d), 0u);
  EXPECT_FALSE(gltang_value_description_sealed(d));
  EXPECT_EQ(GLTANG_VALUE_DEFAULT_NODES, 100000u);
  EXPECT_EQ(GLTANG_VALUE_DEFAULT_BYTES, 16u * 1024 * 1024);
  EXPECT_EQ(GLTANG_VALUE_DEFAULT_DEPTH, 64u);
  EXPECT_EQ(GLTANG_VALUE_MAX_NODES, 4194304u);
  EXPECT_EQ(GLTANG_VALUE_MAX_BYTES, 256u * 1024 * 1024);
  EXPECT_EQ(GLTANG_VALUE_MAX_DEPTH, 2048u);
}

TEST(Description, TheCreateArgumentsAreChecked) {
  GLTANG_ValueDescription * d = nullptr;
  EXPECT_EQ(gltang_value_description_create(nullptr, nullptr), GLTANG_ERR_INVALID);
  GLTANG_ValueLimits wrong = limits(0, 0, 0);
  wrong.size = 1;
  EXPECT_EQ(gltang_value_description_create(&wrong, &d), GLTANG_ERR_INVALID) << "the size prefix is the version";
  EXPECT_EQ(d, nullptr) << "written only on success";
  GLTANG_ValueLimits over_nodes = limits(GLTANG_VALUE_MAX_NODES + 1, 0, 0);
  GLTANG_ValueLimits over_bytes = limits(0, GLTANG_VALUE_MAX_BYTES + 1, 0);
  GLTANG_ValueLimits over_depth = limits(0, 0, GLTANG_VALUE_MAX_DEPTH + 1);
  EXPECT_EQ(gltang_value_description_create(&over_nodes, &d), GLTANG_ERR_LIMIT);
  EXPECT_EQ(gltang_value_description_create(&over_bytes, &d), GLTANG_ERR_LIMIT);
  EXPECT_EQ(gltang_value_description_create(&over_depth, &d), GLTANG_ERR_LIMIT);
  EXPECT_EQ(d, nullptr);
  GLTANG_ValueLimits ceilings = limits(GLTANG_VALUE_MAX_NODES, GLTANG_VALUE_MAX_BYTES, GLTANG_VALUE_MAX_DEPTH);
  ASSERT_EQ(gltang_value_description_create(&ceilings, &d), GLTANG_OK) << "the ceilings themselves are allowed";
  gltang_value_description_release(d);
}

TEST(Description, ANullArgumentIsRefusedEverywhere) {
  Desc d;
  GLTANG_ValueRef r = {0, 0};
  EXPECT_EQ(gltang_value_null(nullptr, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_null(d, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_bool(nullptr, true, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_integer(nullptr, 1, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_float(nullptr, 1.0, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_string(nullptr, "x", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_string(d, nullptr, 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_string(d, "x", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_array(nullptr, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_map(nullptr, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_array_push(nullptr, r, r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_map_set(nullptr, r, "k", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_set_root(nullptr, r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_description_seal(nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_description_retain(nullptr), nullptr);
  gltang_value_description_release(nullptr);
  EXPECT_EQ(gltang_value_description_nodes(d), 0u) << "nothing was made";
  EXPECT_EQ(r.index, 0u) << "a ref is written only on success";
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

TEST(Description, EachNodeKindIsMadeAndCounted) {
  Desc d;
  GLTANG_ValueRef n = {0, 0}, b = {0, 0}, i = {0, 0}, f = {0, 0}, s = {0, 0}, a = {0, 0}, m = {0, 0};
  ASSERT_EQ(gltang_value_null(d, &n), GLTANG_OK);
  ASSERT_EQ(gltang_value_bool(d, true, &b), GLTANG_OK);
  ASSERT_EQ(gltang_value_integer(d, -5, &i), GLTANG_OK);
  ASSERT_EQ(gltang_value_float(d, 2.5, &f), GLTANG_OK);
  ASSERT_EQ(gltang_value_string(d, "héllo", 6, GLTANG_UNICODE_STRING_TYPE_HTML, &s), GLTANG_OK);
  ASSERT_EQ(gltang_value_array(d, &a), GLTANG_OK);
  ASSERT_EQ(gltang_value_map(d, &m), GLTANG_OK);
  EXPECT_EQ(gltang_value_description_nodes(d), 7u);
  // Every node costs 16 and a string its bytes.
  EXPECT_EQ(gltang_value_description_bytes(d), 7u * 16u + 6u);
  std::set<uint32_t> distinct = {n.index, b.index, i.index, f.index, s.index, a.index, m.index};
  EXPECT_EQ(distinct.size(), 7u);
  EXPECT_EQ(distinct.count(0u), 0u) << "zero names no node";
}

TEST(Description, ManyNodesGrowTheArrayAndKeepEveryRef) {
  Desc d;
  GLTANG_ValueRef root = d.array();
  std::vector<GLTANG_ValueRef> refs;
  for (int k = 0; k < 5000; ++k) {
    refs.push_back(d.integer(k));
    ASSERT_EQ(gltang_value_array_push(d, root, refs.back()), GLTANG_OK) << k;
  }
  ASSERT_EQ(gltang_value_set_root(d, root), GLTANG_OK);
  EXPECT_EQ(gltang_value_description_nodes(d), 5001u);
  // A ref made before the node array grew still names its node: pushing it
  // again is refused for having a parent, which needs the node to be found.
  EXPECT_EQ(gltang_value_array_push(d, root, refs.front()), GLTANG_ERR_INVALID);
}

TEST(Description, AStringOfAnyUtf8IsAcceptedAndAnEmptyOneToo) {
  Desc d;
  GLTANG_ValueRef r = {0, 0};
  EXPECT_EQ(gltang_value_string(d, "", 0, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &r), GLTANG_OK);
  EXPECT_EQ(gltang_value_string(d, nullptr, 0, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &r), GLTANG_OK) << "a NULL text of no bytes is empty";
  const std::string text = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80 and a NUL \0 in it";
  EXPECT_EQ(gltang_value_string(d, text.data(), text.size(), GLTANG_UNICODE_STRING_TYPE_PERCENT, &r), GLTANG_OK);
}

TEST(Description, ATextThatIsNotUtf8IsRefusedAndChangesNothing) {
  Desc d;
  GLTANG_ValueRef r = {0, 0};
  const char * bad[] = {"\xff", "\xc3", "\xc0\xaf", "\xed\xa0\x80", "abc\xe2\x82", "\xf8\x88\x80\x80\x80"};
  for (const char * text : bad) {
    SCOPED_TRACE(text);
    EXPECT_EQ(gltang_value_string(d, text, std::char_traits<char>::length(text), GLTANG_UNICODE_STRING_TYPE_TRUSTED, &r), GLTANG_ERR_INVALID);
  }
  EXPECT_EQ(gltang_value_description_nodes(d), 0u);
  EXPECT_EQ(gltang_value_description_bytes(d), 0u);
  EXPECT_EQ(r.index, 0u);
}

TEST(Description, AKeyThatIsNotUtf8IsRefusedAndTheValueStaysFree) {
  Desc d;
  GLTANG_ValueRef map = d.map();
  GLTANG_ValueRef value = d.integer(1);
  const size_t bytes = gltang_value_description_bytes(d);
  EXPECT_EQ(gltang_value_map_set(d, map, "\xff", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, value), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_description_bytes(d), bytes) << "a refused key stores nothing";
  // The value still has no parent, so it can be set under a good key.
  EXPECT_EQ(gltang_value_map_set(d, map, "k", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, value), GLTANG_OK);
}

TEST(Description, ASegmentTagOutOfRangeIsRefused) {
  Desc d;
  GLTANG_ValueRef r = {0, 0};
  GLTANG_ValueRef map = d.map();
  GLTANG_ValueRef value = d.integer(1);
  const auto bad = static_cast<GLTANG_String_Type>(GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT + 1);
  EXPECT_EQ(gltang_value_string(d, "x", 1, bad, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_map_set(d, map, "k", 1, bad, value), GLTANG_ERR_INVALID);
}

// ---------------------------------------------------------------------------
// Attaching
// ---------------------------------------------------------------------------

TEST(Description, AContainerTakesChildrenOfItsKindOfAttachOnly) {
  Desc d;
  GLTANG_ValueRef array = d.array();
  GLTANG_ValueRef map = d.map();
  GLTANG_ValueRef scalar = d.integer(1);
  GLTANG_ValueRef child = d.integer(2);
  GLTANG_ValueRef zero = {0, 0};
  GLTANG_ValueRef beyond = {static_cast<uint32_t>(gltang_value_description_nodes(d) + 1), array.owner};
  EXPECT_EQ(gltang_value_array_push(d, map, child), GLTANG_ERR_INVALID) << "push is for arrays";
  EXPECT_EQ(gltang_value_map_set(d, array, "k", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, child), GLTANG_ERR_INVALID) << "set is for maps";
  EXPECT_EQ(gltang_value_array_push(d, scalar, child), GLTANG_ERR_INVALID) << "a scalar holds nothing";
  EXPECT_EQ(gltang_value_array_push(d, zero, child), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_array_push(d, array, zero), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_array_push(d, array, beyond), GLTANG_ERR_INVALID) << "a ref past the nodes";
  EXPECT_EQ(gltang_value_array_push(d, beyond, child), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_array_push(d, array, child), GLTANG_OK) << "none of the refusals took the child";
}

TEST(Description, ANodeHasOneParentOnly) {
  Desc d;
  GLTANG_ValueRef a = d.array();
  GLTANG_ValueRef b = d.array();
  GLTANG_ValueRef m = d.map();
  GLTANG_ValueRef child = d.integer(1);
  ASSERT_EQ(gltang_value_array_push(d, a, child), GLTANG_OK);
  EXPECT_EQ(gltang_value_array_push(d, a, child), GLTANG_ERR_INVALID) << "twice under one parent";
  EXPECT_EQ(gltang_value_array_push(d, b, child), GLTANG_ERR_INVALID) << "under a second array";
  EXPECT_EQ(gltang_value_map_set(d, m, "k", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, child), GLTANG_ERR_INVALID) << "under a map";
  // A subtree has one parent too: attaching the array that is a child.
  GLTANG_ValueRef outer = d.array();
  ASSERT_EQ(gltang_value_array_push(d, outer, a), GLTANG_OK);
  EXPECT_EQ(gltang_value_array_push(d, b, a), GLTANG_ERR_INVALID);
}

TEST(Description, ARefOfAnotherDescriptionIsRefusedEverywhere) {
  Desc a;
  Desc b;
  GLTANG_ValueRef array_a = a.array();
  GLTANG_ValueRef array_b = b.array();
  GLTANG_ValueRef child_a = a.integer(1);
  GLTANG_ValueRef child_b = b.integer(2);
  EXPECT_EQ(gltang_value_array_push(a, array_b, child_a), GLTANG_ERR_INVALID) << "the container is b's";
  EXPECT_EQ(gltang_value_array_push(a, array_a, child_b), GLTANG_ERR_INVALID) << "the child is b's";
  EXPECT_EQ(gltang_value_map_set(a, array_b, "k", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, child_a), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_set_root(a, array_b), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_set_root(a, child_b), GLTANG_ERR_INVALID);
  EXPECT_EQ(array_a.index, array_b.index) << "the same index in both: only the owner tells them apart";
  EXPECT_NE(array_a.owner, array_b.owner);
  EXPECT_EQ(gltang_value_array_push(a, array_a, child_a), GLTANG_OK) << "and the right ones still work";
  EXPECT_EQ(gltang_value_array_push(b, array_b, child_b), GLTANG_OK);
}

TEST(Description, TheRootCountsAsAParent) {
  Desc d;
  GLTANG_ValueRef array = d.array();
  GLTANG_ValueRef other = d.array();
  ASSERT_EQ(gltang_value_set_root(d, array), GLTANG_OK);
  EXPECT_EQ(gltang_value_array_push(d, other, array), GLTANG_ERR_INVALID) << "the root is under the description";
  GLTANG_ValueRef child = d.integer(3);
  ASSERT_EQ(gltang_value_array_push(d, other, child), GLTANG_OK);
  EXPECT_EQ(gltang_value_set_root(d, child), GLTANG_ERR_INVALID) << "a node with a parent cannot be the root";
  // Replacing the root frees the old one to be attached.
  GLTANG_ValueRef fresh = d.array();
  ASSERT_EQ(gltang_value_set_root(d, fresh), GLTANG_OK);
  EXPECT_EQ(gltang_value_array_push(d, other, array), GLTANG_OK);
}

TEST(Description, ACycleIsRefusedWhateverItsLength) {
  Desc d;
  GLTANG_ValueRef a = d.array();
  EXPECT_EQ(gltang_value_array_push(d, a, a), GLTANG_ERR_INVALID) << "a container in itself";
  GLTANG_ValueRef m = d.map();
  EXPECT_EQ(gltang_value_map_set(d, m, "self", 4, GLTANG_UNICODE_STRING_TYPE_TRUSTED, m), GLTANG_ERR_INVALID) << "a map in itself";
  // a > b > c: a is the ancestor of c, and b of c.
  GLTANG_ValueRef b = d.array();
  GLTANG_ValueRef c = d.map();
  ASSERT_EQ(gltang_value_array_push(d, a, b), GLTANG_OK);
  ASSERT_EQ(gltang_value_array_push(d, b, c), GLTANG_OK);
  EXPECT_EQ(gltang_value_map_set(d, c, "up", 2, GLTANG_UNICODE_STRING_TYPE_TRUSTED, a), GLTANG_ERR_INVALID) << "a under c: two levels up";
  EXPECT_EQ(gltang_value_map_set(d, c, "up", 2, GLTANG_UNICODE_STRING_TYPE_TRUSTED, b), GLTANG_ERR_INVALID) << "b under c: its own child";
  EXPECT_EQ(gltang_value_array_push(d, b, a), GLTANG_ERR_INVALID) << "a under b";
  // Every refusal was for the cycle, and none changed anything: a still has no
  // parent and can be the root.
  EXPECT_EQ(gltang_value_set_root(d, a), GLTANG_OK);
  EXPECT_EQ(gltang_value_description_nodes(d), 4u);
}

TEST(Description, ACycleAtTheDeepestLevelTheLimitAllowsIsRefusedAndTerminates) {
  GLTANG_ValueLimits l = limits(0, 0, 40);
  Desc d(&l);
  std::vector<GLTANG_ValueRef> chain;
  chain.push_back(d.array());
  for (int k = 1; k < 39; ++k) {
    chain.push_back(d.array());
    ASSERT_EQ(gltang_value_array_push(d, chain[k - 1], chain[k]), GLTANG_OK) << k;
  }
  EXPECT_EQ(gltang_value_array_push(d, chain.back(), chain.front()), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_set_root(d, chain.front()), GLTANG_OK);
}

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

TEST(Description, TheNodeLimitRefusesTheNodeAfterTheLast) {
  GLTANG_ValueLimits l = limits(3, 0, 0);
  Desc d(&l);
  d.integer(1);
  d.integer(2);
  d.integer(3);
  GLTANG_ValueRef r = {0, 0};
  EXPECT_EQ(gltang_value_integer(d, 4, &r), GLTANG_ERR_LIMIT);
  EXPECT_EQ(gltang_value_null(d, &r), GLTANG_ERR_LIMIT);
  EXPECT_EQ(gltang_value_string(d, "x", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &r), GLTANG_ERR_LIMIT);
  EXPECT_EQ(gltang_value_description_nodes(d), 3u) << "a refusal changes nothing";
  EXPECT_EQ(r.index, 0u);
}

TEST(Description, TheByteLimitCountsStringsKeysAndTheCostOfEachNode) {
  // 16 for each node: three nodes cost 48, so 48 bytes exactly fits and the
  // fourth does not.
  GLTANG_ValueLimits exact = limits(0, 48, 0);
  {
    Desc d(&exact);
    d.integer(1);
    d.integer(2);
    d.integer(3);
    EXPECT_EQ(gltang_value_description_bytes(d), 48u);
    GLTANG_ValueRef r = {0, 0};
    EXPECT_EQ(gltang_value_integer(d, 4, &r), GLTANG_ERR_LIMIT);
    EXPECT_EQ(gltang_value_description_bytes(d), 48u);
  }
  // A string's bytes count: 16 + 10 fits in 26, 16 + 11 does not.
  GLTANG_ValueLimits strings = limits(0, 26, 0);
  {
    Desc d(&strings);
    GLTANG_ValueRef r = {0, 0};
    EXPECT_EQ(gltang_value_string(d, "01234567890", 11, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &r), GLTANG_ERR_LIMIT);
    EXPECT_EQ(gltang_value_description_nodes(d), 0u);
    EXPECT_EQ(gltang_value_string(d, "0123456789", 10, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &r), GLTANG_OK);
    EXPECT_EQ(gltang_value_description_bytes(d), 26u);
  }
  // A key's bytes count when the entry is attached.
  GLTANG_ValueLimits keys = limits(0, 16 * 2 + 5, 0);
  {
    Desc d(&keys);
    GLTANG_ValueRef map = d.map();
    GLTANG_ValueRef value = d.integer(1);
    EXPECT_EQ(gltang_value_map_set(d, map, "123456", 6, GLTANG_UNICODE_STRING_TYPE_TRUSTED, value), GLTANG_ERR_LIMIT) << "six bytes of key do not fit in five";
    EXPECT_EQ(gltang_value_description_bytes(d), 32u) << "a refused key stores nothing";
    EXPECT_EQ(gltang_value_map_set(d, map, "12345", 5, GLTANG_UNICODE_STRING_TYPE_TRUSTED, value), GLTANG_OK) << "and the value was still free";
    EXPECT_EQ(gltang_value_description_bytes(d), 37u);
  }
}

TEST(Description, ALengthThatWouldWrapTheByteCountIsStillALimit) {
  GLTANG_ValueLimits l = limits(0, 1000, 0);
  Desc d(&l);
  GLTANG_ValueRef r = {0, 0};
  // Never read: the limit is checked before a byte is touched.
  EXPECT_EQ(gltang_value_string(d, "x", SIZE_MAX - 8, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &r), GLTANG_ERR_LIMIT);
  EXPECT_EQ(gltang_value_description_nodes(d), 0u);
  GLTANG_ValueRef map = d.map();
  GLTANG_ValueRef value = d.integer(1);
  EXPECT_EQ(gltang_value_map_set(d, map, "k", SIZE_MAX - 8, GLTANG_UNICODE_STRING_TYPE_TRUSTED, value), GLTANG_ERR_LIMIT) << "a key's length is a limit too";
  EXPECT_EQ(gltang_value_map_set(d, map, "k", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, value), GLTANG_OK) << "and the value was still free";
}

TEST(Description, TheDepthLimitCountsTheNestingOfContainers) {
  GLTANG_ValueLimits l = limits(0, 0, 3);
  Desc d(&l);
  GLTANG_ValueRef a1 = d.array();
  GLTANG_ValueRef a2 = d.array();
  GLTANG_ValueRef a3 = d.array();
  GLTANG_ValueRef a4 = d.array();
  ASSERT_EQ(gltang_value_array_push(d, a1, a2), GLTANG_OK);
  ASSERT_EQ(gltang_value_array_push(d, a2, a3), GLTANG_OK) << "three arrays nested is depth three";
  EXPECT_EQ(gltang_value_array_push(d, a3, a4), GLTANG_ERR_LIMIT) << "a fourth is depth four";
  // The refusal changed nothing: a4 is free, and an element of a3 is allowed
  // (a scalar is depth 0, so an array of scalars is depth 1).
  GLTANG_ValueRef leaf = d.integer(1);
  EXPECT_EQ(gltang_value_array_push(d, a3, leaf), GLTANG_OK);
  EXPECT_EQ(gltang_value_set_root(d, a1), GLTANG_OK);
  EXPECT_EQ(gltang_value_set_root(d, a4), GLTANG_OK) << "the free node is still free";
}

TEST(Description, TheDepthOfASubtreeIsCountedWhenItIsAttachedAbove) {
  // Built bottom-up: a chain of three, then put under two levels.
  GLTANG_ValueLimits l = limits(0, 0, 4);
  Desc d(&l);
  GLTANG_ValueRef top = d.array();
  GLTANG_ValueRef mid = d.array();
  GLTANG_ValueRef low1 = d.array();
  GLTANG_ValueRef low2 = d.array();
  GLTANG_ValueRef low3 = d.array();
  ASSERT_EQ(gltang_value_array_push(d, low1, low2), GLTANG_OK);
  ASSERT_EQ(gltang_value_array_push(d, low2, low3), GLTANG_OK);  // extent 3
  ASSERT_EQ(gltang_value_array_push(d, top, mid), GLTANG_OK);
  EXPECT_EQ(gltang_value_array_push(d, mid, low1), GLTANG_ERR_LIMIT) << "2 above and 3 below is 5";
  ASSERT_EQ(gltang_value_array_push(d, top, low1), GLTANG_OK) << "1 above and 3 below is 4";
  // And the growth below is seen from the top: top holds a chain of three, so
  // a node under the deepest of them is a fifth level.
  GLTANG_ValueRef deeper = d.array();
  EXPECT_EQ(gltang_value_array_push(d, low3, deeper), GLTANG_ERR_LIMIT);
}

TEST(Description, TheDepthCeilingIsTheEnginesAndIsAllowedExactly) {
  GLTANG_ValueLimits l = limits(0, 0, GLTANG_VALUE_MAX_DEPTH);
  Desc d(&l);
  GLTANG_ValueRef previous = d.array();
  GLTANG_ValueRef root = previous;
  for (size_t k = 1; k < GLTANG_VALUE_MAX_DEPTH; ++k) {
    GLTANG_ValueRef next = d.array();
    ASSERT_EQ(gltang_value_array_push(d, previous, next), GLTANG_OK) << k;
    previous = next;
  }
  GLTANG_ValueRef one_more = d.array();
  EXPECT_EQ(gltang_value_array_push(d, previous, one_more), GLTANG_ERR_LIMIT) << "2,049 nested arrays";
  GLTANG_ValueRef leaf = d.integer(1);
  EXPECT_EQ(gltang_value_array_push(d, previous, leaf), GLTANG_OK) << "a scalar in the innermost one is still depth 2,048";
  EXPECT_EQ(gltang_value_set_root(d, root), GLTANG_OK);
}

TEST(Description, TheDefaultDepthIsSixtyFour) {
  Desc d;
  GLTANG_ValueRef previous = d.array();
  for (int k = 1; k < 64; ++k) {
    GLTANG_ValueRef next = d.array();
    ASSERT_EQ(gltang_value_array_push(d, previous, next), GLTANG_OK) << k;
    previous = next;
  }
  GLTANG_ValueRef one_more = d.array();
  EXPECT_EQ(gltang_value_array_push(d, previous, one_more), GLTANG_ERR_LIMIT);
}

TEST(Description, AMapKeyedTwiceIsKeptAndCountsAgainstTheLimits) {
  Desc d;
  GLTANG_ValueRef map = d.map();
  GLTANG_ValueRef one = d.integer(1);
  GLTANG_ValueRef two = d.integer(2);
  ASSERT_EQ(gltang_value_map_set(d, map, "k", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, one), GLTANG_OK);
  ASSERT_EQ(gltang_value_map_set(d, map, "k", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, two), GLTANG_OK);
  EXPECT_EQ(gltang_value_description_nodes(d), 3u);
  EXPECT_EQ(gltang_value_description_bytes(d), 3u * 16u + 2u);
}

// ---------------------------------------------------------------------------
// Sealing and sharing
// ---------------------------------------------------------------------------

TEST(Description, ASealedOneRefusesEveryMutation) {
  Desc d;
  GLTANG_ValueRef array = d.array();
  GLTANG_ValueRef map = d.map();
  GLTANG_ValueRef free_node = d.integer(1);
  ASSERT_EQ(gltang_value_description_seal(d), GLTANG_OK);
  EXPECT_TRUE(gltang_value_description_sealed(d));
  EXPECT_EQ(gltang_value_description_seal(d), GLTANG_OK) << "sealing twice is not an error";
  GLTANG_ValueRef r = {0, 0};
  EXPECT_EQ(gltang_value_null(d, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_bool(d, true, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_integer(d, 1, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_float(d, 1.0, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_string(d, "x", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_array(d, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_map(d, &r), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_array_push(d, array, free_node), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_map_set(d, map, "k", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED, free_node), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_set_root(d, array), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_value_description_nodes(d), 3u) << "a refusal changes nothing";
}

TEST(Description, HandingOneToALibrarySealsItAndTheLibraryKeepsItAlive) {
  GLTANG_Library * library = nullptr;
  ASSERT_EQ(gltang_library_create(nullptr, &library), GLTANG_OK);
  GLTANG_ValueDescription * d = nullptr;
  ASSERT_EQ(gltang_value_description_create(nullptr, &d), GLTANG_OK);
  GLTANG_ValueRef root = {0, 0};
  ASSERT_EQ(gltang_value_array(d, &root), GLTANG_OK);
  ASSERT_EQ(gltang_value_set_root(d, root), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_value(library, "rows", d), GLTANG_OK);
  EXPECT_TRUE(gltang_value_description_sealed(d));
  GLTANG_ValueRef r = {0, 0};
  EXPECT_EQ(gltang_value_integer(d, 1, &r), GLTANG_ERR_INVALID);
  gltang_value_description_release(d);  // the host's reference; the library's remains
  EXPECT_EQ(gltang_library_count(library), 1u);
  gltang_library_release(library);      // the last: frees the member and the description (ASan and Valgrind see a leak)
}

TEST(Description, ARefusedAddLeavesTheLibraryAndTheDescriptionAlone) {
  GLTANG_Library * library = nullptr;
  ASSERT_EQ(gltang_library_create(nullptr, &library), GLTANG_OK);
  Desc d;
  EXPECT_EQ(gltang_library_add_value(library, "x", nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_value(nullptr, "x", d), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_value(library, "", d), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_value(library, "a.b", d), GLTANG_ERR_INVALID);
  EXPECT_FALSE(gltang_value_description_sealed(d)) << "a refused add does not seal";
  ASSERT_EQ(gltang_library_add_value(library, "x", d), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_value(library, "x", d), GLTANG_ERR_INVALID) << "a duplicate name";
  EXPECT_EQ(gltang_library_add_value_factory(library, "y", nullptr, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_count(library), 1u);
  gltang_library_release(library);
}

TEST(Description, ASealedOneIsRetainedAndReleasedFromManyThreads) {
  GLTANG_ValueDescription * d = nullptr;
  ASSERT_EQ(gltang_value_description_create(nullptr, &d), GLTANG_OK);
  GLTANG_ValueRef a = {0, 0};
  ASSERT_EQ(gltang_value_array(d, &a), GLTANG_OK);
  ASSERT_EQ(gltang_value_set_root(d, a), GLTANG_OK);
  ASSERT_EQ(gltang_value_description_seal(d), GLTANG_OK);
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&]() {
      for (int k = 0; k < 2000; ++k) {
        GLTANG_ValueDescription * mine = gltang_value_description_retain(d);
        if (mine == d && gltang_value_description_sealed(mine) && gltang_value_description_nodes(mine) == 1u) {
          ++ok;
        }
        gltang_value_description_release(mine);
      }
    });
  }
  for (auto & t : threads) {
    t.join();
  }
  EXPECT_EQ(ok.load(), 8000);
  gltang_value_description_release(d);  // the last reference frees it
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
