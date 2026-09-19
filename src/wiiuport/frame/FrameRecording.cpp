#include "wiiuport/frame/FrameRecording.h"

#include <algorithm>
#include <cstring>

namespace wiiuport::frame {

FrameRecording::FrameRecording(size_t byteBudget) : m_byteBudget(byteBudget) {
}

bool FrameRecording::reserve(size_t bytes) {
    if (m_byteCount + bytes > m_byteBudget) {
        m_refusedOverBudget++;
        return false;
    }
    m_byteCount += bytes;
    return true;
}

bool FrameRecording::addDisplayList(uint32_t physicalAddress, const void* data,
                                    size_t sizeInBytes) {
    if (!reserve(sizeInBytes)) {
        return false;
    }
    RecordedDisplayList list;
    list.physicalAddress = physicalAddress;
    list.data.resize(sizeInBytes);
    if (sizeInBytes > 0) {
        std::memcpy(list.data.data(), data, sizeInBytes);
    }
    m_displayLists.push_back(std::move(list));
    return true;
}

bool FrameRecording::addUniformAssembly(const RecordedUniformAssembly& assembly) {
    size_t bytes = assembly.data.size() * sizeof(float);
    bytes += assembly.blockSources.size() * sizeof(uint32_t);
    if (!reserve(bytes)) {
        return false;
    }
    m_uniformAssemblies.push_back(assembly);
    return true;
}

void FrameRecording::clear() {
    m_displayLists.clear();
    m_uniformAssemblies.clear();
    m_byteCount = 0;
    m_refusedOverBudget = 0;
}

} // namespace wiiuport::frame
