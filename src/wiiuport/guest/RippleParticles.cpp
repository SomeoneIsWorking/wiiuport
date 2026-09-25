#include "wiiuport/guest/RippleParticles.h"

#include "wiiuport/guest/GuestWords.h"

#include <cmath>
#include <limits>

namespace wiiuport::guest {

namespace {

// The quad's corner `corner`, coordinate `axis` (0 x, 2 z): a big-endian
// float, as the title's code wrote it.
float coordinate(std::span<const std::byte> bytes, size_t corner, size_t axis) {
    return guestFloat(bytes.data(),
                      (corner * RippleParticles::kCornerStride) + (axis * sizeof(float)));
}

// Half a float's spacing at `value`: the most rounding one sum can add.
double halfUlp(float value) {
    float magnitude = std::fabs(value);
    return 0.5 * static_cast<double>(
                     std::nextafter(magnitude, std::numeric_limits<float>::infinity()) - magnitude);
}

// Whether two opposite corners, the centre less and plus one offset, each
// rounded once, are centred on `centre`.
bool pairCentredOn(float less, float plus, float centre) {
    double apart = std::fabs(static_cast<double>(less) + static_cast<double>(plus) -
                             (2.0 * static_cast<double>(centre)));
    return apart <= halfUlp(less) + halfUlp(plus);
}

} // namespace

void RippleParticles::record(const void* source, const Drawn& drawn) {
    m_calls.fetch_add(1);
    std::lock_guard lock(m_mutex);
    m_bySource.insert_or_assign(source, drawn);
}

std::optional<interp::GuestObject>
RippleParticles::objectDrawn(const void* source, std::span<const std::byte> bytes) const {
    Drawn drawn{};
    {
        std::lock_guard lock(m_mutex);
        auto found = m_bySource.find(source);
        if (found == m_bySource.end()) {
            return std::nullopt;
        }
        drawn = found->second;
    }
    if (!centredOn(bytes, drawn.x, drawn.z)) {
        m_offCentre.fetch_add(1);
        return std::nullopt;
    }
    m_identified.fetch_add(1);
    return drawn.object;
}

bool RippleParticles::centredOn(std::span<const std::byte> bytes, float x, float z) {
    if (bytes.size() < kQuadBytes) {
        return false;
    }
    for (size_t corner = 0; corner < kCorners / 2; ++corner) {
        size_t opposite = corner + (kCorners / 2);
        if (!pairCentredOn(coordinate(bytes, corner, kXFloat), coordinate(bytes, opposite, kXFloat),
                           x) ||
            !pairCentredOn(coordinate(bytes, corner, kZFloat), coordinate(bytes, opposite, kZFloat),
                           z)) {
            return false;
        }
    }
    return true;
}

} // namespace wiiuport::guest
