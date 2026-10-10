# The Tang language reference

This is the reference for the Tang language as lang-tang runs it. Once
lang-tang and ctang reach parity this document is the standard and ctang is
deprecated. It is not complete yet: the language reference in the `documentation` directory of the ctang repository (`documentation/language-reference.md` there) still describes everything this document does not cover, and a reader should take that one as
the base and this one as the amendments. **Where the two differ, this one
wins**, and the [divergence ledger](divergence-ledger.md) lists each
difference with the program that shows it.

Only what has been stated here is the standard; sections are added as
behaviour is decided or as an old section of ctang's reference is carried over.
The numbering follows ctang's, so "4.12" means the same part of the language
in both.

## 4.12 Printing numbers

`print(expression)` appends a value to the output, and `expression as string`
makes the same text. For numbers the text is as follows. It is the same in the
interpreter and in compiled code, on every host, and in the debugger's and
the frame observer's text for a value.

### Integers

An integer prints as decimal digits, with a `-` in front when it is negative:
`0`, `42`, `-5`, `-9223372036854775808`. There is no thousands separator, no
leading `+` and no leading zero.

### Floats

A float prints in **fixed notation with six digits after the point, trailing
zeros removed and the point kept**:

| Value | Prints |
| --- | --- |
| `3.5` | `3.5` |
| `1. / 3.` | `0.333333` |
| `100.` | `100.` |
| `1.0` | `1.` |
| `3.0 as string` | `"3."` |
| `0.1 + 0.2` | `0.3` |
| `1234567.891` | `1234567.891` |
| `99999999999999999999999.0` | `99999999999999991611392.` |

There is no exponent form: a large float prints all its integer digits, and
the digits are the exact decimal value of the double that the program holds,
which is why the last row ends in `1611392` and not in nines. The point is
always `.`, whatever the host's locale says.

A float too small to show in six digits prints as zero, keeping its sign:
`0.0000004` prints as `0.` and `-0.0000001` prints as `-0.`.

The three non-finite or signed special values:

| Value | Prints |
| --- | --- |
| positive infinity | `inf` |
| negative infinity | `-inf` |
| negative zero | `-0.` |

`inf` and `-inf` come from a result too large for a double, such as
`99999999999999999999999.0` squared repeatedly. Negative zero is `-0.0` or the
product of zero and a negative number. It compares equal to `0.` but prints with
its sign.

### NaN

**Every NaN prints as `nan`.** It has no sign and no payload in the text: a
NaN made by `inf - inf`, a negated NaN, `"nan" as float`, and
`"-nan(0x1234)" as float` all print `nan`, in `print`, in `as string`, inside
an array or a map, and in the debugger. A program cannot find out which NaN it
holds.

This differs from ctang and from C. C prints `-nan` for a NaN whose sign bit
is set and may drop or keep a payload; which NaN an operation makes depends on
the CPU (the default NaN of `inf - inf` is negative on x86-64 and positive on
arm64) and, when both operands are NaN, on the order the compiler chose for
them. So ctang's `-nan` is a property of the build and the machine, not of the
program. The ledger row is D-031.

What a NaN does otherwise is unchanged:

- `nan == nan` is false and `nan != nan` is true, and a NaN is false in every
  ordered comparison.
- `nan as int` is the error `[NOT A NUMBER]`.
- Arithmetic with a NaN gives a NaN, which prints `nan`.

A host that reads the result of a run with `gltang_execution_result_float`
still gets the double the program holds, with whatever sign and payload it
has; the guarantee is about Tang's text, and a host that compares two
results compares NaNs as it sees fit (the test harnesses compare any two NaNs
as equal).
