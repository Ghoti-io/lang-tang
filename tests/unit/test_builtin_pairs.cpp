// Every pair of built-in operand kinds, through every binary operator and
// subscript, gives the answer and makes the heap objects it gave before the
// custom kind existed (spec-tang-host-extensibility story 5: "Pairs of built-in
// kinds keep their behaviour exactly").
//
// The expected table, tests/data/custom/builtin-pairs.txt, was written by this
// same test run against the engine as it was before story 5 (lang-tang
// fb2b83e), with GLTANG_PAIRS_WRITE set to the file to write. Each line is one
// operand pair: the two operands' sources, then for each operation the result
// as `as string` shows it and the number of heap objects the operation made.
// The count is the heap's own (`allocations`), read by a native before and
// after the operation, so it does not depend on when the collector runs.

#include "exec_harness.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using tt::Compiled;
using tt::Config;
using tt::Context;

namespace {

const char * const kOperands[] = {
  "null", "true", "false", "0", "7", "-3", "1152921504606846976", "1.5", "-0.0",
  "\"s\"", "\"\"", "\"<b>\".html", "[1, 2]", "[]", "{a: 1}", "{k: [1]}", "f", "random.seeded",
  "math", "random.seeded(1)", "1 / 0",
};

const char * const kOperators[] = {"+", "-", "*", "/", "%", "<", "<=", ">", ">=", "==", "!=", "[]"};

const char * const kUnary[] = {"-a", "a.size", "a.length", "a.x", "a as string", "a as int", "a as float",
                               "a as bool", "a[0:1]", "!a"};

bool probe(GLTANG_NativeCall * call, void * user) {
  GRHEAP_Stats stats;
  const Context * context = static_cast<const Context *>(user);
  if (grheap_stats(context->heap, &stats) != GRHEAP_OK) {
    return false;
  }
  gltang_call_return_integer(call, static_cast<int64_t>(stats.allocations));
  return true;
}

/// The source of a program that does each of `operations` once over `a` and `b`
/// and answers an array: each result, then the objects it made.
std::string program(const std::string & a, const std::string & b, const std::vector<std::string> & operations) {
  std::string s = "use probe; use math; use random; function f() { return 1; } a = " + a + "; b = " + b + ";";
  for (size_t i = 0; i < operations.size(); ++i) {
    std::string n = std::to_string(i);
    s += " s" + n + " = probe(); r" + n + " = " + operations[i] + "; d" + n + " = probe() - s" + n + ";";
  }
  s += " [";
  for (size_t i = 0; i < operations.size(); ++i) {
    std::string n = std::to_string(i);
    s += (i ? ", r" : "r") + n + ", d" + n;
  }
  s += "];";
  return s;
}

std::string run(const std::string & source) {
  Compiled compiled(source);
  EXPECT_TRUE(compiled.ok()) << source << ": " << compiled.error.message;
  if (!compiled.ok()) {
    return "<compile error>";
  }
  Config config;
  // The objects an operation makes, not when the collector runs, are counted:
  // the instruments that change collection times are off for this table.
  config.torture = 0;
  config.verify = 0;
  config.moving_stack = 0;
  config.jit_threshold = 0;
  Context context(compiled.program, config);
  EXPECT_TRUE(context.ok());
  if (!context.ok()) {
    return "<context error>";
  }
  EXPECT_EQ(gltang_library_add_native(context.library(), "probe", probe, &context), GLTANG_OK);
  EXPECT_TRUE(context.execute()) << source;
  return context.describe();
}

std::vector<std::string> table() {
  std::vector<std::string> lines;
  std::vector<std::string> binary;
  for (const char * op : kOperators) {
    binary.push_back(std::string(op) == "[]" ? std::string("a[b]") : std::string("a ") + op + " b");
  }
  for (const char * a : kOperands) {
    for (const char * b : kOperands) {
      lines.push_back(std::string(a) + "\t" + b + "\t" + run(program(a, b, binary)));
    }
  }
  std::vector<std::string> unary(std::begin(kUnary), std::end(kUnary));
  for (const char * a : kOperands) {
    lines.push_back(std::string(a) + "\t-\t" + run(program(a, "null", unary)));
  }
  return lines;
}

std::string data_path() {
  return std::string(GLTANG_TEST_DATA) + "/data/custom/builtin-pairs.txt";
}

}  // namespace

TEST(BuiltinPairs, EveryPairGivesTheAnswerAndTheAllocationsItGaveBeforeTheCustomKind) {
  std::vector<std::string> now = table();
  if (const char * out = std::getenv("GLTANG_PAIRS_WRITE")) {
    std::ofstream f(out);
    for (const std::string & line : now) {
      f << line << "\n";
    }
    GTEST_SKIP() << "wrote " << now.size() << " lines to " << out;
  }
  std::ifstream in(data_path());
  ASSERT_TRUE(in.good()) << "the expected table " << data_path() << " is missing";
  std::vector<std::string> expected;
  for (std::string line; std::getline(in, line);) {
    expected.push_back(line);
  }
  ASSERT_EQ(expected.size(), now.size()) << "the table and the operand lists differ in length";
  size_t differences = 0;
  for (size_t i = 0; i < now.size(); ++i) {
    if (now[i] != expected[i]) {
      ADD_FAILURE() << "pair " << i << " changed:\n  was: " << expected[i] << "\n  now: " << now[i];
      if (++differences >= 10) {
        break;
      }
    }
  }
}

TEST(BuiltinPairs, TheTableIsNotVacuous) {
  // The control of the comparison above: the table holds answers and counts,
  // not one repeated failure. Some operations make objects and some make none,
  // and some answer and some fail.
  std::ifstream in(data_path());
  ASSERT_TRUE(in.good());
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("Error: Not supported, 1"), std::string::npos);
  EXPECT_NE(text.find("true, 0"), std::string::npos);
  EXPECT_NE(text.find("[1, 2, 1, 2], 2"), std::string::npos) << "array + array makes a header and its storage and nothing else";
  EXPECT_EQ(text.find("<compile error>"), std::string::npos);
  EXPECT_EQ(text.find("<context error>"), std::string::npos);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
