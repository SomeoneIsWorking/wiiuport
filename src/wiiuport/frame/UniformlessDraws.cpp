#include "wiiuport/frame/UniformlessDraws.h"

#include <functional>
#include <string_view>

namespace wiiuport::frame {

void UniformlessDraws::onDraw(const LatteFrameHooks::DrawPrepared& draw) {
    uint64_t hash = 0;
    uint64_t bytes = 0;
    for (uint32_t index = 0; index < draw.vertexBufferCount; ++index) {
        const LatteFrameHooks::DrawPrepared::VertexBuffer& buffer = draw.vertexBuffers[index];
        std::string_view data(static_cast<const char*>(buffer.data), buffer.sizeInBytes);
        // Order matters: the same bytes in another buffer are another draw.
        hash = (hash * 0x100000001b3ULL) ^ std::hash<std::string_view>{}(data);
        bytes += buffer.sizeInBytes;
    }
    VertexShader shader{draw.vertexShaderBaseHash, draw.vertexShaderAuxHash};
    std::lock_guard lock(m_mutex);
    Counts& counts = m_counts[shader];
    ++counts.draws;
    counts.bytesHashed += bytes;
    m_frame[shader].push_back(hash);
}

void UniformlessDraws::onFrameComplete() {
    std::lock_guard lock(m_mutex);
    for (const auto& [shader, hashes] : m_frame) {
        auto before = m_previous.find(shader);
        size_t matched =
            before == m_previous.end() ? 0 : std::min(hashes.size(), before->second.size());
        Counts& counts = m_counts[shader];
        for (size_t occurrence = 0; occurrence < matched; ++occurrence) {
            if (hashes[occurrence] != before->second[occurrence]) {
                ++counts.changed;
            }
        }
        counts.unmatched += hashes.size() - matched;
    }
    m_previous.swap(m_frame);
    for (auto& [shader, hashes] : m_frame) {
        hashes.clear();
    }
}

std::map<UniformlessDraws::VertexShader, UniformlessDraws::Counts>
UniformlessDraws::byShader() const {
    std::lock_guard lock(m_mutex);
    return m_counts;
}

} // namespace wiiuport::frame
