#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace wiiuport::title {

// A JSON object written one member at a time, owning the separators and the quoting.
//
// Three reports in this namespace are written by hand rather than by a library, and
// they all had their own copy of this. Two copies of a builder is one fix away from
// a third, and the bug this shape exists to prevent has happened twice in this
// project already: a report came out with a stray quote and a doubled comma, and
// another came out with an unquoted word where a client parses JSON, which made the
// whole report unreadable and every caller fall back on something else. A member
// goes through one of these methods, `text()` is the only way to nest, and `finish()`
// the only way to close.
//
// The names are `raw`, `string`, `number` and `object`, and the value each takes is
// the form that form must be: `raw` for something already in its final shape, the
// others for the three things JSON has. Nothing here validates, because a report that
// is going to be wrong should say so at its own call site.
class JsonBody {
  public:
    // A measured value as a JSON number, and the one way to write one.
    //
    // Nine significant digits, not six fixed decimals: a difference of one part in 10^40
    // prints as zero under %.6f, and a reader told "moved 18, biggest delta 0.000000" has no
    // way to tell a bit of noise from a value that was never computed.
    //
    // **And never `inf` or `nan`, because a client parses this.** Guest memory holds denormals
    // and values large enough to overflow, and a numeric formatter writes those as `inf` and
    // `nan`, which JSON does not allow; one of them made a whole report unparseable and the
    // run reported it as an I/O failure. A non-finite measurement is a fact about memory, not
    // a number, so it is written as a quoted string and the reader is told.
    //
    // This was two copies of a formatter with two precisions, one of which could emit `inf`,
    // in two locators that read the same kind of thing.
    // **A number as `0x…`, and the one way this project spells a hash or an address in a report.**
    // Three files wanted it and two of them had a copy -- `NodePoseLocator::hexValue` and
    // `GlobalPoseCensus::hexValue` -- and a report whose addresses are hex in one section and
    // decimal in the next is a report a reader has to check twice. Every one of these is a value
    // that was written as hex by the thing that produced it, so it is written as hex here.
    static std::string hex(uint64_t value) {
        char text[24];
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
        return {text};
    }

    static std::string real(double value, int significant = 9) {
        if (!std::isfinite(value)) {
            return value > 0.0 ? "\"inf\"" : (value < 0.0 ? "\"-inf\"" : "\"nan\"");
        }
        char text[40];
        std::snprintf(text, sizeof(text), "%.*g", significant, value);
        return {text};
    }

    // A value that brings its own form: true, false, a number, a nested object.
    void raw(const char* name, const std::string& value) {
        separate();
        m_body += '"';
        m_body += name;
        m_body += "\":";
        m_body += value;
    }

    void raw(const std::string& name, const std::string& value) {
        raw(name.c_str(), value);
    }

    void string(const char* name, const std::string& value) {
        raw(name, "\"" + value + "\"");
    }

    void string(const std::string& name, const std::string& value) {
        raw(name.c_str(), "\"" + value + "\"");
    }

    // Signed, because a translation's second component is negative in general and a
    // `uint64_t` cast of a negative float reports 18446744073709548616 -- a number a
    // reader has to know is a wrap, which is a field nobody reads correctly twice.
    void signedNumber(const char* name, int64_t value) {
        std::array<char, 32> text{};
        std::snprintf(text.data(), text.size(), "%lld", static_cast<long long>(value));
        raw(name, text.data());
    }

    void signedNumber(const std::string& name, int64_t value) {
        signedNumber(name.c_str(), value);
    }

    void number(const char* name, uint64_t value) {
        raw(name, std::to_string(value));
    }

    void number(const std::string& name, uint64_t value) {
        raw(name.c_str(), std::to_string(value));
    }

    // An object as a member: `inner` is another body's `text()`, unterminated,
    // because this one closes the whole thing.
    void object(const char* name, const std::string& inner) {
        raw(name, inner);
    }

    void object(const std::string& name, const std::string& inner) {
        raw(name.c_str(), inner);
    }

    // The body closed, with the newline a report ends on.
    std::string finish() const {
        return text() + "\n";
    }

    // The body closed, without it: for a member of another body.
    std::string text() const {
        return m_body + "}";
    }

  private:
    void separate() {
        if (!m_first) {
            m_body += ',';
        }
        m_first = false;
    }

    std::string m_body = "{";
    bool m_first = true;
};

} // namespace wiiuport::title
