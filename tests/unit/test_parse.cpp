/**
 * @file
 *
 * The parse interface, row by row of the I/O matrix, and the parse-only tests
 * ported from ctang's test-tangLanguageParse.cpp (the ones that used
 * simplify() are not ported: simplify is not).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>

#include <ghoti.io/lang-tang/ast/astNodeAll.h>

using std::string;

namespace {
const GLTANG_ParseMode kScript = GLTANG_PARSE_SCRIPT;
const GLTANG_ParseMode kTemplate = GLTANG_PARSE_TEMPLATE;
} // namespace

TEST(Parse, EmptySourceIsATreeWithNoRootInBothModes) {
  for (GLTANG_ParseMode mode : {kScript, kTemplate}) {
    Balance balance;
    Parsed parsed("", mode);
    ASSERT_EQ(parsed.result, GLTANG_OK);
    ASSERT_NE(parsed.tree, nullptr);
    EXPECT_EQ(gltang_tree_root(parsed.tree), nullptr);
    EXPECT_EQ(gltang_tree_node_count(parsed.tree), 0u);
    testing::internal::CaptureStdout();
    gltang_tree_print(parsed.tree);
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "");
    gltang_tree_destroy(parsed.tree);
    parsed.tree = nullptr;
    EXPECT_TRUE(balance.balanced());
  }
}

TEST(Parse, OnlyWhitespaceAndCommentsIsEmptyToo) {
  Parsed script("  // a comment\n/* another */\n", kScript);
  ASSERT_EQ(script.result, GLTANG_OK);
  EXPECT_EQ(gltang_tree_root(script.tree), nullptr);
  Parsed tmpl("<% /* nothing but a comment */ %>", kTemplate);
  ASSERT_EQ(tmpl.result, GLTANG_OK);
  EXPECT_EQ(gltang_tree_node_count(tmpl.tree), 0u); // a tag holding nothing makes no node either
}

TEST(Parse, NodeCountsMatchWhatCtangsOwnTestsStatedBeforeSimplifying) {
  // The counts below are the ones test-tangLanguageParse.cpp asserts on the
  // parsed tree, before its simplify() step.
  struct { const char * source; size_t count; } cases[] = {
    {"1 + 2", 3},
    {"[1, 2 + 3 + 4, 5 * 6 * 7]", 12},
    {"1.1 + 2.2", 3},
    {"1.1 / 0", 3},
    {"[1.1, 2.2 + 3.3 + 4.4, 5.5 * 6.6 * 7.7]", 12},
    {"1 + 2. == 3", 5},
    {"1 + 2. < 3", 5},
    {"true && false || 3", 5},
    {"true && false && 3", 5},
    {R"("a" + "b" + "c")", 5},
    {R"("a" == "b")", 3},
    {"-3", 2},
    {"-3.3", 2},
    {"!true", 2},
    {"!!true", 3},
    {"!3", 2},
    {"!0", 2},
    {"!3.3", 2},
  };
  for (const auto & c : cases) {
    Parsed parsed(c.source, kScript);
    ASSERT_EQ(parsed.result, GLTANG_OK) << c.source;
    EXPECT_EQ(gltang_tree_node_count(parsed.tree), c.count) << c.source;
    EXPECT_EQ(gltang_ast_node_count(gltang_tree_root(parsed.tree)), c.count) << c.source;
  }
}

TEST(Parse, TemplateTextAndTagsBecomePrintsAndBlocks) {
  Parsed parsed("a <%= 1 %> b", kTemplate);
  ASSERT_EQ(parsed.result, GLTANG_OK);
  ASSERT_NE(gltang_tree_root(parsed.tree), nullptr);
  EXPECT_TRUE(GLTANG_AST_IS_BLOCK(gltang_tree_root(parsed.tree)));
  testing::internal::CaptureStdout();
  gltang_tree_print(parsed.tree);
  string out = testing::internal::GetCapturedStdout();
  EXPECT_NE(out.find("Print"), string::npos);
  EXPECT_NE(out.find("String: \"a \""), string::npos);
  EXPECT_NE(out.find("Integer: 1"), string::npos);
  EXPECT_NE(out.find("String: \" b\""), string::npos);
}

TEST(Parse, PrintWritesTheAstsOwnIndentedDump) {
  Parsed parsed("1 + 2", kScript);
  ASSERT_EQ(parsed.result, GLTANG_OK);
  testing::internal::CaptureStdout();
  gltang_tree_print(parsed.tree);
  EXPECT_EQ(testing::internal::GetCapturedStdout(),
      "Binary (+):\n  LHS:\n    Integer: 1\n  RHS:\n    Integer: 2\n");
}

TEST(Parse, SyntaxErrorIsFormatWithAOneBasedPositionAndNoTree) {
  Balance balance;
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Tree * tree = nullptr;
  ASSERT_EQ(gltang_parse("x = ;", kScript, &error, &tree), GLTANG_ERR_FORMAT);
  EXPECT_EQ(tree, nullptr);
  EXPECT_EQ(error.line, 1);
  EXPECT_EQ(error.column, 5);
  EXPECT_NE(string(error.message).find("syntax error"), string::npos) << error.message;
  EXPECT_TRUE(balance.balanced());
}

TEST(Parse, ErrorPositionCountsLines) {
  Parsed parsed("a = 1;\nb = 2;\nc = ;\n", kScript);
  ASSERT_EQ(parsed.result, GLTANG_ERR_FORMAT);
  EXPECT_EQ(parsed.error.line, 3);
  EXPECT_EQ(parsed.error.column, 5);
  EXPECT_EQ(parsed.tree, nullptr);
}

TEST(Parse, TemplateErrorPositionIsReportedInsideTheTag) {
  Parsed parsed("text\n<% x = ; %>", kTemplate);
  ASSERT_EQ(parsed.result, GLTANG_ERR_FORMAT);
  EXPECT_EQ(parsed.error.line, 2);
}

TEST(Parse, ScannerErrorsAreFormatToo) {
  struct { const char * name; const char * source; GLTANG_ParseMode mode; } cases[] = {
    {"an unterminated string", "x = \"abc", kScript},
    {"a trailing backslash", "x = \"abc\\", kScript},
    {"an octal escape over 255", "\"\\400\";", kScript},
    {"a bad digit escape", "\"\\48\";", kScript},
    {"%> in script mode", "a = 1; %> b = 2;", kScript},
    {"a stray character", "a # b;", kScript},
    {"a single quote", "a = 'b';", kScript},
    {"a comment tag", "<%# note %>", kTemplate},
    {"nested tags", "<% <% %> %>", kTemplate},
    {"a truncated block", "if (a) {", kScript},
    {"truncated input", "function f(", kScript},
  };
  for (const auto & c : cases) {
    Balance balance;
    GLTANG_ParseError error = {-1, -1, {0}};
    GLTANG_Tree * tree = nullptr;
    EXPECT_EQ(gltang_parse(c.source, c.mode, &error, &tree), GLTANG_ERR_FORMAT) << c.name;
    EXPECT_EQ(tree, nullptr) << c.name;
    EXPECT_GE(error.line, 1) << c.name;
    EXPECT_STRNE(error.message, "") << c.name;
    EXPECT_TRUE(balance.balanced()) << c.name;
  }
}

TEST(Parse, InvalidUtf8IsFormatNamingTheCauseNotOutOfMemory) {
  // ctang reported this as "Out of memory", because the same failed string
  // creation covers both. A refusal of the author's bytes is a format error.
  for (const char * source : {"\"\xff\"", "\"\x80\"", "\"\xc3\"", "\"\xf0\x9f\""}) {
    Balance balance;
    Parsed parsed(source, kScript);
    EXPECT_EQ(parsed.result, GLTANG_ERR_FORMAT) << source;
    EXPECT_STREQ(parsed.error.message, "Invalid UTF-8 in a string") << source;
    EXPECT_GE(parsed.error.line, 1) << source;
    EXPECT_EQ(parsed.tree, nullptr);
    EXPECT_TRUE(balance.balanced()) << source;
  }
  Parsed text("\xc7", kTemplate);
  EXPECT_EQ(text.result, GLTANG_ERR_FORMAT);
  EXPECT_STREQ(text.error.message, "Invalid UTF-8 in a string");
  Parsed valid("\"caf\xc3\xa9\";", kScript);
  EXPECT_EQ(valid.result, GLTANG_OK);
}

TEST(Parse, CompoundAssignmentOnAnIndexIsRefusedAndOnANameIsNot) {
  Parsed refused("a[i] += 1;", kScript);
  EXPECT_EQ(refused.result, GLTANG_ERR_FORMAT);
  Parsed accepted("a += 1;", kScript);
  ASSERT_EQ(accepted.result, GLTANG_OK);
  // `a += 1` is the tree for `a = a + 1`: assign, identifier, binary,
  // identifier, integer - six with the block a statement list makes.
  EXPECT_EQ(gltang_tree_node_count(accepted.tree), 6u);
}

TEST(Parse, IntegerLiteralEdges) {
  EXPECT_EQ(Parsed("9223372036854775807;", kScript).result, GLTANG_OK);
  // The minimum magnitude is legal in exactly one place: directly after unary
  // minus (language reference 2.6).
  EXPECT_EQ(Parsed("-9223372036854775808;", kScript).result, GLTANG_OK);
  EXPECT_EQ(Parsed("9223372036854775808;", kScript).result, GLTANG_ERR_FORMAT);
  EXPECT_EQ(Parsed("1 - 9223372036854775808;", kScript).result, GLTANG_ERR_FORMAT);
  EXPECT_EQ(Parsed("99999999999999999999;", kScript).result, GLTANG_ERR_FORMAT);
}

TEST(Parse, AnUnterminatedTagIsNotAnError) {
  // Language reference 2.2: the code inside is compiled as if it were closed.
  EXPECT_EQ(Parsed("text <% print(1);", kTemplate).result, GLTANG_OK);
}

TEST(Parse, ModesDifferOnlyInTheStartingState) {
  // `%>` is text in a template and an error in a script.
  EXPECT_EQ(Parsed("a %> b", kTemplate).result, GLTANG_OK);
  EXPECT_EQ(Parsed("a %> b", kScript).result, GLTANG_ERR_FORMAT);
}

TEST(Parse, ArgumentsAreChecked) {
  GLTANG_Tree * sentinel = reinterpret_cast<GLTANG_Tree *>(0x1);
  GLTANG_Tree * tree = sentinel;
  GLTANG_ParseError error = {7, 8, "keep"};
  EXPECT_EQ(gltang_parse(nullptr, kScript, &error, &tree), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_parse("1", kScript, &error, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_parse("1", (GLTANG_ParseMode)99, &error, &tree), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_parse("1", GLTANG_PARSE_MODE_COUNT, &error, &tree), GLTANG_ERR_INVALID);
  EXPECT_EQ(tree, sentinel); // written only on success
  EXPECT_EQ(error.line, 7);
  EXPECT_STREQ(error.message, "keep");
}

TEST(Parse, OutputsAreWrittenOnlyWhenTheirResultSaysSo) {
  GLTANG_ParseError error = {7, 8, "keep"};
  GLTANG_Tree * tree = nullptr;
  ASSERT_EQ(gltang_parse("1 + 2", kScript, &error, &tree), GLTANG_OK);
  EXPECT_EQ(error.line, 7); // success leaves the error untouched
  EXPECT_STREQ(error.message, "keep");
  gltang_tree_destroy(tree);

  GLTANG_Tree * untouched = reinterpret_cast<GLTANG_Tree *>(0x2);
  tree = untouched;
  ASSERT_EQ(gltang_parse("1 +", kScript, &error, &tree), GLTANG_ERR_FORMAT);
  EXPECT_EQ(tree, untouched); // failure leaves the tree untouched
  EXPECT_NE(error.line, 7);
}

TEST(Parse, TheErrorOutputIsOptional) {
  GLTANG_Tree * tree = nullptr;
  EXPECT_EQ(gltang_parse("1 +", kScript, nullptr, &tree), GLTANG_ERR_FORMAT);
  EXPECT_EQ(tree, nullptr);
}

TEST(Parse, NullTreeQueriesAreSafe) {
  EXPECT_EQ(gltang_tree_node_count(nullptr), 0u);
  EXPECT_EQ(gltang_tree_root(nullptr), nullptr);
  gltang_tree_print(nullptr);
  gltang_tree_destroy(nullptr);
  EXPECT_EQ(gltang_ast_node_count(nullptr), 0u);
}

TEST(Parse, NestingPastTheParsersStackIsALimitNotACrash) {
  // bison's stack stops at 10000 levels, and says only "memory exhausted".
  Balance balance;
  string deep(20000, '(');
  Parsed parsed(deep, kScript);
  EXPECT_EQ(parsed.result, GLTANG_ERR_LIMIT);
  EXPECT_EQ(parsed.tree, nullptr);
  EXPECT_TRUE(balance.balanced());
}

TEST(Parse, ALongFlatSourceIsFine) {
  Balance balance;
  string source;
  for (int i = 0; i < 5000; i++) {
    source += "print(\"x\");";
  }
  {
    Parsed parsed(source, kScript);
    ASSERT_EQ(parsed.result, GLTANG_OK);
    EXPECT_GT(gltang_tree_node_count(parsed.tree), 10000u);
  }
  EXPECT_TRUE(balance.balanced());
}

// ---- ported from ctang's test-tangLanguageParse.cpp ----------------------

TEST(Parse, FailingRuleFreesWhatItWasHanded) {
  // A rule action that bails is the last owner of everything it was handed:
  // bison has already popped the RHS into $1..$n, so the %destructor will not
  // run for those. Each case below leaked before ctang's fix; each is the
  // smallest input reaching its arm.
  for (const char * src : {
      "s=\"\xc8\"",
      "\"\xa0\";r",
      "\"\xff\".g",
      "f(\"\xff\")",
      "[\"\xff\"]",
      "{a:\"\xff\"}",
      "for(i:\"\xff\"){}",
      "function f(\"\xff\"){}",
      // Scanner side, same shape: an error return that skipped CLEANUP_BUFFER.
      "\"1\\421",
      "\"abc\\777def\"",
      }) {
    Balance balance;
    Parsed parsed(src, kScript);
    EXPECT_NE(parsed.result, GLTANG_OK) << src;
    gltang_tree_destroy(parsed.tree);
    parsed.tree = nullptr;
    EXPECT_TRUE(balance.balanced()) << "leaked on: " << src;
  }
}

TEST(Parse, InvalidUtf8InATemplateDoesNotLeaveAFreedPointer) {
  for (const char * src : {
      "\xc7",
      "<%= \"\xc7" "a\" %>",
      "\xdb\xdf plain text \xc7",
      }) {
    Balance balance;
    Parsed parsed(src, kTemplate);
    EXPECT_EQ(parsed.result, GLTANG_ERR_FORMAT) << src;
    EXPECT_TRUE(balance.balanced()) << "leaked on: " << src;
  }
  Balance balance;
  {
    // A valid template must still parse, or the guard has broken templates.
    Parsed parsed("a <%= \"b\" %> c", kTemplate);
    EXPECT_EQ(parsed.result, GLTANG_OK);
  }
  EXPECT_TRUE(balance.balanced());
}

TEST(Parse, QuickPrintFreesTheExpressionItWasHanded) {
  for (const char * src : {
      "\x87<%=t%>",
      "\x87<%= \"a\" + b %>",
      "\xdb<%=t%> tail <%=u%>",
      }) {
    Balance balance;
    Parsed parsed(src, kTemplate);
    EXPECT_EQ(parsed.result, GLTANG_ERR_FORMAT) << src;
    EXPECT_TRUE(balance.balanced()) << "leaked on: " << src;
  }
  Balance balance;
  {
    Parsed parsed("hi <%= t %>", kTemplate);
    EXPECT_EQ(parsed.result, GLTANG_OK);
  }
  EXPECT_TRUE(balance.balanced());
}

TEST(Parse, ATrailingTokenAfterAnInvalidStringStillBalances) {
  // Error recovery discards a symbol and runs its %destructor, which has to
  // accept the null the failed arm leaves.
  Balance balance;
  Parsed parsed("\"\xff\"<", kScript);
  EXPECT_EQ(parsed.result, GLTANG_ERR_FORMAT);
  EXPECT_TRUE(balance.balanced());
}

TEST(Parse, UnterminatedStringDoesNotLeak) {
  // The empty literal does not leak, because nothing was accumulated, so a
  // test using only `"` would have passed against the bug.
  for (const char * src : {"x = \"abc", "x = \"abcdefghijklmnopqrstuvwxyz0123456789"}) {
    Balance balance;
    Parsed parsed(src, kScript);
    EXPECT_EQ(parsed.result, GLTANG_ERR_FORMAT) << src;
    EXPECT_TRUE(balance.balanced()) << src;
  }
  Balance balance;
  {
    Parsed parsed("x = \"abc\";", kScript);
    EXPECT_EQ(parsed.result, GLTANG_OK);
  }
  EXPECT_TRUE(balance.balanced());
}

TEST(Parse, EscapedCharactersBecomeTheirBytes) {
  Parsed parsed(R"("a\n\t\x41\101\"\\\q")", kScript);
  ASSERT_EQ(parsed.result, GLTANG_OK);
  GLTANG_Ast_Node * root = gltang_tree_root(parsed.tree);
  ASSERT_TRUE(GLTANG_AST_IS_STRING(root));
  EXPECT_STREQ(((GLTANG_Ast_Node_String *)root)->string->buffer, "a\n\tAA\"\\q");
}

TEST(Parse, StringLiteralTypesSelectTheEncoding) {
  struct { const char * source; GLTANG_String_Type type; } cases[] = {
    {R"("a")", GLTANG_UNICODE_STRING_TYPE_TRUSTED},
    {R"(!"a")", GLTANG_UNICODE_STRING_TYPE_HTML},
    {R"(%"a")", GLTANG_UNICODE_STRING_TYPE_PERCENT},
  };
  for (const auto & c : cases) {
    Parsed parsed(c.source, kScript);
    ASSERT_EQ(parsed.result, GLTANG_OK) << c.source;
    GLTANG_Ast_Node * root = gltang_tree_root(parsed.tree);
    ASSERT_TRUE(GLTANG_AST_IS_STRING(root)) << c.source;
    GLTANG_Unicode_String * s = ((GLTANG_Ast_Node_String *)root)->string;
    EXPECT_EQ(GLTANG_UC_GET_TYPE_FROM_TYPE_OFFSET_PAIR(s->string_type->data[0]), (uint64_t)c.type) << c.source;
  }
}

TEST(Parse, TheMinimumMagnitudeParsesToTheMostNegativeInteger) {
  Parsed parsed("-9223372036854775808", kScript);
  ASSERT_EQ(parsed.result, GLTANG_OK);
  // The parser folds the minus into the literal: that is the one place the
  // magnitude is legal, and the result is a single Integer node.
  GLTANG_Ast_Node * root = gltang_tree_root(parsed.tree);
  ASSERT_TRUE(GLTANG_AST_IS_INTEGER(root));
  EXPECT_EQ(((GLTANG_Ast_Node_Integer *)root)->value, INT64_MIN);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
