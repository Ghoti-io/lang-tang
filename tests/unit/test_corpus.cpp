/**
 * @file
 *
 * The corpus: every file in it, parsed in its directory's mode, against the
 * name it carries, and the walk that proves every node type is produced by
 * some accepted file.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "oracle/oracle.h"

#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#include <ghoti.io/lang-tang/ast/astNodeAll.h>

namespace {

const std::string kCorpus = std::string(GLTANG_TEST_DATA) + "/corpus";

GLTANG_ParseMode mode_of(const std::string & file) {
  return file.compare(0, 7, "script/") == 0 ? GLTANG_PARSE_SCRIPT : GLTANG_PARSE_TEMPLATE;
}

bool named_reject(const std::string & file) {
  return file.find("/reject-") != std::string::npos;
}

void collect(GLTANG_Ast_Node * node, void * data, void *) {
  static_cast<std::set<std::string> *>(data)->insert(node->vtable->name);
}

// Every node class, by vtable. The count is checked against the headers so
// that a class added there and not here fails this test.
const GLTANG_Ast_Node_VTable * kVtables[] = {
  &gltang_ast_node_array_vtable, &gltang_ast_node_assign_vtable,
  &gltang_ast_node_binary_vtable, &gltang_ast_node_block_vtable,
  &gltang_ast_node_boolean_vtable, &gltang_ast_node_break_vtable,
  &gltang_ast_node_cast_vtable, &gltang_ast_node_continue_vtable,
  &gltang_ast_node_do_while_vtable, &gltang_ast_node_float_vtable,
  &gltang_ast_node_for_vtable, &gltang_ast_node_function_vtable,
  &gltang_ast_node_function_call_vtable, &gltang_ast_node_global_vtable,
  &gltang_ast_node_identifier_vtable, &gltang_ast_node_if_else_vtable,
  &gltang_ast_node_index_vtable, &gltang_ast_node_integer_vtable,
  &gltang_ast_node_library_vtable, &gltang_ast_node_map_vtable,
  &gltang_ast_node_parse_error_vtable, &gltang_ast_node_period_vtable,
  &gltang_ast_node_print_vtable, &gltang_ast_node_ranged_for_vtable,
  &gltang_ast_node_return_vtable, &gltang_ast_node_slice_vtable,
  &gltang_ast_node_string_vtable, &gltang_ast_node_ternary_vtable,
  &gltang_ast_node_unary_vtable, &gltang_ast_node_use_vtable,
  &gltang_ast_node_while_vtable,
};

} // namespace

TEST(Corpus, HoldsAtLeastSixtyFilesInBothModes) {
  auto files = oracle::list_corpus(kCorpus);
  EXPECT_GE(files.size(), 60u);
  size_t script = 0, tmpl = 0;
  for (const auto & f : files) {
    (f.compare(0, 7, "script/") == 0 ? script : tmpl)++;
  }
  EXPECT_GE(script, 20u);
  EXPECT_GE(tmpl, 10u);
}

TEST(Corpus, EveryFileIsAcceptedOrRefusedAsItsNameSays) {
  // A file named reject-* is refused and every other file is accepted. The
  // name is what a reader sees first; a file whose verdict contradicts it is a
  // mislabelled case, and this is where that is found.
  for (const auto & file : oracle::list_corpus(kCorpus)) {
    std::string source = read_file(kCorpus + "/" + file);
    Balance balance;
    Parsed parsed(source, mode_of(file));
    if (named_reject(file)) {
      EXPECT_EQ(parsed.result, GLTANG_ERR_FORMAT) << file << " should be refused";
    }
    else {
      EXPECT_EQ(parsed.result, GLTANG_OK) << file << " should be accepted: " << parsed.error.message << " at " << parsed.error.line << ":" << parsed.error.column;
    }
    gltang_tree_destroy(parsed.tree);
    parsed.tree = nullptr;
    EXPECT_TRUE(balance.balanced()) << "leaked on " << file;
  }
}

TEST(Corpus, EveryRefusedFileNamesAPositionAndAMessage) {
  for (const auto & file : oracle::list_corpus(kCorpus)) {
    if (!named_reject(file)) {
      continue;
    }
    Parsed parsed(read_file(kCorpus + "/" + file), mode_of(file));
    ASSERT_EQ(parsed.result, GLTANG_ERR_FORMAT) << file;
    EXPECT_GE(parsed.error.line, 1) << file;
    EXPECT_GE(parsed.error.column, 1) << file;
    EXPECT_STRNE(parsed.error.message, "") << file;
  }
}

TEST(Corpus, EveryNodeTypeIsProducedBySomeAcceptedFile) {
  std::set<std::string> seen;
  size_t accepted = 0;
  for (const auto & file : oracle::list_corpus(kCorpus)) {
    if (named_reject(file)) {
      continue;
    }
    Parsed parsed(read_file(kCorpus + "/" + file), mode_of(file));
    ASSERT_EQ(parsed.result, GLTANG_OK) << file;
    accepted++;
    if (GLTANG_Ast_Node * root = gltang_tree_root(parsed.tree)) {
      gltang_ast_node_walk(root, collect, &seen, nullptr);
    }
  }
  EXPECT_GE(accepted, 40u);

  // A parse-error node is never inside an accepted tree - a refusal is a
  // result, not a tree - so it is the one class made directly.
  GLTANG_PARSER_LTYPE nowhere = {0, 0, 0, 0};
  GLTANG_Ast_Node_Parse_Error * error = gltang_ast_node_parse_error_create("planted", nowhere);
  ASSERT_NE(error, nullptr);
  gltang_ast_node_walk((GLTANG_Ast_Node *)error, collect, &seen, nullptr);
  gltang_ast_node_destroy((GLTANG_Ast_Node *)error);

  for (const GLTANG_Ast_Node_VTable * vtable : kVtables) {
    EXPECT_TRUE(seen.count(vtable->name)) << "no corpus file produces a " << vtable->name << " node";
  }
}

TEST(Corpus, TheNodeTypeTableCoversEveryClassTheHeadersDeclare) {
  // Count the node vtables declared under include/ and compare, so that a new
  // node class cannot be added without the coverage test hearing about it.
  size_t declared = 0;
  for (const char * header : {"Array", "Assign", "Binary", "Block", "Boolean", "Break", "Cast", "Continue", "DoWhile", "Float", "For", "FunctionCall", "Function", "Global", "Identifier", "IfElse", "Index", "Integer", "Library", "Map", "ParseError", "Period", "Print", "RangedFor", "Return", "Slice", "String", "Ternary", "Unary", "Use", "While"}) {
    std::string text = read_file(std::string(GLTANG_TEST_DATA) + "/../include/ghoti.io/lang-tang/ast/astNode" + header + ".h");
    if (text.find("GLTANG_API_DATA extern GLTANG_Ast_Node_VTable") != std::string::npos) {
      declared++;
    }
  }
  EXPECT_EQ(declared, sizeof(kVtables) / sizeof(kVtables[0]));
  // And no header was missed: the directory holds exactly these plus the base
  // and the umbrella.
  size_t count = 0;
  for (const auto & entry : std::filesystem::directory_iterator(std::string(GLTANG_TEST_DATA) + "/../include/ghoti.io/lang-tang/ast")) {
    (void)entry;
    ++count;
  }
  EXPECT_EQ(count, declared + 2); // astNode.h and astNodeAll.h
}

TEST(Corpus, EveryAcceptedTreeCountsTheSameWaysOverAndWalksOnce) {
  for (const auto & file : oracle::list_corpus(kCorpus)) {
    if (named_reject(file)) {
      continue;
    }
    Parsed parsed(read_file(kCorpus + "/" + file), mode_of(file));
    ASSERT_EQ(parsed.result, GLTANG_OK) << file;
    EXPECT_EQ(gltang_tree_node_count(parsed.tree), gltang_ast_node_count(gltang_tree_root(parsed.tree))) << file;
  }
}

TEST(Corpus, EveryAcceptedTreePrintsAndTheOutputNamesItsRoot) {
  // print is the AST's own dump, and walking every class's print is how a
  // class whose print was lost in the port is found. The first line names the
  // root's class.
  for (const auto & file : oracle::list_corpus(kCorpus)) {
    if (named_reject(file)) {
      continue;
    }
    Parsed parsed(read_file(kCorpus + "/" + file), mode_of(file));
    ASSERT_EQ(parsed.result, GLTANG_OK) << file;
    GLTANG_Ast_Node * root = gltang_tree_root(parsed.tree);
    testing::internal::CaptureStdout();
    gltang_tree_print(parsed.tree);
    std::string out = testing::internal::GetCapturedStdout();
    if (!root) {
      EXPECT_EQ(out, "") << file;
      continue;
    }
    ASSERT_FALSE(out.empty()) << file;
    EXPECT_EQ(out.back(), '\n') << file;
    EXPECT_EQ(out.compare(0, strlen(root->vtable->name), root->vtable->name), 0) << file << ": " << out.substr(0, 40);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
