/**
 * @file
 *
 * Version, result strings and allocators.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>

TEST(Core, VersionStringIsTheGeneratedOne) {
  EXPECT_STREQ(gltang_version_string(), GLTANG_VERSION_STRING);
  EXPECT_STRNE(gltang_version_string(), "");
}

TEST(Core, VersionNumberIsPackedOneBytePerComponent) {
  EXPECT_EQ(gltang_version_number(), GLTANG_MAKE_VERSION(GLTANG_VERSION_MAJOR, GLTANG_VERSION_MINOR, GLTANG_VERSION_PATCH));
  EXPECT_EQ(GLTANG_MAKE_VERSION(1, 2, 3), 0x010203u);
}

TEST(Core, EveryResultHasADistinctDescription) {
  std::vector<std::string> seen;
  for (int i = 0; i < (int)GLTANG_RESULT_COUNT; i++) {
    const char * text = gltang_result_string((GLTANG_Result)i);
    ASSERT_NE(text, nullptr);
    EXPECT_STRNE(text, "Unknown error") << "result " << i << " has no description";
    for (const auto & other : seen) {
      EXPECT_NE(other, text);
    }
    seen.push_back(text);
  }
}

TEST(Core, ResultsAreNumberedAsInTheRestOfTheSuite) {
  // The suite's fixed vocabulary, and the numbers runtime-heap uses.
  EXPECT_EQ(GLTANG_OK, 0);
  EXPECT_EQ(GLTANG_ERR_IO, 1);
  EXPECT_EQ(GLTANG_ERR_FORMAT, 2);
  EXPECT_EQ(GLTANG_ERR_UNSUPPORTED, 3);
  EXPECT_EQ(GLTANG_ERR_LIMIT, 4);
  EXPECT_EQ(GLTANG_ERR_CORRUPT, 5);
  EXPECT_EQ(GLTANG_ERR_OOM, 6);
  EXPECT_EQ(GLTANG_ERR_INVALID, 7);
  EXPECT_EQ(GLTANG_ERR_INTERNAL, 8);
  EXPECT_EQ(GLTANG_RESULT_COUNT, 9);
}

TEST(Core, ResultStringForAValueOutsideTheEnumIsStillAString) {
  const char * text = gltang_result_string((GLTANG_Result)12345);
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(text, "Unknown error");
}

TEST(Allocator, DefaultIsCutilsAndNeverNull) {
  EXPECT_NE(gltang_allocator_default(), nullptr);
  EXPECT_EQ(gltang_allocator_default(), gcu_allocator_default());
}

TEST(Allocator, ZeroSizeRequestsReturnARealPointer) {
  // NULL has to mean failure and nothing else (CONVENTIONS section 5).
  const GLTANG_Allocator * a = gltang_allocator();
  ASSERT_NE(a, nullptr);
  void * p = a->malloc_fn(a->ctx, 0);
  EXPECT_NE(p, nullptr);
  a->free_fn(a->ctx, p);
  void * q = a->calloc_fn(a->ctx, 0, 8);
  EXPECT_NE(q, nullptr);
  a->free_fn(a->ctx, q);
}

TEST(Allocator, CallocRefusesAnOverflowingProduct) {
  const GLTANG_Allocator * a = gltang_allocator();
  EXPECT_EQ(a->calloc_fn(a->ctx, SIZE_MAX / 2 + 1, 4), nullptr);
}

TEST(Allocator, BlocksAreCountedOnceEachWayThroughRealloc) {
  // The invariant the leak checks in every other test depend on: a block
  // that is grown from NULL, regrown and freed is one allocation and one free.
  const GLTANG_Allocator * a = gltang_allocator();
  Balance balance;
  void * p = a->realloc_fn(a->ctx, nullptr, 16);
  ASSERT_NE(p, nullptr);
  p = a->realloc_fn(a->ctx, p, 64);
  ASSERT_NE(p, nullptr);
  a->free_fn(a->ctx, p);
  a->free_fn(a->ctx, nullptr); // a NULL free is not a free
  EXPECT_EQ(balance.alloc(), 1);
  EXPECT_EQ(balance.freed(), 1);
  EXPECT_TRUE(balance.balanced());
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
