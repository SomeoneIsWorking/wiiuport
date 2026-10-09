#pragma once

#include <cctype>
#include <cstddef>
#include <functional>
#include <string>

namespace wiiuport::tests {

// JSON's number grammar: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
inline bool isJsonNumber(const std::string& token) {
    size_t i = 0;
    const size_t n = token.size();
    auto digits = [&] {
        const size_t start = i;
        while (i < n && std::isdigit(static_cast<unsigned char>(token[i])) != 0) {
            ++i;
        }
        return i > start;
    };
    if (i < n && token[i] == '-') {
        ++i;
    }
    if (i < n && token[i] == '0') {
        ++i;
    } else if (!digits()) {
        return false;
    }
    if (i < n && token[i] == '.') {
        ++i;
        if (!digits()) {
            return false;
        }
    }
    if (i < n && (token[i] == 'e' || token[i] == 'E')) {
        ++i;
        if (i < n && (token[i] == '+' || token[i] == '-')) {
            ++i;
        }
        if (!digits()) {
            return false;
        }
    }
    return i == n;
}

// The offset of the first thing a JSON client refuses, and why; empty when the body parses.
inline std::string firstJsonFault(const std::string& text, size_t& at) {
    const size_t n = text.size();
    size_t i = 0;
    auto skip = [&] {
        while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
            ++i;
        }
    };
    std::function<bool()> value = [&]() -> bool {
        skip();
        if (i >= n) {
            return false;
        }
        const char c = text[i];
        if (c == '"') {
            ++i;
            while (i < n) {
                if (text[i] == '"') {
                    ++i;
                    return true;
                }
                if (text[i] == '\\') {
                    i += 2;
                    continue;
                }
                ++i;
            }
            at = n;
            return false;
        }
        if (c == '{' || c == '[') {
            char close = c == '{' ? '}' : ']';
            ++i;
            skip();
            if (i < n && text[i] == close) {
                ++i;
                return true;
            }
            while (true) {
                if (close == '}') {
                    skip();
                    if (i >= n || text[i] != '"') {
                        return false;
                    }
                    if (!value()) {
                        return false;
                    }
                    skip();
                    if (i >= n || text[i] != ':') {
                        return false;
                    }
                    ++i;
                }
                if (!value()) {
                    return false;
                }
                skip();
                if (i < n && text[i] == ',') {
                    ++i;
                    skip();
                    if (i < n && text[i] == close) {
                        at = i;
                        return false; // a trailing comma
                    }
                    continue;
                }
                if (i < n && text[i] == close) {
                    ++i;
                    return true;
                }
                return false;
            }
        }
        size_t j = i;
        while (j < n && text[j] != ',' && text[j] != '}' && text[j] != ']' && text[j] != '\n' &&
               text[j] != '\r' && text[j] != ' ' && text[j] != '\t') {
            ++j;
        }
        const std::string token = text.substr(i, j - i);
        if (token == "true" || token == "false" || token == "null") {
            i = j;
            return true;
        }
        if (token.empty()) {
            return false;
        }
        if (!isJsonNumber(token)) {
            at = i;
            return false;
        }
        i = j;
        return true;
    };
    if (!value()) {
        if (at == std::string::npos) {
            at = i;
        }
        return "the body stops being JSON here";
    }
    skip();
    if (i != n) {
        at = i;
        return "the body continues after the value";
    }
    at = n;
    return "";
}

} // namespace wiiuport::tests
