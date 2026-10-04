# Copied from runtime-heap's Makefile, which was copied from runtime-core's,
# which was copied from color's, per CONVENTIONS.md section 12. PROJECT, the
# dependency block (cutil and unicode), the generated parser and scanner, the
# `tang` command, the oracle, the corpus gates and the fuzz replay changed.
#
# What is different from the libraries before it:
#
#  * bison and flex generate the parser and the scanner into $(GEN_DIR) at
#    build time. The generated files are never committed, and the parser
#    header is ordered before everything that includes an AST header.
#  * The ported AST and the generated code are compiled with
#    -Wstrict-aliasing=3. The suite's default is level 1, which reports the
#    C struct-inheritance downcast `(Node_Binary *) self` that the whole AST is
#    built from, and which C17 6.7.2.1p15 makes well defined (see design.md).
#  * `test-oracle` links ctang, which nothing else here may do (AD-2, AD-16),
#    into one runner under tests/oracle/, and fails when ctang is absent.
#
# The test-executable rules keep section 6's shape: the static archive is a
# normal prerequisite of every test because the recipe links it, and the .so
# is order-only because check-symbols wants it and the tests do not link it.

SUITE := ghoti.io
PROJECT := lang-tang

BUILD ?= release

# The baseline JIT (story 15) is an option: JIT=yes (the default) builds it and
# makes ghoti.io-runtime-jit a hard dependency; JIT=no compiles none of
# src/jit/, links nothing of runtime-jit, and builds in a tree of its own
# (build/<os>/<build>-nojit) so that the two arms never share an object. Any
# other value is an error, named here and not later (AD-2: the interpreter-only
# engine is a supported arm, not a degraded one).
JIT ?= yes
ifeq ($(filter yes no,$(JIT)),)
$(error JIT must be yes or no, not '$(JIT)')
endif
MAJOR_VERSION := 0
MINOR_VERSION := 0.0
VERSION_MINOR_ONLY := $(word 1,$(subst ., ,$(MINOR_VERSION)))
VERSION_PATCH_ONLY := $(or $(word 2,$(subst ., ,$(MINOR_VERSION))),0)
VERSION := $(MAJOR_VERSION).$(MINOR_VERSION)

# Names this build everywhere: the .pc file, the install directory, the soname
# and the symbol token. Defaults to the major version. Override for a build
# that wants its own identity: make BRANCH=-dev
BRANCH ?= -$(MAJOR_VERSION)

ifeq ($(BRANCH),-$(MAJOR_VERSION))
VERSION_STRING := $(VERSION)
else
VERSION_STRING := $(VERSION)$(BRANCH)
endif

# override: BRANCH may have come from the command line, and a command-line
# variable otherwise beats a plain assignment. Without it, `make BRANCH=-dev
# BUILD=debug` produced a debug build carrying the release token.
ifeq ($(BUILD),debug)
    override BRANCH := $(BRANCH)-debug
    override VERSION_STRING := $(VERSION_STRING)-debug
endif

# Decided here, before the platform rewrite below. `BUILD := linux/$(BUILD)`
# is a plain assignment, so a command-line BUILD=debug stays "debug" and an
# environment BUILD=debug becomes "linux/debug". Testing it up here is true
# in both cases. Release is -O2 because that is what ships. -O3 is not the
# default because nothing here has measured a figure that would justify it,
# and the poll and the frame walk are the places to measure it when something
# does (AD-26).
ifeq ($(BUILD),debug)
OPT_CFLAGS := -O0
else
OPT_CFLAGS := -O2
endif

BASE_NAME := lib$(SUITE)-$(PROJECT)$(BRANCH).so
LIBVER_SYMBOL := $(shell echo "ghotiio_$(PROJECT)$(BRANCH)" | sed 's/[.-]/_/g')
BASE_NAME_PREFIX := lib$(SUITE)-$(PROJECT)$(BRANCH)
SO_NAME := $(BASE_NAME).$(MAJOR_VERSION)
STATIC_TARGET := $(BASE_NAME_PREFIX).a
ENV_VARS :=

# Do not assign PKG_CONFIG_PATH. Make exports an inherited variable with
# whatever value the makefile last gave it, so overwriting it handed every
# sub-make a different path from the parent's.
PKG_CONFIG_PATH_ENV := $(PKG_CONFIG_PATH)

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Linux)
	OS_NAME := Linux
	LIB_EXTENSION := so
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-soname,$(SO_NAME)
	TARGET := $(SO_NAME).$(MINOR_VERSION)
	EXE_EXTENSION :=
	PC_INSTALL_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
	override BUILD := linux/$(BUILD)

else ifeq ($(UNAME_S), Darwin)
	OS_NAME := Mac
	LIB_EXTENSION := dylib
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-install_name,$(BASE_NAME_PREFIX).dylib
	TARGET := $(BASE_NAME_PREFIX).dylib
	EXE_EXTENSION :=
	PC_INSTALL_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
	override BUILD := mac/$(BUILD)

# TODO(windows): the Windows branches in this file were adapted from font's
# and have never been run, nor has GLTANG_API's dllexport/dllimport switching.
# See notes/suite/WINDOWS-TODO.md.
else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	PC_INSTALL_PATH := /mingw32/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw32/include
	LIB_INSTALL_PATH := /mingw32/lib
	BIN_INSTALL_PATH := /mingw32/bin
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
	override BUILD := win32/$(BUILD)

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	PC_INSTALL_PATH := /mingw64/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw64/include
	LIB_INSTALL_PATH := /mingw64/lib
	BIN_INSTALL_PATH := /mingw64/bin
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
	override BUILD := win64/$(BUILD)

else
    $(error Unsupported OS: $(UNAME_S))
endif

# The interpreter-only arm builds in its own tree.
ifeq ($(JIT),no)
override BUILD := $(BUILD)-nojit
endif

ifdef PREFIX
INCLUDE_INSTALL_PATH := $(PREFIX)/include
LIB_INSTALL_PATH := $(PREFIX)/lib
BIN_INSTALL_PATH := $(PREFIX)/bin
PC_INSTALL_PATH := $(PREFIX)/share/pkgconfig
ifeq ($(OS_NAME), Windows)
PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
else
PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
endif
LDCONF_INSTALL_PATH :=
endif

PKG_CONFIG_LOOKUP_PATH := $(if $(PKG_CONFIG_PATH_ENV),$(PKG_CONFIG_PATH_ENV):)$(PC_INSTALL_PATH)

CXX := g++
CXXFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c++20 -O1 -g -DGLTANG_TEST_DATA='"$(CURDIR)/tests"' $(EXTRA_CXXFLAGS)
CC := cc
# -Wstrict-aliasing=1 and -fstrict-aliasing, named rather than inherited.
# -Wall sets the aliasing warning to level 3, which is silent on the probe
# check-aliasing compiles. An explicit level beats -Wall from either side.
# -fstrict-aliasing is off below -O2 unless named, so a debug or coverage
# tree would otherwise have the warning armed and the assumption off.
# The probe is a pointer parameter stored through a second variable: that
# shape is reported at level 1 and silent at 0, 2, and 3. Do not simplify it
# to `*(int *)&local`, which fires at every level from 1 up and certifies
# nothing. check-aliasing is the measurement.
CFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wfloat-conversion -fstrict-aliasing -Wstrict-aliasing=1 -Wno-error=unused-function -Wfatal-errors -std=c17 $(OPT_CFLAGS) -g $(EXTRA_CFLAGS)
ifeq ($(OS_NAME), Windows)
CFLAGS += -DGLTANG_STATIC
CXXFLAGS += -DGLTANG_STATIC
endif
LIB_CFLAGS := $(CFLAGS) -fvisibility=hidden -DGLTANG_BUILD $(EXTRA_CFLAGS)
LDFLAGS := -L /usr/lib -lstdc++ -lm $(EXTRA_LDFLAGS)
ifdef PREFIX
LDFLAGS += -Wl,-rpath,$(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Windows)
export PATH := $(BIN_INSTALL_PATH):$(PATH)
endif
endif

BUILD_DIR := ./build/$(BUILD)
OBJ_DIR := $(BUILD_DIR)/objects
FLAGS_STAMP := $(OBJ_DIR)/.flags
GEN_DIR := $(BUILD_DIR)/generated
APP_DIR := $(BUILD_DIR)/apps

ifeq ($(UNAME_S), Linux)
	LIB_CFLAGS += -fPIC
endif

INCLUDE := -I include/ -I $(GEN_DIR)/

DEPLESS_GOALS := docs docs-pdf clean fuzz-clean cloc help
ifeq ($(filter-out $(DEPLESS_GOALS),$(or $(MAKECMDGOALS),all)),)
SKIP_DEP_CHECK := 1
endif

# The name carries $(BRANCH). ghoti.io-cutil never matches the installed file.
CUTIL_PC ?= ghoti.io-cutil$(BRANCH)
CUTIL_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(CUTIL_PC) 2>/dev/null)
CUTIL_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(CUTIL_PC) 2>/dev/null)
ifeq ($(strip $(CUTIL_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-cutil was not found by pkg-config. Run ./bootstrap.sh at the root of the workspace - two levels up, the directory holding libs/ - to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback.)
endif
endif
INCLUDE += $(CUTIL_CFLAGS)

# unicode: grapheme boundaries and UTF-8 validation for the string type.
UNICODE_PC ?= ghoti.io-unicode$(BRANCH)
UNICODE_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(UNICODE_PC) 2>/dev/null)
UNICODE_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(UNICODE_PC) 2>/dev/null)
ifeq ($(strip $(UNICODE_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-unicode was not found by pkg-config. Run ./bootstrap.sh at the root of the workspace - two levels up, the directory holding libs/ - to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback.)
endif
endif
INCLUDE += $(UNICODE_CFLAGS)

# runtime-core and runtime-heap: the engine runs on the frame protocol and the
# collector (AD-8, AD-11). Hard errors that name the fix, like the others.
RCORE_PC ?= ghoti.io-runtime-core$(BRANCH)
RCORE_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(RCORE_PC) 2>/dev/null)
RCORE_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(RCORE_PC) 2>/dev/null)
ifeq ($(strip $(RCORE_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-runtime-core was not found by pkg-config. Run ./bootstrap.sh at the root of the workspace - two levels up, the directory holding libs/ - to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback.)
endif
endif
INCLUDE += $(RCORE_CFLAGS)

RHEAP_PC ?= ghoti.io-runtime-heap$(BRANCH)
RHEAP_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(RHEAP_PC) 2>/dev/null)
RHEAP_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(RHEAP_PC) 2>/dev/null)
ifeq ($(strip $(RHEAP_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-runtime-heap was not found by pkg-config. Run ./bootstrap.sh at the root of the workspace - two levels up, the directory holding libs/ - to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback.)
endif
endif
INCLUDE += $(RHEAP_CFLAGS)

# runtime-jit: the baseline JIT's code generator (story 15), a hard dependency
# of JIT=yes and nothing at all of JIT=no. It is in INCLUDE and DEP_LIBS only
# for JIT=yes, so a JIT=no library neither includes nor links it, and
# tools/check-edges.sh checks that by file and by NEEDED entry for both arms.
ifeq ($(JIT),yes)
RJIT_PC ?= ghoti.io-runtime-jit$(BRANCH)
RJIT_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(RJIT_PC) 2>/dev/null)
RJIT_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(RJIT_PC) 2>/dev/null)
ifeq ($(strip $(RJIT_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-runtime-jit was not found by pkg-config. The baseline JIT needs it. Run ./bootstrap.sh at the root of the workspace - two levels up, the directory holding libs/ - to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file - or pass JIT=no to build the interpreter-only engine. There is deliberately no sibling-checkout fallback.)
endif
endif
JIT_CFLAGS := -DGLTANG_WITH_JIT $(RJIT_CFLAGS)
else
RJIT_PC :=
RJIT_CFLAGS :=
RJIT_LIBS :=
JIT_CFLAGS :=
endif
INCLUDE += $(JIT_CFLAGS)

# The JIT module holds the poll helper, which finds the compiled frame through
# its own frame-pointer chain (src/jit/helpers.c), so it is built with frame
# pointers whatever the optimiser would do. The sanitizer trees already are.
JIT_MODULE_CFLAGS := -fno-omit-frame-pointer

# runtime-debug and text: for the two hosts, the `tang` command (src/tang.c)
# and the web-server example (examples/web_server.c), and for nothing else
# (AD-2, story 13). They are not in INCLUDE or DEP_LIBS, so the shared and the
# static library link neither, and tools/check-edges.sh checks that by file and
# by NEEDED entry. text is listed because runtime-debug reads JSON with it; the
# debugger's own .pc file requires it, and it is a hard error to be without
# either, like every other dependency. WITH_DEBUG=no is the way to build the
# library and its unit tests on a machine without them: the two hosts then
# refuse to build, by name, instead of building without --dap.
WITH_DEBUG ?= yes
ifeq ($(WITH_DEBUG),yes)
RDEBUG_PC ?= ghoti.io-runtime-debug$(BRANCH)
RDEBUG_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(RDEBUG_PC) 2>/dev/null)
RDEBUG_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(RDEBUG_PC) 2>/dev/null)
ifeq ($(strip $(RDEBUG_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-runtime-debug was not found by pkg-config. The tang command and the web-server example use it. Run ./bootstrap.sh at the root of the workspace - two levels up, the directory holding libs/ - to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file - or pass WITH_DEBUG=no to build the library and its tests without the two hosts. There is deliberately no sibling-checkout fallback.)
endif
endif
TEXT_PC ?= ghoti.io-text$(BRANCH)
TEXT_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(TEXT_PC) 2>/dev/null)
TEXT_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(TEXT_PC) 2>/dev/null)
ifeq ($(strip $(TEXT_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-text was not found by pkg-config. runtime-debug requires it, so the tang command and the web-server example do. Run ./bootstrap.sh at the root of the workspace - two levels up, the directory holding libs/ - to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file - or pass WITH_DEBUG=no. There is deliberately no sibling-checkout fallback.)
endif
endif
# What the two hosts add to their compile and link lines, and nothing else does.
HOST_CFLAGS := $(RDEBUG_CFLAGS) $(TEXT_CFLAGS) -DGLTANG_WITH_DEBUG
HOST_LIBS := $(RDEBUG_LIBS) $(TEXT_LIBS)
else ifeq ($(WITH_DEBUG),no)
HOST_CFLAGS :=
HOST_LIBS :=
else
$(error WITH_DEBUG must be yes or no, not '$(WITH_DEBUG)')
endif

# Everything a link needs, in dependency order: the collector, the core, then
# the libraries the ported front end uses.
DEP_LIBS := $(RJIT_LIBS) $(RHEAP_LIBS) $(RCORE_LIBS) $(UNICODE_LIBS) $(CUTIL_LIBS)

# bison and flex generate the parser and the scanner. A generator that cannot
# run stops the build and names what is missing; nothing is written in its
# place (CONVENTIONS.md section 6).
ifndef SKIP_DEP_CHECK
ifeq ($(shell command -v bison 2>/dev/null),)
$(error bison was not found. It generates the parser; install it (version 3.8.2 or later) and run make again.)
endif
ifeq ($(shell command -v flex 2>/dev/null),)
$(error flex was not found. It generates the scanner; install it and run make again.)
endif
endif

# The oracle. ctang is frozen and is the reference this engine is compared
# against (AD-16). It is linked into one runner under tests/oracle/ and nowhere
# else; ORACLE_PC names its package so that the gate can be seen to fail when
# it is missing. $(BRANCH) as for every other dependency.
ORACLE_PC ?= ghoti.io-tang$(BRANCH)
ORACLE_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(ORACLE_PC) 2>/dev/null)
ORACLE_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(ORACLE_PC) 2>/dev/null)

# src/tang.c is a host, not part of the library: it has a main().
SOURCES := $(filter-out src/tang.c,$(shell find src -type f -name '*.c'))
ifeq ($(JIT),no)
SOURCES := $(filter-out src/jit/%,$(SOURCES))
endif
GENERATED_OBJECTS := $(OBJ_DIR)/tangParser.o $(OBJ_DIR)/tangScanner.o
LIBOBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SOURCES)) $(GENERATED_OBJECTS)

TESTFLAGS := `PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs --cflags gtest`

# coverage clears this: --coverage links the gcov runtime, whose mangle_path
# check-symbols is right to reject in a shipping library.
TEST_GATES ?= check-symbols check-aliasing check-stamps check-labels \
	check-edges check-gates examples cli-test fuzz-replay test-oracle check-planted-quick

# Valgrind runs threads one at a time under a lock that is not fair by default:
# a thread that never blocks (the interpreter loop) can hold it for minutes while
# the profiler's timer thread, woken after its millisecond, waits to be
# scheduled, so a timer-driven test sees a handful of samples instead of
# thousands. --fair-sched=yes hands the lock over in turn.
VALGRIND_FLAGS := --leak-check=full --show-leak-kinds=definite,indirect,possible --track-origins=yes --error-exitcode=1 --suppressions=tests/valgrind.supp --fair-sched=yes

TEST_HELPER_SRC := $(wildcard tests/test_helpers.cpp)
TEST_HELPER_OBJ := $(patsubst tests/%.cpp,$(OBJ_DIR)/tests/%.o,$(TEST_HELPER_SRC))

# The archive, not the shared library: a static link resolves hidden symbols.
# --whole-archive because a constructor-registered object would otherwise be
# dropped. The archive is a normal prerequisite of every test, so a clean
# tree builds it; the .so is order-only because check-symbols wants it and
# the tests do not link it.
CORELIBRARY := -Wl,--whole-archive $(APP_DIR)/$(STATIC_TARGET) -Wl,--no-whole-archive $(DEP_LIBS)

# Every allocation the library makes by plain malloc, calloc or realloc - its
# own through cutil's inline gcu_malloc, flex's, and bison's stack - passes
# through test_helpers.cpp first, which can fail the Nth one. That is how the
# allocation-failure sweep reaches each arm that checks for NULL. A pass-through
# until armed, so it is linked into every test. --wrap resolves at link time and
# so works under ASan and TSan, where defining malloc outright would not.
TEST_LDFLAGS := -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free

TEST_PAIRS := $(shell find tests/unit -type f -name 'test_*.cpp' 2>/dev/null | sort | grep -v test_helpers | while read f; do \
	echo "$$f|$$(basename "$$f" .cpp | sed 's/test_/test/; s/^test\([a-z]\)/test\U\1/')"; done)
# test_tang_dap drives the real `tang` command, which exists only with
# WITH_DEBUG=yes (it links the debugger). Without it the suite is left out and
# the build says so; it is the one unit suite WITH_DEBUG=no drops. Under
# WITH_DEBUG=no the library (`all`) and single test binaries build, but `make
# test` does not: its gates (check-edges, examples, cli-test) need the two hosts.
ifeq ($(WITH_DEBUG),no)
$(info WITH_DEBUG=no: tests/unit/test_tang_dap.cpp is not built (it runs the tang command, which needs runtime-debug))
TEST_PAIRS := $(filter-out tests/unit/test_tang_dap.cpp|%,$(TEST_PAIRS))
endif
TEST_SOURCES := $(foreach pair,$(TEST_PAIRS),$(word 1,$(subst |, ,$(pair))))
TEST_NAMES := $(foreach pair,$(TEST_PAIRS),$(word 2,$(subst |, ,$(pair))))
TEST_EXECUTABLES := $(addprefix $(APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))

EXAMPLE_SOURCES := $(shell find examples -type f -name '*.c' 2>/dev/null)
EXAMPLES := $(patsubst examples/%.c,$(APP_DIR)/examples/%$(EXE_EXTENSION),$(EXAMPLE_SOURCES))

ifeq ($(WITH_DEBUG),yes)
all: $(APP_DIR)/$(TARGET) $(APP_DIR)/$(STATIC_TARGET) $(APP_DIR)/tang$(EXE_EXTENSION) ## Build the shared and static libraries and the tang command
else
all: $(APP_DIR)/$(TARGET) $(APP_DIR)/$(STATIC_TARGET) ## Build the shared and static libraries (WITH_DEBUG=no: not the tang command)
	@printf 'all: WITH_DEBUG=no, so the tang command and the web-server example were not built (they need runtime-debug)\n' >&2
endif

TEST_DEPFILES := $(foreach pair,$(TEST_PAIRS),$(OBJ_DIR)/tests/$(basename $(notdir $(word 1,$(subst |, ,$(pair))))).d)
DEPFILES := $(LIBOBJECTS:.o=.d) $(OBJ_DIR)/tang.d $(TEST_HELPER_OBJ:.o=.d) $(TEST_DEPFILES)
-include $(DEPFILES)

LIBVER_GEN := $(GEN_DIR)/ghoti.io/$(PROJECT)/libver_gen.h

.PHONY: force-libver
force-libver:

$(LIBVER_GEN): force-libver
	@if [ -z "$(LIBVER_SYMBOL)" ]; then \
		printf "### LIBVER_SYMBOL is empty ###\n" >&2; \
		printf "Every exported symbol would lose its version namespace.\n" >&2; \
		exit 1; \
	fi
	@mkdir -p $(@D)
	@printf '%s\n' \
		'// Generated by the Makefile. Do not edit; see CONVENTIONS.md section 4.' \
		'#ifndef GHOTI_IO_GLTANG_LIBVER_GEN_H' \
		'#define GHOTI_IO_GLTANG_LIBVER_GEN_H' \
		'' \
		'/** The symbol namespace for this build, from the Makefile'"'"'s BRANCH. */' \
		'#define GHOTIIO_LANG_TANG_NAME $(LIBVER_SYMBOL)' \
		'' \
		'/** Human-readable version of this build. */' \
		'#define GHOTIIO_LANG_TANG_VERSION "$(VERSION_STRING)"' \
		'' \
		'/** The same version as three integers. */' \
		'#define GHOTIIO_LANG_TANG_VERSION_MAJOR $(MAJOR_VERSION)' \
		'#define GHOTIIO_LANG_TANG_VERSION_MINOR $(VERSION_MINOR_ONLY)' \
		'#define GHOTIIO_LANG_TANG_VERSION_PATCH $(VERSION_PATCH_ONLY)' \
		'' \
		'#endif // GHOTI_IO_GLTANG_LIBVER_GEN_H' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

# The generated parser header is included, through astNode.h, by almost every
# translation unit, and nothing but this ordering makes bison run before them:
# a serial build happened to generate it first, and a -j build raced ahead and
# failed with "tangParser.h: No such file or directory". Order-only, so that a
# regenerated header does not by itself rebuild everything; the .d files carry
# the real dependency.
PARSER_HEADER := $(GEN_DIR)/ghoti.io/$(PROJECT)/ast/tangParser.h
SCANNER_HEADER := $(GEN_DIR)/flexTangScanner.h
GEN_HEADERS := $(LIBVER_GEN) $(PARSER_HEADER) $(SCANNER_HEADER)

####################################################################
# Bison and flex
####################################################################

$(PARSER_HEADER): bison/tangParser.y include/ghoti.io/$(PROJECT)/ast/astNode.h \
		include/ghoti.io/$(PROJECT)/location.h
	@printf '\n### Generating the parser ###\n'
	@mkdir -p $(@D)
	bison -v -o $(GEN_DIR)/tangParser.c --defines=$@ $<

$(GEN_DIR)/tangParser.c: $(PARSER_HEADER)

# flex writes the scanner and its header in one run. The header is the target
# with the recipe, and the scanner depends on it, so that a parallel build does
# not run flex twice or compile the parser's callers before the header exists.
$(SCANNER_HEADER): flex/tangScanner.l $(PARSER_HEADER) \
		include/ghoti.io/$(PROJECT)/unicodeString.h include/ghoti.io/$(PROJECT)/tangScanner.h
	@printf '\n### Generating the scanner ###\n'
	@mkdir -p $(@D)
	flex -o $(GEN_DIR)/tangScanner.c --header-file=$@ $<

$(GEN_DIR)/tangScanner.c: $(SCANNER_HEADER)

# The parser and the scanner are bison's and flex's output and not ours to
# edit, and gcc and clang disagree about which parts of it to complain about:
# gcc wants -Wno-unused-function for the scanner, clang additionally reports an
# unused-but-set variable in the parser and an unneeded internal declaration in
# the scanner. Each compiler accepts the other's -Wno- spelling, so one list
# serves both.
#
# -Wno-float-conversion: the scanner reads a float literal as a long double
# and the Float node stores a double. That narrowing is ctang's, and changing
# it would change which literals are the same value (see design.md).
GENERATED_CFLAGS := -Wno-unused-function -Wno-unused-but-set-variable \
                    -Wno-unneeded-internal-declaration -Wno-float-conversion

# The ported AST: see the header comment for why level 3. clang accepts
# -Wstrict-aliasing=1 and implements nothing behind it, and rejects =3 as an
# unknown option, so under clang there is nothing to relax.
ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
AST_CFLAGS :=
else
AST_CFLAGS := -Wstrict-aliasing=3
endif

$(OBJ_DIR)/%.o: src/%.c $(FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(OBJ_DIR)/ast/%.o: src/ast/%.c $(FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(AST_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# The compiler reads the tree through the same struct-inheritance downcast the
# tree is built on, so it takes the same relaxation.
$(OBJ_DIR)/compile/compile.o: src/compile/compile.c $(FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(AST_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# The baseline JIT's module, with frame pointers (see JIT_MODULE_CFLAGS).
$(OBJ_DIR)/jit/%.o: src/jit/%.c $(FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(JIT_MODULE_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Explicit rules replace the pattern rule's prerequisites rather than adding to
# them, so these name the flags stamp and the generated headers themselves. The
# stamp goes after the source so that $< is still the source.
$(OBJ_DIR)/tangParser.o: $(GEN_DIR)/tangParser.c $(FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(AST_CFLAGS) $(GENERATED_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(OBJ_DIR)/tangScanner.o: $(GEN_DIR)/tangScanner.c $(FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(AST_CFLAGS) $(GENERATED_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(APP_DIR)/$(TARGET): $(LIBOBJECTS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) $(DEP_LIBS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG)
ifeq ($(OS_NAME), Linux)
	@ln -f -s $(TARGET) $(APP_DIR)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(APP_DIR)/$(BASE_NAME)
endif

$(APP_DIR)/$(STATIC_TARGET): $(LIBOBJECTS)
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

# The tang command: a host, linked against the shared library as a consumer
# would be, so that `readelf -d` on it shows what an installed copy needs.
$(OBJ_DIR)/tang.o: src/tang.c $(FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) $(HOST_CFLAGS) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

ifeq ($(WITH_DEBUG),yes)
$(APP_DIR)/tang$(EXE_EXTENSION): $(OBJ_DIR)/tang.o $(APP_DIR)/$(TARGET)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/tang.o -L$(APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH) $(LDFLAGS) $(HOST_LIBS) $(DEP_LIBS) -Wl,-rpath,$(abspath $(APP_DIR))
else
# A tang without --dap is not a build this makefile makes: the command either
# has the debugger or does not exist, so that nobody runs one believing it has.
# (force-flags is phony: without a prerequisite that is never current, a tang
# built earlier with the debugger would satisfy this rule and nothing would say
# that this build does not make one.)
$(APP_DIR)/tang$(EXE_EXTENSION): force-flags
	@printf 'make: the tang command is a host of runtime-debug (--dap) and WITH_DEBUG=no was given, so it is not built. Use WITH_DEBUG=yes with ghoti.io-runtime-debug installed.\n' >&2
	@exit 1
endif

ifneq ($(TEST_HELPER_SRC),)
$(TEST_HELPER_OBJ): $(TEST_HELPER_SRC) $(FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@
endif

$(OBJ_DIR)/tests/%.o: tests/%.cpp $(FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(OBJ_DIR)/tests/%.o: tests/unit/%.cpp $(FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

define test-executable-rule
TEST_OBJ_$1 := $(OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(APP_DIR)/$2$(EXE_EXTENSION): $$(TEST_OBJ_$1) $(TEST_HELPER_OBJ) \
		$(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@mkdir -p $$(@D)
	$(CXX) $(CXXFLAGS) -o $$@ $$(TEST_OBJ_$1) $(TEST_HELPER_OBJ) $(LDFLAGS) $(TEST_LDFLAGS) $(CORELIBRARY) $(CUTIL_LIBS) $(TESTFLAGS)
endef

$(foreach pair,$(TEST_PAIRS),\
	$(eval $(call test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

# The suite that runs the real command needs it built, in every tree: the
# sanitizer and valgrind runs of it still drive the ordinary release `tang`.
ifeq ($(WITH_DEBUG),yes)
$(APP_DIR)/testTang_dap$(EXE_EXTENSION): | $(APP_DIR)/tang$(EXE_EXTENSION)
endif

$(APP_DIR)/examples/%$(EXE_EXTENSION): examples/%.c $(APP_DIR)/$(STATIC_TARGET) \
		$(FLAGS_STAMP) | $(APP_DIR)/$(TARGET) $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(LDFLAGS) $(CORELIBRARY) $(CUTIL_LIBS)

# The web-server example is a host of the debugger: it links runtime-debug and
# text, which no other example may (tools/check-edges.sh).
ifeq ($(WITH_DEBUG),yes)
$(APP_DIR)/examples/web_server$(EXE_EXTENSION): examples/web_server.c $(APP_DIR)/$(STATIC_TARGET) \
		$(FLAGS_STAMP) | $(APP_DIR)/$(TARGET) $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) $(HOST_CFLAGS) -MMD -MP -MF $(@D)/web_server.d -o $@ $< $(LDFLAGS) $(CORELIBRARY) $(HOST_LIBS) $(CUTIL_LIBS) -pthread
-include $(APP_DIR)/examples/web_server.d
else
$(APP_DIR)/examples/web_server$(EXE_EXTENSION): force-flags
	@printf 'make: the web-server example is a host of runtime-debug and WITH_DEBUG=no was given, so it is not built. Use WITH_DEBUG=yes with ghoti.io-runtime-debug installed.\n' >&2
	@exit 1
endif

.PHONY: clean cloc docs docs-pdf examples coverage check-symbols check-stamps check-aliasing test-nojit
.PHONY: check-planted check-planted-quick check-planted-slow check-planted-selftest
.PHONY: check-labels check-edges check-gates bench test-tsan test-torture test-oracle fuzz-diff cli-test fuzz-replay fuzz-parse
.PHONY: all install test test-quiet test-asan test-valgrind test-valgrind-quiet test-watch uninstall watch
.PHONY: all-debug install-debug test-debug test-valgrind-debug test-watch-debug uninstall-debug watch-debug
.PHONY: fuzz fuzz-clean

watch: ## Watch sources and rebuild
	@while true; do \
		make --no-print-directory all; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile; \
		done

test-watch: ## Watch sources and rerun the tests
	@while true; do \
		make --no-print-directory test; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile; \
		done

TEST_LD_PATH := $(APP_DIR):$(LIB_INSTALL_PATH)/$(SUITE)

# Builds each example and runs it: an example that is only built can rot into
# something that compiles and does nothing it claims. It is a test gate for
# the same reason (a failing example fails `make test`).
# The web server is a server: run with --self-test it plays its own client,
# checks what it served, and exits.
examples: $(APP_DIR)/$(TARGET) $(EXAMPLES) ## Build the examples and run each
	@for e in $(EXAMPLES); do \
		printf '\n### Example %s ###\n\n' "$$(basename $$e $(EXE_EXTENSION))"; \
		case "$$(basename $$e $(EXE_EXTENSION))" in \
			web_server) args=--self-test ;; \
			*) args= ;; \
		esac; \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$e $$args || exit 1; \
	done

# clang accepts -Wstrict-aliasing and implements nothing, so under clang the
# probe can never be reported and the gate would fail for a reason that says
# nothing about the code. It is skipped there, by name, and only there: the gcc
# job is the instrument (CI runs both compilers), and gcc without the warning
# armed still fails below.
ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
check-aliasing: ## Skipped under clang, which does not implement -Wstrict-aliasing
	@printf 'check-aliasing: skipped under clang, which does not implement -Wstrict-aliasing; the gcc build is the gate\n'
else
check-aliasing: ## Fail if -Wstrict-aliasing is not armed at level 1
	@mkdir -p $(BUILD_DIR)
	@printf 'int gltang_alias_probe(float * f);\nint gltang_alias_probe(float * f) { int * i = (int *)f; *i = 7; return *i; }\n' > $(BUILD_DIR)/alias_probe.c
	@printf 'int gltang_alias_clean(int * i);\nint gltang_alias_clean(int * i) { *i = 7; return *i; }\n' > $(BUILD_DIR)/alias_clean.c
	@probe=$$($(CC) $(CFLAGS) -Wno-error -c $(BUILD_DIR)/alias_probe.c -o $(BUILD_DIR)/alias_probe.o 2>&1); \
	ctl=$$($(CC) $(CFLAGS) -Wno-error -c $(BUILD_DIR)/alias_clean.c -o $(BUILD_DIR)/alias_clean.o 2>&1); ctlrc=$$?; \
	if [ $$ctlrc -ne 0 ]; then \
		printf 'check-aliasing: the control file did not compile, so this gate is measuring nothing:\n%s\n' "$$ctl" >&2; \
		exit 1; \
	fi; \
	if printf '%s' "$$ctl" | grep -q 'strict-aliasing'; then \
		printf 'check-aliasing: the control file drew a strict-aliasing diagnostic, so the probe proves nothing:\n%s\n' "$$ctl" >&2; \
		exit 1; \
	fi; \
	if printf '%s' "$$probe" | grep -q 'strict-aliasing'; then \
		printf 'check-aliasing: the planted violation is reported\n'; \
		exit 0; \
	fi; \
	qout=$$($(CC) -Q --help=warnings $(CFLAGS) 2>/dev/null); qrc=$$?; \
	level=$$(printf '%s' "$$qout" | awk '/-Wstrict-aliasing=</{print $$2}'); \
	if [ $$qrc -ne 0 ] || [ -z "$$level" ]; then \
		printf 'check-aliasing: no diagnostic, and %s reports no -Wstrict-aliasing level. That is a compiler which accepts the option and implements nothing.\n' "$(CC)" >&2; \
	elif [ "$$level" = 1 ]; then \
		printf 'check-aliasing: level 1 is set and the planted store still drew no diagnostic.\n' >&2; \
	else \
		printf 'check-aliasing: CFLAGS resolves to -Wstrict-aliasing=%s; only level 1 reports this probe.\n' "$$level" >&2; \
	fi; \
	exit 1
endif

check-stamps: ## Fail if a compile rule names no flags stamp, or a stamp omits a variable
	@python3 tools/check-stamps.py

####################################################################
# Layering gates (AD-2, AD-3, AD-14)
#
# Each script takes a root directory, so check-gates can run the real script
# against tests/gates fixtures: a planted defect that must fail and a control
# that must pass. A gate that has never been seen to fail is a gate that may
# measure nothing, and each one also fails on an empty population.
####################################################################

check-labels: ## Fail if a public header has no (or the wrong) stable/free label
	@tools/check-labels.sh .

# After the shared library is built, because the link line it reads is that
# file's NEEDED list: the manifest and the #include lines can both be clean
# while the .so, or the tang command that links it, links something forbidden.
check-edges: $(APP_DIR)/$(TARGET) $(APP_DIR)/tang$(EXE_EXTENSION) $(EXAMPLES) ## Fail on a forbidden #include or NEEDED edge (AD-2)
	@GLTANG_EDGES_JIT=$(JIT) tools/check-edges.sh --includes .
	@GLTANG_EDGES_JIT=$(JIT) tools/check-edges.sh --links $(APP_DIR)/$(TARGET) $(APP_DIR)/tang$(EXE_EXTENSION) $(EXAMPLES)

check-gates: ## Prove each gate fails on its planted defect and passes its control
	@env -u GLTANG_EDGES_JIT CC="$(CC)" tools/check-gates.sh

####################################################################
# Planted defects in the library itself (CAP-7)
#
# tools/check-planted.sh builds a throwaway copy under build/planted/, applies
# one patch from tests/planted/ at a time, and requires the test named for it
# to fail and, with the patch out, to pass. Nothing in this tree is changed.
# `make test` runs the quick cases (about a minute); the two torture cases are
# part of `make test-torture`; `make check-planted` runs all twelve.
####################################################################

PLANTED_ENV = PLANTED_JIT="$(JIT)" PLANTED_PREFIX="$(PREFIX)" PLANTED_LIBDIR="$(LIB_INSTALL_PATH)/$(SUITE)" PKG_CONFIG_PATH="$(PKG_CONFIG_PATH_ENV)"

check-planted-quick: ## Planted defects 03 to 12 (phase shuffle, native gate, frame observer, oracle, the JIT, the two of snapshots)
	@$(PLANTED_ENV) tools/check-planted.sh --quick

check-planted-slow: ## Planted defects 01 and 02 (missing root, missing gc_store) under GC torture, and the script's own self-test
	@$(PLANTED_ENV) tools/check-planted.sh --slow
	@$(PLANTED_ENV) tools/check-planted.sh --selftest

check-planted: ## All ten planted defects: each caught by its instrument, each control passing
	@$(PLANTED_ENV) tools/check-planted.sh --all

check-planted-selftest: ## The script fails on a patch that matches nothing and on one that breaks nothing
	@$(PLANTED_ENV) tools/check-planted.sh --selftest

####################################################################
# Benchmarks (AD-26)
####################################################################

BENCH_SOURCES := $(shell find bench -type f -name '*.c' 2>/dev/null | sort)
BENCH_EXECUTABLES := $(patsubst bench/%.c,$(APP_DIR)/bench/%$(EXE_EXTENSION),$(BENCH_SOURCES))
-include $(patsubst bench/%.c,$(APP_DIR)/bench/%.d,$(BENCH_SOURCES))

$(APP_DIR)/bench/%$(EXE_EXTENSION): bench/%.c $(APP_DIR)/$(STATIC_TARGET) \
		$(FLAGS_STAMP) | $(APP_DIR)/$(TARGET) $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -MMD -MP -MF $(@D)/$(*F).d -o $@ $< $(LDFLAGS) $(CORELIBRARY) $(CUTIL_LIBS)

bench: $(BENCH_EXECUTABLES) ## Run the benchmark harness (prints a calibration result first)
ifeq ($(strip $(BENCH_EXECUTABLES)),)
	@printf 'bench: no benchmark sources under bench/, so this measures nothing\n' >&2; exit 1
endif
	@for b in $(BENCH_EXECUTABLES); do \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$b || exit 1; \
	done

####################################################################
# Symbols
####################################################################

check-symbols: $(APP_DIR)/$(TARGET) ## Fail if any exported symbol lacks the version namespace
ifeq ($(OS_NAME), Linux)
	@leaked=$$(nm -D --defined-only $(APP_DIR)/$(TARGET) \
		| awk '$$2 ~ /^[TDBR]$$/ {print $$3}' \
		| grep -v '^$(LIBVER_SYMBOL)_' | grep -v '^_' || true); \
	if [ -n "$$leaked" ]; then \
		printf '### Exported symbols missing the $(LIBVER_SYMBOL)_ namespace ###\n%s\n' "$$leaked" >&2; \
		exit 1; \
	fi
	@unexported=$$(find include -name '*.h' -exec awk '/^#if DOXYGEN/{d=1} d==0 && !/GLTANG_API/ && /^[A-Za-z_][A-Za-z0-9_ ]*\**[[:space:]]*gltang_[a-z0-9_]+[[:space:]]*\(/{print FILENAME": "$$0} /^#endif/{d=0}' {} + \
		| grep -vE 'typedef|static inline' || true); \
	if [ -n "$$unexported" ]; then \
		printf '### Public declarations without GLTANG_API ###\n%s\n' "$$unexported" >&2; \
		exit 1; \
	fi
	@missing=$$(grep -h 'GLTANG_API' include/ghoti.io/lang-tang/*.h \
		| grep -oE 'gltang_[a-z0-9_]+\(' | tr -d '(' | sort -u \
		| while read -r f; do \
			nm -D --defined-only $(APP_DIR)/$(TARGET) | awk '{print $$3}' \
				| grep -qx "$(LIBVER_SYMBOL)_$$f" || echo "$$f"; \
		done); \
	if [ -n "$$missing" ]; then \
		printf '### Declared GLTANG_API functions the shared library does not export ###\n%s\n' "$$missing" >&2; \
		printf '(a definition whose translation unit never saw its declaration is hidden by -fvisibility=hidden, and the tests link the archive, so they cannot see it)\n' >&2; \
		exit 1; \
	fi
	@split=$$(nm -D --undefined-only $(APP_DIR)/$(TARGET) \
		| awk '{print $$2}' | grep '^$(LIBVER_SYMBOL)_' || true); \
	if [ -n "$$split" ]; then \
		printf '### Renamed but undefined - a split symbol ###\n%s\n' "$$split" >&2; \
		exit 1; \
	fi
	@nomacros=$$(find include src -name '*.h' \
		! -name 'libver.h' ! -name 'libver_gen.h' ! -name 'namespace.h' ! -name 'macros.h' \
		-exec grep -L '#include <ghoti.io/lang-tang/macros.h>' {} + || true); \
	if [ -n "$$nomacros" ]; then \
		printf '### Headers that do not include macros.h ###\n%s\n' "$$nomacros" >&2; \
		exit 1; \
	fi
	@badguards=$$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $$2; d=1}' {} + \
		| awk '$$1 !~ /^GHOTI_IO_GLTANG_/ {print $$1}' || true); \
	if [ -n "$$badguards" ]; then \
		printf '### Include guards with the wrong prefix ###\n%s\n' "$$badguards" >&2; \
		exit 1; \
	fi
	@dupguards=$$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $$2; d=1}' {} + \
		| sort | uniq -d || true); \
	if [ -n "$$dupguards" ]; then \
		printf '### Headers sharing an include guard ###\n%s\n' "$$dupguards" >&2; \
		exit 1; \
	fi
	@printf 'Every exported symbol carries the %s_ namespace.\n' "$(LIBVER_SYMBOL)"
else
	@printf 'check-symbols: skipped (Linux only)\n'
endif

# Every unit suite runs again with the heap collecting before every allocation
# and checking every store (torture and verify), and again with a guest stack
# that moves on every push, and `make test-torture` runs all of them under ASan
# and UBSan with all three at once. `make test` runs the engine-driving suites
# (TORTURE_BOUNDED) in both modes, which keeps it short; test-torture runs
# EVERY suite (TORTURE_SUITES) except the ones listed with their reason in
# TORTURE_EXCLUDED.
#
# Excluded from test-torture, with the reason each:
#   (none) - every unit suite of this library runs there. The thread-migration
#   tests (testEngine, testTemplate) run under it as under plain ASan: the
#   collector's torture mode is per context and the migrations are of whole
#   contexts. test-tsan runs them under the thread sanitizer separately. The
#   allocation-failure sweep (testOom) fails the Nth allocation of each run, and
#   under torture an allocation also collects, which only makes it slower.
TORTURE_EXCLUDED :=
TORTURE_SUITES := $(filter-out $(TORTURE_EXCLUDED),$(TEST_NAMES))
TORTURE_BOUNDED := testExecute_simple testExecute_complex testEngine testCompile testLibrary testRandom testErrors testTemplate testGen testObserver testNative_gate testExec_corpus

# With the JIT built, `make test` is two arms. The JIT arm is everything below,
# and then the same unit suites once more with every execution tiering up at
# its first poll (GLTANG_TEST_JIT_THRESHOLD=1), which is how the suites that
# were written for the interpreter become a differential against the JIT. The
# interpreter-only arm (test-nojit) is the library built with JIT=no in a tree
# of its own, running the whole unit suite, the command's test, the examples and
# the gates that apply; it does not repeat the torture modes, which exercise
# the loop it shares with the JIT arm.
NOJIT_GATES := check-symbols check-aliasing check-stamps check-labels check-edges check-gates examples cli-test fuzz-replay
JIT_THRESHOLD_ENV := GLTANG_TEST_JIT_THRESHOLD=1

test: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) $(BENCH_EXECUTABLES) $(TEST_GATES) ## Build and run the tests
	@for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf '\n### Running %s ###\n\n' "$$test_name"; \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$test_exe --gtest_brief=1 || exit 1; \
	done
ifndef GLTANG_NESTED_ARM
	@for mode in "GRHEAP_TORTURE=1 GRHEAP_VERIFY=1" "GLTANG_TEST_MOVING_STACK=1"; do \
		for t in $(TORTURE_BOUNDED); do \
			printf '\n### %s with %s ###\n\n' "$$t" "$$mode"; \
			env $$mode LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(APP_DIR)/$$t$(EXE_EXTENSION) --gtest_brief=1 || exit 1; \
		done; \
	done
ifeq ($(JIT),yes)
	@for mode in "$(JIT_THRESHOLD_ENV)" "$(JIT_THRESHOLD_ENV) GLTANG_TEST_MOVING_STACK=1" "$(JIT_THRESHOLD_ENV) GRHEAP_TORTURE=1 GRHEAP_VERIFY=1"; do \
		for t in $(TORTURE_BOUNDED); do \
			printf '\n### %s with %s ###\n\n' "$$t" "$$mode"; \
			env $$mode LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(APP_DIR)/$$t$(EXE_EXTENSION) --gtest_brief=1 || exit 1; \
		done; \
	done
endif
endif
	@if [ -z "$(strip $(BENCH_EXECUTABLES))" ]; then \
		printf 'test: no benchmark harness under bench/ (AD-26 requires one)\n' >&2; exit 1; \
	fi
	@for b in $(BENCH_EXECUTABLES); do \
		printf '\n### Benchmark smoke %s ###\n\n' "$$(basename $$b $(EXE_EXTENSION))"; \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$b --smoke || exit 1; \
	done
ifeq ($(JIT),yes)
ifndef GLTANG_NESTED_ARM
	@$(MAKE) --no-print-directory test-nojit
endif
endif

# The interpreter-only arm: the library built with JIT=no, in its own tree, and
# its suites (AD-2). It is a nested make, so every variable given on this
# command line (PREFIX, EXTRA_CFLAGS, CC) reaches it.
test-nojit: ## The JIT=no arm: build without the JIT in its own tree and run its suites and gates
	@printf '\n### The interpreter-only arm (JIT=no) ###\n\n'
	@$(MAKE) --no-print-directory test JIT=no GLTANG_NESTED_ARM=1 TEST_GATES="$(NOJIT_GATES)"

test-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) ## Run tests, one line per suite
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; any_failed=0; \
	printf '\n%-30s %8s %10s %s\n' "Test Suite" "Tests" "Time" "Status"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		if [ $$exit_code -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf '%-30s %8d %8dms PASS\n' "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			any_failed=1; \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=$$num_tests; \
			[ "$$failures" -eq 0 ] && failures=1; \
			total_failed=$$((total_failed + failures)); \
			total_passed=$$((total_passed + num_tests - failures)); \
			printf '%-30s %8d %8dms FAIL (exit %d)\n' "$$test_name" "$$num_tests" "$$time_ms" "$$exit_code"; \
			failed_suites="$$failed_suites\n=== $$test_name FAILURES ===\n$$output\n"; \
		fi; \
	done; \
	if [ $$any_failed -eq 0 ]; then \
		printf '%-30s %8d %6dms PASS\n\n' "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf '%-30s %8d %6dms FAIL (%d failed)\n' "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf '%s\n' "$$failed_suites"; \
		exit 1; \
	fi

test-valgrind: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) ## Run the tests under Valgrind
ifeq ($(OS_NAME), Linux)
	@for test_exe in $(TEST_EXECUTABLES); do \
		printf '\n### Valgrind %s ###\n\n' "$$(basename $$test_exe)"; \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1 || exit 1; \
	done
else
	@printf 'Valgrind is only available on Linux\n' >&2; exit 1
endif

test-valgrind-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) ## Valgrind, one line per suite
ifeq ($(OS_NAME), Linux)
	@total_tests=0; total_failed=0; total_time=0; failed_suites=""; \
	printf '\n%-30s %8s %10s %s\n' "Test Suite (Valgrind)" "Tests" "Time" "Status"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(TEST_LD_PATH)" valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		has_leak=$$(echo "$$output" | grep -c "are definitely lost\|are indirectly lost\|are possibly lost" || true); \
		if [ $$exit_code -eq 0 ] && [ $$has_leak -eq 0 ]; then \
			printf '%-30s %8d %8dms PASS\n' "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			total_failed=$$((total_failed + 1)); \
			printf '%-30s %8d %8dms FAIL\n' "$$test_name" "$$num_tests" "$$time_ms"; \
			failed_suites="$$failed_suites\n=== $$test_name ===\n$$output\n"; \
		fi; \
	done; \
	if [ $$total_failed -eq 0 ]; then \
		printf '%-30s %8d %6dms PASS\n\n' "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf '%-30s %8d %6dms FAIL (%d suites)\n' "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf '%s\n' "$$failed_suites"; \
		exit 1; \
	fi
else
	@printf 'Valgrind is only available on Linux\n' >&2; exit 1
endif

####################################################################
# ASan + UBSan, in their own tree
####################################################################

UBSAN_CHECKS := undefined,float-cast-overflow
ASAN_UBSAN_FLAGS := -fsanitize=address,$(UBSAN_CHECKS) -fno-sanitize-recover=$(UBSAN_CHECKS) -fno-omit-frame-pointer -g -O1
# clang's `undefined` group includes `enum`, which GCC's does not: it reports a
# load of an enum value outside the enumerators. The tests pass such values on
# purpose (static_cast<Kind>(99)) to prove the C API refuses an unknown kind,
# which is undefined in C++ but is exactly the input under test, so the check
# is switched off under clang and the compilers sanitize the same set.
ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
ASAN_UBSAN_FLAGS += -fno-sanitize=enum
endif
COV_BUILD_DIR := ./build/$(BUILD)-cov
ASAN_BUILD_DIR := ./build/$(BUILD)-asan
ASAN_OBJ_DIR := $(ASAN_BUILD_DIR)/objects
ASAN_FLAGS_STAMP := $(ASAN_OBJ_DIR)/.flags
ASAN_APP_DIR := $(ASAN_BUILD_DIR)/apps
ASAN_LIBOBJECTS := $(patsubst src/%.c,$(ASAN_OBJ_DIR)/%.o,$(SOURCES)) $(ASAN_OBJ_DIR)/tangParser.o $(ASAN_OBJ_DIR)/tangScanner.o
ASAN_DEPFILES := $(ASAN_LIBOBJECTS:.o=.d) \
    $(foreach pair,$(TEST_PAIRS),$(ASAN_OBJ_DIR)/tests/$(basename $(notdir $(word 1,$(subst |, ,$(pair))))).d)
-include $(ASAN_DEPFILES)
ASAN_ARCHIVE := $(ASAN_APP_DIR)/$(STATIC_TARGET)
ASAN_TEST_HELPER_OBJ := $(patsubst tests/%.cpp,$(ASAN_OBJ_DIR)/tests/%.o,$(TEST_HELPER_SRC))
ASAN_CORELIBRARY := -Wl,--whole-archive $(ASAN_ARCHIVE) -Wl,--no-whole-archive $(DEP_LIBS)
ASAN_CFLAGS := $(CFLAGS) $(ASAN_UBSAN_FLAGS) -DGLTANG_BUILD
ASAN_CXXFLAGS := $(CXXFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_LDFLAGS := $(LDFLAGS) $(ASAN_UBSAN_FLAGS)
ifeq ($(UNAME_S), Linux)
	ASAN_CFLAGS += -fPIC
endif

$(ASAN_OBJ_DIR)/%.o: src/%.c $(ASAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ASAN_OBJ_DIR)/ast/%.o: src/ast/%.c $(ASAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(AST_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ASAN_OBJ_DIR)/compile/compile.o: src/compile/compile.c $(ASAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(AST_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ASAN_OBJ_DIR)/tangParser.o: $(GEN_DIR)/tangParser.c $(ASAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(AST_CFLAGS) $(GENERATED_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ASAN_OBJ_DIR)/tangScanner.o: $(GEN_DIR)/tangScanner.c $(ASAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(AST_CFLAGS) $(GENERATED_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ASAN_ARCHIVE): $(ASAN_LIBOBJECTS)
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

$(ASAN_OBJ_DIR)/tests/%.o: tests/%.cpp $(ASAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ASAN_OBJ_DIR)/tests/%.o: tests/unit/%.cpp $(ASAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

define asan-test-executable-rule
ASAN_TEST_OBJ_$1 := $(ASAN_OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(ASAN_APP_DIR)/$2$(EXE_EXTENSION): $$(ASAN_TEST_OBJ_$1) $(ASAN_TEST_HELPER_OBJ) $(ASAN_ARCHIVE)
	@mkdir -p $$(@D)
	$(CXX) $(ASAN_CXXFLAGS) -o $$@ $$(ASAN_TEST_OBJ_$1) $(ASAN_TEST_HELPER_OBJ) $(ASAN_LDFLAGS) $(TEST_LDFLAGS) $(ASAN_CORELIBRARY) $(CUTIL_LIBS) $(TESTFLAGS)
endef

$(foreach pair,$(TEST_PAIRS),\
	$(eval $(call asan-test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

ifeq ($(WITH_DEBUG),yes)
$(ASAN_APP_DIR)/testTang_dap$(EXE_EXTENSION): | $(APP_DIR)/tang$(EXE_EXTENSION)
endif

ASAN_TEST_EXECUTABLES := $(addprefix $(ASAN_APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))
ASAN_RUNTIME := $(shell $(CC) -print-file-name=libasan.so 2>/dev/null)
# How the sanitized test programs are started. GCC's runtime is a shared
# libasan.so, which has to be preloaded because the libraries under test are
# loaded by a program that was not linked against it. clang links its own
# runtime statically into every program it builds with -fsanitize=address, so
# there is nothing to preload - and preloading libasan.so beside it, or
# anything else (this workstation's desktop sets LD_PRELOAD), makes the runtime
# abort with "ASan runtime does not come first". So with clang the preload is
# emptied instead of set.
ifneq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
ASAN_PRELOAD = LD_PRELOAD=
else
ASAN_PRELOAD = LD_PRELOAD="$(ASAN_RUNTIME)$${LD_PRELOAD:+:$$LD_PRELOAD}"
endif

test-asan: $(ASAN_TEST_EXECUTABLES) ## Build with ASan+UBSan and run the tests
	@for test_exe in $(ASAN_TEST_EXECUTABLES); do \
		printf '\n### ASan+UBSan %s ###\n\n' "$$(basename $$test_exe)"; \
		$(ASAN_PRELOAD) \
		LD_LIBRARY_PATH="$(ASAN_APP_DIR):$(LIB_INSTALL_PATH)/$(SUITE)" \
			$$test_exe --gtest_brief=1 || exit 1; \
	done
	@printf '\nASan+UBSan suite clean.\n'

ASAN_TORTURE_EXECUTABLES := $(addprefix $(ASAN_APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TORTURE_SUITES)))

test-torture: $(ASAN_TORTURE_EXECUTABLES) check-planted-slow ## ASan+UBSan with the heap in torture and verify and a moving guest stack
	@for test_exe in $(ASAN_TORTURE_EXECUTABLES); do \
		printf '\n### ASan+UBSan torture %s ###\n\n' "$$(basename $$test_exe)"; \
		GRHEAP_TORTURE=1 GRHEAP_VERIFY=1 GLTANG_TEST_MOVING_STACK=1 \
		$(ASAN_PRELOAD) \
		LD_LIBRARY_PATH="$(ASAN_APP_DIR):$(LIB_INSTALL_PATH)/$(SUITE)" \
			$$test_exe --gtest_brief=1 || exit 1; \
	done
	@printf '\nASan+UBSan torture suite clean.\n'

####################################################################
# ThreadSanitizer, in its own tree
####################################################################

# Not combinable with ASan, so its own tree. -O1 keeps the instrumented run
# fast enough to be worth running.
TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer -g -O1
TSAN_BUILD_DIR := ./build/$(BUILD)-tsan
TSAN_OBJ_DIR := $(TSAN_BUILD_DIR)/objects
TSAN_FLAGS_STAMP := $(TSAN_OBJ_DIR)/.flags
TSAN_APP_DIR := $(TSAN_BUILD_DIR)/apps
TSAN_LIBOBJECTS := $(patsubst src/%.c,$(TSAN_OBJ_DIR)/%.o,$(SOURCES)) $(TSAN_OBJ_DIR)/tangParser.o $(TSAN_OBJ_DIR)/tangScanner.o
TSAN_DEPFILES := $(TSAN_LIBOBJECTS:.o=.d) \
    $(foreach pair,$(TEST_PAIRS),$(TSAN_OBJ_DIR)/tests/$(basename $(notdir $(word 1,$(subst |, ,$(pair))))).d)
-include $(TSAN_DEPFILES)
TSAN_ARCHIVE := $(TSAN_APP_DIR)/$(STATIC_TARGET)
TSAN_TEST_HELPER_OBJ := $(patsubst tests/%.cpp,$(TSAN_OBJ_DIR)/tests/%.o,$(TEST_HELPER_SRC))
TSAN_CORELIBRARY := -Wl,--whole-archive $(TSAN_ARCHIVE) -Wl,--no-whole-archive $(DEP_LIBS)
TSAN_CFLAGS := $(CFLAGS) $(TSAN_FLAGS) -DGLTANG_BUILD
TSAN_CXXFLAGS := $(CXXFLAGS) $(TSAN_FLAGS)
TSAN_LDFLAGS := $(LDFLAGS) $(TSAN_FLAGS)
ifeq ($(UNAME_S), Linux)
	TSAN_CFLAGS += -fPIC
endif

$(TSAN_OBJ_DIR)/%.o: src/%.c $(TSAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(TSAN_OBJ_DIR)/ast/%.o: src/ast/%.c $(TSAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) $(AST_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(TSAN_OBJ_DIR)/compile/compile.o: src/compile/compile.c $(TSAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) $(AST_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(TSAN_OBJ_DIR)/tangParser.o: $(GEN_DIR)/tangParser.c $(TSAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) $(AST_CFLAGS) $(GENERATED_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(TSAN_OBJ_DIR)/tangScanner.o: $(GEN_DIR)/tangScanner.c $(TSAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) $(AST_CFLAGS) $(GENERATED_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(TSAN_ARCHIVE): $(TSAN_LIBOBJECTS)
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

$(TSAN_OBJ_DIR)/tests/%.o: tests/%.cpp $(TSAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(TSAN_CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(TSAN_OBJ_DIR)/tests/%.o: tests/unit/%.cpp $(TSAN_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(TSAN_CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

define tsan-test-executable-rule
TSAN_TEST_OBJ_$1 := $(TSAN_OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(TSAN_APP_DIR)/$2$(EXE_EXTENSION): $$(TSAN_TEST_OBJ_$1) $(TSAN_TEST_HELPER_OBJ) $(TSAN_ARCHIVE)
	@mkdir -p $$(@D)
	$(CXX) $(TSAN_CXXFLAGS) -o $$@ $$(TSAN_TEST_OBJ_$1) $(TSAN_TEST_HELPER_OBJ) $(TSAN_LDFLAGS) $(TEST_LDFLAGS) $(TSAN_CORELIBRARY) $(CUTIL_LIBS) $(TESTFLAGS)
endef

$(foreach pair,$(TEST_PAIRS),\
	$(eval $(call tsan-test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

ifeq ($(WITH_DEBUG),yes)
$(TSAN_APP_DIR)/testTang_dap$(EXE_EXTENSION): | $(APP_DIR)/tang$(EXE_EXTENSION)
endif

TSAN_TEST_EXECUTABLES := $(addprefix $(TSAN_APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))

# No LD_PRELOAD, and the environment's is emptied: -fsanitize=thread at link
# time already puts libtsan first in the executable's NEEDED list, and a
# preloaded runtime is inherited by every process a test spawns.
# halt_on_error stops at the first race rather than repeating it per test.
TSAN_RUN_ENV := LD_PRELOAD= LD_LIBRARY_PATH="$(TSAN_APP_DIR):$(LIB_INSTALL_PATH)/$(SUITE)" \
	TSAN_OPTIONS="halt_on_error=1:history_size=7:second_deadlock_stack=1"

test-tsan: $(TSAN_TEST_EXECUTABLES) ## Build with TSan and run the tests (Linux only)
ifeq ($(OS_NAME), Linux)
	@for test_exe in $(TSAN_TEST_EXECUTABLES); do \
		printf '\n### TSan %s ###\n\n' "$$(basename $$test_exe)"; \
		$(TSAN_RUN_ENV) $$test_exe --gtest_brief=1 || exit 1; \
	done
	@printf '\nTSan suite clean.\n'
else
	@printf 'ThreadSanitizer builds are only supported on Linux\n' >&2; exit 1
endif

####################################################################
# The tang command
####################################################################

# tests/cli-test.sh drives the built command: the unit tests link the library
# and never run the binary, so what the command does on its own - reading a
# file, reading stdin, naming an error, its exit status - is checked here.
cli-test: $(APP_DIR)/tang$(EXE_EXTENSION) ## Run the tang command over its documented cases
	@LD_LIBRARY_PATH="$(TEST_LD_PATH)" sh tests/cli-test.sh $(APP_DIR)/tang$(EXE_EXTENSION)

####################################################################
# The ctang oracle (AD-16)
####################################################################

# Nothing else in this repository may include ctang. The runner is the one
# program that does, and it is run as a child process with a wall-clock kill, so
# that a ctang crash or hang is a verdict and not a crashed test.
ORACLE_RUNNER := $(APP_DIR)/oracle/oracle_ctang$(EXE_EXTENSION)
ORACLE_TEST := $(APP_DIR)/testOracle$(EXE_EXTENSION)
ORACLE_RPATH := $(foreach f,$(filter -L%,$(ORACLE_LIBS)),-Wl,-rpath,$(patsubst -L%,%,$f))
-include $(OBJ_DIR)/oracle/test_oracle.d

.PHONY: oracle-present
# Fails, never skips. A differential that is skipped when its reference is
# missing reports success over a population of zero, which is the failure this
# whole target exists to prevent.
oracle-present:
	@if ! PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --exists $(ORACLE_PC) 2>/dev/null; then \
		printf 'test-oracle: ctang (pkg-config package %s) was not found, and the oracle differential does not skip.\n' "$(ORACLE_PC)" >&2; \
		printf 'Run ./bootstrap.sh at the root of the workspace to install ctang, then pass the same PREFIX here,\n' >&2; \
		printf 'or point PKG_CONFIG_PATH at the directory holding ghoti.io-tang-0.pc.\n' >&2; \
		exit 1; \
	fi

$(ORACLE_RUNNER): tests/oracle/oracle_ctang.c $(FLAGS_STAMP) | oracle-present
	@mkdir -p $(@D)
	$(CC) -std=c17 -Wall -Wextra -Werror -O1 -g $(ORACLE_CFLAGS) -o $@ $< $(ORACLE_LIBS) $(ORACLE_RPATH)

$(OBJ_DIR)/oracle/test_oracle.o: tests/oracle/test_oracle.cpp $(FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ORACLE_TEST): $(OBJ_DIR)/oracle/test_oracle.o $(TEST_HELPER_OBJ) $(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ_DIR)/oracle/test_oracle.o $(TEST_HELPER_OBJ) $(LDFLAGS) $(TEST_LDFLAGS) $(CORELIBRARY) $(CUTIL_LIBS) $(TESTFLAGS)

# The differential fuzz run: programs from tests/fuzz/gen.h, each run on both
# engines, compared as the corpus is. It needs the same runner, so it is built
# here and run by test-oracle (a fixed batch) and fuzz-diff (a campaign).
FUZZDIFF_TEST := $(APP_DIR)/testFuzzDiff$(EXE_EXTENSION)
-include $(OBJ_DIR)/fuzz/test_fuzzdiff.d

$(OBJ_DIR)/fuzz/test_fuzzdiff.o: tests/fuzz/test_fuzzdiff.cpp $(FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(FUZZDIFF_TEST): $(OBJ_DIR)/fuzz/test_fuzzdiff.o $(TEST_HELPER_OBJ) $(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ_DIR)/fuzz/test_fuzzdiff.o $(TEST_HELPER_OBJ) $(LDFLAGS) $(TEST_LDFLAGS) $(CORELIBRARY) $(CUTIL_LIBS) $(TESTFLAGS)

test-oracle: oracle-present $(ORACLE_RUNNER) $(ORACLE_TEST) $(FUZZDIFF_TEST) ## Compare lang-tang with ctang over the corpus and a fixed batch of generated programs (fails, never skips, without ctang)
	@printf '\n### Oracle differential ###\n\n'
	@GLTANG_ORACLE_RUNNER=$(abspath $(ORACLE_RUNNER)) LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(ORACLE_TEST) --gtest_brief=1
	@printf '\n### Differential fuzz run, fixed batch ###\n\n'
	@GLTANG_ORACLE_RUNNER=$(abspath $(ORACLE_RUNNER)) LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(FUZZDIFF_TEST) --gtest_brief=1

# A campaign: FUZZ_DIFF_COUNT programs from seed FUZZ_DIFF_SEED, both modes
# alternating. A divergence prints its seed and the whole program.
FUZZ_DIFF_SEED ?= 1
fuzz-diff: oracle-present $(ORACLE_RUNNER) $(FUZZDIFF_TEST) ## Differential fuzz campaign: make fuzz-diff FUZZ_DIFF_COUNT=N FUZZ_DIFF_SEED=S
	@if [ -z "$(FUZZ_DIFF_COUNT)" ]; then \
		printf 'fuzz-diff: set FUZZ_DIFF_COUNT, for example: make fuzz-diff FUZZ_DIFF_COUNT=2000 FUZZ_DIFF_SEED=1\n' >&2; exit 1; \
	fi
	@printf '\n### Differential fuzz campaign: %s programs from seed %s ###\n\n' "$(FUZZ_DIFF_COUNT)" "$(FUZZ_DIFF_SEED)"
	@GLTANG_ORACLE_RUNNER=$(abspath $(ORACLE_RUNNER)) FUZZ_DIFF_COUNT=$(FUZZ_DIFF_COUNT) FUZZ_DIFF_SEED=$(FUZZ_DIFF_SEED) \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(FUZZDIFF_TEST) --gtest_brief=1 --gtest_filter=FuzzDiff.Campaign

####################################################################
# Fuzzing
####################################################################

# The replay needs no libFuzzer and no clang: it feeds every corpus and seed
# file once through the same entry points the fuzzers call, in an ordinary
# build, and fails on a crash. It is part of `make test`, so a regression that
# a fuzzer once found is a failing test and not a campaign to repeat. It is not
# a campaign; the differential fuzz run, which needs ctang, is `fuzz-diff` below.
FUZZ_REPLAYS := $(APP_DIR)/fuzz/replay_parse$(EXE_EXTENSION) $(APP_DIR)/fuzz/replay_template$(EXE_EXTENSION) \
	$(APP_DIR)/fuzz/replay_run$(EXE_EXTENSION)
-include $(APP_DIR)/fuzz/replay_parse.d $(APP_DIR)/fuzz/replay_template.d $(APP_DIR)/fuzz/replay_run.d

$(APP_DIR)/fuzz/replay_%$(EXE_EXTENSION): tests/fuzz/fuzz_%.c tests/fuzz/replay_main.c \
		$(APP_DIR)/$(STATIC_TARGET) $(FLAGS_STAMP) | $(APP_DIR)/$(TARGET) $(GEN_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -MMD -MP -MF $(APP_DIR)/fuzz/replay_$*.d -o $@ $< tests/fuzz/replay_main.c $(CORELIBRARY)

FUZZ_INPUTS = $(shell find tests/corpus tests/fuzz/corpus -type f 2>/dev/null | sort)

fuzz-replay: $(FUZZ_REPLAYS) ## Feed every corpus and seed file once through the fuzz entry points
	@if [ -z "$(strip $(FUZZ_INPUTS))" ]; then \
		printf 'fuzz-replay: no corpus or seed files; this measures nothing\n' >&2; exit 1; \
	fi
	@for r in $(FUZZ_REPLAYS); do \
		LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$r $(FUZZ_INPUTS) || { printf 'fuzz-replay: %s failed\n' "$$r" >&2; exit 1; }; \
	done

# The fuzzers themselves: libFuzzer, so clang. These instrument this library's
# own objects and nothing else; cutil and unicode are linked as ordinary shared
# libraries, so a bad access that happens inside them is not seen until the
# damage reaches memory this library owns.
FUZZ_CC ?= clang
FUZZ_CC_OK := $(shell command -v $(FUZZ_CC) 2>/dev/null)
SAN_CHECKS := $(UBSAN_CHECKS)
FUZZ_SAN := -fsanitize=address,$(SAN_CHECKS) -fno-sanitize-recover=$(SAN_CHECKS) \
            -fno-omit-frame-pointer -g -O1
FUZZ_LIB_FLAGS := $(FUZZ_SAN) -fsanitize=fuzzer-no-link
FUZZ_BIN_FLAGS := $(FUZZ_SAN) -fsanitize=fuzzer
# Everything the fuzz recipes pass, in one variable, so the stamp records the
# whole command. UNICODE_CFLAGS and CUTIL_CFLAGS come from pkg-config and so
# change when a dependency is reinstalled rather than when anyone edits this.
FUZZ_LIB_CFLAGS := $(FUZZ_LIB_FLAGS) -std=c17 -w -fPIC -DGLTANG_BUILD
FUZZ_BIN_CFLAGS := $(FUZZ_BIN_FLAGS) -std=c17 -w

FUZZ_DIR := $(BUILD_DIR)-fuzz
FUZZ_OBJ_DIR := $(FUZZ_DIR)/objects
FUZZ_FLAGS_STAMP := $(FUZZ_OBJ_DIR)/.flags
FUZZ_APP_DIR := $(FUZZ_DIR)/apps
# The corpus a run grows is working state; only the seeds are tracked, and they
# are copied in so that a run never rewrites them.
FUZZ_SEEDS := tests/fuzz/corpus
FUZZ_CORPUS := build/fuzz-corpus
# Crash artifacts, kept out of the repository root where the next `git add`
# would sweep them up.
FUZZ_ARTIFACTS := build/fuzz-artifacts
FUZZ_TIME ?= 60
FUZZ_RSS_MB ?= 2048
# A refused allocation is a code path here, not a stop (see the OOM sweep).
FUZZ_ASAN_OPTIONS ?= allocator_may_return_null=1:max_allocation_size_mb=512:quarantine_size_mb=64

FUZZ_OBJECTS := $(patsubst $(OBJ_DIR)/%,$(FUZZ_OBJ_DIR)/%,$(LIBOBJECTS))
-include $(FUZZ_OBJECTS:.o=.d)
-include $(patsubst %,$(FUZZ_APP_DIR)/%.d,fuzz_parse fuzz_template fuzz_run)

ifdef PREFIX
FUZZ_RPATH := -Wl,-rpath,$(LIB_INSTALL_PATH)/$(SUITE)
endif

$(FUZZ_OBJ_DIR)/%.o: src/%.c $(FUZZ_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	@$(FUZZ_CC) $(FUZZ_LIB_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(FUZZ_OBJ_DIR)/tangParser.o: $(GEN_DIR)/tangParser.c $(FUZZ_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	@$(FUZZ_CC) $(FUZZ_LIB_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(FUZZ_OBJ_DIR)/tangScanner.o: $(GEN_DIR)/tangScanner.c $(FUZZ_FLAGS_STAMP) | $(GEN_HEADERS)
	@mkdir -p $(@D)
	@$(FUZZ_CC) $(FUZZ_LIB_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# $1 = harness basename, $2 = target suffix
define fuzz-rule
fuzz-$2: ## Build the $2 fuzz harness (requires clang)
fuzz-$2: $$(FUZZ_APP_DIR)/$1

$$(FUZZ_APP_DIR)/$1: tests/fuzz/$1.c $$(FUZZ_OBJECTS) $$(FUZZ_FLAGS_STAMP)
	@if [ -z "$$(FUZZ_CC_OK)" ]; then \
		echo "fuzzing requires $$(FUZZ_CC); install clang or set FUZZ_CC" >&2; \
		exit 1; \
	fi
	@mkdir -p $$(@D) $$(FUZZ_CORPUS)/$2
	@printf "\n### Building $1 ###\n"
	$$(FUZZ_CC) $$(FUZZ_BIN_CFLAGS) $$(INCLUDE) \
		-MMD -MP -MF $$(FUZZ_APP_DIR)/$1.d \
		-o $$@ $$< $$(FUZZ_OBJECTS) $$(DEP_LIBS) -lstdc++ -lm $$(FUZZ_RPATH)

# env -u LD_PRELOAD: a sanitizer runtime insists on loading first, and this
# workstation's desktop session sets LD_PRELOAD for unrelated reasons.
fuzz-run-$2: ## Run the $2 fuzzer for $$(FUZZ_TIME) seconds
fuzz-run-$2: $$(FUZZ_APP_DIR)/$1
	@mkdir -p $$(FUZZ_CORPUS)/$2 $$(FUZZ_ARTIFACTS)
	@cp -n $$(FUZZ_SEEDS)/$2/* $$(FUZZ_CORPUS)/$2/ 2>/dev/null || true
	@printf "\n### Fuzzing $2 for $$(FUZZ_TIME)s ###\n"
	@env -u LD_PRELOAD LAST_INPUT_FILE=$$(FUZZ_ARTIFACTS)/last-input-$2.bin ASAN_OPTIONS=$$(FUZZ_ASAN_OPTIONS) $$(FUZZ_APP_DIR)/$1 $$(FUZZ_CORPUS)/$2 \
		-max_total_time=$$(FUZZ_TIME) \
		-timeout=10 -rss_limit_mb=$$(FUZZ_RSS_MB) -print_final_stats=1 \
		-artifact_prefix=$$(FUZZ_ARTIFACTS)/$2-
endef

$(eval $(call fuzz-rule,fuzz_parse,parse))
$(eval $(call fuzz-rule,fuzz_template,template))
$(eval $(call fuzz-rule,fuzz_run,run))

fuzz: ## Build and run every fuzzer for $(FUZZ_TIME) seconds each
fuzz: fuzz-run-parse fuzz-run-template fuzz-run-run

fuzz-clean: ## Remove the fuzz build (keeps the corpus)
fuzz-clean:
	-@rm -rf $(FUZZ_DIR)

####################################################################
# Install
####################################################################

LDCONF_INSTALL_PATH ?= /etc/ld.so.conf.d
PC_REQUIRES := $(CUTIL_PC) $(UNICODE_PC) $(RCORE_PC) $(RHEAP_PC) $(RJIT_PC)
PKGCONFIG_INSTALL_PATH ?= $(PC_INSTALL_PATH)

install: all ## Install the library
	@mkdir -p $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
	@cp $(APP_DIR)/$(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/
	@ln -f -s $(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then mkdir -p $(LDCONF_INSTALL_PATH); fi
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then echo "$(LIB_INSTALL_PATH)/$(SUITE)" > $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf; fi
endif
ifeq ($(OS_NAME), Windows)
	@mkdir -p $(BIN_INSTALL_PATH) $(LIB_INSTALL_PATH)/$(SUITE)
	@cp $(APP_DIR)/$(TARGET).a $(LIB_INSTALL_PATH)/$(SUITE)/
	@cp $(APP_DIR)/$(TARGET) $(BIN_INSTALL_PATH)/
endif
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@if [ -d include/ghoti.io ]; then \
		cp -r include/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	@if [ -d $(GEN_DIR)/ghoti.io ]; then \
		cp -r $(GEN_DIR)/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	@mkdir -p $(PKGCONFIG_INSTALL_PATH)
	@cat pkgconfig/$(SUITE)-$(PROJECT).pc | sed 's/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g; s/(VERSION)/$(VERSION)/g; s|(PC_LIB_DIR)|$(PC_LIB_DIR)|g; s|(PC_INCLUDE_DIR)|$(PC_INCLUDE_DIR)|g; s|(REQUIRES)|$(PC_REQUIRES)|g' > $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
ifeq ($(OS_NAME), Linux)
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) installed"

uninstall: ## Delete the installed files
ifeq ($(OS_NAME), Linux)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)*
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then rm -f $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf; fi
endif
ifeq ($(OS_NAME), Windows)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(TARGET).a
	@rm -f $(BIN_INSTALL_PATH)/$(TARGET)
endif
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@rm -f $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
ifeq ($(OS_NAME), Linux)
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) has been uninstalled"

debug: ## Build in DEBUG mode
	make all BUILD=debug

install-debug: ## Install the DEBUG library
	make install BUILD=debug

uninstall-debug: ## Uninstall the DEBUG library
	make uninstall BUILD=debug

test-debug: ## Run the tests in DEBUG mode
	make test BUILD=debug

test-valgrind-debug: ## Valgrind in DEBUG mode
	make test-valgrind BUILD=debug

watch-debug: ## Watch and rebuild in DEBUG mode
	make watch BUILD=debug

test-watch-debug: ## Watch and test in DEBUG mode
	make test-watch BUILD=debug

docs: ## Generate documentation under docs/
	doxygen

docs-pdf: docs ## Generate the PDF manual
	cd ./docs/latex/ && make
	mv -f ./docs/latex/refman.pdf ./docs/$(SUITE)-$(PROJECT)$(BRANCH)-docs.pdf

cloc: ## Count the lines of code used in the project
	cloc src include bison flex tests Makefile

coverage: ## Build instrumented, run the tests, and report line coverage
	@rm -rf $(COV_BUILD_DIR)/objects/*.gcda \
		$(COV_BUILD_DIR)/objects/*/*.gcda 2> /dev/null || true
	@status=0; \
	$(MAKE) --no-print-directory test TEST_GATES= \
		BUILD_DIR=$(COV_BUILD_DIR) \
		EXTRA_CFLAGS="--coverage -O0 -fprofile-update=atomic" \
		EXTRA_LDFLAGS="--coverage" > /dev/null || status=$$?; \
	if [ $$status -eq 0 ]; then \
		tools/coverage.sh $(COV_BUILD_DIR)/objects || status=$$?; \
	else \
		printf 'coverage: the instrumented test run failed; no report\n' >&2; \
	fi; \
	exit $$status

clean: ## Remove the build directories
	-@rm -rf ./build

help: ## Display this help
	@grep -E '^[ a-zA-Z_-]+:.*?## .*$$' Makefile | sort | sed 's/\([^:]*\):.*## \(.*\)/\1:\2/' | awk -F: '{printf "%-22s %s\n", $$1, $$2}'

####################################################################
# Flag stamps. At the end so they are not the default goal, and so the
# directories they name have already been assigned.
####################################################################

.PHONY: force-flags

$(FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CXX) $(LIB_CFLAGS) $(CFLAGS) $(CXXFLAGS) $(LDFLAGS) $(INCLUDE) $(CORELIBRARY) $(DEP_LIBS) $(RHEAP_LIBS) $(RCORE_LIBS) $(UNICODE_LIBS) $(CUTIL_LIBS) $(TESTFLAGS) $(TEST_LDFLAGS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG) $(SUITE) $(PROJECT) $(BRANCH) $(AST_CFLAGS) $(GENERATED_CFLAGS) $(ORACLE_CFLAGS) $(ORACLE_LIBS) $(ORACLE_RPATH) $(HOST_CFLAGS) $(HOST_LIBS) JIT=$(JIT) $(RJIT_CFLAGS) $(RJIT_LIBS) $(JIT_CFLAGS) $(JIT_MODULE_CFLAGS)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(TSAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CXX) $(TSAN_CFLAGS) $(TSAN_CXXFLAGS) $(TSAN_LDFLAGS) $(INCLUDE) $(TSAN_CORELIBRARY) $(DEP_LIBS) $(RHEAP_LIBS) $(RCORE_LIBS) $(UNICODE_LIBS) $(CUTIL_LIBS) $(TESTFLAGS) $(TEST_LDFLAGS) $(AST_CFLAGS) $(GENERATED_CFLAGS) JIT=$(JIT) $(RJIT_CFLAGS) $(RJIT_LIBS) $(JIT_CFLAGS)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(ASAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CXX) $(ASAN_CFLAGS) $(ASAN_CXXFLAGS) $(ASAN_LDFLAGS) $(INCLUDE) $(ASAN_CORELIBRARY) $(DEP_LIBS) $(RHEAP_LIBS) $(RCORE_LIBS) $(UNICODE_LIBS) $(CUTIL_LIBS) $(TESTFLAGS) $(TEST_LDFLAGS) $(AST_CFLAGS) $(GENERATED_CFLAGS) JIT=$(JIT) $(RJIT_CFLAGS) $(RJIT_LIBS) $(JIT_CFLAGS)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(FUZZ_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(FUZZ_CC) $(FUZZ_LIB_CFLAGS) $(FUZZ_BIN_CFLAGS) $(INCLUDE) $(DEP_LIBS) $(RHEAP_LIBS) $(RCORE_LIBS) $(UNICODE_LIBS) $(CUTIL_LIBS) $(FUZZ_RPATH) $(FUZZ_APP_DIR) JIT=$(JIT) $(RJIT_CFLAGS) $(RJIT_LIBS) $(JIT_CFLAGS)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@
