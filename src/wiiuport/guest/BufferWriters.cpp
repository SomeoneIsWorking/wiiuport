#include "wiiuport/guest/BufferWriters.h"

#include <algorithm>

namespace wiiuport::guest {

void BufferWriters::record(const void* source, const interp::GuestObject& object,
                           const Leading& leading) {
    m_calls.fetch_add(1);
    std::lock_guard lock(m_mutex);
    Write write{.object = object, .leading = leading, .before = {}};
    const auto latest = m_byObject.find(object.address);
    // A pool slot given to a new object since is not this object's past.
    if (latest != m_byObject.end() && latest->second.object.continuesAs(object)) {
        write.before = latest->second.leading;
    }
    m_byObject.insert_or_assign(object.address, Latest{object, leading});
    m_bySource.insert_or_assign(source, write);
}

std::optional<BufferWriters::Write> BufferWriters::written(const void* source,
                                                           std::span<const std::byte> bytes) const {
    std::lock_guard lock(m_mutex);
    auto found = m_bySource.find(source);
    if (found == m_bySource.end()) {
        return std::nullopt;
    }
    const Write& write = found->second;
    if (bytes.size() < kLeadingBytes ||
        !std::ranges::equal(bytes.first(kLeadingBytes), write.leading)) {
        m_rewritten.fetch_add(1);
        return std::nullopt;
    }
    m_identified.fetch_add(1);
    return write;
}

} // namespace wiiuport::guest
