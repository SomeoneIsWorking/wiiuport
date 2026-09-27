/// Writing a measured value into a JSON body.
///
/// One formatter, shared by every report in the title namespace, because there were three:
/// two precisions, and one of them with no guard. Both the bug and its cause are in here --
/// a value that prints as zero when it is not zero, and a value that prints as `inf` and takes
/// the whole report with it.
#include "check.h"
#include "suites.h"
#include "wiiuport/title/JsonBody.h"

#include <cmath>
#include <limits>
#include <string>

namespace {

using wiiuport::title::JsonBody;

} // namespace

void wiiuport::tests::runJsonBodyTests() {
    // Nine significant digits, because six fixed decimals print a difference of one part in
    // 10^40 as zero, and a report that says "moved 18, biggest delta 0.000000" cannot be read
    // at all: it looks like a measurement that contradicts itself.
    check::isTrue(JsonBody::real(0.0) == "0", "zero is zero");
    check::isTrue(JsonBody::real(1.5) == "1.5", "one and a half is not one and a half million");
    check::isTrue(JsonBody::real(1e-40) == "1e-40",
                  "and a value a hundred billion times smaller than the tolerance is still a "
                  "number rather than a zero, which is the case six decimals lost");
    check::isTrue(JsonBody::real(0.1f + 0.2f) != "0",
                  "a float that is not exactly representable does not print as nothing");

    // **Non-finite is a fact about memory, not a number.** Guest memory holds denormals and
    // values large enough to overflow a float, and a numeric formatter writes those as `inf`
    // and `nan`. JSON allows neither, a parser stops at the first one, and the whole report
    // becomes unreadable -- which happened: `GET /blocks` answered with something no client
    // could read and the run called it an I/O failure. So it is a quoted string, and the
    // reader is told rather than handed a document that will not parse.
    check::isTrue(JsonBody::real(std::numeric_limits<double>::infinity()) == "\"inf\"",
                  "infinity is written as a quoted string, not as a bare token");
    check::isTrue(JsonBody::real(-std::numeric_limits<double>::infinity()) == "\"-inf\"",
                  "and so is negative infinity, with its sign");
    check::isTrue(JsonBody::real(std::numeric_limits<double>::quiet_NaN()) == "\"nan\"",
                  "and not-a-number, which is not even a sign");

    // The formatter's contract is that a body it wrote parses. Checked by construction here --
    // the values above are the ones that broke it, and a body carrying one of them is the
    // thing a client reads.
    {
        JsonBody body;
        body.raw("scale", JsonBody::real(std::numeric_limits<double>::infinity()));
        body.raw("delta", JsonBody::real(1e-40));
        body.number("count", 7);
        body.raw("moving", "false");
        const std::string text = body.finish();
        check::isTrue(text.find("inf") != std::string::npos &&
                          text.find("\"scale\":\"inf\"") != std::string::npos,
                      "the inf survives inside its quotes, so the fact is not lost by being "
                      "made safe: " +
                          text);
        check::isTrue(text.find("\"delta\":1e-40") != std::string::npos,
                      "and a tiny value keeps its own magnitude: " + text);
        check::isTrue(text.find("\"count\":7") != std::string::npos &&
                          text.find("\"moving\":false") != std::string::npos,
                      "while a whole number and a literal are untouched, because the guard is "
                      "for measured floats and not for everything");
        check::isTrue(text.front() == '{' && text.find("}\n") != std::string::npos,
                      "and the body is one object that ends");
    }

    // The names and the value each takes are the form that form must be, and `raw` is the
    // only one that takes a value as it stands -- which is why the formatter exists: a caller
    // with a measured float should not be reaching for `raw` and a format string.
    {
        JsonBody body;
        body.string("name", "node");
        body.raw("hex", "0x02160018");
        const std::string text = body.finish();
        check::isTrue(text.find("\"name\":\"node\"") != std::string::npos &&
                          text.find("\"hex\":0x02160018") != std::string::npos,
                      "a string is quoted and a raw value is not, and neither needs the other: " +
                          text);
    }
}
