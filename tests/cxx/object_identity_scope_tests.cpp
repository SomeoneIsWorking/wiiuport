/// Which object a uniform assembly belongs to.
///
/// The measurement this exists for: over 836,990 assembled buffers the fork's `blockSources`
/// matched exactly one identity across the 438,872 that had sources, because the uniform block
/// is re-uploaded at a new guest address each frame. Every "did this value change between two
/// draws of one object" comparison in the pose locator therefore happened 63 times, and a
/// movement count over 63 comparisons is not a measurement.
///
/// The node is not in anything the GX2 hook sees. It is one step away: the draw calls its
/// sub-object and the binder probe there publishes the object before the draw's uniforms are
/// uploaded. These tests are about that step working, and about the error rate being reported
/// rather than assumed -- a correlation whose error rate is not stated is one nobody can trust.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/ObjectIdentityScope.h"

#include <string>

namespace {

using wiiuport::title::ObjectIdentityScope;

std::string field(const std::string& body, const std::string& name) {
    const size_t at = body.find("\"" + name + "\":");
    if (at == std::string::npos) {
        return "";
    }
    const size_t start = at + name.size() + 3;
    size_t end = start;
    while (end < body.size() && body[end] != ',' && body[end] != '}') {
        end++;
    }
    return body.substr(start, end - start);
}

} // namespace

void wiiuport::tests::runObjectIdentityScopeTests() {
    // Nothing bound, nothing to read: zero is the answer, and it is a different answer from
    // any object address, so no flag is needed to tell them apart.
    {
        ObjectIdentityScope scope;
        check::isTrue(scope.current() == 0, "with nothing bound, a read is zero");
        check::isTrue(field(scope.json(), "assemblyQueries") == "1", "and the read is counted");
        check::isTrue(field(scope.json(), "assemblyQueriesWithObject") == "0",
                      "and counted as a read that found nothing, which is the coverage number "
                      "that matters");
    }

    // The whole mechanism: a bind, then reads, and the reads name the object.
    {
        ObjectIdentityScope scope;
        scope.bind(0x43e01000u);
        check::isTrue(scope.current() == 0x43e01000u, "a bound object is what a read returns");
        check::isTrue(scope.current() == 0x43e01000u,
                      "and still what a second read returns, because one binding covers several "
                      "of a draw's uniform uploads");
        const ObjectIdentityScope::Report r = scope.report();
        check::isTrue(r.binds == 1 && r.assemblyQueries == 2 && r.assemblyQueriesWithObject == 2,
                      "one bind, two reads, both with an object");
        check::isTrue(r.bindsSinceLastQuery == 0,
                      "and nothing bound between the reads, which is the possible-error count a "
                      "reader needs in order to believe the identity");
    }

    // **The error rate, stated.** A single slot is right only if nothing else binds between a
    // draw's binder call and its uniform uploads. If something does, the identity read is the
    // wrong object's -- not missing, wrong, which is worse -- so the count of binds seen since
    // the last read is reported and is expected to be zero.
    {
        ObjectIdentityScope scope;
        scope.bind(0x43e01000u);
        scope.current();
        scope.bind(0x43e02000u);
        scope.bind(0x43e03000u);
        const uint32_t read = scope.current();
        check::isTrue(read == 0x43e03000u, "three binds in a row, and the read names the last");
        const ObjectIdentityScope::Report r = scope.report();
        check::isTrue(r.bindsSinceLastQuery == 2,
                      "and the report says two binds happened since the previous read, so a "
                      "reader can see that the slot moved and judge the identity themselves");
        check::isTrue(r.binds == 3 && r.assemblyQueries == 2,
                      "with every bind and every read counted, neither of which can be dropped "
                      "from the total");
    }

    // An object address of zero is not a thing, so publishing one is a no-op rather than a
    // binding of the null object that a reader would then have to recognise.
    {
        ObjectIdentityScope scope;
        scope.bind(0x43e01000u);
        scope.bind(0);
        const ObjectIdentityScope::Report r = scope.report();
        check::isTrue(r.binds == 2,
                      "a publish of zero is counted as a publish, because hiding it would make "
                      "the bind count disagree with what the probe saw");
        check::isTrue(scope.current() == 0,
                      "and it does clear the slot, so an assembly after it is told nothing "
                      "rather than being told the previous object -- a wrong identity is worse "
                      "than a missing one");
    }

    // The report names its own source, so a reader can tell a node identity from a fallback
    // without cross-referencing the code.
    {
        ObjectIdentityScope scope;
        scope.bind(0x43e01000u);
        scope.current();
        const std::string body = scope.json();
        check::isTrue(field(body, "identitySource") == "\"binderObject\"",
                      "the report says where the identity came from: " + body);
        check::isTrue(field(body, "queriesWithObjectShare") == "1",
                      "and the share of reads that found an object is a number, not a claim");
        check::isTrue(body.front() == '{' && body.find("}\n") != std::string::npos,
                      "and the body is one object that ends, because a client parses it");
    }
}
