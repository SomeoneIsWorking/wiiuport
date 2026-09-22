#include "wiiuport/frame/RecordingSnapshot.h"

#include <cstring>
#include <utility>

namespace wiiuport::frame {
namespace {

template <typename T> void append(std::string& out, const T& value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

void appendU32(std::string& out, size_t value) {
    append(out, static_cast<uint32_t>(value));
}

} // namespace

bool RecordingSnapshot::arm(size_t frames) {
    std::lock_guard lock(m_mutex);
    if (frames == 0 || frames > kMaxFrames || m_wanted != 0) {
        return false;
    }
    m_wanted = frames;
    m_filling.clear();
    return true;
}

void RecordingSnapshot::onFrameRecorded(const FrameRecording& recording) {
    std::lock_guard lock(m_mutex);
    if (m_wanted == 0) {
        return;
    }
    m_filling.push_back(Frame{recording.isComplete(), recording.uniformAssemblies()});
    if (m_filling.size() < m_wanted) {
        return;
    }
    m_ready = std::move(m_filling);
    m_filling.clear();
    m_wanted = 0;
    ++m_completed;
}

std::string RecordingSnapshot::framed() const {
    std::lock_guard lock(m_mutex);
    if (m_ready.empty()) {
        return {};
    }
    std::string out(kMagic, std::strlen(kMagic));
    appendU32(out, m_ready.size());
    for (const Frame& frame : m_ready) {
        appendU32(out, frame.complete ? 1 : 0);
        appendU32(out, frame.assemblies.size());
        for (const RecordedUniformAssembly& assembly : frame.assemblies) {
            append(out, assembly.shaderBaseHash);
            append(out, assembly.shaderAuxHash);
            append(out, assembly.stageIndex);
            appendU32(out, assembly.blockSources.size());
            out.append(reinterpret_cast<const char*>(assembly.blockSources.data()),
                       assembly.blockSources.size() * sizeof(uint32_t));
            appendU32(out, assembly.data.size());
            out.append(reinterpret_cast<const char*>(assembly.data.data()),
                       assembly.data.size() * sizeof(float));
        }
    }
    return out;
}

uint64_t RecordingSnapshot::snapshotsCompleted() const {
    std::lock_guard lock(m_mutex);
    return m_completed;
}

} // namespace wiiuport::frame
