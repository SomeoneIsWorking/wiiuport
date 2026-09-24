#include "wiiuport/frame/RecordingSnapshot.h"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace wiiuport::frame {
namespace {

template <typename T> void append(std::string& out, const T& value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

void appendU32(std::string& out, size_t value) {
    append(out, static_cast<uint32_t>(value));
}

// Takes values off the front of a framed snapshot, refusing to read past it.
class Reader {
  public:
    explicit Reader(std::string_view bytes) : m_bytes(bytes) {
    }

    template <typename T> T take() {
        T value{};
        std::memcpy(&value, claim(sizeof(T)), sizeof(T));
        return value;
    }

    template <typename T> std::vector<T> takeArray(uint32_t count) {
        std::vector<T> values(count);
        std::memcpy(values.data(), claim(size_t{count} * sizeof(T)), size_t{count} * sizeof(T));
        return values;
    }

    bool finished() const {
        return m_bytes.empty();
    }

  private:
    const char* claim(size_t size) {
        if (size > m_bytes.size()) {
            throw std::invalid_argument("the snapshot ends in the middle of a frame");
        }
        const char* start = m_bytes.data();
        m_bytes.remove_prefix(size);
        return start;
    }

    std::string_view m_bytes;
};

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
    std::span<const RecordedUniformAssembly> assemblies = recording.uniformAssemblies();
    m_filling.push_back(Frame{recording.isComplete(), {assemblies.begin(), assemblies.end()}});
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
            appendU32(out, (assembly.writesColour ? kWritesColour : 0) |
                               (assembly.looksUpDepthMap ? kLooksUpDepthMap : 0));
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

std::vector<RecordingSnapshot::Frame> RecordingSnapshot::parse(std::string_view framed) {
    std::string_view magic(kMagic);
    if (!framed.starts_with(magic)) {
        throw std::invalid_argument("the snapshot does not start with WIIUREC3");
    }
    Reader reader(framed.substr(magic.size()));
    std::vector<Frame> frames(reader.take<uint32_t>());
    for (Frame& frame : frames) {
        frame.complete = reader.take<uint32_t>() != 0;
        frame.assemblies.resize(reader.take<uint32_t>());
        for (RecordedUniformAssembly& assembly : frame.assemblies) {
            assembly.shaderBaseHash = reader.take<uint64_t>();
            assembly.shaderAuxHash = reader.take<uint64_t>();
            assembly.stageIndex = reader.take<uint32_t>();
            uint32_t flags = reader.take<uint32_t>();
            assembly.writesColour = (flags & kWritesColour) != 0;
            assembly.looksUpDepthMap = (flags & kLooksUpDepthMap) != 0;
            assembly.blockSources = reader.takeArray<uint32_t>(reader.take<uint32_t>());
            assembly.data = reader.takeArray<float>(reader.take<uint32_t>());
        }
    }
    if (!reader.finished()) {
        throw std::invalid_argument("the snapshot has bytes after its last frame");
    }
    return frames;
}

uint64_t RecordingSnapshot::snapshotsCompleted() const {
    std::lock_guard lock(m_mutex);
    return m_completed;
}

} // namespace wiiuport::frame
