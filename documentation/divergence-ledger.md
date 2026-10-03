# Divergence ledger

Every way lang-tang is allowed to differ from frozen ctang is a row below.
Nothing else is. ctang is the oracle (spine AD-16): the corpus is parsed by
both, and from the story that adds execution on, run by both, and a difference
that no row names fails the differential. ctang retires when the ledger is
**closed**, meaning no row is `open` (AD-3).

| Column | Meaning |
| --- | --- |
| `id` | `D-001`, `D-002`, ... Never reused. |
| `category` | `ctang-defect` (ctang is wrong and lang-tang is right), `limit` (a bound ctang has no equivalent of), `error-reporting` (how an error reaches the host), `rng` (the random generator). |
| `status` | `open` (nobody has decided yet), `recorded` (a deliberate departure, accepted), `fixed` (the difference is gone). Only a `recorded` row that names corpus files accepts a divergence on them, and it is **stale**, and fails the differential, once the named file stops diverging. |
| `ref` | The number of the language reference's section 13 item the row comes from, or `-`. |
| `corpus` | The corpus files (under `tests/corpus/`, comma separated) the row covers, or `-`. |
| `resolved-by` | The story that decides the row or records its end state. |
| `summary` | What differs and why. |

The table is validated by `tests/unit/test_ledger.cpp`: unique ids, valid
categories and statuses, every named corpus file exists, and a malformed row
fails (shown on a planted ledger). `tests/oracle/test_oracle.cpp` holds the
differential against it.

| id | category | status | ref | corpus | resolved-by | summary |
| --- | --- | --- | --- | --- | --- | --- |
| D-001 | ctang-defect | open | 9 | - | 11 | Section 13.9. Assigning through a string index is `Not supported` and the string is unchanged, but the error is discarded unless it is the last value of the program: errors are values, and nothing tells the host about one that was swallowed. Not a defect of the string, a gap in the error model (section 14); lang-tang's error list is what closes it, and the execution differential decides whether the row becomes `recorded` or `fixed`. |
| D-002 | ctang-defect | open | 13 | script/reject-date-now.tang, script/reject-date-today.tang, script/reject-date-absolute.tang, script/reject-date-absolute-time.tang, script/reject-date-relative.tang, script/reject-date-timezone.tang, template/reject-date.tang | 9 | Section 13.13. `@` starts a date literal that the scanner recognises (now, today, relative, absolute, time zones) and the parser declares eleven tokens for but no grammar rule consumes. Both engines refuse every one of these files today, so there is no divergence yet. Finishing dates (a date type) or removing the scanner state is a language decision (section 14); the compiler story takes it, and it stays open until then. |
| D-003 | rng | recorded | - | - | 10 | ctang shares one `random.global` across the whole process and seeds `random.default` from the clock. lang-tang gives each context its own generator, created lazily and seeded from the group's seed sequence. `seeded(n)` still matches `std::mt19937_64`. |
| D-004 | limit | recorded | - | - | 9 | Budgets. ctang's only limit is call depth (`max_call_depth`, 512); a template can loop until the host kills it. lang-tang adds fuel, wall-clock, memory and guest-stack budgets, which ctang has no equivalent of, so a program that exhausts one has no ctang result to compare. |
| D-005 | limit | recorded | - | - | 10 | The pause and unwind outcomes. A run that exhausts a budget pauses (`paused`: the host may raise the budget and resume) or unwinds with a limit error; ctang has neither. "lang-tang paused" and "ctang killed by the harness" are counted as agreement (AD-16). |
| D-006 | error-reporting | recorded | - | - | 10 | Errors reach the host. ctang loses an error that is not the last statement's value; lang-tang records each swallowed error in the context's error list with its template, the chain of template calls above it, and file and line, logs every error at creation under the host's switch, and unwinds with `ERR_GUEST` under the host's halt-on-first-error option. Errors stay values inside the language. |
| D-007 | ctang-defect | fixed | - | - | 8 | Allocation failure in the grammar. About thirty ctang rules leaked their operands when a node's `create` refused, three (ranged `for`, `global`, `use IDENTIFIER;`) freed an adopted string twice, and flex called `exit(2)` when it could not allocate. lang-tang frees once, reserves the scanner's memory before the scan, and answers `OOM`. Observable only under allocation failure. |
| D-008 | ctang-defect | fixed | - | - | 8 | A string that fails UTF-8 validation was reported by ctang as out of memory. lang-tang reports the syntax error and names the cause. |
