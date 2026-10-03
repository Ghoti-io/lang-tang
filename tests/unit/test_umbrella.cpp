/**
 * @file
 *
 * The umbrella header, included from C++ and on its own: everything a
 * consumer of the stable interface needs, and none of the free one.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/lang-tang/lang-tang.h>

#include <gtest/gtest.h>

TEST(Umbrella, ParsesAndReportsThroughOnlyTheUmbrella) {
  GLTANG_Tree * tree = nullptr;
  ASSERT_EQ(gltang_parse("Hello <%= 1 + 1 %>", GLTANG_PARSE_TEMPLATE, nullptr, &tree), GLTANG_OK);
  EXPECT_GT(gltang_tree_node_count(tree), 0u);
  EXPECT_NE(gltang_tree_root(tree), nullptr);
  gltang_tree_destroy(tree);

  GLTANG_ParseError error;
  ASSERT_EQ(gltang_parse("x = ;", GLTANG_PARSE_SCRIPT, &error, &tree), GLTANG_ERR_FORMAT);
  EXPECT_STRNE(error.message, "");
}

TEST(Umbrella, DoesNotDragInTheSyntaxTreeClasses) {
  // GLTANG_Ast_Node is only named, never defined, by the stable headers:
  // the node classes are labelled free and live under ast/.
#ifdef GHOTI_IO_GLTANG_AST_ASTNODE_H
  FAIL() << "the umbrella includes ast/astNode.h";
#endif
  SUCCEED();
}

TEST(Umbrella, VersionAndAllocatorAreReachable) {
  EXPECT_STRNE(gltang_version_string(), "");
  EXPECT_NE(gltang_allocator(), nullptr);
  EXPECT_NE(gltang_allocator_default(), nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
