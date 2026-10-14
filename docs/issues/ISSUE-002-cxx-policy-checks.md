# ISSUE-002 — Syntax-aware C++ policy checks

**State:** fixed (2026-10-14). **Affects:** ST-VERIFIER.

Three rules `clang-tidy` cannot express need an AST gate:

1. project-owned functions or variables in the global namespace,
2. `extern` declarations of project-owned symbols,
3. block-scope `static` variables.

The first gate (`tools/wiiuport/cxxpolicy.py` over libclang, written here) took about 48 minutes,
16-45 s per unit over 96 units, and was removed. Its replacement runs the shared re-harness
`tools/cpp_policy.py`, which the other ports gate on: it caches each unit's result beside its
compile database and scans both databases (114 units) in about 25 s cold. The new
`tools/wiiuport/cxxpolicy.py` only finds the checkout and runs it; `tests/test_cxx_policy.py`
shows it naming a global function and its static, passing a namespaced class, and refusing a
missing database or checkout.
