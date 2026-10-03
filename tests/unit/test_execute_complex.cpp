// Ported from ctang's test-tangLanguageExecuteComplex.cpp onto lang-tang's API.
//
// Every test of that file is here under its original name, or is accounted for
// in documentation/design.md ("The ported execution tests"). See
// test_execute_simple.cpp for what changed in the porting.

#include "exec_harness.h"
#include "test_helpers.h"

#include <cinttypes>
#include <cstdint>
#include <string>

using namespace std;

using tt::expect_boolean;
using tt::expect_error;
using tt::expect_integer;
using tt::expect_null;
using tt::expect_output;
using tt::expect_string;

#define TEST_REUSABLE_PROGRAM(code, mode) \
  tt::Compiled compiled_(code, mode); \
  ASSERT_TRUE(compiled_.ok()) << (code); \
  GLTANG_Program * program = compiled_.program;

#define TEST_REUSABLE_PROGRAM_TEARDOWN()

#define TEST_CONTEXT_SETUP() \
  tt::Context context_(program); \
  ASSERT_TRUE(context_.ok()); \
  tt::Context * context = &context_;

#define TEST_CONTEXT_TEARDOWN()

#define TEST_PROGRAM_SETUP(code) \
  TEST_REUSABLE_PROGRAM(code, tt::Mode::Script); \
  TEST_CONTEXT_SETUP(); \
  ASSERT_TRUE(context->execute()); \
  ASSERT_TRUE(context->ok());

#define TEST_PROGRAM_SETUP_NO_RUN(code) \
  TEST_REUSABLE_PROGRAM(code, tt::Mode::Script); \
  TEST_CONTEXT_SETUP();

#define TEST_PROGRAM_TEARDOWN()

#define TEST_TEMPLATE_SETUP(code) \
  TEST_REUSABLE_PROGRAM(code, tt::Mode::Template); \
  TEST_CONTEXT_SETUP(); \
  ASSERT_TRUE(context->execute()); \
  ASSERT_TRUE(context->ok());

#define TEST_TEMPLATE_SETUP_NO_RUN(code) \
  TEST_REUSABLE_PROGRAM(code, tt::Mode::Template); \
  TEST_CONTEXT_SETUP();

TEST(ControlFlow, IF) {
  {
    // True condition with else branch.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      if (true) {
        print("true");
      } else {
        print("false");
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start true end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // False condition with else branch.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      if (false) {
        print("true");
      } else {
        print("false");
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start false end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // True condition without else branch.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      if (true) {
        print("true");
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start true end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // False condition without else branch.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      if (false) {
        print("true");
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start  end");
    TEST_PROGRAM_TEARDOWN();
  }
}

TEST(ControlFlow, While) {
  {
    // True condition.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      i = 0;
      while (i < 3) {
        print(i);
        i = i + 1;
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 012 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // False condition.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      i = 3;
      while (i < 3) {
        print(i);
        i = i + 1;
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start  end");
    TEST_PROGRAM_TEARDOWN();
  }
}

TEST(ControlFlow, DoWhile) {
  {
    // True condition.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      i = 0;
      do {
        print(i);
        i = i + 1;
      } while (i < 3);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 012 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // False condition.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      i = 3;
      do {
        print(i);
        i = i + 1;
      } while (i < 3);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 3 end");
    TEST_PROGRAM_TEARDOWN();
  }
}

TEST(ControlFlow, For) {
  {
    // True condition.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      for (i = 0; i < 3; i = i + 1) {
        print(i);
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 012 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // False condition.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      for (i = 3; i < 3; i = i + 1) {
        print(i);
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start  end");
    TEST_PROGRAM_TEARDOWN();
  }
}

TEST(ControlFlow, RangedFor) {
  {
    // Non-empty container.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      for (i : [0, 1, 2]) {
        print(i);
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 012 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Empty container.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      for (i : []) {
        print(i);
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start  end");
    TEST_PROGRAM_TEARDOWN();
  }
}

TEST(ControlFlow, RangedForBindsTheElement) {
  {
    // The loop variable is bound to the element, not to a copy of it, so
    // writing through it reaches the container. Arrays are reference types
    // everywhere else in the language - `b = a` and passing to a function
    // both share - and this binding is not an exception.
    //
    // The two engines disagreed here for the life of the library: the
    // bytecode interpreter emitted ADOPT, which deep copies, while the
    // x86_64 compiler asked for the same thing and never got it, because it
    // read the one-byte is_temporary and is_singleton flags as 64-bit words.
    TEST_PROGRAM_SETUP(R"(
      a = [[1], [2]];
      for (x : a) {
        x[0] = 9;
      }
      print(a[0][0]);
      print(a[1][0]);
    )");
    ASSERT_STREQ(context->raw().c_str(), "99");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Rebinding the loop variable is not mutation, and must not reach the
    // array. This is the line between the two, and it is why clearing
    // is_temporary is not the same as copying.
    TEST_PROGRAM_SETUP(R"(
      a = [1, 2];
      for (x : a) {
        x = 99;
      }
      print(a[0]);
      print(a[1]);
    )");
    ASSERT_STREQ(context->raw().c_str(), "12");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // A map element is shared the same way.
    TEST_PROGRAM_SETUP(R"(
      a = [{k: 1}];
      for (x : a) {
        x["k"] = 9;
      }
      print(a[0]["k"]);
    )");
    ASSERT_STREQ(context->raw().c_str(), "9");
    TEST_PROGRAM_TEARDOWN();
  }
}

TEST(Assignment, ContainersAreShared) {
  {
    // Assignment does not copy: `b = a` gives both names the same array, in
    // both engines. This is the language as it stands rather than a
    // conclusion anyone reached - see the comments in
    // gta_ast_node_assign_compile_to_bytecode() and its x86_64 counterpart -
    // and it is pinned here so that changing it has to be deliberate.
    TEST_PROGRAM_SETUP(R"(
      a = [1];
      b = a;
      b[0] = 9;
      print(a[0]);
    )");
    ASSERT_STREQ(context->raw().c_str(), "9");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // A map is shared the same way.
    TEST_PROGRAM_SETUP(R"(
      a = {k: 1};
      b = a;
      b["k"] = 9;
      print(a["k"]);
    )");
    ASSERT_STREQ(context->raw().c_str(), "9");
    TEST_PROGRAM_TEARDOWN();
  }
}

TEST(ControlFlow, Break) {
  {
    // Break in a while loop.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      i = 0;
      while (i < 4) {
        i = i + 1;
        if (i == 3) {
          break;
        }
        print(i);
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 12 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Break in a do..while loop.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      i = 0;
      do {
        i = i + 1;
        if (i == 3) {
          break;
        }
        print(i);
      } while (i < 4);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 12 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Break in a for loop.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      for (i = 0; i < 4; i = i + 1) {
        if (i == 2) {
          break;
        }
        print(i);
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 01 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Break outside of a control flow structure.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      break;
      print(" end");
    )");
    ASSERT_TRUE(context->ok());
    ASSERT_TRUE(context->is_null());
    ASSERT_STREQ(context->raw().c_str(), "start ");
    TEST_PROGRAM_TEARDOWN();
  }
}

TEST(ControlFlow, Continue) {
  {
    // Continue in a while loop.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      i = 0;
      while (i < 3) {
        i = i + 1;
        if (i == 2) {
          continue;
        }
        print(i);
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 13 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Continue in a do..while loop.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      i = 0;
      do {
        i = i + 1;
        if (i == 2) {
          continue;
        }
        print(i);
      } while (i < 3);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 13 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Continue in a for loop.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      for (i = 0; i < 3; i = i + 1) {
        if (i == 1) {
          continue;
        }
        print(i);
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 02 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Continue outside of a control flow structure.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      continue;
      print(" end");
    )");
    ASSERT_TRUE(context->ok());
    ASSERT_TRUE(context->is_null());
    ASSERT_STREQ(context->raw().c_str(), "start ");
    TEST_PROGRAM_TEARDOWN();
  }
}

TEST(Function, Simple) {
  {
    // Function with no arguments.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      function foo() {
        print("foo");
      }
      foo();
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start foo end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Function with arguments.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      function foo(a, b) {
        print(a + b);
      }
      foo(1, 2);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 3 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Function with return value.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      function foo(a, b) {
        return a + b;
      }
      print(foo(1, 2));
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 3 end");
    TEST_PROGRAM_TEARDOWN();
  }
}


TEST(ControlFlowEdgeCases, Break) {
  {
    // Break in global context.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      break;
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start ");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Break in function.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      function foo() {
        print("foo");
        break;
        print("bar");
      }
      foo();
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start foo end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Multiple breaks.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      i = 0;
      while (i < 3) {
        if (i == 2) {
          break;
        }
        print(i);
        i = i + 1;
      }
      i = 0;
      while (i < 3) {
        if (i == 2) {
          break;
        }
        print("a");
        i = i + 1;
        break;
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 01a end");
    TEST_PROGRAM_TEARDOWN();
  }
}


TEST(ControlFlowEdgeCases, Continue) {
  {
    // Continue in global context.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      continue;
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start ");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Continue in function.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      function foo() {
        print("foo");
        continue;
        print("bar");
      }
      foo();
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start foo end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Multiple continues.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      i = 0;
      while (i < 3) {
        i = i + 1;
        if (i == 2) {
          continue;
        }
        print(i);
      }
      i = 0;
      while (i < 3) {
        i = i + 1;
        if (i == 2) {
          continue;
        }
        print("a");
        continue;
      }
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 13aa end");
    TEST_PROGRAM_TEARDOWN();
  }
}


TEST(VariableScope, Global) {
  {
    // Local variable in a function.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      a = 1;
      function foo() {
        a = 2;
        print(a);
      }
      print(a);
      foo();
      print(a);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 121 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Global variable in a function, assignment separate.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      a = 1;
      function foo() {
        global a;
        print(a);
        a = 2;
        print(a);
      }
      print(a);
      foo();
      print(a);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 1122 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Global variable in a function, assignment combined.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      a = 1;
      function foo() {
        global a = 2;
        print(a);
      }
      print(a);
      foo();
      print(a);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 122 end");
    TEST_PROGRAM_TEARDOWN();
  }
}


TEST(NativeFunction, Library) {
  // ctang's three other cases here call native function values supplied by the
  // host's library: a function with no arguments, one with two, and one bound
  // to an object. A native function value is story 10's (it is the library
  // registry's), so those three are deferred and named in design.md.
  {
    // Function not found. Does not crash.
    TEST_PROGRAM_SETUP_NO_RUN(R"(
      use a;
      print("start ");
      print(a());
      print(" end");
    )");
    ASSERT_TRUE(context->execute());
    ASSERT_TRUE(context->ok());
    ASSERT_STREQ(context->raw().c_str(), "start  end");
    TEST_PROGRAM_TEARDOWN();
  }
}


TEST(Attributes, String) {
  {
    // Length (in graphemes).
    // The long string of hex values is a UTF-8 encoding of the Scottish Flag.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      print("$\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xB3\xF3\xA0\x81\xA3\xF3\xA0\x81\xB4\xF3\xA0\x81\xBF.".length);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 3 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Length (in bytes).
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      print("$\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xB3\xF3\xA0\x81\xA3\xF3\xA0\x81\xB4\xF3\xA0\x81\xBF.".byte_length);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 30 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // HTML encoding.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      print("a&b".html);
      print(" end");
    )");
    ASSERT_EQ(context->rendered(), "start a&amp;b end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Undo the HTML encoding.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      print("a&b\"'".html.raw);
      print(" end");
    )");
    ASSERT_EQ(context->rendered(), "start a&b\"' end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Double-encoding.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      print("a&b".html.render.html);
      print(" end");
    )");
    ASSERT_EQ(context->rendered(), "start a&amp;amp;b end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // HTML attribute encoding.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      print("a&b\"'".html_attribute);
      print(" end");
    )");
    ASSERT_EQ(context->rendered(), "start a&amp;b&quot;&#39; end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Percent encoding.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      print("a & b".percent);
      print(" end");
    )");
    ASSERT_EQ(context->rendered(), "start a+%26+b end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // JavaScript encoding.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      print("a&b\"'\n".javascript);
      print(" end");
    )");
    ASSERT_EQ(context->rendered(), "start a\\u0026b\\\"\\'\\n end");
    TEST_PROGRAM_TEARDOWN();
  }
}


TEST(Attributes, Array) {
  {
    // size (empty array).
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      print([].size);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 0 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // size (non-empty array).
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      print([1, 2, 3].size);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 3 end");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // size (sliced array).
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      a = [1, 2, 3];
      print(a[-2:].size);
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 2 end");
    TEST_PROGRAM_TEARDOWN();
  }
}

TEST(Recursion, Fibonacci) {
  {
    // Fibonacci sequence.
    TEST_PROGRAM_SETUP(R"(
      print("start ");
      function fib(n) {
        if (n <= 0) {
          return 0;
        }
        else if (n <= 2) {
          return 1;
        }
        return fib(n - 1) + fib(n - 2);
      }
      print(fib(10));
      print(" end");
    )");
    ASSERT_STREQ(context->raw().c_str(), "start 55 end");
    TEST_PROGRAM_TEARDOWN();
  }
}

TEST(Assignment, StoredValueIsNotTemporary) {
  // The arithmetic operators have an in-place fast path: when an operand is
  // still marked temporary they mutate it rather than allocating a result.
  // A value that has been stored in a variable must therefore not be marked
  // temporary, or the next expression that reads the variable rewrites it.
  //
  // The bytecode compiler emitted no SET_NOT_TEMP before its POKE, so `a` below
  // came back as 9 instead of 6. The x86_64 path was always correct, which is
  // why this file must keep asserting under both engines - it is run twice,
  // once with TANG_DISABLE_BINARY and once with TANG_DISABLE_BYTECODE, and a
  // single-engine assertion would have passed throughout.
  {
    // Reading a self-assigned variable must not modify it.
    TEST_PROGRAM_SETUP(R"(
      a = 5;
      a = a + 1;
      b = a + 3;
      print(a); print(","); print(b);
    )");
    ASSERT_STREQ(context->raw().c_str(), "6,9");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // The step is the constant, so a wrong answer here is not off-by-one.
    TEST_PROGRAM_SETUP(R"(
      a = 5;
      a = a * 2;
      b = a + 3;
      print(a); print(","); print(b);
    )");
    ASSERT_STREQ(context->raw().c_str(), "10,13");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // The case that makes it matter: `i = i + 1` advances every loop, so a
    // body that reads the counter arithmetically used to corrupt it. This
    // printed "0 2 " before the fix.
    TEST_PROGRAM_SETUP(R"(
      for (i = 0; i < 3; i = i + 1) {
        x = i + 1;
        print(i); print(" ");
      }
    )");
    ASSERT_STREQ(context->raw().c_str(), "0 1 2 ");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Same defect, opposite sign: this one did not terminate at all, because
    // the body decremented the counter by exactly what the loop added.
    TEST_PROGRAM_SETUP(R"(
      for (i = 0; i < 4; i = i + 1) {
        x = i - 1;
        print(i); print(" ");
      }
    )");
    ASSERT_STREQ(context->raw().c_str(), "0 1 2 3 ");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // while and do-while advance by the same self-assignment.
    TEST_PROGRAM_SETUP(R"(
      i = 0;
      while (i < 3) {
        x = i + 1;
        print(i); print(" ");
        i = i + 1;
      }
    )");
    ASSERT_STREQ(context->raw().c_str(), "0 1 2 ");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    TEST_PROGRAM_SETUP(R"(
      i = 0;
      do {
        x = i + 1;
        print(i); print(" ");
        i = i + 1;
      } while (i < 3);
    )");
    ASSERT_STREQ(context->raw().c_str(), "0 1 2 ");
    TEST_PROGRAM_TEARDOWN();
  }
  {
    // Assigning a variable to another must still not alias a scalar.
    TEST_PROGRAM_SETUP(R"(
      a = 5;
      b = a;
      b = b + 1;
      print(a); print(","); print(b);
    )");
    ASSERT_STREQ(context->raw().c_str(), "5,6");
    TEST_PROGRAM_TEARDOWN();
  }
}


TEST(Execute, Template) {
  {
    // Fibonacci sequence.
    TEST_TEMPLATE_SETUP(R"(<%
      function fib(n) {
        if (n <= 0) {
          return 0;
        }
        else if (n <= 2) {
          return 1;
        }
        return fib(n - 1) + fib(n - 2);
      }
      num = fib(10);
    %>start <%= num %> end)");
    ASSERT_STREQ(context->raw().c_str(), "start 55 end");
    TEST_PROGRAM_TEARDOWN();
  }
}


// Run a source that must yield an integer, and say which integer.



// A local is addressed as stack[fp + position], and the bytecode engine never
// made the slots past the arguments exist.  They were above the top of the
// stack, which is where the next push goes, so a local and an expression
// temporary shared a slot; and reading one that had never been assigned read
// the vector's spare capacity, found a zero, and dereferenced it as a value.
// The x86-64 engine reserves the frame and fills it with null, and disagreed.
TEST(Function, ALocalHasASlotOfItsOwn) {
  // A temporary deeper than one slot used to land on the first local.
  //
  // Ported with `return`: ctang answers a function that falls off its end with
  // the value of its last statement, and the reference says that is null
  // (sections 5.8 and 7), which is what lang-tang does. Ledger row D-010, and
  // Function.FallingOffTheEndReturnsNull below.
  expect_integer("function f() { z = 5; w = ((1 + 2) + 3); return z; } f();", 5);
  expect_integer("function f() { z = 5; w = ((1 + 2) + 3); return w; } f();", 6);
  expect_integer("function f(a, b) { c = a + b; d = ((c * 2) + 1); return c; } f(3, 4);", 7);
  expect_integer("function f(a, b) { c = a + b; d = ((c * 2) + 1); return d; } f(3, 4);", 15);
  // Section 6: reading a name that was never assigned yields null, and that
  // has to be the null value rather than a null pointer - so something has to
  // use it.
  expect_null("function f() { z; } f();");
  expect_boolean("function f() { z; } (!f());", true);
  expect_null("function f() { return z; } f();");
  expect_null("function f(a) { z; } f(1);");
  // The frame is given back, so a second call starts from null again.
  expect_integer("function f() { if (z) { return 1; } z = 2; return 0; } f(); f();", 0);
}


// Language reference 5.8 and 7: "falling off the end returns null". ctang
// returns the last statement's value instead (and, after an `if` or a loop, the
// condition it left on its stack); the divergence is ledger row D-010.
TEST(Function, FallingOffTheEndReturnsNull) {
  expect_null("function f() { 5; } f();");
  expect_null("function f() { 5; 6; } f();");
  expect_null("function f() { x = 3; } f();");
  expect_null("function f() { if (true) { 1; } } f();");
  expect_null("function f() { if (false) { 1; } } f();");
  expect_null("function f() { while (false) { } } f();");
  expect_null("function f() { } f();");
  expect_null("function f() { return; } f();");
  expect_integer("function f() { 5; return 6; } f();", 6);
}


// The x86-64 caller walked the arguments backwards while walking the slots it
// wrote them into forwards, so every parameter was bound to the wrong
// argument: `f(1, 2, 3)` bound a to 3 and c to 1.  Silent wrong answers, not
// a crash, under the engine that runs by default, and the bytecode engine
// disagreed with it.
//
// Each parameter is read on its own so that a failure says which slot moved.
TEST(Function, EachParameterGetsItsOwnArgument) {
  expect_integer("function f(a) { return a; } f(7);", 7);
  expect_integer("function f(a, b) { return a; } f(1, 2);", 1);
  expect_integer("function f(a, b) { return b; } f(1, 2);", 2);
  expect_integer("function f(a, b, c) { return a; } f(1, 2, 3);", 1);
  expect_integer("function f(a, b, c) { return b; } f(1, 2, 3);", 2);
  expect_integer("function f(a, b, c) { return c; } f(1, 2, 3);", 3);
  expect_integer("function f(a, b, c, d) { return a; } f(1, 2, 3, 4);", 1);
  expect_integer("function f(a, b, c, d) { return d; } f(1, 2, 3, 4);", 4);
  // A weighted sum catches any permutation at once.
  expect_integer("function f(a, b, c, d, e) { return a + b * 2 + c * 4 + d * 8 + e * 16; } f(1, 2, 3, 4, 5);", 129);
  // Through a nested call, so that one frame's layout does not cover another's.
  expect_integer("function g(x, y) { return x - y; } function f(a, b) { return g(a, b); } f(10, 3);", 7);
}


// A repeated parameter name gives two parameters one slot, so the scope holds
// fewer variables than the function has parameters - and the x86-64 prologue
// works that difference out in size_t.  It wrapped, and the loop that fills
// the locals with null then ran about 2^64 times, which is what "hangs the
// compiler" was.
TEST(Function, ARepeatedParameterNameIsRejected) {
  const char * cases[] = {
    "function f(a, a) {}",
    "function f(a, b, a) {} 1;",
    "function f(a, a, a) {} 1;",
  };
  for (const char * code : cases) {
    { Balance balance; tt::Compiled refused(code); ASSERT_FALSE(refused.ok()) << code; } /* nothing may be left over */
  }
  // Distinct names in the same positions still compile.
  expect_integer("function f(a, b) { return b; } f(1, 2);", 2);
}


// Reporting one of these used to abort the process.  gta_ast_node_destroy()
// folded its is_singleton test into the condition that picks the destructor,
// which sent singletons to the fallback - and the fallback is a free(), not a
// no-op.  So the check that exists to protect a singleton was what freed it,
// and every error reported by returning a parse-error singleton ended in
// "free(): invalid pointer".
TEST(Function, RedeclarationAndForwardCallsFailCompilation) {
  const char * cases[] = {
    "function f() {} function f() {}",
    "x = 1; function x() {}",
    "foo(); function foo() {}",
  };
  for (const char * code : cases) {
    { Balance balance; tt::Compiled refused(code); ASSERT_FALSE(refused.ok()) << code; } /* nothing may be left over */
  }
  // Two different functions, and a call after the declaration, still compile.
  expect_integer("function f() { return 1; } function g() { return 2; } f() + g();", 3);
}



// Run a source that must yield an error, and say which message.

// A call is an expression and has to leave exactly one value behind, like
// every other one.  The bytecode engine's refusals set context->result and
// left nothing, so the stack was short by one: the POP after the statement
// took the value underneath, context->result was overwritten at the end by
// whatever was then on top, and `f = 3; f();` came out as 3 - no error, while
// the x86-64 engine said `Invalid function call` for the same source.
TEST(Function, CallingSomethingThatIsNotAFunctionIsAnError) {
  expect_error("f = 3; f();", "Error: Invalid function call");
  expect_error("(1)(2);", "Error: Invalid function call");
  expect_error("f = \"x\"; f();", "Error: Invalid function call");
  expect_error("f = [1]; f();", "Error: Invalid function call");
  expect_error("null();", "Error: Invalid function call");
  // The statement after it still runs, and gets the stack it expects.
  expect_integer("f = 3; f(); 7;", 7);
  expect_integer("f = 3; f(); f(); f(); 7;", 7);
}


TEST(Function, TheWrongNumberOfArgumentsIsAnError) {
  expect_error("function f(a) { return a; } f();", "Error: Argument Count Mismatch");
  expect_error("function f(a) { return a; } f(1, 2);", "Error: Argument Count Mismatch");
  expect_error("function f() { return 1; } f(1);", "Error: Argument Count Mismatch");
  expect_integer("function f(a) { return a; } f(1, 2); 7;", 7);
}



// Unbounded recursion used to run the process's own stack out under the
// x86-64 engine, which calls compiled functions with real `call`
// instructions - a template could take the host down.  Both engines now share
// one limit, carried on the execution context.
TEST(Function, RecursionIsBounded) {
  expect_error("function f(n) { return f(n + 1); } f(0);",
    "Error: Recursion Limit Exceeded");
  expect_error("function f() { return f(); } f();",
    "Error: Recursion Limit Exceeded");
  // Indirect recursion counts the same way.
  expect_error("function g(n) { return g(n); } function f(n) { return g(n); } f(1);",
    "Error: Recursion Limit Exceeded");
}


// The limit must not leak between calls: a call that hits it has to give back
// everything it counted, or a later call inherits the depth and fails too.
TEST(Function, TheCallDepthIsGivenBack) {
  const char * deep = "function d(n) { if (n <= 0) { return 0; } return d(n - 1); }";
  // Comfortably inside the default limit.
  expect_integer((std::string(deep) + " d(400);").c_str(), 0);
  // Past it, and then inside it again.
  expect_integer((std::string(deep) + " d(100000); d(400);").c_str(), 0);
  expect_integer((std::string(deep) + " d(100000); d(100000); d(400);").c_str(), 0);
  // And in a loop, where the count is spent and recovered repeatedly.
  expect_integer((std::string(deep) + " x = 1; for (i : [1, 2, 3]) { x = d(400); } x;").c_str(), 0);
}


// A host that knows its own stack can move the limit, and zero removes it.
TEST(Function, TheCallDepthLimitIsTheHostsToSet) {
  const char * code = "function d(n) { if (n <= 0) { return 0; } return d(n - 1); } d(40);";
  {
    // The default is ctang's 512 (the host's guest-depth budget is that and the
    // program's own frame).
    tt::Compiled compiled(code);
    ASSERT_TRUE(compiled.ok());
    tt::Config config;
    ASSERT_EQ(config.calls, 512u);
    config.calls = 10;
    tt::Context context(compiled.program, config);
    ASSERT_TRUE(context.ok());
    ASSERT_TRUE(context.execute());
    ASSERT_TRUE(context.is_error());
    ASSERT_EQ(context.error_kind(), GLTANG_ERROR_RECURSION_LIMIT);
  }
  {
    tt::Compiled compiled(code);
    ASSERT_TRUE(compiled.ok());
    tt::Config config;
    config.calls = 100;
    tt::Context context(compiled.program, config);
    ASSERT_TRUE(context.ok());
    ASSERT_TRUE(context.execute());
    ASSERT_FALSE(context.is_error());
    ASSERT_TRUE(context.is_integer());
  }
  {
    // Exactly at the limit: 10 nested calls are allowed, an 11th is not.
    tt::Config config;
    config.calls = 10;
    tt::Compiled nine("function d(n) { if (n <= 0) { return 0; } return d(n - 1); } d(9);");
    tt::Context at_limit(nine.program, config);
    ASSERT_TRUE(at_limit.execute());
    ASSERT_TRUE(at_limit.is_integer());
    tt::Compiled ten("function d(n) { if (n <= 0) { return 0; } return d(n - 1); } d(10);");
    tt::Context past_limit(ten.program, config);
    ASSERT_TRUE(past_limit.execute());
    ASSERT_TRUE(past_limit.is_error());
  }
  {
    // None at all: nothing counts, and the C stack is untouched because a
    // call is a frame on the guest stack.
    tt::Config config;
    config.calls = GRCORE_UNLIMITED;
    // A guest stack that moves on every push copies itself each time, so the
    // moving variant of the suite goes less deep.
    std::string depth = tt::moving_stack_requested() ? "3000" : "100000";
    tt::Compiled compiled("function d(n) { if (n <= 0) { return 0; } return d(n - 1); } d(" + depth + ");");
    tt::Context context(compiled.program, config);
    ASSERT_TRUE(context.execute());
    ASSERT_TRUE(context.is_integer());
    ASSERT_EQ(context.integer(), 0);
  }
}


int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
