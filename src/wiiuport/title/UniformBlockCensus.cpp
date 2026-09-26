#include "wiiuport/title/UniformBlockCensus.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace wiiuport::title {

namespace {

// A JSON object written one member at a time, owning the separators and the
// quoting, so a report cannot come out with a stray quote in it.
class JsonBody {
  public:
    void raw(const char* name, const std::string& value) {
        separate();
        m_body += '"';
        m_body += name;
        m_body += "\":";
        m_body += value;
    }

    void string(const char* name, const std::string& value) {
        raw(name, "\"" + value + "\"");
    }

    void number(const char* name, uint64_t value) {
        raw(name, std::to_string(value));
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

std::string hex(uint32_t value) {
    std::array<char, 11> text{};
    std::snprintf(text.data(), text.size(), "0x%08x", value);
    return {text.data()};
}

std::string_view installationName(std::optional<GuestCallProbes::Installation> value) {
    if (!value.has_value()) {
        return "pending";
    }
    switch (*value) {
    case GuestCallProbes::Installation::Installed:
        return "installed";
    case GuestCallProbes::Installation::EntryHeldOther:
        return "entryHeldOther";
    case GuestCallProbes::Installation::EntryNotRelocatable:
        return "entryNotRelocatable";
    case GuestCallProbes::Installation::NoCodeSpace:
        return "noCodeSpace";
    }
    return "unknown";
}

} // namespace

UniformBlockCensus::UniformBlockCensus(Register registerProbe, ReadWord readWord)
    : m_register(registerProbe), m_readWord(readWord) {
}

void UniformBlockCensus::install() {
    m_register(kBinder, kFirstInstruction, m_first);
    m_register(kBinderSecond, kFirstInstruction, m_second);
}

void UniformBlockCensus::Binder::OnInstall(GuestCallProbes::Installation result) {
    std::scoped_lock lock(mutex);
    installation = result;
}

void UniformBlockCensus::Binder::OnCall(std::span<const uint32_t, 32> gpr,
                                        uint32_t /*returnAddress*/) {
    m_owner.record(gpr[3], m_second);
}

void UniformBlockCensus::record(uint32_t object, bool second) {
    // Counted before the lock: a binding on the display thread must not be able
    // to block behind a report being written.
    m_bindings.fetch_add(1, std::memory_order_relaxed);
    if (object == 0) {
        return;
    }
    Binding binding;
    binding.read = m_readWord(object + kCursorOffset, binding.cursor);
    if (binding.read) {
        // The whole entry, word for word. A partial read is a partial answer and
        // is reported as unread rather than as zeroes, because a block that was
        // dumped from zeroes would look like a real one.
        const uint32_t entry = object + kEntriesOffset + binding.cursor * kEntrySize;
        for (size_t word = 0; word < kEntryWords; word++) {
            binding.read = m_readWord(entry + 4 * static_cast<uint32_t>(word),
                                      binding.entry[word]) &&
                           binding.read;
        }
    }
    if (binding.read) {
        // Which words, added to the offset, name memory the guest can read.
        // One word is enough to answer that, and reading four costs nothing.
        uint32_t probe = 0;
        for (size_t word = 0; word < kEntryWords; word++) {
            const uint32_t base = binding.entry[word] + binding.entry[kEntryOffsetOffset / 4];
            binding.mapped[word] = m_readWord(base, probe) && m_readWord(base + 4, probe);
        }
    }
    std::scoped_lock lock(m_mutex);
    if (std::find(m_seen.begin(), m_seen.end(), object) == m_seen.end()) {
        // Capped: a list of every object in a scene is a list of everything the
        // title has ever drawn, and the count is what the report needs.
        if (m_seen.size() < 4096) {
            m_seen.push_back(object);
            m_objects = m_seen.size();
        } else {
            m_objects = m_seen.size() + 1;
        }
    }
    if (binding.read && binding.cursor < static_cast<uint32_t>(kEntries)) {
        m_cursors[binding.cursor]++;
    } else {
        m_cursorsOutOfRange++;
    }
    if (m_exampleCount < kExamples) {
        m_examples[m_exampleCount] = binding;
        m_exampleCount++;
    }
    (void)second;
}

std::string UniformBlockCensus::json() const {
    std::scoped_lock lock(m_mutex);
    JsonBody body;
    body.string("binder", hex(kBinder));
    body.string("binderSecond", hex(kBinderSecond));
    body.string("probe", std::string(installationName(m_first.installation)));
    body.string("probeSecond", std::string(installationName(m_second.installation)));
    body.number("entries", kEntries);
    body.number("bindings", m_bindings.load());
    body.number("objects", m_objects);
    JsonBody cursors;
    for (int entry = 0; entry < kEntries; entry++) {
        cursors.number(std::to_string(entry).c_str(), m_cursors[entry]);
    }
    body.raw("cursors", cursors.text());
    body.number("cursorsOutOfRange", m_cursorsOutOfRange);
    JsonBody examples;
    for (size_t index = 0; index < m_exampleCount; index++) {
        const Binding& binding = m_examples[index];
        JsonBody one;
        one.number("cursor", binding.cursor);
        one.number("offset", binding.entry[kEntryOffsetOffset / 4]);
        one.number("size", binding.entry[kEntrySizeOffset / 4]);
        JsonBody words;
        for (size_t word = 0; word < kEntryWords; word++) {
            words.number(std::to_string(word).c_str(), binding.entry[word]);
        }
        one.raw("entry", words.text());
        JsonBody readable;
        for (size_t word = 0; word < kEntryWords; word++) {
            readable.raw(std::to_string(word).c_str(), binding.mapped[word] ? "true" : "false");
        }
        one.raw("readableAtOffset", readable.text());
        one.raw("read", binding.read ? "true" : "false");
        examples.raw(std::to_string(index).c_str(), one.text());
    }
    body.raw("examples", examples.text());
    if (!m_refusal.empty()) {
        body.string("refusal", m_refusal);
    }
    return body.finish();
}

} // namespace wiiuport::title
