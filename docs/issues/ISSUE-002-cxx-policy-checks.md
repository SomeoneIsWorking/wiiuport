# ISSUE-002 — Syntax-aware C++ policy checks are not written yet

**State:** open, blocking the first first-party C++ file. **Affects:** ST-VERIFIER.

`.clang-format` and `.clang-tidy` are in place and confirmed: `clang-tidy --dump-config`
shows `clang-diagnostic-*` preserved alongside the added groups, and `--list-checks`
reports 251 active checks including `readability-braces-around-statements`.

Three rules `clang-tidy` cannot express still have no gate:

1. project-owned functions or variables declared in the global namespace,
2. `extern` declarations of project-owned symbols,
3. block-scope `static`, `const`, or `constexpr` variables.

**Why they are not written yet.** There is no first-party C++ in this repository. A
checker built now would have nothing real to run against, and a check that has never
diagnosed the forbidden form on actual shipping source is not enforcement — it is a
process asset that looks like one.

**What must happen.** The commit that introduces the first first-party C++ file also
introduces these checks, each with an accepted and a rejected fixture, wired into the
normal verifier and run against the real source tree. A text grep does not satisfy this:
it counts comments and dead references, so it cannot tell a declaration from a mention.

Until then the C++ format gate honestly reports that it examined zero files rather than
passing silently.
