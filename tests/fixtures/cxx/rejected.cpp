// Each declaration here must be reported. Keep one violation per line so a
// test can assert the rule and the line together.
namespace wiiuport::fixture {
void body() {
    static int cached = 0;
    const int limit = 1;
    constexpr int stride = 2;
    // A const whose initializer is computed, which the rules keep as an ordinary local, and a
    // static whose initializer is not a literal -- the rule fires on `static` regardless, because a
    // function-local static is state that outlives the call.
    static int* derived = nullptr;
    const int computed = limit + stride;
    (void)cached;
    (void)limit;
    (void)stride;
    (void)derived;
    (void)computed;
}
}  // namespace wiiuport::fixture

int orphan_function() { return 0; }
int orphan_variable = 0;
extern int borrowed_variable;
