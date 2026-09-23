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
    if (m_displayListCount == m_displayLists.size()) {
        m_displayLists.emplace_back();
    }
    RecordedDisplayList& list = m_displayLists[m_displayListCount++];
    list.physicalAddress = physicalAddress;
    list.data.resize(sizeInBytes);
    if (sizeInBytes > 0) {
        std::memcpy(list.data.data(), data, sizeInBytes);
    }
    return true;
}

bool FrameRecording::addUniformAssembly(const RecordedUniformAssembly& assembly) {
    size_t bytes = assembly.data.size() * sizeof(float);
    bytes += assembly.blockSources.size() * sizeof(uint32_t);
    if (!reserve(bytes)) {
        return false;
    }
    if (m_uniformAssemblyCount == m_uniformAssemblies.size()) {
        m_uniformAssemblies.emplace_back();
    }
    // Copy-assigned, so the slot's vectors keep their storage.
    m_uniformAssemblies[m_uniformAssemblyCount++] = assembly;
    return true;
}

void FrameRecording::clear() {
    m_displayListCount = 0;
    m_uniformAssemblyCount = 0;
    m_byteCount = 0;
    m_refusedOverBudget = 0;
}

} // namespace wiiuport::frame
