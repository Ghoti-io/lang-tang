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
  int loop_vars_used_ = 0;
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

  std::string float_literal() {
    static const char * const f[] = {"0.5", "1.5", "2.25", "3.", ".75", "100.", "0.1", "0.3333", "7.125", "1.", "99999999999999999999999.0", "0.0", "123456.789", "2.5"};
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
    switch (below(14)) {
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
      case 8: return "(\"f=\" + san(" + floating(d - 1) + "))";
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
          code(arr_var() + "[(" + integer(1) + ") % 10] = " + integer(2) + ";");
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
