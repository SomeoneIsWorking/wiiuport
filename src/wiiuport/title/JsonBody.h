#pragma once

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
