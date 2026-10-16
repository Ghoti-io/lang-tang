// The rest of the host API (execution.h, language.h, unicodeString.h): the host
// context a callback reads, the language layer a `use` searches between the
// program's libraries and the built-ins, the output sink that receives each
// print as it finishes, and the string helpers. Each is driven by a program or,
// for the helpers, by the same tagged text the engine prints.

#include "description_harness.h"

#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/language.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using tt::Compiled;
using tt::Config;
using tt::Context;

namespace {

using Hook = std::function<bool(GLTANG_NativeCall *)>;

bool run_hook(GLTANG_NativeCall * call, void * user) {
  return (*static_cast<Hook *>(user))(call);
}

/// Adds a native whose body is a lambda the test keeps alive.
void add(Context & context, const char * name, Hook & hook) {
  ASSERT_EQ(gltang_library_add_native(context.library(), name, run_hook, &hook), GLTANG_OK);
}

/// A native that answers a string carrying `tag`.
Hook tagged(const std::string & text, GLTANG_String_Type tag) {
  return [text, tag](GLTANG_NativeCall * call) {
    gltang_call_return_string(call, text.data(), text.size(), tag);
    return true;
  };
}

/// What a sink was given, in order.
struct Delivery {
  std::vector<std::pair<std::string, GLTANG_String_Type>> segments;
  std::string bytes() const {
    std::string all;
    for (const auto & s : segments) {
      all += s.first;
    }
    return all;
  }
  int refuse_at = -1;  ///< The call (counted from 0) that returns false; -1 for none.
  int calls = 0;
};

bool sink_fn(void * user, const char * bytes, size_t length, GLTANG_String_Type type) {
  Delivery * d = static_cast<Delivery *>(user);
  int call = d->calls++;
  if (call == d->refuse_at) {
    return false;
  }
  d->segments.emplace_back(std::string(bytes, length), type);
  return true;
}

/// Holds the process's language layer for a test and clears it afterwards.
/// Takes the caller's reference to `library`: the layer keeps its own.
struct Layer {
  explicit Layer(GLTANG_Library * library) {
    EXPECT_EQ(gltang_language_set_library(library), GLTANG_OK);
    gltang_library_release(library);
  }
  ~Layer() { EXPECT_EQ(gltang_language_set_library(nullptr), GLTANG_OK); }
};

/// A named library with one integer member `n`, as `pages` is in the tests.
GLTANG_Library * pages(int64_t n, const char * name = "pages") {
  GLTANG_Library * lib = nullptr;
  EXPECT_EQ(gltang_library_create(name, &lib), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_integer(lib, "n", n), GLTANG_OK);
  return lib;
}

/// An unnamed layer library holding `child`, which it retains; the child's reference is released.
GLTANG_Library * layer_with(GLTANG_Library * child) {
  GLTANG_Library * root = nullptr;
  EXPECT_EQ(gltang_library_create(nullptr, &root), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_library(root, child), GLTANG_OK);
  gltang_library_release(child);
  return root;
}

Config forced_torture() {
  Config c;
  c.torture = 1;
  c.verify = 1;
  return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// The host context
// ---------------------------------------------------------------------------

TEST(HostContext, ANativeReadsTheContextOfTheExecutionThatRunsIt) {
  int one = 1;
  int two = 2;
  std::vector<void *> seen;
  Hook hook = [&](GLTANG_NativeCall * call) {
    seen.push_back(gltang_call_host_context(call));
    gltang_call_return_null(call);
    return true;
  };
  Compiled compiled("use f; f(); 1;");
  {
    Context a(compiled.program);
    add(a, "f", hook);
    ASSERT_EQ(gltang_execution_set_host_context(a.execution, &one), GLTANG_OK);
    ASSERT_TRUE(a.execute());
  }
  {
    Context b(compiled.program);
    add(b, "f", hook);
    ASSERT_EQ(gltang_execution_set_host_context(b.execution, &two), GLTANG_OK);
    ASSERT_TRUE(b.execute());
  }
  {
    Context c(compiled.program);
    add(c, "f", hook);
    ASSERT_TRUE(c.execute());
  }
  EXPECT_EQ(seen, (std::vector<void *>{&one, &two, nullptr}));
}

TEST(HostContext, TheExecutionAnswersWithTheSamePointer) {
  int x = 0;
  Compiled compiled("1;");
  Context context(compiled.program);
  EXPECT_EQ(gltang_execution_host_context(context.execution), nullptr);
  ASSERT_EQ(gltang_execution_set_host_context(context.execution, &x), GLTANG_OK);
  EXPECT_EQ(gltang_execution_host_context(context.execution), &x);
  ASSERT_EQ(gltang_execution_set_host_context(context.execution, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_execution_host_context(context.execution), nullptr);
  EXPECT_EQ(gltang_execution_host_context(nullptr), nullptr);
  EXPECT_EQ(gltang_execution_set_host_context(nullptr, &x), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_call_host_context(nullptr), nullptr);
}

TEST(HostContext, AFactoryAndAValueFactoryReadItToo) {
  int token = 7;
  void * handle_factory_saw = nullptr;
  void * value_factory_saw = nullptr;
  Hook factory = [&](GLTANG_NativeCall * call) {
    handle_factory_saw = gltang_call_host_context(call);
    gltang_call_return_integer(call, 5);
    return true;
  };
  struct Probe {
    void ** saw;
  } probe{&value_factory_saw};
  Compiled compiled("use hf; use vf; print(hf); print(vf);");
  Context context(compiled.program);
  ASSERT_EQ(gltang_library_add_handle_factory(context.library(), "hf", run_hook, &factory), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_value_factory(context.library(), "vf",
    [](void * user, GLTANG_ValueDescription * out) {
      *static_cast<Probe *>(user)->saw = gltang_description_host_context(out);
      GLTANG_ValueRef root;
      EXPECT_EQ(gltang_value_integer(out, 9, &root), GLTANG_OK);
      EXPECT_EQ(gltang_value_set_root(out, root), GLTANG_OK);
      return true;
    }, &probe), GLTANG_OK);
  ASSERT_EQ(gltang_execution_set_host_context(context.execution, &token), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "59");
  EXPECT_EQ(handle_factory_saw, &token);
  EXPECT_EQ(value_factory_saw, &token);
}

TEST(HostContext, ADescriptionTheHostMadeHasNone) {
  tt::Desc d;
  EXPECT_EQ(gltang_description_host_context(d), nullptr);
  EXPECT_EQ(gltang_description_host_context(nullptr), nullptr);
}

TEST(HostContext, SettingItAfterTheRunStartedOrInsideACallbackIsRefused) {
  int x = 0;
  GLTANG_Result inside = GLTANG_OK;
  Context * running = nullptr;
  Hook hook = [&](GLTANG_NativeCall * call) {
    inside = gltang_execution_set_host_context(running->execution, &x);
    gltang_call_return_null(call);
    return true;
  };
  Compiled compiled("use f; f(); 1;");
  Context context(compiled.program);
  running = &context;
  add(context, "f", hook);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(inside, GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_set_host_context(context.execution, &x), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_host_context(context.execution), nullptr);
}

TEST(HostContext, ASnapshotAppliedToAnotherExecutionLeavesItsContextAlone) {
  int source_token = 1;
  int dest_token = 2;
  Compiled compiled("print(\"a\"); for (i = 0; i < 50; i += 1) { } print(\"b\"); 1;");
  Config config;
  config.fuel = 60;
  Context src(compiled.program, config);
  ASSERT_EQ(gltang_execution_set_host_context(src.execution, &source_token), GLTANG_OK);
  src.execute();
  ASSERT_TRUE(src.paused());
  GLTANG_Snapshot * snapshot = nullptr;
  ASSERT_EQ(gltang_snapshot_take(src.execution, &snapshot), GLTANG_OK);
  Context dst(compiled.program, Config());
  ASSERT_EQ(gltang_execution_set_host_context(dst.execution, &dest_token), GLTANG_OK);
  ASSERT_EQ(gltang_snapshot_restore(dst.execution, snapshot), GLTANG_OK);
  gltang_snapshot_release(snapshot);
  EXPECT_EQ(gltang_execution_host_context(dst.execution), &dest_token);
  ASSERT_TRUE(dst.resume());
  EXPECT_EQ(dst.raw(), "ab");
}

// ---------------------------------------------------------------------------
// The language layer
// ---------------------------------------------------------------------------

TEST(LanguageLayer, AMemberIsFoundAfterTheProgramsAndBeforeTheBuiltIns) {
  Layer layer(layer_with(pages(7)));
  Compiled compiled("use pages; print(pages.n);");
  Context context(compiled.program);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "7");
}

TEST(LanguageLayer, TheExecutionsLibrariesAndTheProgramsWinOverIt) {
  Layer layer(layer_with(pages(7)));
  // The execution's own library comes first.
  {
    Compiled compiled("use pages; print(pages.n);");
    Context context(compiled.program);
    GLTANG_Library * mine = pages(1);
    ASSERT_EQ(gltang_library_add_library(context.library(), mine), GLTANG_OK);
    gltang_library_release(mine);
    ASSERT_TRUE(context.execute());
    EXPECT_EQ(context.raw(), "1");
  }
  // Then the program's.
  {
    Compiled compiled("use pages; print(pages.n);");
    GLTANG_Library * program_layer = layer_with(pages(2));
    ASSERT_EQ(gltang_program_set_libraries(compiled.program, program_layer), GLTANG_OK);
    gltang_library_release(program_layer);
    Context context(compiled.program);
    ASSERT_TRUE(context.execute());
    EXPECT_EQ(context.raw(), "2");
  }
}

TEST(LanguageLayer, ItShadowsABuiltIn) {
  // `math` is a built-in; the layer's `math` is found first.
  Layer layer(layer_with(pages(41, "math")));
  Compiled compiled("use math; print(math.n);");
  Context context(compiled.program);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "41");
}

TEST(LanguageLayer, WithoutItTheBuiltInsAreStillThere) {
  Compiled compiled("use math; print(math.pi);");
  Context context(compiled.program);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw().substr(0, 4), "3.14");
}

TEST(LanguageLayer, ReplacingItMidRunChangesNothingForARunningExecutionAndEverythingForANewOne) {
  Layer layer(layer_with(pages(1)));
  Hook swap = [&](GLTANG_NativeCall * call) {
    GLTANG_Library * replacement = layer_with(pages(2));
    EXPECT_EQ(gltang_language_set_library(replacement), GLTANG_OK);
    gltang_library_release(replacement);
    gltang_call_return_null(call);
    return true;
  };
  Compiled compiled("use pages as first; use swap; swap(); use pages as second; print(first.n); print(second.n);");
  {
    Context context(compiled.program);
    add(context, "swap", swap);
    ASSERT_TRUE(context.execute());
    EXPECT_EQ(context.raw(), "11") << "the layer the execution took when it was created is the one it keeps";
  }
  Compiled later("use pages; print(pages.n);");
  Context fresh(later.program);
  ASSERT_TRUE(fresh.execute());
  EXPECT_EQ(fresh.raw(), "2");
}

TEST(LanguageLayer, TheLibraryIsFreedWhenTheLastExecutionThatTookItIsGone) {
  // The libraries are cutil's memory, not the context's: count cutil's allocations across it.
  const size_t live_before = gcu_memory_alloc_count - gcu_memory_free_count;
  {
    Layer layer(layer_with(pages(1)));
    Compiled compiled("use pages; print(pages.n);");
    {
      Context context(compiled.program);
      ASSERT_EQ(gltang_language_set_library(nullptr), GLTANG_OK);  // the host lets go while the execution holds it
      ASSERT_TRUE(context.execute());
      EXPECT_EQ(context.raw(), "1") << "an execution keeps the layer it took even after the host cleared it";
    }
  }
  EXPECT_EQ(gcu_memory_alloc_count - gcu_memory_free_count, live_before) << "a language library was not released";
}

TEST(LanguageLayer, ItIsReadable) {
  EXPECT_EQ(gltang_language_library(), nullptr);
  GLTANG_Library * root = layer_with(pages(3));
  {
    Layer layer(gltang_library_retain(root));
    GLTANG_Library * held = gltang_language_library();
    EXPECT_EQ(held, root);
    EXPECT_TRUE(gltang_library_sealed(held));
    gltang_library_release(held);
  }
  gltang_library_release(root);
  EXPECT_EQ(gltang_language_library(), nullptr);
}

TEST(LanguageLayer, ThreadsReadAndReplaceItTogether) {
  GLTANG_Library * a = layer_with(pages(1));
  GLTANG_Library * b = layer_with(pages(2));
  Layer layer(gltang_library_retain(a));
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  const int rounds = tt::heavy_instruments() ? 3 : 40;
  // The program is compiled once, here: a program is immutable and shared by every thread, and
  // cutil's allocation counters are plain integers, so the threads must not allocate through it.
  Compiled compiled("use pages; print(pages.n);");
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      for (int i = 0; i < rounds; ++i) {
        Context context(compiled.program);
        if (!context.execute() || (context.raw() != "1" && context.raw() != "2")) {
          ++bad;
        }
      }
    });
  }
  std::thread writer([&] {
    for (int i = 0; !stop.load(); ++i) {
      EXPECT_EQ(gltang_language_set_library((i & 1) ? a : b), GLTANG_OK);
    }
  });
  for (auto & r : readers) {
    r.join();
  }
  stop = true;
  writer.join();
  EXPECT_EQ(bad.load(), 0);
  gltang_library_release(a);
  gltang_library_release(b);
}

namespace {

bool tick_native(GLTANG_NativeCall * call, void *) {
  gltang_call_return_integer(call, 42);
  return true;
}

/// A layer whose `pages` library holds a native `tick` and a template `page` (it prints `T`).
GLTANG_Library * pages_with_members(GLTANG_Program * page) {
  GLTANG_Library * pages = nullptr;
  EXPECT_EQ(gltang_library_create("pages", &pages), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(pages, "tick", tick_native, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_template(pages, "page", page, 1000, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_handle_factory(pages, "made", [](GLTANG_NativeCall * call, void *) {
    gltang_call_return_integer(call, 9);
    return true;
  }, nullptr), GLTANG_OK);
  return layer_with(pages);
}

}  // namespace

TEST(LanguageLayer, ItHoldsNativesTemplatesFactoriesAndLibrariesLikeAnyOther) {
  Compiled page("print(\"T\");", tt::Mode::Script, "page.tang");
  Layer layer(pages_with_members(page.program));
  Compiled program("use pages; use pages.tick as t; use pages.page as p; use pages.made as m; print(t()); print(p()); print(m);");
  Context context(program.program);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "42T9");
}

TEST(LanguageLayer, ASnapshotOfARunHoldingALayerMemberIsRestoredFromTheDestinationsLayer) {
  Compiled page("print(\"T\");", tt::Mode::Script, "page.tang");
  Layer layer(pages_with_members(page.program));
  Compiled program("use pages.tick as t; use pages.page as p; for (i = 0; i < 50; i += 1) { } print(t()); print(p());");
  Config config;
  config.fuel = 70;
  Context src(program.program, config);
  src.execute();
  ASSERT_TRUE(src.paused());
  GLTANG_Snapshot * snapshot = nullptr;
  ASSERT_EQ(gltang_snapshot_take(src.execution, &snapshot), GLTANG_OK) << "a native and a template of the layer are named and can be taken";
  {
    Context dst(program.program, Config());
    ASSERT_EQ(gltang_snapshot_restore(dst.execution, snapshot), GLTANG_OK);
    ASSERT_TRUE(dst.resume());
    EXPECT_EQ(dst.raw(), "42T");
  }
  // A destination created while the layer is gone cannot find the members by name: refused, nothing changed.
  ASSERT_EQ(gltang_language_set_library(nullptr), GLTANG_OK);
  {
    Context bare(program.program, Config());
    EXPECT_NE(gltang_snapshot_restore(bare.execution, snapshot), GLTANG_OK);
  }
  gltang_snapshot_release(snapshot);
}

// ---------------------------------------------------------------------------
// The output sink
// ---------------------------------------------------------------------------

TEST(OutputSink, EachPrintIsDeliveredWhole_InOrder_WithItsTagAndMatchesTheBuffer) {
  Hook html = tagged("<b>&", GLTANG_UNICODE_STRING_TYPE_HTML);
  Compiled compiled("use h; print(\"a\"); print(h()); print(\"b\" + h()); print(7);");
  Context context(compiled.program);
  add(context, "h", html);
  Delivery d;
  ASSERT_EQ(gltang_execution_set_output_sink(context.execution, sink_fn, &d), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  using Seg = std::pair<std::string, GLTANG_String_Type>;
  const GLTANG_String_Type T = GLTANG_UNICODE_STRING_TYPE_TRUSTED;
  const GLTANG_String_Type H = GLTANG_UNICODE_STRING_TYPE_HTML;
  EXPECT_EQ(d.segments, (std::vector<Seg>{{"a", T}, {"<b>&", H}, {"b", T}, {"<b>&", H}, {"7", T}}));
  EXPECT_EQ(d.bytes(), context.raw()) << "the sink received what the buffer holds";
  EXPECT_EQ(context.rendered(), "a&lt;b&gt;&amp;b&lt;b&gt;&amp;7");
}

TEST(OutputSink, TheBufferIsUnchangedByASink) {
  Compiled compiled("print(\"x\"); print([1, 2]); print(\"y\");");
  Context plain(compiled.program);
  ASSERT_TRUE(plain.execute());
  Context sunk(compiled.program);
  Delivery d;
  ASSERT_EQ(gltang_execution_set_output_sink(sunk.execution, sink_fn, &d), GLTANG_OK);
  ASSERT_TRUE(sunk.execute());
  EXPECT_EQ(sunk.raw(), plain.raw());
  EXPECT_EQ(sunk.rendered(), plain.rendered());
  EXPECT_EQ(d.bytes(), plain.raw());
}

TEST(OutputSink, ARefusalRollsBackThatPrintAndTheRunGoesOn) {
  Compiled compiled("print(\"a\"); x = print(\"b\"); print(\"c\"); x;");
  Context context(compiled.program);
  Delivery d;
  d.refuse_at = 1;
  ASSERT_EQ(gltang_execution_set_output_sink(context.execution, sink_fn, &d), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "ac");
  EXPECT_EQ(d.bytes(), "ac") << "the second call was the refused one; the others were delivered";
  ASSERT_TRUE(context.is_error());
  EXPECT_EQ(context.error_kind(), GLTANG_ERROR_HOST_FAILED);
}

TEST(OutputSink, ARefusedPrintOfTemplateTextIsRolledBackToo) {
  // The text of a template is printed as a constant, by another path than `print`.
  Hook html = tagged("<b>", GLTANG_UNICODE_STRING_TYPE_HTML);
  Compiled page("<% use h; %>one<%= h() %>two", tt::Mode::Template);
  Context context(page.program);
  add(context, "h", html);
  Delivery d;
  d.refuse_at = 0;
  ASSERT_EQ(gltang_execution_set_output_sink(context.execution, sink_fn, &d), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "<b>two") << "the first text was refused and taken back; the rest was delivered";
  EXPECT_EQ(d.bytes(), "<b>two");
}

TEST(OutputSink, APrintThatFailsForWantOfMemoryDeliversNothingOfItAndTheSinkNeverGetsAheadOfTheBuffer) {
  // Fail the nth allocation for each n the run makes: whatever fails, the sink has been given
  // only whole prints, so what it holds is the buffer, or a prefix of it when the run was cut off.
  Hook html = tagged("<b>", GLTANG_UNICODE_STRING_TYPE_HTML);
  Compiled compiled("use h; print(\"a\"); print([1, h(), \"x\"]); print(h()); print({k: [h()]}); print(\"z\");");
  std::string whole;
  long total = 0;
  {
    Context reference(compiled.program);
    add(reference, "h", html);
    ASSERT_TRUE(reference.execute());
    whole = reference.raw();
    total = reference.tracker.calls;
    ASSERT_GT(whole.size(), 10u);
  }
  // The prints are the last of the run's allocations: sweep the last of them (every one when the
  // run is cheap, every third under torture and the like), through the end.
  const long first = total > 200 ? total - 200 : 1;
  const long stride = tt::heavy_instruments() ? 3 : 1;
  int finished_short = 0;
  for (long n = first; n <= total; n += stride) {
    Config config;
    config.fail_at = n;
    Context context(compiled.program, config);
    if (!context.ok()) {
      continue;
    }
    add(context, "h", html);
    Delivery d;
    ASSERT_EQ(gltang_execution_set_output_sink(context.execution, sink_fn, &d), GLTANG_OK);
    bool finished = context.execute();
    const std::string raw = context.raw();
    EXPECT_EQ(raw.compare(0, d.bytes().size(), d.bytes()), 0) << "n=" << n << ": the sink holds bytes the buffer does not";
    if (finished) {
      EXPECT_EQ(d.bytes(), raw) << "n=" << n;
      if (raw != whole) {
        ++finished_short;   // a print was refused its memory and rolled back, and the run went on
      }
    }
  }
  // Under relocation an allocation that fails may be one the collector itself needs, and such a run
  // is cut short rather than carried on: the control is for the arms where a print can fail alone.
  const char * relocate = std::getenv("GRHEAP_RELOCATE");
  if (!relocate || !*relocate || std::strcmp(relocate, "0") == 0) {
    EXPECT_GT(finished_short, 0) << "the sweep never made a print fail and carry on, so it measured nothing";
  }
}

TEST(OutputSink, ATemplateCallsOwnOutputIsDeliveredWhenItsValueIsPrinted) {
  Compiled inner("print(\"in\"); print(\"ner\");", tt::Mode::Script, "inner.tang");
  GLTANG_Library * lib = nullptr;
  ASSERT_EQ(gltang_library_create("views", &lib), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_template(lib, "t", inner.program, 1000, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  Compiled page("use views.t as t; print(\"[\"); print(t()); print(\"]\");");
  Context context(page.program);
  ASSERT_EQ(gltang_library_add_library(context.library(), lib), GLTANG_OK);
  gltang_library_release(lib);
  Delivery d;
  ASSERT_EQ(gltang_execution_set_output_sink(context.execution, sink_fn, &d), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "[inner]");
  EXPECT_EQ(d.bytes(), "[inner]") << "what the template printed reached the sink once, as part of the print that used its value";
}

TEST(OutputSink, ItIsSetBeforeTheRunOnly) {
  Compiled compiled("print(1);");
  Context context(compiled.program);
  Delivery d;
  EXPECT_EQ(gltang_execution_set_output_sink(nullptr, sink_fn, &d), GLTANG_ERR_INVALID);
  ASSERT_EQ(gltang_execution_set_output_sink(context.execution, sink_fn, &d), GLTANG_OK);
  ASSERT_EQ(gltang_execution_set_output_sink(context.execution, nullptr, nullptr), GLTANG_OK);
  ASSERT_EQ(gltang_execution_set_output_sink(context.execution, sink_fn, &d), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(d.bytes(), "1");
  EXPECT_EQ(gltang_execution_set_output_sink(context.execution, nullptr, nullptr), GLTANG_ERR_INVALID);
}

TEST(OutputSink, ASinkCannotDestroyOrReconfigureTheExecutionThatCallsIt) {
  Compiled compiled("print(1); print(2);");
  Context context(compiled.program);
  struct State {
    Context * context;
    GLTANG_Result setter = GLTANG_OK;
    int calls = 0;
  } state{&context};
  ASSERT_EQ(gltang_execution_set_output_sink(context.execution,
    [](void * user, const char *, size_t, GLTANG_String_Type) {
      State * s = static_cast<State *>(user);
      ++s->calls;
      int x = 0;
      s->setter = gltang_execution_set_host_context(s->context->execution, &x);
      gltang_execution_destroy(s->context->execution);   // does nothing from inside a host function
      return true;
    }, &state), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(state.calls, 2);
  EXPECT_EQ(state.setter, GLTANG_ERR_INVALID);
  EXPECT_EQ(context.raw(), "12") << "the execution was not destroyed under the run";
}

TEST(OutputSink, AfterASnapshotIsAppliedDeliveryStartsFromTheRestoredOutput) {
  Compiled compiled("print(\"one\"); print(\"two\"); for (i = 0; i < 50; i += 1) { } print(\"three\"); 1;");
  Config config;
  config.fuel = 80;
  Context src(compiled.program, config);
  Delivery from_source;
  ASSERT_EQ(gltang_execution_set_output_sink(src.execution, sink_fn, &from_source), GLTANG_OK);
  src.execute();
  ASSERT_TRUE(src.paused());
  ASSERT_EQ(src.raw().substr(0, 6), "onetwo");
  GLTANG_Snapshot * snapshot = nullptr;
  ASSERT_EQ(gltang_snapshot_take(src.execution, &snapshot), GLTANG_OK);
  Context dst(compiled.program, Config());
  Delivery from_dest;
  ASSERT_EQ(gltang_execution_set_output_sink(dst.execution, sink_fn, &from_dest), GLTANG_OK);
  ASSERT_EQ(gltang_snapshot_restore(dst.execution, snapshot), GLTANG_OK);
  gltang_snapshot_release(snapshot);
  ASSERT_TRUE(dst.resume());
  EXPECT_EQ(dst.raw(), "onetwothree");
  EXPECT_EQ(from_dest.bytes(), "three") << "what was printed before the snapshot is not delivered again";
}

TEST(OutputSink, CompiledCodeDeliversTheSameBytesAsTheInterpreter) {
  // Every function is compiled at its first poll in one run and never in the other; prints from
  // compiled code come through the same print as the interpreter's, and the sink cannot tell.
  Hook html = tagged("<i>", GLTANG_UNICODE_STRING_TYPE_HTML);
  const char * source = "function line(i) { use h; print(\"n\"); print(h()); print(i); return i; } s = 0; for (i = 0; i < 30; i += 1) { s += line(i); } s;";
  std::vector<std::string> bytes;
  for (long threshold : {0L, 1L}) {
    Compiled compiled(source);
    Config config;
    config.jit_threshold = threshold;
    Context context(compiled.program, config);
    add(context, "h", html);
    Delivery d;
    ASSERT_EQ(gltang_execution_set_output_sink(context.execution, sink_fn, &d), GLTANG_OK);
    ASSERT_TRUE(context.execute());
    EXPECT_EQ(d.bytes(), context.raw());
    EXPECT_EQ(d.segments.size(), 90u);
    bytes.push_back(d.bytes());
  }
  EXPECT_EQ(bytes[0], bytes[1]);
}

TEST(OutputSink, DeliveryHoldsUnderTortureAndMatchesTheBuffer) {
  Hook html = tagged("<i>", GLTANG_UNICODE_STRING_TYPE_HTML);
  Compiled compiled("use h; i = 0; while (i < 20) { print(h()); print(i); i += 1; }");
  Context context(compiled.program, forced_torture());
  add(context, "h", html);
  Delivery d;
  ASSERT_EQ(gltang_execution_set_output_sink(context.execution, sink_fn, &d), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(d.bytes(), context.raw());
  EXPECT_EQ(d.segments.size(), 40u);
}

// ---------------------------------------------------------------------------
// The string helpers
// ---------------------------------------------------------------------------

namespace {

struct Str {
  GLTANG_Unicode_String * s = nullptr;
  Str(const std::string & text, GLTANG_String_Type type = GLTANG_UNICODE_STRING_TYPE_TRUSTED) {
    s = gltang_unicode_string_create(text.data(), text.size(), type);
  }
  explicit Str(GLTANG_Unicode_String * adopted) : s(adopted) {}
  ~Str() {
    if (s) {
      gltang_unicode_string_destroy(s);
    }
  }
  Str(const Str &) = delete;
  Str & operator=(const Str &) = delete;
  std::string rendered() const {
    GLTANG_Unicode_Rendered_String r = gltang_unicode_string_render(s);
    std::string out = r.buffer ? std::string(r.buffer, r.length) : std::string("<null>");
    gcu_free(r.buffer);
    return out;
  }
  size_t runs() const { return s->string_type->count; }
  GLTANG_String_Type run_type(size_t i) const { return static_cast<GLTANG_String_Type>(GLTANG_UC_GET_TYPE_FROM_TYPE_OFFSET_PAIR(s->string_type->data[i])); }
  size_t run_start(size_t i) const { return GLTANG_UC_GET_OFFSET_FROM_TYPE_OFFSET_PAIR(s->string_type->data[i]); }
};

const GLTANG_String_Type T = GLTANG_UNICODE_STRING_TYPE_TRUSTED;
const GLTANG_String_Type H = GLTANG_UNICODE_STRING_TYPE_HTML;
const GLTANG_String_Type HA = GLTANG_UNICODE_STRING_TYPE_HTML_ATTRIBUTE;
const GLTANG_String_Type P = GLTANG_UNICODE_STRING_TYPE_PERCENT;
const GLTANG_String_Type J = GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT;

}  // namespace

TEST(UnicodeStringHelpers, RenderEncodesEachRunByItsTag) {
  EXPECT_EQ(Str("<a href=\"x\">'&", T).rendered(), "<a href=\"x\">'&");
  EXPECT_EQ(Str("<a href=\"x\">'&", H).rendered(), "&lt;a href=\"x\"&gt;'&amp;");
  EXPECT_EQ(Str("<a href=\"x\">'&", HA).rendered(), "&lt;a href=&quot;x&quot;&gt;&#39;&amp;");
  EXPECT_EQ(Str("a b/é", P).rendered(), "a+b%2F%C3%A9");
  EXPECT_EQ(Str("", H).rendered(), "");
}

TEST(UnicodeStringHelpers, RenderGivesTheBytesTheEnginesOwnOutputGives) {
  // The engine prints the same tagged text; the two renders must agree for every tag.
  const std::string text = "a<b>&\"'/ é\n";
  const GLTANG_String_Type tags[] = {T, H, HA, P, J};
  for (GLTANG_String_Type tag : tags) {
    Hook hook = tagged(text, tag);
    Compiled compiled("use f; print(f());");
    Context context(compiled.program);
    add(context, "f", hook);
    ASSERT_TRUE(context.execute());
    EXPECT_EQ(Str(text, tag).rendered(), context.rendered()) << "tag " << static_cast<int>(tag);
  }
}

TEST(UnicodeStringHelpers, HtmlEncodeIsTheHtmlTagWithoutAString) {
  GLTANG_Unicode_Rendered_String r = gltang_unicode_string_html_encode("<i>&\"'", 6);
  ASSERT_NE(r.buffer, nullptr);
  EXPECT_EQ(std::string(r.buffer, r.length), "&lt;i&gt;&amp;\"'");
  gcu_free(r.buffer);
  GLTANG_Unicode_Rendered_String empty = gltang_unicode_string_html_encode("", 0);
  ASSERT_NE(empty.buffer, nullptr);
  EXPECT_EQ(empty.length, 0u);
  gcu_free(empty.buffer);
  GLTANG_Unicode_Rendered_String bad = gltang_unicode_string_html_encode(nullptr, 3);
  EXPECT_EQ(bad.buffer, nullptr);
}

TEST(UnicodeStringHelpers, ConcatKeepsEachRunsTagAndJoinsEqualOnes) {
  Str a("<a>", T);
  Str b("<b>", H);
  Str c("<c>", H);
  Str ab(gltang_unicode_string_concat(a.s, b.s));
  ASSERT_NE(ab.s, nullptr);
  EXPECT_EQ(ab.rendered(), "<a>&lt;b&gt;");
  EXPECT_EQ(ab.runs(), 2u);
  Str abc(gltang_unicode_string_concat(ab.s, c.s));
  ASSERT_NE(abc.s, nullptr);
  EXPECT_EQ(abc.rendered(), "<a>&lt;b&gt;&lt;c&gt;");
  EXPECT_EQ(abc.runs(), 2u) << "two html runs in a row are one";
  EXPECT_EQ(abc.s->grapheme_length, 9u);
  EXPECT_EQ(abc.run_type(0), T);
  EXPECT_EQ(abc.run_type(1), H);
  EXPECT_EQ(abc.run_start(1), 3u);
}

TEST(UnicodeStringHelpers, ConcatWithAnEmptyStringIsTheOther) {
  Str empty("", T);
  Str x("<x>", H);
  Str left(gltang_unicode_string_concat(empty.s, x.s));
  Str right(gltang_unicode_string_concat(x.s, empty.s));
  ASSERT_NE(left.s, nullptr);
  ASSERT_NE(right.s, nullptr);
  EXPECT_EQ(left.rendered(), "&lt;x&gt;");
  EXPECT_EQ(right.rendered(), "&lt;x&gt;");
  EXPECT_EQ(left.runs(), 1u);
  EXPECT_EQ(right.runs(), 1u);
  Str both(gltang_unicode_string_concat(empty.s, empty.s));
  ASSERT_NE(both.s, nullptr);
  EXPECT_EQ(both.s->grapheme_length, 0u);
}

TEST(UnicodeStringHelpers, ConcatFindsGraphemesAgainAcrossTheJoin) {
  // A combining acute accent at the start of the second joins the last grapheme of the first.
  Str base("e", T);
  Str accent("\xCC\x81", H);
  Str joined(gltang_unicode_string_concat(base.s, accent.s));
  ASSERT_NE(joined.s, nullptr);
  EXPECT_EQ(joined.s->grapheme_length, 1u);
  EXPECT_EQ(joined.runs(), 1u) << "the grapheme belongs to the run it began in";
  EXPECT_EQ(joined.run_type(0), T);
  EXPECT_EQ(joined.rendered(), "e\xCC\x81");
}

TEST(UnicodeStringHelpers, SubstringKeepsTheTagsOfWhatItTakes) {
  Str a("ab", T);
  Str b("<cd>", H);
  Str c("ef", T);
  Str ab(gltang_unicode_string_concat(a.s, b.s));
  Str all(gltang_unicode_string_concat(ab.s, c.s));  // ab <cd> ef: 8 graphemes
  ASSERT_EQ(all.s->grapheme_length, 8u);
  Str mid(gltang_unicode_string_substring(all.s, 1, 4));  // b < c d
  ASSERT_NE(mid.s, nullptr);
  EXPECT_EQ(mid.rendered(), "b&lt;cd");
  EXPECT_EQ(mid.runs(), 2u);
  EXPECT_EQ(mid.run_type(0), T);
  EXPECT_EQ(mid.run_type(1), H);
  EXPECT_EQ(mid.run_start(1), 1u);
  Str tail(gltang_unicode_string_substring(all.s, 5, 100));  // d>ef, clamped
  ASSERT_NE(tail.s, nullptr);
  EXPECT_EQ(tail.rendered(), "&gt;ef");
  EXPECT_EQ(tail.run_type(0), H);
  Str ends_at_run(gltang_unicode_string_substring(all.s, 0, 2));  // exactly the first run
  EXPECT_EQ(ends_at_run.runs(), 1u) << "a run that begins where the substring ends is not in it";
  Str whole(gltang_unicode_string_substring(all.s, 0, 100));
  EXPECT_EQ(whole.rendered(), all.rendered());
  EXPECT_EQ(whole.runs(), 3u);
}

TEST(UnicodeStringHelpers, SubstringOutOfRangeIsEmptyAndTrusted) {
  Str s("abc", H);
  Str past(gltang_unicode_string_substring(s.s, 3, 1));
  Str none(gltang_unicode_string_substring(s.s, 0, 0));
  ASSERT_NE(past.s, nullptr);
  ASSERT_NE(none.s, nullptr);
  EXPECT_EQ(past.s->grapheme_length, 0u);
  EXPECT_EQ(none.s->grapheme_length, 0u);
  EXPECT_EQ(past.run_type(0), T);
}

TEST(UnicodeStringHelpers, TheEnginesPlusAndConcatAgreeOnTheTags) {
  // The language's own `+` of a trusted and an html string, rendered, equals concat then render.
  Hook html = tagged("<b>", H);
  Compiled compiled("use h; print(\"<a>\" + h() + \"<c>\");");
  Context context(compiled.program);
  add(context, "h", html);
  ASSERT_TRUE(context.execute());
  Str a("<a>", T);
  Str b("<b>", H);
  Str c("<c>", T);
  Str ab(gltang_unicode_string_concat(a.s, b.s));
  Str abc(gltang_unicode_string_concat(ab.s, c.s));
  EXPECT_EQ(abc.rendered(), context.rendered());
}

TEST(UnicodeStringHelpers, MultiByteTextAndClustersAreCutAndJoinedByGraphemes) {
  // "é" as e + combining acute, "日本", a family emoji (one grapheme of seven code points).
  const std::string text = "e\xCC\x81" "\xE6\x97\xA5\xE6\x9C\xAC" "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
  Str s(text, H);
  ASSERT_EQ(s.s->grapheme_length, 4u);
  Str first(gltang_unicode_string_substring(s.s, 0, 1));
  EXPECT_EQ(first.s->grapheme_length, 1u);
  EXPECT_EQ(first.s->byte_length, 3u) << "the accent stays with its letter";
  Str last(gltang_unicode_string_substring(s.s, 3, 1));
  EXPECT_EQ(last.s->grapheme_length, 1u) << "the whole family is one grapheme";
  Str middle(gltang_unicode_string_substring(s.s, 1, 2));
  EXPECT_EQ(middle.rendered(), "\xE6\x97\xA5\xE6\x9C\xAC");
  Str rejoined(gltang_unicode_string_concat(first.s, middle.s));
  Str all(gltang_unicode_string_concat(rejoined.s, last.s));
  EXPECT_EQ(all.rendered(), s.rendered());
  EXPECT_EQ(all.s->grapheme_length, 4u);
  EXPECT_EQ(all.runs(), 1u);
}

TEST(UnicodeStringHelpers, ANullArgumentIsRefusedNotDereferenced) {
  Str x("x", T);
  EXPECT_EQ(gltang_unicode_string_concat(nullptr, x.s), nullptr);
  EXPECT_EQ(gltang_unicode_string_concat(x.s, nullptr), nullptr);
  EXPECT_EQ(gltang_unicode_string_substring(nullptr, 0, 1), nullptr);
  EXPECT_EQ(gltang_unicode_string_render(nullptr).buffer, nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
