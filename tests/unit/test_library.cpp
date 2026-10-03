// The library registry (story 10): what a host gives a program, how `use`
// finds it, and how a host function is called.
//
// Ported from ctang's test-tangLanguageLibrary.cpp (Library.Load, Math.Constants,
// Library.UseAs; Random.Random is in test_random.cpp), then the cases the
// spec's matrix adds (the three native function cases of ctang's
// NativeFunction.Library are in test_execute_complex.cpp): resolution order, laziness and shadowing,
// context injection, sealing, and the callback contract.

#include "exec_harness.h"

#include <atomic>
#include <thread>

using tt::Compiled;
using tt::Config;
using tt::Context;

namespace {

// ---------------------------------------------------------------------------
// Host functions
// ---------------------------------------------------------------------------

bool make_three(GLTANG_NativeCall * call, void *) {
  gltang_call_return_integer(call, 3);
  return true;
}

bool add_two(GLTANG_NativeCall * call, void *) {
  if (gltang_call_count(call) != 2) {
    gltang_call_return_error(call, GLTANG_ERROR_ARGUMENT_COUNT_MISMATCH);
    return true;
  }
  if (gltang_call_kind(call, 0) != GLTANG_KIND_INTEGER || gltang_call_kind(call, 1) != GLTANG_KIND_INTEGER) {
    gltang_call_return_error(call, GLTANG_ERROR_INVALID_FUNCTION_CALL);
    return true;
  }
  gltang_call_return_integer(call, gltang_call_integer(call, 0) + gltang_call_integer(call, 1));
  return true;
}

bool fails(GLTANG_NativeCall *, void *) {
  return false;
}

bool echoes(GLTANG_NativeCall * call, void *) {
  size_t length = 0;
  const char * text = gltang_call_text(call, 0, &length);
  if (!text) {
    gltang_call_return_error(call, GLTANG_ERROR_NOT_SUPPORTED);
    return true;
  }
  gltang_call_return_string(call, text, length, GLTANG_UNICODE_STRING_TYPE_HTML);
  return true;
}

bool returns_each_kind(GLTANG_NativeCall * call, void *) {
  switch (gltang_call_integer(call, 0)) {
    case 0: gltang_call_return_null(call); break;
    case 1: gltang_call_return_bool(call, true); break;
    case 2: gltang_call_return_integer(call, 1ll << 62); break;
    case 3: gltang_call_return_float(call, 2.5); break;
    case 4: gltang_call_return_string(call, "s", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED); break;
    case 5: gltang_call_return_string(call, "\xff", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED); break;
    default: gltang_call_return_error(call, GLTANG_ERROR_DIVIDE_BY_ZERO); break;
  }
  return true;
}

// A factory that counts its calls.
struct Counting {
  int calls = 0;
  bool available = true;
};

bool counting_factory(void * user, GLTANG_HostValue * out) {
  Counting * counting = static_cast<Counting *>(user);
  ++counting->calls;
  if (!counting->available) {
    return false;
  }
  out->kind = GLTANG_HOST_INTEGER;
  out->integer = counting->calls;
  return true;
}

GLTANG_Library * make_library(const char * name) {
  GLTANG_Library * library = nullptr;
  EXPECT_EQ(gltang_library_create(name, &library), GLTANG_OK);
  return library;
}

// A library holding one integer under `name`, used to tell layers apart.
GLTANG_Library * library_with_integer(const char * library_name, const char * member, int64_t value) {
  GLTANG_Library * library = make_library(library_name);
  EXPECT_EQ(gltang_library_add_integer(library, member, value), GLTANG_OK);
  return library;
}

}  // namespace

// ---------------------------------------------------------------------------
// ctang: Library.Load, Math.Constants, Library.UseAs
// ---------------------------------------------------------------------------

TEST(Library, Load) {
  {
    tt::Run run("use math; math;");
    ASSERT_EQ(run.context.kind(), GLTANG_KIND_LIBRARY);
    EXPECT_EQ(run.context.text(), "math");
  }
  {
    tt::Run run("use random; random;");
    ASSERT_EQ(run.context.kind(), GLTANG_KIND_LIBRARY);
    EXPECT_EQ(run.context.text(), "random");
  }
}

TEST(Math, Constants) {
  tt::Run run("use math; print(math.pi);");
  EXPECT_TRUE(run.context.is_null());
  EXPECT_EQ(run.context.raw(), "3.141593");
}

TEST(Library, UseAs) {
  {
    tt::Run run("use math as m; m;");
    ASSERT_EQ(run.context.kind(), GLTANG_KIND_LIBRARY);
    EXPECT_EQ(run.context.text(), "math");
  }
  {
    tt::Run run("use math as m; print(m.pi);");
    EXPECT_TRUE(run.context.is_null());
    EXPECT_EQ(run.context.raw(), "3.141593");
  }
  {
    tt::Run run("use math.pi as pi; print(pi);");
    EXPECT_TRUE(run.context.is_null());
    EXPECT_EQ(run.context.raw(), "3.141593");
  }
}

// ---------------------------------------------------------------------------
// Host functions (ctang's NativeFunction.Library is in test_execute_complex.cpp)
// ---------------------------------------------------------------------------

TEST(NativeFunction, AWrongArgumentCountIsAnErrorTheNativeReturnsAndTheProgramContinues) {
  Compiled compiled("use a; x = a(1); print(\"|\"); [x, a(1, 2)];");
  Context context(compiled.program);
  ASSERT_EQ(gltang_library_add_native(context.library(), "a", add_two, nullptr), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.describe(), "[Error: Argument Count Mismatch, 3]");
  EXPECT_EQ(context.raw(), "|");
}

TEST(NativeFunction, EveryKindOfAnswerArrivesAsThatKind) {
  Compiled compiled("use f; [f(0), f(1), f(2), f(3), f(4), f(5), f(6)];");
  Context context(compiled.program);
  ASSERT_EQ(gltang_library_add_native(context.library(), "f", returns_each_kind, nullptr), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.describe(), "[null, true, 4611686018427387904, 2.5, s, Error: Host function failed, Error: Divide by zero]");
}

TEST(NativeFunction, ReturningFalseIsTheHostFunctionFailedError) {
  Compiled compiled("use f; f();");
  Context context(compiled.program);
  ASSERT_EQ(gltang_library_add_native(context.library(), "f", fails, nullptr), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  ASSERT_TRUE(context.is_error());
  EXPECT_EQ(context.error_kind(), GLTANG_ERROR_HOST_FAILED);
  EXPECT_EQ(context.describe(), "Error: Host function failed");
}

TEST(NativeFunction, AStringKeepsItsEncodingThroughTheOutput) {
  Compiled compiled("use echo; print(echo(\"<i>\")); print(\"|\" + echo(\"<b>\"));");
  Context context(compiled.program);
  ASSERT_EQ(gltang_library_add_native(context.library(), "echo", echoes, nullptr), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.rendered(), "&lt;i&gt;|&lt;b&gt;");
  EXPECT_EQ(context.raw(), "<i>|<b>");
}

TEST(NativeFunction, ANativeCostsFuelLikeAnyCallAndMoreForABigAnswer) {
  auto cost = [](const char * source) {
    Compiled compiled(source);
    Context context(compiled.program);
    EXPECT_EQ(gltang_library_add_native(context.library(), "big", [](GLTANG_NativeCall * call, void *) {
      std::string text(64 * 1024, 'x');
      gltang_call_return_string(call, text.data(), text.size(), GLTANG_UNICODE_STRING_TYPE_TRUSTED);
      return true;
    }, nullptr), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_native(context.library(), "small", make_three, nullptr), GLTANG_OK);
    EXPECT_TRUE(context.execute());
    return grcore_context_fuel_used(context.context);
  };
  uint64_t small = cost("use small; use big; small();");
  uint64_t big = cost("use small; use big; big();");
  EXPECT_GE(big, small + 1000) << "64 KiB at one unit per 64 bytes is 1,024 units";
}

TEST(NativeFunction, ANativeValueIsAFunctionAndCanBeStored) {
  Compiled compiled("use a; x = a; [x(), x == x];");
  Context context(compiled.program);
  ASSERT_EQ(gltang_library_add_native(context.library(), "a", make_three, nullptr), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.describe(), "[3, Error: Not supported]");
}

// ---------------------------------------------------------------------------
// The built-ins
// ---------------------------------------------------------------------------

TEST(Builtins, MathAndItsAbsentMembers) {
  tt::Run run("use math; use math.pi as pi; [math.pi, pi, math.tau, math.pi = 3, math.sqrt];");
  EXPECT_EQ(run.context.describe(), "[3.141593, 3.141593, Error: Not implemented, Error: Not supported, Error: Not implemented]");
}

TEST(Builtins, ALibraryIsTruthyPrintsNothingAndIsNotSupportedForTheOperators) {
  tt::Run run("use math as m; print(m); [!m, m ? 1 : 2, m && true, m + 1, 1 + m, -m, m * 2, m < 1, m == m, m != 1, m[0], m[0:1], m as int, m as float, m as bool, m as string, m(), m(1), m.pi()];");
  EXPECT_EQ(run.context.raw(), "");
  EXPECT_EQ(run.context.describe(),
      "[false, 1, true, Error: Not supported, Error: Not supported, Error: Not supported, Error: Not supported, "
      "Error: Not supported, Error: Not supported, Error: Not supported, Error: Not supported, Error: Not supported, "
      "Error: Not supported, Error: Not supported, Error: Not supported, Error: Not supported, "
      "Error: Invalid function call, Error: Invalid function call, Error: Invalid function call]");
}

TEST(Builtins, ALibraryInsideAContainerShowsItsName) {
  tt::Run run("use math; use random; [\"\" + [math], [math, random] as string, [math] as string];");
  EXPECT_EQ(run.context.describe(), "[[Library: math], [Library: math, Library: random], [Library: math]]");
}

TEST(Builtins, ALibraryAddedByAStringConcatenationIsNotSupported) {
  tt::Run run("use math; x = \"a\" + math; y = math + \"a\"; print(x); print(y); [x, y];");
  EXPECT_EQ(run.context.raw(), "");
  EXPECT_EQ(run.context.describe(), "[Error: Not supported, Error: Not supported]");
}

TEST(Builtins, AssigningToTheNameReplacesTheVariableOnly) {
  tt::Run run("use math; math = 5; print(math); use math; print(math.pi);");
  EXPECT_EQ(run.context.raw(), "53.141593");
}

// ---------------------------------------------------------------------------
// Resolution (reference 9.1)
// ---------------------------------------------------------------------------

TEST(Resolution, TheExecutionLayerWinsThenTheProgramThenTheBuiltin) {
  const char * source = "use math; math.pi;";
  Compiled compiled(source);
  ASSERT_TRUE(compiled.ok());
  // The program's layer.
  GLTANG_Library * program_root = make_library(nullptr);
  GLTANG_Library * program_math = library_with_integer("math", "pi", 2);
  ASSERT_EQ(gltang_library_add_library(program_root, program_math), GLTANG_OK);
  ASSERT_EQ(gltang_program_set_libraries(compiled.program, program_root), GLTANG_OK);
  {
    // Program over built-in.
    Context context(compiled.program);
    ASSERT_TRUE(context.execute());
    EXPECT_EQ(context.integer(), 2);
  }
  {
    // Execution over program.
    Context context(compiled.program);
    GLTANG_Library * root = context.library();
    GLTANG_Library * exec_math = library_with_integer("math", "pi", 1);
    ASSERT_EQ(gltang_library_add_library(root, exec_math), GLTANG_OK);
    gltang_library_release(exec_math);
    ASSERT_TRUE(context.execute());
    EXPECT_EQ(context.integer(), 1);
  }
  {
    // Neither layer has `math` as a member: the built-in answers.
    Compiled other(source);
    Context context(other.program);
    context.add_library("unrelated", tt::Host::integer(9));
    ASSERT_TRUE(context.execute());
    EXPECT_NEAR(context.number(), 3.14159265, 1e-6);
  }
  gltang_library_release(program_math);
  gltang_library_release(program_root);
}

TEST(Resolution, ANameNothingProvidesIsNullAndAPathThroughItIsNull) {
  tt::Run run("use nope; use nope.x as y; use nope.x.z as w; [nope, y, w];");
  EXPECT_EQ(run.context.describe(), "[null, null, null]");
}

TEST(Resolution, APathWalksMembersWithTheAttributeRule) {
  Compiled compiled("use a.b.c as d; use a.b.missing as m; use a.b as ab; [d, m, ab, ab.c];");
  Context context(compiled.program);
  GLTANG_Library * a = make_library("a");
  GLTANG_Library * b = library_with_integer("b", "c", 7);
  ASSERT_EQ(gltang_library_add_library(a, b), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_library(context.library(), a), GLTANG_OK);
  gltang_library_release(b);
  gltang_library_release(a);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.describe(), "[7, Error: Not implemented, Library: b, 7]");
}

TEST(Resolution, AProgramLayerCanOnlyBeSetWhileTheProgramIsUnshared) {
  Compiled compiled("1;");
  ASSERT_TRUE(compiled.ok());
  GLTANG_Library * library = make_library(nullptr);
  ASSERT_EQ(gltang_program_set_libraries(compiled.program, library), GLTANG_OK);
  gltang_program_retain(compiled.program);
  EXPECT_EQ(gltang_program_set_libraries(compiled.program, nullptr), GLTANG_ERR_INVALID) << "a second reference: the program is shared";
  gltang_program_release(compiled.program);
  EXPECT_EQ(gltang_program_set_libraries(compiled.program, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_program_set_libraries(nullptr, library), GLTANG_ERR_INVALID);
  gltang_library_release(library);
}

// ---------------------------------------------------------------------------
// Laziness and shadowing
// ---------------------------------------------------------------------------

TEST(Factory, IsNeverCalledAtRegistrationOrWithoutAUse) {
  Counting counting;
  {
    Compiled compiled("1 + 1;");
    Context context(compiled.program);
    ASSERT_EQ(gltang_library_add_factory(context.library(), "user", counting_factory, &counting), GLTANG_OK);
    ASSERT_TRUE(context.execute());
  }
  EXPECT_EQ(counting.calls, 0);
}

TEST(Factory, IsCalledOncePerExecutedUse) {
  Counting counting;
  Compiled compiled("use user; use user as again; if (false) { use user as never; } [user, again];");
  Context context(compiled.program);
  ASSERT_EQ(gltang_library_add_factory(context.library(), "user", counting_factory, &counting), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(counting.calls, 2);
  EXPECT_EQ(context.describe(), "[1, 2]");
}

TEST(Factory, ReturningFalseBindsNull) {
  Counting counting;
  counting.available = false;
  tt::Run run("use user; [user];");
  Compiled compiled("use user; [user];");
  Context context(compiled.program);
  ASSERT_EQ(gltang_library_add_factory(context.library(), "user", counting_factory, &counting), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.describe(), "[null]");
  EXPECT_EQ(counting.calls, 1);
}

TEST(Factory, AssignmentAfterUseReplacesTheVariableAndNeverTheLibrary) {
  Counting counting;
  Compiled compiled("use user; user = 42; use user as second; [user, second];");
  Context context(compiled.program);
  ASSERT_EQ(gltang_library_add_factory(context.library(), "user", counting_factory, &counting), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.describe(), "[42, 2]");
}

TEST(Factory, MakesAScalarOfAnyHostKind) {
  struct Kinds {
    static bool as_string(void *, GLTANG_HostValue * out) {
      out->kind = GLTANG_HOST_STRING;
      out->text = "a<b";
      out->length = 3;
      out->encoding = GLTANG_UNICODE_STRING_TYPE_HTML;
      return true;
    }
    static bool as_float(void *, GLTANG_HostValue * out) {
      out->kind = GLTANG_HOST_FLOAT;
      out->number = 1.5;
      return true;
    }
    static bool as_bool(void *, GLTANG_HostValue * out) {
      out->kind = GLTANG_HOST_BOOL;
      out->boolean = true;
      return true;
    }
    static bool as_null(void *, GLTANG_HostValue * out) {
      out->kind = GLTANG_HOST_NULL;
      return true;
    }
  };
  Compiled compiled("use s; use f; use b; use n; print(s); [s, f, b, n];");
  Context context(compiled.program);
  GLTANG_Library * library = context.library();
  ASSERT_EQ(gltang_library_add_factory(library, "s", Kinds::as_string, nullptr), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_factory(library, "f", Kinds::as_float, nullptr), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_factory(library, "b", Kinds::as_bool, nullptr), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_factory(library, "n", Kinds::as_null, nullptr), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.rendered(), "a&lt;b");
  EXPECT_EQ(context.describe(), "[a<b, 1.5, true, null]");
}

// ---------------------------------------------------------------------------
// Context injection
// ---------------------------------------------------------------------------

TEST(Injection, OneProgramTwoContextsTwoUsers) {
  Compiled compiled("use user; print(\"hello \" + user.name);");
  ASSERT_TRUE(compiled.ok());
  std::string outputs[2];
  const char * names[2] = {"ann", "bob"};
  for (int i = 0; i < 2; ++i) {
    GLTANG_Library * user = make_library("user");
    ASSERT_EQ(gltang_library_add_string(user, "name", names[i], strlen(names[i]), GLTANG_UNICODE_STRING_TYPE_TRUSTED), GLTANG_OK);
    Context context(compiled.program);
    ASSERT_EQ(gltang_library_add_library(context.library(), user), GLTANG_OK);
    gltang_library_release(user);
    ASSERT_TRUE(context.execute());
    outputs[i] = context.raw();
  }
  EXPECT_EQ(outputs[0], "hello ann");
  EXPECT_EQ(outputs[1], "hello bob");
}

TEST(Injection, ASharedLibraryIsReadByManyContextsOnManyThreads) {
  Compiled compiled("use shared; use shared.n as n; s = 0; for (i = 0; i < 50; i += 1) { s += shared.n + n; } print(s);");
  ASSERT_TRUE(compiled.ok());
  GLTANG_Library * root = make_library(nullptr);
  GLTANG_Library * shared = library_with_integer("shared", "n", 3);
  ASSERT_EQ(gltang_library_add_library(root, shared), GLTANG_OK);
  gltang_library_release(shared);
  std::atomic<int> good{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&]() {
      Context context(compiled.program);
      if (context.ok() && gltang_execution_set_libraries(context.execution, root) == GLTANG_OK) {
        context.attached = true;
        if (context.execute() && context.raw() == "300") {
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

// ---------------------------------------------------------------------------
// Sealing
// ---------------------------------------------------------------------------

TEST(Sealing, AnAttachedLibraryRefusesEveryMutation) {
  Compiled compiled("1;");
  Context context(compiled.program);
  GLTANG_Library * library = make_library("lib");
  EXPECT_FALSE(gltang_library_sealed(library));
  ASSERT_EQ(gltang_library_add_integer(library, "a", 1), GLTANG_OK);
  ASSERT_EQ(gltang_execution_set_libraries(context.execution, library), GLTANG_OK);
  EXPECT_TRUE(gltang_library_sealed(library));
  size_t count = gltang_library_count(library);
  EXPECT_EQ(gltang_library_add_null(library, "n"), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_bool(library, "b", true), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_integer(library, "i", 1), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_float(library, "f", 1.0), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_string(library, "s", "x", 1, GLTANG_UNICODE_STRING_TYPE_TRUSTED), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_native(library, "n2", make_three, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_factory(library, "f2", counting_factory, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_count(library), count) << "a refusal changes nothing";
  gltang_library_release(library);
}

TEST(Sealing, ALibraryAddedToAnotherIsSealedAndACycleCannotBeBuilt) {
  GLTANG_Library * parent = make_library("parent");
  GLTANG_Library * child = make_library("child");
  EXPECT_EQ(gltang_library_add_library(parent, parent), GLTANG_ERR_INVALID) << "itself";
  ASSERT_EQ(gltang_library_add_library(parent, child), GLTANG_OK);
  EXPECT_TRUE(gltang_library_sealed(child));
  EXPECT_FALSE(gltang_library_sealed(parent));
  EXPECT_EQ(gltang_library_add_library(child, parent), GLTANG_ERR_INVALID) << "the child is sealed, so the cycle is refused";
  EXPECT_EQ(gltang_library_count(child), 0u);
  gltang_library_release(child);
  gltang_library_release(parent);
}

TEST(Sealing, ABadNameADuplicateAndAnUnnamedChildAreRefused) {
  GLTANG_Library * library = make_library(nullptr);
  EXPECT_EQ(gltang_library_add_integer(library, "", 1), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_integer(library, "a.b", 1), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_integer(library, nullptr, 1), GLTANG_ERR_INVALID);
  ASSERT_EQ(gltang_library_add_integer(library, "a", 1), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_integer(library, "a", 2), GLTANG_ERR_INVALID);
  GLTANG_Library * unnamed = make_library(nullptr);
  EXPECT_EQ(gltang_library_add_library(library, unnamed), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_native(library, "n", nullptr, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_factory(library, "f", nullptr, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_template(library, "t", nullptr, 1, GLTANG_SCOPE_EMPTY), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_add_string(library, "s", nullptr, 3, GLTANG_UNICODE_STRING_TYPE_TRUSTED), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_count(library), 1u);
  GLTANG_Library * bad = nullptr;
  EXPECT_EQ(gltang_library_create("", &bad), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_create("a.b", &bad), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_create("x", nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(bad, nullptr);
  EXPECT_STREQ(gltang_library_name(library), nullptr);
  EXPECT_EQ(gltang_library_name(nullptr), nullptr);
  EXPECT_EQ(gltang_library_count(nullptr), 0u);
  EXPECT_FALSE(gltang_library_sealed(nullptr));
  gltang_library_release(unnamed);
  gltang_library_release(library);
  gltang_library_release(nullptr);
  EXPECT_EQ(gltang_library_retain(nullptr), nullptr);
}

TEST(Sealing, SettersAreRefusedOnceTheExecutionHasStarted) {
  Compiled compiled("1;");
  Context context(compiled.program);
  GLTANG_Library * library = make_library(nullptr);
  GLTANG_SeedSequence * seeds = nullptr;
  ASSERT_EQ(gltang_seeds_create(1, &seeds), GLTANG_OK);
  ASSERT_EQ(gltang_execution_set_libraries(context.execution, library), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(gltang_execution_set_libraries(context.execution, library), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_set_seeds(context.execution, seeds), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_set_name(context.execution, "x"), GLTANG_ERR_INVALID);
  gltang_seeds_destroy(seeds);
  gltang_library_release(library);
}

// ---------------------------------------------------------------------------
// The callback contract (AD-23)
// ---------------------------------------------------------------------------

namespace {

struct Reentry {
  GLTANG_Execution * execution = nullptr;
  GRCORE_Context * context = nullptr;
  GLTANG_Library * library = nullptr;
  GLTANG_SeedSequence * seeds = nullptr;
  GLTANG_Result set_libraries = GLTANG_OK;
  GLTANG_Result set_seeds = GLTANG_OK;
  GLTANG_Result set_name = GLTANG_OK;
  GRCORE_Result resumed = GRCORE_OK;
  GRCORE_Result ran = GRCORE_OK;
  bool called = false;
};

bool reenters(GLTANG_NativeCall * call, void * user) {
  Reentry * r = static_cast<Reentry *>(user);
  r->called = true;
  r->set_libraries = gltang_execution_set_libraries(r->execution, r->library);
  r->set_seeds = gltang_execution_set_seeds(r->execution, r->seeds);
  r->set_name = gltang_execution_set_name(r->execution, "inside");
  GRCORE_Outcome outcome;
  r->resumed = grcore_resume(r->context, &outcome);
  r->ran = grcore_run(r->context, gltang_execution_entry, r->execution, &outcome);
  gltang_call_return_integer(call, 5);
  return true;
}

}  // namespace

TEST(Callback, ANativeCannotRunTheContextOrChangeTheExecutionAndTheRunIsUnaffected) {
  Compiled compiled("use f; print(\"a\"); x = f(); print(x); print(\"b\");");
  Context context(compiled.program);
  Reentry reentry;
  reentry.execution = context.execution;
  reentry.context = context.context;
  reentry.library = make_library(nullptr);
  ASSERT_EQ(gltang_seeds_create(1, &reentry.seeds), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_native(context.library(), "f", reenters, &reentry), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_TRUE(reentry.called);
  EXPECT_EQ(reentry.set_libraries, GLTANG_ERR_INVALID);
  EXPECT_EQ(reentry.set_seeds, GLTANG_ERR_INVALID);
  EXPECT_EQ(reentry.set_name, GLTANG_ERR_INVALID);
  EXPECT_EQ(reentry.resumed, GRCORE_ERR_INVALID);
  EXPECT_EQ(reentry.ran, GRCORE_ERR_INVALID);
  EXPECT_EQ(context.raw(), "a5b");
  gltang_seeds_destroy(reentry.seeds);
  gltang_library_release(reentry.library);
}

TEST(Callback, AFactoryIsHeldToTheSameContract) {
  static Reentry reentry;
  reentry = Reentry();
  Compiled compiled("use g; print(g);");
  Context context(compiled.program);
  reentry.execution = context.execution;
  reentry.context = context.context;
  reentry.library = make_library(nullptr);
  ASSERT_EQ(gltang_library_add_factory(context.library(), "g", [](void * user, GLTANG_HostValue * out) {
    Reentry * r = static_cast<Reentry *>(user);
    r->called = true;
    r->set_libraries = gltang_execution_set_libraries(r->execution, r->library);
    GRCORE_Outcome outcome;
    r->resumed = grcore_resume(r->context, &outcome);
    out->kind = GLTANG_HOST_INTEGER;
    out->integer = 8;
    return true;
  }, &reentry), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_TRUE(reentry.called);
  EXPECT_EQ(reentry.set_libraries, GLTANG_ERR_INVALID);
  EXPECT_EQ(reentry.resumed, GRCORE_ERR_INVALID);
  EXPECT_EQ(context.raw(), "8");
  gltang_library_release(reentry.library);
}

TEST(Callback, TheCallObjectAnswersForAnyIndexAndForNull) {
  struct Probe {
    static bool run(GLTANG_NativeCall * call, void * user) {
      bool * ok = static_cast<bool *>(user);
      size_t length = 99;
      *ok = gltang_call_count(call) == 3 && gltang_call_kind(call, 0) == GLTANG_KIND_BOOL && gltang_call_bool(call, 0)
          && gltang_call_kind(call, 1) == GLTANG_KIND_FLOAT && gltang_call_float(call, 1) == 2.5
          && gltang_call_kind(call, 2) == GLTANG_KIND_STRING && std::string(gltang_call_text(call, 2, &length)) == "hi" && length == 2
          && gltang_call_kind(call, 3) == GLTANG_KIND_NULL && gltang_call_integer(call, 3) == 0
          && gltang_call_text(call, 0, &length) == nullptr && !gltang_call_bool(call, 9) && gltang_call_float(call, 0) == 0.0;
      gltang_call_return_null(nullptr);
      gltang_call_return_integer(nullptr, 1);
      EXPECT_EQ(gltang_call_count(nullptr), 0u);
      EXPECT_EQ(gltang_call_kind(nullptr, 0), GLTANG_KIND_NULL);
      return true;
    }
  };
  bool ok = false;
  Compiled compiled("use p; p(true, 2.5, \"hi\");");
  Context context(compiled.program);
  ASSERT_EQ(gltang_library_add_native(context.library(), "p", Probe::run, &ok), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_TRUE(ok);
}

// ---------------------------------------------------------------------------
// Values stored in containers and across collections
// ---------------------------------------------------------------------------

TEST(Values, ALibraryAndANativeSurviveAcrossAllocationsInAContainer) {
  Compiled compiled("use math; use a; xs = [math, a, math.pi]; s = \"\"; for (i = 0; i < 300; i += 1) { s = s + \"x\"; } xs[0].pi + xs[2] + xs[1]();");
  Context context(compiled.program);
  ASSERT_EQ(gltang_library_add_native(context.library(), "a", make_three, nullptr), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_NEAR(context.number(), 2 * 3.14159265358979 + 3, 1e-9);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
