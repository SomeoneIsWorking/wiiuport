// Each declaration here must be reported. Keep one violation per line so a
// test can assert the rule and the line together.
namespace wiiuport::fixture {
void body() {
    static int cached = 0;
    const int limit = 1;
    constexpr int stride = 2;
    (void)cached;
    (void)limit;
    (void)stride;
}
}  // namespace wiiuport::fixture

int orphan_function() { return 0; }
int orphan_variable = 0;
extern int borrowed_variable;
