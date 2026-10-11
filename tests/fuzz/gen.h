/**
 * @file
 *
 * A deterministic, grammar-based program generator for the differential fuzz
 * run (CAP-6, AD-16). Test code; C++; seeded `std::mt19937_64`.
 *
 * `gen::generate(seed, mode)` produces a valid Tang program for one seed. The
 * same seed gives the same program, byte for byte, on every machine: the
 * engine is the standard Mersenne Twister and every choice is made by integer
 * arithmetic on its words (no `std::uniform_*_distribution`, whose output the
 * standard leaves to the library).
 *
 * The program is built as a tree of typed expressions and statements and then
 * printed, in script form or in template form (code in `<% %>` tags, text
 * between them, `<%= %>` for a print). It covers the whole language: every
 * operator and cast, strings and their slices, indexing and encodings, arrays
 * and maps, `if`, `while`, `do`, both `for`s, `break` and `continue`, functions
 * and recursion (including recursion to the depth budget), `use` of `math` and
 * `random.seeded`, and errors (division by zero, bad indices, overflow, casts
 * of non-numbers), which are values and flow on.
 *
 * Bounded: a program is a few kilobytes, a loop runs at most six times, loops
 * nest three deep, recursion is single (never a call tree), and an assignment
 * inside a loop is cut to a bounded length, so a program ends in milliseconds
 * on either engine. It is not a fuzzer for hangs.
 *
 * AVOIDED CONSTRUCTS. Each is a recorded departure (documentation/
 * divergence-ledger.md); the generator steers around the construct and does
 * not hide anything else:
 *
 *  - D-009: the value of a loop or an untaken `if` (ctang leaks the
 *    condition). A program always ends in an expression statement, and a
 *    `break`/`continue`/`return` at the top level, which would end it earlier
 *    with a leaked value, is never generated.
 *  - D-010: a function that falls off its end. Every function ends in a
 *    `return`.
 *  - D-011: a trailing function declaration. Functions are declared first.
 *  - D-012: `use` of a path nothing provides. Only `math`, `random` and the
 *    members they have are used.
 *  - D-013: a container rendered to text. Containers hold integers only, so
 *    `as string`, `print` and `+` on one have no encoding to flatten.
 *  - D-014: `+` on arrays whose elements are containers. Arrays are flat.
 *  - D-015: the order of a map's keys. A map with more than one key is never
 *    printed, rendered or iterated; its members are read by name.
 *  - D-017: containers nested past the value-depth bound. Nesting is flat.
 *  - D-023: a program ending in `use`. See D-009.
 *  - D-024: a parameter named like its function. Parameters are `p0`, `p1`.
 *  - D-025: a store into a global array from a recursive function. Functions
 *    take their data as parameters and the recursive ones touch no global.
 *  - D-026: `use` inside a function or of an already bound name. Each `use`
 *    is at the top, once, before its name is assigned.
 *  - D-027: `!=` on two arrays (ctang answers what `==` answers). Arrays are
 *    only compared with `==`; `!=` is generated for numbers and strings.
 *  - D-028: arithmetic on an element of an array that `*` or a slice built
 *    (ctang does it in place). No array is repeated or sliced; `+` joins them.
 *  - D-029: a string plus the error `Not implemented` as its right operand
 *    (ctang answers Not supported; every cast of an error, and an attribute
 *    of one, is that error). The right operand of every string `+` is
 *    `san(x)`, which is `x` unless `x == x` is not true (an error, a map, a
 *    NaN), then 0; the only `as string` is of `null`; every other conversion
 *    to text is a `+` with a string.
 *  - D-030: arithmetic on an element that an index store put there from a
 *    computed value held in a variable or returned by a call (ctang does the
 *    arithmetic in place, as for D-028). A stored value is `(e + 0)`, which is
 *    a number of its own; an array literal's elements and `+` are not affected.
 *  - D-031 is not steered around but read: a NaN prints as `nan` on lang-tang
 *    and as `nan` or `-nan` on ctang, by the sign bit its CPU and compiler
 *    gave it. The generator makes NaNs on purpose (`"nan" as float`, `inf - inf`
 *    of the special constants below) and may print them; the differential reads
 *    ctang's `-nan` as `nan` in float text only (oracle.h, `agree_reading_nan`)
 *    and keeps every other byte exact. A NaN that reaches a string `+` is still
 *    kept out by `san` (it is not the NaN that D-029 is about, but `san` cannot
 *    tell a NaN from the errors it exists for); `sanf` is its float twin, which
 *    lets a NaN through (`x != x` is true of a NaN and false of an error or a
 *    map) and turns the errors and maps into 0, and `"f=" + sanf(float)` is how
 *    a generated program builds text from a float that may be a NaN.
 *  - D-003 and D-021: `random.global`, `random.default` and native values.
 *    Only `random.seeded(n)` with a literal seed is used.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GLTANG_TESTS_FUZZ_GEN_H
#define GHOTI_IO_GLTANG_TESTS_FUZZ_GEN_H

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace gen {

enum class Mode { Script, Template };

struct Program {
  uint64_t seed = 0;
  Mode mode = Mode::Script;
  std::string source;
};

namespace detail {

enum class T { Int, Float, Str, Bool, Arr, Any };

class Builder {
 public:
  Builder(uint64_t seed, Mode mode) : e_(seed), mode_(mode) {}

  std::string build() {
    prelude();
    functions();
    int count = 8 + (int)below(18);
    for (int i = 0; i < count; ++i) {
      statement(0);
    }
    // The program ends in an expression statement (D-009, D-011, D-023).
    code("final_ = " + any(2) + ";");
    code(final_expression() + ";");
    close_tag();
    return out_;
  }

 private:
  std::mt19937_64 e_;
  Mode mode_;
  std::string out_;
  bool tag_open_ = false;
  bool in_ranged_ = false;
  int loop_depth_ = 0;
  bool have_random_ = false;
  bool have_math_ = false;

  // ----------------------------------------------------------------------
  // Choices
  // ----------------------------------------------------------------------

  uint64_t below(uint64_t n) { return e_() % n; }
  bool chance(unsigned percent) { return below(100) < percent; }
  template <size_t N>
  const char * pick(const char * const (&items)[N]) { return items[below(N)]; }

  // ----------------------------------------------------------------------
  // Output: a template is code in tags and text between them
  // ----------------------------------------------------------------------

  void raw(const std::string & s) { out_ += s; }

  void open_tag() {
    if (mode_ == Mode::Template && !tag_open_) {
      out_ += "<% ";
      tag_open_ = true;
    }
  }
  void close_tag() {
    if (mode_ == Mode::Template && tag_open_) {
      out_ += " %>";
      tag_open_ = false;
    }
  }
  /// A piece of code (a statement, or a statement-opening or -closing brace).
  void code(const std::string & s) {
    open_tag();
    out_ += s;
    out_ += mode_ == Mode::Script ? "\n" : " ";
  }
  /// Text between tags: a template only.
  void text_node() {
    if (mode_ != Mode::Template) {
      return;
    }
    close_tag();
    static const char * const frags[] = {"lorem ", "<b>", "</b>", " & ", "100", "% done", "\n", "\"q\"",
        " // c ", " /* c */ ", "\xc3\xa9", "\xe2\x98\x83", "\xf0\x9f\x8c\x8d", "x<y", "a>b", " ", "<ul>", "</ul>", "'s "};
    int n = 1 + (int)below(3);
    for (int i = 0; i < n; ++i) {
      out_ += pick(frags);
    }
  }
  /// `print(x);`, or in a template sometimes `<%= x %>`.
  void print_statement(const std::string & x) {
    if (mode_ == Mode::Template && chance(50)) {
      close_tag();
      out_ += "<%= " + x + " %>";
    }
    else {
      code("print(" + x + ");");
    }
  }

  // ----------------------------------------------------------------------
  // Literals
  // ----------------------------------------------------------------------

  std::string int_literal() {
    switch (below(10)) {
      case 0: return "0";
      case 1: return "1";
      case 2: return std::to_string(below(1000));
      case 3: return pick(kBig);
      default: return std::to_string(below(21));
    }
  }
  static constexpr const char * const kBig[] = {"9223372036854775807", "4611686018427387904", "3037000500", "2147483648", "65536", "1000000007"};

  std::string signed_int_literal() {
    std::string s = int_literal();
    return chance(20) ? "(0 - " + s + ")" : s;
  }

  /// The floats a program is built to meet: NaN of either sign and a payload,
  /// the infinities, negative zero, subnormals (the least one, a middle one, the
  /// largest), the least and the largest normal, and one ulp either side of 1.
  /// Read from text because the language has no literal for most of them.
  std::string special_float() {
    static const char * const f[] = {"(\"nan\" as float)", "(\"-nan(0x1234)\" as float)", "(\"inf\" as float)", "(\"-inf\" as float)", "(-0.0)", "(0.0 * (-1.0))",
        "(\"5e-324\" as float)", "(\"1e-320\" as float)", "(\"2.2250738585072009e-308\" as float)", "(\"2.2250738585072014e-308\" as float)",
        "(\"1.7976931348623157e308\" as float)", "(\"-1.7976931348623157e308\" as float)", "(\"1.0000000000000002\" as float)", "(\"0.9999999999999999\" as float)",
        "(\"4503599627370496.5\" as float)", "(\"9007199254740993\" as float)"};
    return pick(f);
  }

  std::string float_literal() {
    static const char * const f[] = {"0.5", "1.5", "2.25", "3.", ".75", "100.", "0.1", "0.3333", "7.125", "1.", "99999999999999999999999.0", "0.0", "123456.789", "2.5"};
    if (chance(15)) {
      return special_float();
    }
    return pick(f);
  }

  std::string string_body() {
    static const char * const parts[] = {"a", "bc", "hello", "x y", "<b>", "a&b", "100%", "\\\"q\\\"", "\\n", "caf\xc3\xa9", "\xe2\x98\x83", "tab\\t", "Z", "", "0", "42", "-7", "3.5abc", "  12"};
    std::string s;
    int n = (int)below(3);
    for (int i = 0; i <= n; ++i) {
      s += pick(parts);
    }
    return s;
  }
  std::string string_literal() {
    std::string body = "\"" + string_body() + "\"";
    switch (below(8)) {
      case 0: return "!" + body;
      case 1: return "%" + body;
      default: return body;
    }
  }

  // ----------------------------------------------------------------------
  // Variables
  // ----------------------------------------------------------------------

  std::string int_var() { return "i" + std::to_string(below(4)); }
  std::string float_var() { return "f" + std::to_string(below(3)); }
  /// Outside a loop a variable is assigned only from variables with a lower
  /// index, so no chain of assignments can double its length (or an array's)
  /// more often than there are variables. Inside a loop an assignment is cut.
  int str_limit_ = 3;
  int arr_limit_ = 3;
  std::string str_var() { return str_limit_ == 0 ? string_literal() : "s" + std::to_string(below((uint64_t)str_limit_)); }
  std::string bool_var() { return "b" + std::to_string(below(2)); }
  std::string arr_var() { return arr_limit_ == 0 ? array_literal(0) : "a" + std::to_string(below((uint64_t)arr_limit_)); }
  std::string map_key() { return "k" + std::to_string(below(6)); }

  // ----------------------------------------------------------------------
  // Expressions
  // ----------------------------------------------------------------------

  std::string any(int d) {
    switch (below(5)) {
      case 0: return integer(d);
      case 1: return floating(d);
      case 2: return string(d);
      case 3: return boolean(d);
      default: return array(d);
    }
  }

  std::string scalar(int d) {
    switch (below(4)) {
      case 0: return integer(d);
      case 1: return floating(d);
      case 2: return string(d);
      default: return boolean(d);
    }
  }

  std::string integer(int d) {
    if (d <= 0) {
      return chance(55) ? signed_int_literal() : int_var();
    }
    switch (below(24)) {
      case 0: case 1: return signed_int_literal();
      case 2: case 3: return int_var();
      case 4: case 5: case 6: case 7: case 8: {
        static const char * const ops[] = {"+", "-", "*", "/", "%"};
        return "(" + integer(d - 1) + " " + pick(ops) + " " + integer(d - 1) + ")";
      }
      case 9: return "(-" + integer(d - 1) + ")";
      case 10: return "(" + string(d - 1) + " as int)";
      case 11: return "(" + floating(d - 1) + " as int)";
      case 12: return "(" + boolean(d - 1) + " as int)";
      case 13: return arr_var() + "[" + integer(d - 1) + "]";
      case 14: return arr_var() + ".size";
      case 15: return "(" + string(d - 1) + ").length";
      case 16: return "(" + string(d - 1) + ").byte_length";
      case 17: return "(" + boolean(d - 1) + " ? " + integer(d - 1) + " : " + integer(d - 1) + ")";
      case 18: return "fa(" + integer(d - 1) + ")";
      case 19: return "rec((" + integer(d - 1) + ") % 21, " + integer(d - 1) + ")";
      case 20: return "m0." + map_key();
      case 21: return "m0[\"" + map_key() + "\"]";
      case 22: return have_random_ ? "r0.next_int" : integer(d - 1);
      default: return "(" + integer(d - 1) + " " + (chance(50) ? "+" : "-") + " " + integer(d - 1) + ")";
    }
  }

  std::string floating(int d) {
    if (d <= 0) {
      return chance(60) ? float_literal() : float_var();
    }
    switch (below(16)) {
      case 0: return float_literal();
      case 1: case 2: return float_var();
      case 3: case 4: case 5: {
        static const char * const ops[] = {"+", "-", "*", "/"};
        return "(" + floating(d - 1) + " " + pick(ops) + " " + (chance(30) ? integer(d - 1) : floating(d - 1)) + ")";
      }
      case 6: return "(-" + floating(d - 1) + ")";
      case 7: return "(" + integer(d - 1) + " as float)";
      case 8: return "(" + string(d - 1) + " as float)";
      case 9: return "(" + boolean(d - 1) + " ? " + floating(d - 1) + " : " + floating(d - 1) + ")";
      case 10: return have_random_ ? "r0.next_float" : floating(d - 1);
      case 11: return have_math_ ? "math.pi" : floating(d - 1);
      case 12: return "(" + integer(d - 1) + " * " + floating(d - 1) + ")";
      case 13: return special_float();
      case 14: {
        // inf - inf, 0 * inf, a subnormal halved, the largest doubled: the results the compiled tiers must reproduce.
        static const char * const ops[] = {"+", "-", "*", "/"};
        return "(" + special_float() + " " + pick(ops) + " " + (chance(40) ? special_float() : floating(d - 1)) + ")";
      }
      default: return "(" + floating(d - 1) + " % " + floating(d - 1) + ")";  // Not supported: an error value
    }
  }

  std::string string(int d) {
    if (d <= 0) {
      return chance(60) ? string_literal() : str_var();
    }
    switch (below(20)) {
      case 0: case 1: return string_literal();
      case 2: case 3: return str_var();
      case 4: case 5: case 6: {
        std::string rhs = chance(50) ? string(d - 1) : scalar(d - 1);
        return "(" + string(d - 1) + " + san(" + rhs + "))";
      }
      case 7: return "(\"n=\" + san(" + integer(d - 1) + "))";
      case 8: return "(\"f=\" + sanf(" + floating(d - 1) + "))";
      case 9: return "(\"b=\" + san(" + boolean(d - 1) + "))";
      case 10: return "(" + string(d - 1) + ")[" + slice(d - 1) + "]";
      case 11: return "(" + string(d - 1) + ")[" + integer(d - 1) + "]";
      case 12: {
        static const char * const attrs[] = {"html", "html_attribute", "percent", "javascript", "raw", "render"};
        return "(" + string(d - 1) + ")." + pick(attrs);
      }
      case 13: return "fs(" + string(d - 1) + ", " + integer(d - 1) + ")";
      case 14: return "(" + boolean(d - 1) + " ? " + string(d - 1) + " : " + string(d - 1) + ")";
      case 15: return "(null as string)";
      case 16: return "(" + string(d - 1) + " + san(" + integer(d - 1) + "))";
      default: return string_literal();
    }
  }

  /// `a:b`, `a:b:c`, `:b`, `a:` and the negative forms.
  std::string slice(int d) {
    auto part = [&]() -> std::string {
      switch (below(5)) {
        case 0: return "";
        case 1: return "-" + std::to_string(1 + below(6));
        case 2: return integer(d > 0 ? 1 : 0);
        default: return std::to_string(below(8));
      }
    };
    std::string s = part() + ":" + part();
    if (chance(35)) {
      static const char * const steps[] = {"", "1", "2", "3", "-1", "-2", "0"};
      s += std::string(":") + pick(steps);
    }
    return s;
  }

  std::string boolean(int d) {
    if (d <= 0) {
      return chance(50) ? (chance(50) ? "true" : "false") : bool_var();
    }
    switch (below(14)) {
      case 0: return chance(50) ? "true" : "false";
      case 1: return bool_var();
      case 2: case 3: {
        static const char * const ops[] = {"<", "<=", ">", ">=", "==", "!="};
        return "(" + integer(d - 1) + " " + pick(ops) + " " + integer(d - 1) + ")";
      }
      case 4: {
        static const char * const ops[] = {"<", "<=", ">", ">=", "==", "!="};
        return "(" + floating(d - 1) + " " + pick(ops) + " " + (chance(50) ? integer(d - 1) : floating(d - 1)) + ")";
      }
      case 5: {
        static const char * const ops[] = {"<", "<=", ">", ">=", "==", "!="};
        return "(" + string(d - 1) + " " + pick(ops) + " " + string(d - 1) + ")";
      }
      case 6: return "(" + array(d - 1) + " == " + array(d - 1) + ")";  // never `!=` on arrays: D-027
      case 7: return "(!" + any(d - 1) + ")";
      case 8: return "(" + any(d - 1) + " && " + any(d - 1) + ")";
      case 9: return "(" + any(d - 1) + " || " + any(d - 1) + ")";
      case 10: return "(" + any(d - 1) + " as bool)";
      case 11: return "(" + integer(d - 1) + " == " + string(d - 1) + ")";  // not supported across types: an error value
      case 12: return "(null == " + any(d - 1) + ")";
      default: return "(" + boolean(d - 1) + " == " + boolean(d - 1) + ")";
    }
  }

  std::string array(int d) {
    if (d <= 0) {
      return chance(50) ? arr_var() : array_literal(0);
    }
    switch (below(10)) {
      case 0: case 1: return arr_var();
      case 2: case 3: return array_literal(d - 1);
      case 4: return "(" + array(d - 1) + " + " + array_literal(d - 1) + ")";  // never a slice: D-028
      case 5: return "(" + array(d - 1) + " + " + array(d - 1) + ")";
      case 6: return "(" + array(d - 1) + " + " + array(d - 1) + ")";
      case 7: return "(" + array(d - 1) + " + " + array(d - 1) + ")";  // never `*`: D-028
      case 8: return "(" + array(d - 1) + " + " + integer(d - 1) + ")";  // not supported: an error value
      default: return array_literal(d - 1);
    }
  }

  std::string array_literal(int d) {
    std::string s = "[";
    int n = (int)below(5);
    for (int i = 0; i < n; ++i) {
      s += (i ? ", " : "") + (d > 0 ? integer(d - 1) : signed_int_literal());
    }
    return s + "]";
  }

  /// The program's final expression: one that shows something.
  std::string final_expression() {
    switch (below(6)) {
      case 0: return "i0";
      case 1: return "s0";
      case 2: return "a0";
      case 3: return "final_";
      case 4: return "[i1, f0, s1, b0]";
      default: return "(" + any(2) + ")";
    }
  }

  // ----------------------------------------------------------------------
  // Prelude and functions
  // ----------------------------------------------------------------------

  void prelude() {
    // `use` once, at the top, before the names are assigned (D-026), and only
    // names that exist (D-012).
    have_math_ = chance(40);
    have_random_ = chance(40);
    if (have_math_) {
      code("use math;");
    }
    if (have_random_) {
      code("use random;");
      code("r0 = random.seeded(" + std::to_string(below(100000)) + ");");
    }
    for (int i = 0; i < 4; ++i) {
      code("i" + std::to_string(i) + " = " + signed_int_literal() + ";");
    }
    for (int i = 0; i < 3; ++i) {
      code("f" + std::to_string(i) + " = " + float_literal() + ";");
      code("s" + std::to_string(i) + " = " + string_literal() + ";");
    }
    code("b0 = true;");
    code("b1 = false;");
    code("a0 = [1, 2, 3];");
    code("a1 = [];");
    code("a2 = " + array_literal(0) + ";");
    code("m0 = {k0: 1, k1: 2, k2: 3};");
    code("m1 = {:};");
    code("final_ = null;");
    text_node();
  }

  void functions() {
    // Declared first (D-011), each ending in a return (D-010), parameters named
    // p0, p1 (D-024), pure of the globals so that the recursive ones touch no
    // array (D-025).
    code("function fa(p0) {");
    code("local = " + local_int(2) + ";");
    code("if (" + local_bool() + ") { return " + local_int(2) + "; }");
    code("return local + " + local_int(1) + ";");
    code("}");
    code("function rec(p0, p1) {");
    code("if (p0 <= 0) { return p1; }");
    code("return rec(p0 - 1, p1 + " + local_int(1) + ");");
    code("}");
    code("function san(p0) {");
    code("if (p0 == p0) { return p0; }");
    code("return 0;");
    code("}");
    code("function sanf(p0) {");
    code("if (p0 == p0) { return p0; }");
    code("if (p0 != p0) { return p0; }");
    code("return 0;");
    code("}");
    code("function fs(p0, p1) {");
    code("local = p0 + san(p1);");
    code("if (" + local_bool() + ") { return local; }");
    code("return (local + \"-\")[:12];");
    code("}");
    if (chance(25)) {
      code("function deep(p0) { return deep(p0 + 1); }");
      code("deep_ = deep(0);");
    }
    if (chance(20)) {
      code("function loopy(p0) {");
      code("t = 0;");
      code("for (j = 0; j < 5; j += 1) { if (j == 3) { continue; } t += j * p0; }");
      code("return t;");
      code("}");
      code("loopy_ = loopy(" + int_literal() + ");");
    }
    text_node();
  }

  /// Expressions over a function's parameter only.
  std::string local_int(int d) {
    if (d <= 0) {
      return chance(50) ? "p0" : int_literal();
    }
    static const char * const ops[] = {"+", "-", "*", "/", "%"};
    switch (below(4)) {
      case 0: return "p0";
      case 1: return int_literal();
      default: return "(" + local_int(d - 1) + " " + pick(ops) + " " + local_int(d - 1) + ")";
    }
  }
  std::string local_bool() {
    static const char * const ops[] = {"<", "<=", ">", ">=", "==", "!="};
    return "p0 " + std::string(pick(ops)) + " " + int_literal();
  }

  // ----------------------------------------------------------------------
  // Statements
  // ----------------------------------------------------------------------

  void block(int d, int n) {
    for (int i = 0; i < n; ++i) {
      statement(d);
    }
  }

  void statement(int d) {
    int kind = (int)below(d >= 3 ? 12 : 22);
    switch (kind) {
      case 0: case 1: case 2: {
        std::string v = int_var();
        if (chance(40)) {
          static const char * const ops[] = {"+=", "-=", "*=", "/=", "%="};
          code(v + " " + pick(ops) + " " + integer(2) + ";");
        }
        else {
          code(v + " = " + integer(3) + ";");
        }
        break;
      }
      case 3: code(float_var() + " = " + floating(3) + ";"); break;
      case 4: {
        std::string v = str_var();
        if (loop_depth_ > 0) {
          // Cut to a bounded length: a string that references itself would
          // otherwise double on every pass.
          code(v + " = (" + string(2) + ")[:24];");
        }
        else if (chance(30)) {
          code(v + " += " + (chance(50) ? string_literal() : "san(" + integer(1) + ")") + ";");
        }
        else {
          str_limit_ = v[1] - '0';
          code(v + " = " + string(3) + ";");
          str_limit_ = 3;
        }
        break;
      }
      case 5: code(bool_var() + " = " + boolean(3) + ";"); break;
      case 6: {
        std::string v = arr_var();
        if (in_ranged_) {
          code("print(" + v + ".size);");
        }
        else if (loop_depth_ > 0) {
          // Bounded growth in a loop: a literal, or a variable and one more.
          code(v + " = " + (chance(50) ? array_literal(1) : "(" + arr_var() + " + [" + integer(1) + "])") + ";");
        }
        else {
          arr_limit_ = v[1] - '0';
          code(v + " = " + array(3) + ";");
          arr_limit_ = 3;
        }
        break;
      }
      case 7: {
        if (in_ranged_) {
          code("print(1);");
        }
        else {
          // D-030: stored as `(e + 0)`, a fresh number, never the shared one.
          code(arr_var() + "[(" + integer(1) + ") % 10] = (" + integer(2) + " + 0);");
        }
        break;
      }
      case 8: {
        if (chance(50)) {
          code("m0." + map_key() + " = " + integer(2) + ";");
        }
        else {
          code("m0[\"" + map_key() + "\"] = " + integer(2) + ";");
        }
        break;
      }
      case 9: case 10: print_statement(scalar(3)); break;
      case 11: {
        std::string a = array(2);
        print_statement(a);
        break;
      }
      case 12: case 13: {
        // if / else
        code("if (" + boolean(3) + ") {");
        block(d + 1, 1 + (int)below(3));
        if (chance(50)) {
          code("} else {");
          block(d + 1, 1 + (int)below(2));
        }
        code("}");
        if (mode_ == Mode::Template) {
          text_node();
        }
        break;
      }
      case 14: case 15: {
        // for (;;) with a loop counter that the body does not touch
        if (loop_depth_ >= 3) {
          print_statement(scalar(2));
          break;
        }
        std::string v = "l" + std::to_string(loop_depth_);
        code("for (" + v + " = 0; " + v + " < " + std::to_string(1 + below(6)) + "; " + v + " += 1) {");
        ++loop_depth_;
        block(d + 1, 1 + (int)below(3));
        if (chance(30)) {
          code("if (" + v + " == " + std::to_string(below(4)) + ") { " + (chance(50) ? "break;" : "continue;") + " }");
        }
        --loop_depth_;
        code("}");
        text_node();
        break;
      }
      case 16: {
        // while with its own counter
        if (loop_depth_ >= 3) {
          print_statement(scalar(2));
          break;
        }
        std::string v = "l" + std::to_string(loop_depth_);
        code(v + " = 0;");
        code("while (" + v + " < " + std::to_string(1 + below(5)) + ") {");
        ++loop_depth_;
        code(v + " += 1;");
        block(d + 1, 1 + (int)below(3));
        if (chance(30)) {
          code("if (" + boolean(2) + ") { continue; }");
        }
        --loop_depth_;
        code("}");
        text_node();
        break;
      }
      case 17: {
        // do-while
        if (loop_depth_ >= 3) {
          print_statement(scalar(2));
          break;
        }
        std::string v = "l" + std::to_string(loop_depth_);
        code(v + " = 0;");
        code("do {");
        ++loop_depth_;
        code(v + " += 1;");
        block(d + 1, 1 + (int)below(2));
        --loop_depth_;
        code("} while (" + v + " < " + std::to_string(1 + below(4)) + ");");
        text_node();
        break;
      }
      case 18: {
        // ranged for over an array the body does not change
        if (loop_depth_ >= 3 || in_ranged_) {
          print_statement(scalar(2));
          break;
        }
        std::string var = "e" + std::to_string(loop_depth_);
        code("for (" + var + " : " + array(1) + ") {");
        ++loop_depth_;
        in_ranged_ = true;
        code("i" + std::to_string(below(4)) + " = " + var + ";");
        block(d + 1, 1 + (int)below(2));
        in_ranged_ = false;
        --loop_depth_;
        code("}");
        text_node();
        break;
      }
      case 19: code("fa(" + integer(2) + ");"); break;
      case 20: code(any(3) + ";"); break;
      default: {
        text_node();
        code("i" + std::to_string(below(4)) + " = " + (chance(50) ? integer(2) : "rec(" + std::to_string(below(15)) + ", " + integer(1) + ")") + ";");
        break;
      }
    }
  }
};

}  // namespace detail

namespace detail {

/**
 * The call-graph generator (calls between compiled functions, spec-runtime-calls).
 * A script of three to eight functions, each calling the ones declared before
 * it (Tang resolves a name when it reads it, so a function cannot name one that
 * comes after, and mutual recursion is not expressible with named functions),
 * of up to fifteen parameters, in loops, by single and tree recursion, with
 * functions passed as values and called through a parameter (a call the compiler
 * cannot name, so it stays an exit), an array passed and read, and, now and then,
 * recursion to the depth budget. Every value is an integer kept small by `%`, so
 * a result is the same on every engine; every function ends in a `return`; the
 * program ends in an expression statement (D-009, D-010, D-011).
 */
class CallBuilder {
 public:
  /// With \p natives the program also calls the library the tests register
  /// (`inc`, `sum`, `alloc_n`, `reenter`, `deopt`; tests/unit/test_native_calls.cpp):
  /// natives called with a `use` in the function and through a `global`, one that
  /// allocates, one that re-enters an earlier function as a nested activation and one
  /// that asks compiled code to leave. Without it the program is the one the seed has
  /// always made, and no engine but this one has the library.
  explicit CallBuilder(uint64_t seed, bool natives = false) : e_(seed), natives_(natives) {}

  std::string build() {
    if (natives_) {
      line("use inc; use sum; use alloc_n; use reenter; use deopt;");
    }
    int count = 3 + (int)below(6);
    for (int i = 0; i < count; ++i) {
      function(i);
    }
    if (chance(30)) {
      line("function apply(p0, p1) { return p0(p1); }");
      line("function pick(p0, p1) { return p0[p1 % 3] + 1; }");
      has_values_ = true;
    }
    if (chance(15)) {
      line("function deep(p0) { return deep(p0 + 1); }");
      line("deep_ = deep(0);");
    }
    line("acc = 0;");
    line("arr = [3, 5, 7];");
    int drivers = 2 + (int)below(4);
    for (int d = 0; d < drivers; ++d) {
      const Fn & f = fns_[below(fns_.size())];
      std::string call = f.name + "(" + literal_args(f) + ")";
      if (chance(60)) {
        line("for (k" + std::to_string(d) + " = 0; k" + std::to_string(d) + " < " + std::to_string(2 + below(30)) + "; k" + std::to_string(d) + " += 1) { acc = (acc + " + f.name + "(" + loop_args(f, "k" + std::to_string(d)) + ")) % 100003; }");
      }
      else {
        line("acc = (acc + " + call + ") % 100003;");
      }
      line("print(acc);");
      line("print(\",\");");
    }
    if (has_values_) {
      for (const Fn & f : fns_) {
        if (f.params == 1) {
          line("acc = (acc + apply(" + f.name + ", " + std::to_string(below(9)) + ")) % 100003;");
          break;
        }
      }
      line("acc = (acc + pick(arr, " + std::to_string(below(9)) + ")) % 100003;");
    }
    line("acc;");
    return out_;
  }

 private:
  struct Fn {
    std::string name;
    int params;
  };
  std::mt19937_64 e_;
  std::string out_;
  std::vector<Fn> fns_;
  bool has_values_ = false;
  bool natives_ = false;

  uint64_t below(uint64_t n) { return e_() % n; }
  bool chance(unsigned percent) { return below(100) < percent; }
  void line(const std::string & s) { out_ += s + "\n"; }

  std::string param(int params) { return "p" + std::to_string(below(params)); }
  /// An integer over the parameters, small whatever they are.
  std::string expr(int params, int depth) {
    if (depth <= 0 || chance(35)) {
      return params > 0 && chance(70) ? param(params) : std::to_string(below(21));
    }
    static const char * const ops[] = {"+", "-", "*"};
    return "((" + expr(params, depth - 1) + " " + ops[below(3)] + " " + expr(params, depth - 1) + ") % 1000)";
  }
  std::string args_over(int callee_params, int params, int depth) {
    std::string s;
    for (int i = 0; i < callee_params; ++i) {
      s += (i ? ", " : "") + expr(params, depth);
    }
    return s;
  }
  std::string literal_args(const Fn & f) {
    std::string s;
    for (int i = 0; i < f.params; ++i) {
      s += (i ? ", " : "") + std::to_string(below(15));
    }
    return s;
  }
  std::string loop_args(const Fn & f, const std::string & counter) {
    std::string s;
    for (int i = 0; i < f.params; ++i) {
      s += (i ? ", " : "") + (i == 0 ? counter : std::to_string(below(15)));
    }
    return s;
  }

  void function(int index) {
    // Few parameters most of the time, and now and then up to fifteen.
    int params = chance(25) ? (int)below(16) : (int)below(4);
    std::string name = "g" + std::to_string(index);
    std::string head = "function " + name + "(";
    for (int i = 0; i < params; ++i) {
      head += (i ? ", p" : "p") + std::to_string(i);
    }
    head += ") {";
    int shape = (int)below(7);
    if (natives_ && chance(35)) {
      shape = 7 + (int)below(3);  // a call of the library
    }
    if (index == 0 && shape >= 2 && shape != 7 && shape != 9) {
      shape = (int)below(2);  // nothing declared before it to call
    }

    // Shape 6 needs two earlier functions of one parameter count to choose between.
    int pair_a = -1, pair_b = -1;
    for (size_t x = 0; shape == 6 && x < fns_.size() && pair_b < 0; ++x) {
      for (size_t y = x + 1; y < fns_.size(); ++y) {
        if (fns_[x].params == fns_[y].params) {
          pair_a = (int)x;
          pair_b = (int)y;
          break;
        }
      }
    }
    if (shape == 6 && pair_b < 0) {
      shape = 2;
    }
    if (params == 0 && (shape == 4 || shape == 5)) {
      shape = 0;
    }
    line(head);
    switch (shape) {
      case 0:
      case 1:  // a leaf
        line("  local = " + expr(params, 2) + ";");
        line("  if (" + expr(params, 1) + " < " + expr(params, 1) + ") { return " + expr(params, 2) + "; }");
        line("  return local + " + expr(params, 1) + ";");
        break;
      case 2: {  // calls the functions before it
        const Fn & a = fns_[below(fns_.size())];
        const Fn & b = fns_[below(fns_.size())];
        line("  t = " + a.name + "(" + args_over(a.params, params, 1) + ");");
        line("  u = " + b.name + "(" + args_over(b.params, params, 1) + ");");
        line("  return (t + u + " + expr(params, 1) + ") % 100003;");
        break;
      }
      case 3: {  // a loop of calls
        const Fn & a = fns_[below(fns_.size())];
        line("  total = 0;");
        line("  for (j = 0; j < " + std::to_string(2 + below(5)) + "; j += 1) {");
        line("    total = (total + " + a.name + "(" + args_over(a.params, params, 1) + ")) % 100003;");
        line("  }");
        line("  return total;");
        break;
      }
      case 4: {  // single recursion on the first parameter, at most thirty deep
        line("  if (p0 <= 0 || p0 > 30) { return " + expr(params, 1) + "; }");
        std::string rest;
        for (int i = 1; i < params; ++i) {
          rest += ", " + expr(params, 1);
        }
        line("  return (" + name + "(p0 - 1" + rest + ") + " + expr(params, 1) + ") % 100003;");
        break;
      }
      case 7: {  // library natives: a `use` in the function, or the top level's, named by `global`
        if (chance(50)) {
          line("  use inc; use sum;");
        }
        else {
          line("  global inc; global sum;");
        }
        line("  t = inc(" + expr(params, 1) + ");");
        line("  return (t + sum(" + expr(params, 1) + ", " + expr(params, 1) + ") + " + expr(params, 1) + ") % 100003;");
        break;
      }
      case 8: {  // re-enters an earlier function as a nested activation
        const Fn * a = &fns_[below(fns_.size())];
        for (size_t tries = 0; a->params > 14 && tries < fns_.size(); ++tries) {
          a = &fns_[(&*a - &fns_[0] + 1) % fns_.size()];
        }
        if (a->params > 14) {
          line("  return " + expr(params, 2) + ";");
          break;
        }
        line("  use reenter;");
        std::string args = args_over(a->params, params, 1);
        line("  t = reenter(" + a->name + (args.empty() ? "" : ", " + args) + ");");
        line("  return (t + " + expr(params, 1) + ") % 100003;");
        break;
      }
      case 9: {  // a native that allocates, and one that asks compiled code to leave
        line("  use alloc_n; use deopt;");
        line("  r = alloc_n(" + std::to_string(below(8)) + ");");
        line("  return (deopt(" + expr(params, 1) + ") + " + expr(params, 1) + ") % 100003;");
        break;
      }
      case 6: {  // the callee is a value two paths produce: never a compiled call site
        const Fn & a = fns_[pair_a];
        const Fn & b = fns_[pair_b];
        line("  c = " + expr(params, 1) + " % 2 == 0;");
        line("  return ((c ? " + a.name + " : " + b.name + ")(" + args_over(a.params, params, 1) + ") + " + expr(params, 1) + ") % 100003;");
        break;
      }
      default: {  // tree recursion, as fib, at most nine deep
        line("  if (p0 <= 1 || p0 > 9) { return " + expr(params, 1) + "; }");
        std::string rest;
        for (int i = 1; i < params; ++i) {
          rest += ", " + expr(params, 1);
        }
        line("  return (" + name + "(p0 - 1" + rest + ") + " + name + "(p0 - 2" + rest + ")) % 100003;");
        break;
      }
    }
    line("}");
    fns_.push_back({name, params});
  }
};

/// Programs that carry floats (spec-runtime-float story 6): functions that assign a
/// float constant to locals before a loop, copy it through other locals and
/// duplicates, leave it live over polls and over calls of earlier functions, and
/// end in a consumer (`print`, a global store, a comparison, arithmetic, a call
/// argument, a return), now and then a float assigned inside an `if` or a loop (a
/// merge of unequal kinds, which compiles with no raw slot). The tier comparison
/// reads their frames by bits. Used only against the JIT, never against ctang: the
/// aim is the interpreter against compiled code holding floats raw.
class CarryBuilder {
 public:
  explicit CarryBuilder(uint64_t seed) : e_(seed) {}

  std::string build() {
    line("gs = 0;");
    line("gt = 0.0;");
    int functions = 3 + (int)below(5);
    for (int i = 0; i < functions; ++i) {
      function(i);
    }
    line("acc = 0;");
    for (int i = 0; i < functions; ++i) {
      std::string call = "c" + std::to_string(i) + "(" + std::to_string(below(9)) + ", " + std::to_string(below(9)) + ")";
      if (chance(50)) {
        line("for (k" + std::to_string(i) + " = 0; k" + std::to_string(i) + " < " + std::to_string(2 + below(6)) + "; k" + std::to_string(i) + " += 1) { print(" + call + "); print(\",\"); }");
      }
      else {
        line("print(" + call + "); print(\",\");");
      }
    }
    line("print(gs); print(gt);");
    line("acc;");
    return out_;
  }

 private:
  std::mt19937_64 e_;
  std::string out_;
  int functions_ = 0;

  uint64_t below(uint64_t n) { return e_() % n; }
  bool chance(unsigned percent) { return below(100) < percent; }
  void line(const std::string & s) { out_ += s + "\n"; }

  /// A float literal. Scientific notation is not Tang syntax, so the extremes are spelled
  /// out: 1e308 is a one and three hundred and eight zeros, the subnormal 1e-320 a point,
  /// three hundred and nineteen zeros and a one.
  std::string literal() {
    static const char * const f[] = {"0.5", "1.5", "2.25", "0.1", "0.0", "3.0", "123456789.125", "100000000000000000000.0", "0.00000000000000000001",
        "9007199254740993.0", "0.30000000000000004", "100.0"};
    switch (below(16)) {
      case 12: return "1" + std::string(308, '0') + ".0";
      case 13: return "0." + std::string(319, '0') + "1";
      case 14: return "0." + std::string(307, '0') + "22250738585072014";
      default: return f[below(sizeof f / sizeof f[0])];
    }
  }
  std::string local(int count) { return "q" + std::to_string(below(count)); }

  void function(int index) {
    std::string name = "c" + std::to_string(index);
    line("function " + name + "(p0, p1) {");
    int floats = 1 + (int)below(5);
    line("  global gs; global gt;");
    // The conflict shapes first: a float assigned only on some paths.
    bool conflict = chance(25);
    if (conflict) {
      line("  if (p0 > 4) { q0 = " + literal() + "; } else { q0 = " + std::to_string(below(9)) + "; }");
    }
    for (int i = conflict ? 1 : 0; i < floats; ++i) {
      std::string v = "q" + std::to_string(i);
      switch (below(4)) {
        case 0: line("  " + v + " = " + literal() + ";"); break;
        case 1: line("  " + v + " = " + (i > 0 ? local(i) : literal()) + ";"); break;                       // a copy
        case 2: line("  " + v + " = w" + std::to_string(i) + " = " + literal() + ";"); break;               // a chain (a duplicate)
        default: line("  " + v + " = " + literal() + "; " + "w" + std::to_string(i) + " = " + v + ";"); break;
      }
    }
    line("  acc = p0;");
    int loops = (int)below(3);
    for (int l = 0; l < loops; ++l) {
      std::string j = "j" + std::to_string(l);
      line("  for (" + j + " = 0; " + j + " < " + std::to_string(2 + below(12)) + "; " + j + " += 1) {");
      line("    acc = (acc + " + j + " * 3) % 1000;");
      if (functions_ > 0 && chance(60)) {
        line("    acc = (acc + c" + std::to_string(below(functions_)) + "(" + j + ", acc % 7)) % 1000;");
      }
      if (chance(25)) {
        line("    " + local(floats) + " = " + literal() + ";");  // reassigned in the loop: a kind that may or may not agree
      }
      line("  }");
    }
    if (functions_ > 0 && chance(50)) {
      line("  acc = (acc + c" + std::to_string(below(functions_)) + "(p1, p0)) % 1000;");
    }
    // Consumers: each reads a raw float and is an exit.
    int consumers = (int)below(3);
    for (int c = 0; c < consumers; ++c) {
      std::string v = local(floats);
      switch (below(8)) {
        case 0: line("  print(" + v + "); print(\";\");"); break;
        case 1: line("  gt = " + v + ";"); break;
        case 2: line("  if (" + v + " == " + literal() + ") { acc = acc + 1; }"); break;
        case 3: line("  print(" + v + " + " + literal() + "); print(\";\");"); break;
        case 4: line("  gs = (gs + p0) % 1000; print(-" + v + "); print(\";\");"); break;
        case 5: line("  print(" + v + " < " + local(floats) + "); print(\";\");"); break;
        case 6: line("  if (" + v + ") { acc = acc + 2; }"); break;
        default: line("  print(" + local(floats) + " as string); print(\";\");"); break;
      }
    }
    if (chance(15)) {
      line("  return " + local(floats) + ";");
    }
    else {
      line("  return (acc + p1) % 1000;");
    }
    line("}");
    ++functions_;
  }
};

/// Programs that compute with floats and divide (spec-runtime-float story 7): functions that
/// make float locals from literals and casts, compute with `+ - * /`, unary minus, the six
/// comparisons and `as int` / `as float` over floats, integers and the special floats, in loops and
/// `if`s, with divisors and moduli the program steers to zero, -1, the small-integer bounds, NaN,
/// infinity and negative zero, and parameters that are floats, integers, strings and null. The
/// results are consumed by `print`, `return`, a comparison and a branch. Every error case of the
/// matrix is reachable: a float or mixed or integer division by zero, a modulo by zero, `%` on a
/// float, `INT_MIN / -1`, `as int` of NaN, infinity and 2^59 and beyond, a non-number operand.
/// Used only against the JIT, never against ctang: the aim is the interpreter against compiled
/// code, floats compared by bits. A function exits at most a few times in the driver, so most
/// of its calls run compiled.
class ArithBuilder {
 public:
  explicit ArithBuilder(uint64_t seed) : e_(seed) {}

  std::string build() {
    line("big = \"1.7976931348623157e308\" as float; pinf = big * 10.0; ninf = 0.0 - pinf; nan = pinf - pinf; nz = -0.0;");
    line("tiny = \"5e-324\" as float; small = \"2.2250738585072014e-308\" as float; smax = 576460752303423487; smin = (0 - 576460752303423487) - 1;");
    line("gs = 0; gt = 0.0;");
    int functions = 3 + (int)below(5);
    for (int i = 0; i < functions; ++i) {
      function(i);
    }
    // The arguments: floats, integers (both bounds), the specials, a string, null, a boolean.
    line("sp = [0.0, nz, 1.5, -2.5, pinf, ninf, nan, tiny, big, 3, -3, 7, 0, 1, -1, 2, smax, smin, 9007199254740993, small, \"s\", null, true];");
    line("acc = 0;");
    for (int i = 0; i < functions; ++i) {
      std::string n = std::to_string(i);
      line("for (a" + n + " = 0; a" + n + " < " + std::to_string(3 + below(5)) + "; a" + n + " += 1) { print(c" + n + "(sp[" + pick_index() + "], sp[" + pick_index() + "])); print(\",\"); }");
    }
    line("print(gs); print(gt);");
    line("acc;");
    return out_;
  }

 private:
  std::mt19937_64 e_;
  std::string out_;
  int functions_ = 0;

  uint64_t below(uint64_t n) { return e_() % n; }
  bool chance(unsigned percent) { return below(100) < percent; }
  void line(const std::string & s) { out_ += s + "\n"; }
  std::string pick_index() { return std::to_string(below(23)); }
  template <size_t N>
  const char * pick(const char * const (&items)[N]) { return items[below(N)]; }

  std::string literal() {
    static const char * const f[] = {"0.5", "1.5", "2.25", "0.1", "0.0", "3.0", "-1.5", "123456789.125", "9007199254740993.0", "0.30000000000000004", "100.0", "576460752303423424.0"};
    return f[below(sizeof f / sizeof f[0])];
  }
  std::string integer() {
    static const char * const f[] = {"0", "1", "-1", "2", "-2", "3", "7", "-7", "10", "smax", "smin", "1000", "5"};
    return f[below(sizeof f / sizeof f[0])];
  }
  std::string special() {
    static const char * const f[] = {"pinf", "ninf", "nan", "nz", "tiny", "big", "small"};
    return f[below(sizeof f / sizeof f[0])];
  }
  /// A float operand: a local, a literal, a special, an integer (mixed), a parameter, or the loop counter.
  std::string operand(int fl, int in) {
    switch (below(10)) {
      case 0: case 1: case 2: case 3: return "x" + std::to_string(below(fl));
      case 4: return literal();
      case 5: return special();
      case 6: return "i" + std::to_string(below(in));
      case 7: return integer();
      case 8: return below(2) ? "p0" : "p1";
      default: return "j";
    }
  }
  /// A divisor, steered to zero some of the time, to NaN and infinity, to -1 and to a bound.
  std::string divisor(int fl, int in) {
    switch (below(12)) {
      case 0: return "0.0";
      case 1: return "nz";
      case 2: return "(x" + std::to_string(below(fl)) + " - x" + std::to_string(below(fl)) + ")";
      case 3: return "0";
      case 4: return "nan";
      case 5: return "pinf";
      case 6: return "-1";
      case 7: return "(j - j)";
      case 8: return literal();
      case 9: return "j";
      default: return operand(fl, in);
    }
  }
  std::string idivisor(int in) {
    switch (below(9)) {
      case 0: return "0";
      case 1: return "-1";
      case 2: return "(j - j)";
      case 3: return "smin";
      case 4: return "smax";
      case 5: return "j";
      case 6: return "2";
      case 7: return "i" + std::to_string(below(in));
      default: return "3";
    }
  }
  std::string compare_op() {
    static const char * const o[] = {"<", "<=", ">", ">=", "==", "!="};
    return o[below(6)];
  }
  std::string float_op() {
    static const char * const o[] = {"+", "-", "*", "/"};
    return o[below(4)];
  }

  void statement(int fl, int in, const std::string & indent) {
    std::string a = "x" + std::to_string(below(fl));
    std::string i = "i" + std::to_string(below(in));
    switch (below(16)) {
      case 0: case 1: case 2: {
        std::string op = float_op();
        std::string right = op == "/" ? divisor(fl, in) : operand(fl, in);
        // The left operand is a float local (a raw source), so that the result is raw too: a local that
        // is a word on one path and a double on another would merge unequal kinds, which compiles with
        // no raw slot at all. One statement in six has two operands that may both be words.
        line(indent + a + " = " + (chance(85) ? "x" + std::to_string(below(fl)) : operand(fl, in)) + " " + op + " " + right + ";");
        break;
      }
      case 3:
        line(indent + a + " = -" + (chance(85) ? "x" + std::to_string(below(fl)) : operand(fl, in)) + ";");
        break;
      case 4:
        line(indent + "if (" + operand(fl, in) + " " + compare_op() + " " + operand(fl, in) + ") { " + a + " = " + a + " + 1.5; } else { " + a + " = " + a + " * 0.5; }");
        break;
      case 5:
        line(indent + i + " = " + integer() + " / " + idivisor(in) + ";");
        break;
      case 6:
        // Mostly integers; a float operand now and then, which the interpreter refuses.
        line(indent + i + " = " + (chance(20) ? operand(fl, in) : integer()) + " % " + idivisor(in) + ";");
        break;
      case 7:
        // Casts to int: any float local, a special, and values beyond the range of either size.
        switch (below(5)) {
          case 0: line(indent + i + " = " + a + " as int;"); break;
          case 1: line(indent + i + " = " + special() + " as int;"); break;
          case 2: line(indent + i + " = (" + a + " * 1000000000000000000.0) as int;"); break;
          case 3: line(indent + i + " = (" + a + " * big) as int;"); break;
          default: line(indent + i + " = (" + a + " - 0.5) as int;"); break;
        }
        break;
      case 8:
        line(indent + a + " = " + i + " as float;");
        break;
      case 9:
        line(indent + a + " = " + operand(fl, in) + " as float;");
        break;
      case 10:
        line(indent + "acc = (acc + (" + a + " " + compare_op() + " " + operand(fl, in) + ")) % 1000;");
        break;
      case 11:
        line(indent + i + " = " + i + " + " + integer() + " * " + integer() + ";");
        break;
      case 12:
        line(indent + i + " = " + i + " % " + idivisor(in) + ";");
        break;
      case 13:
        line(indent + a + " = x" + std::to_string(below(fl)) + " " + float_op() + " " + operand(fl, in) + " " + float_op() + " " + operand(fl, in) + ";");
        break;
      case 14:
        line(indent + "print(" + a + " " + compare_op() + " " + operand(fl, in) + "); print(\"c\");");
        break;
      default:
        line(indent + "if (" + a + " " + compare_op() + " " + operand(fl, in) + ") { acc = acc + 1; }");
        break;
    }
  }

  void function(int index) {
    std::string name = "c" + std::to_string(index);
    line("function " + name + "(p0, p1) {");
    line("  global big; global pinf; global ninf; global nan; global nz; global tiny; global small; global smax; global smin; global gs; global gt;");
    int fl = 2 + (int)below(4);
    int in = 1 + (int)below(3);
    // The raw sources: a literal, a cast of a parameter, a cast of an integer, a special.
    for (int k = 0; k < fl; ++k) {
      std::string v = "  x" + std::to_string(k) + " = ";
      switch (below(5)) {
        case 0: line(v + literal() + ";"); break;
        case 1: line(v + "p0 as float;"); break;
        case 2: line(v + "p1 as float;"); break;
        case 3: line(v + integer() + " as float;"); break;
        default: line(v + literal() + " + " + literal() + ";"); break;
      }
    }
    for (int k = 0; k < in; ++k) {
      line("  i" + std::to_string(k) + " = " + integer() + ";");
    }
    line("  acc = 0; j = 0;");
    int body = 1 + (int)below(3);
    for (int s = 0; s < body; ++s) {
      statement(fl, in, "  ");
    }
    int loops = 1 + (int)below(2);
    for (int l = 0; l < loops; ++l) {
      line("  for (j = 0; j < " + std::to_string(2 + below(6)) + "; j += 1) {");
      int count = 2 + (int)below(5);
      for (int s = 0; s < count; ++s) {
        statement(fl, in, "    ");
      }
      line("  }");
    }
    // Consumers: a print, a global store, a branch, and what the function returns.
    int consumers = (int)below(3);
    for (int c = 0; c < consumers; ++c) {
      std::string v = "x" + std::to_string(below(fl));
      switch (below(5)) {
        case 0: line("  print(" + v + "); print(\";\");"); break;
        case 1: line("  gt = " + v + ";"); break;
        case 2: line("  if (" + v + " " + compare_op() + " " + operand(fl, in) + ") { acc = acc + 3; }"); break;
        case 3: line("  print(i" + std::to_string(below(in)) + "); print(\";\");"); break;
        default: line("  gs = (gs + 1) % 1000;"); break;
      }
    }
    switch (below(4)) {
      case 0: line("  return x" + std::to_string(below(fl)) + ";"); break;
      case 1: line("  return i" + std::to_string(below(in)) + ";"); break;
      default: line("  return acc;"); break;
    }
    line("}");
    ++functions_;
  }
};

}  // namespace detail

/// A call-graph program for `seed` (a script): calls between functions of up to
/// fifteen parameters, in loops, by recursion and tree recursion, with function
/// values, an array argument, and now and then recursion to the depth budget.
inline Program generate_calls(uint64_t seed, bool natives = false) {
  Program p;
  p.seed = seed;
  p.mode = Mode::Script;
  p.source = detail::CallBuilder(seed, natives).build();
  return p;
}

/// A program for `seed` that carries floats in locals and operand slots across loops
/// and calls and consumes them in every way (a script).
inline Program generate_carry(uint64_t seed) {
  Program p;
  p.seed = seed;
  p.mode = Mode::Script;
  p.source = detail::CarryBuilder(seed).build();
  return p;
}

/// A program for `seed` that computes with floats and divides (a script).
inline Program generate_arith(uint64_t seed) {
  Program p;
  p.seed = seed;
  p.mode = Mode::Script;
  p.source = detail::ArithBuilder(seed).build();
  return p;
}

/// The program for `seed` in `mode`.
inline Program generate(uint64_t seed, Mode mode) {
  Program p;
  p.seed = seed;
  p.mode = mode;
  p.source = detail::Builder(seed, mode).build();
  return p;
}

}  // namespace gen

#endif
