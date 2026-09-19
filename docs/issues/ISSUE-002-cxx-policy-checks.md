# ISSUE-002 — Syntax-aware C++ policy checks

**State:** resolved. **Affects:** ST-VERIFIER.

Three rules `clang-tidy` cannot express now have a gate:

1. project-owned functions or variables declared in the global namespace,
2. `extern` declarations of project-owned symbols,
3. block-scope `static`, `const`, or `constexpr` variables.

## How they are checked

`tools/wiiuport/cxxpolicy.py` walks libclang cursors, not text. A grep counts comments,
strings and dead references alongside live declarations, so it cannot tell a declaration
from a mention; the AST can. Real translation units are parsed with their own flags from
`build/compile_commands.json` when it exists, and headers with a `-std=c++20` baseline.

Exemptions are deliberate and narrow: an `extern "C"` boundary is exempt from rules 1
and 2 because a required C ABI declaration has nowhere else to live, and `main` is exempt
from rule 1. Rule 3 tests the variable's own type rather than its pointee, so `const T*`
and `const T&` locals and const-correct parameters stay legal.

## Evidence that it fires

`tests/fixtures/cxx/rejected.cpp` yields seven findings covering all three rules, each
carrying its file and line. `tests/fixtures/cxx/accepted.cpp` yields none while
containing a class, an `extern "C"` declaration and a const-pointee local, so it is not
passing by being empty — `test_the_accepted_fixture_is_not_empty` asserts that. A source
that cannot be parsed raises `CxxPolicyUnavailable` and fails the gate rather than
reporting clean, because a file that was never read had none of its declarations
inspected.

The gate prints its denominator every run (`parsed N first-party translation units`), so
today's `parsed 0` is legible as "there is no first-party C++ yet" rather than as a pass.

## What remains, and why it is elsewhere

Executing `clang-tidy` itself against a compile database still needs a compile database,
which needs the runtime to build. That gap stays recorded on ST-VERIFIER.
