#include "wiiuport/input/InputDriver.h"

#include <algorithm>
#include <array>

namespace wiiuport::input {
namespace {

// Only the player the tools drive. A second gamepad is a real feature but not
// this one, and silently driving every player would be a surprise.
constexpr std::size_t kDrivenPlayerIndex = 0;

constexpr auto kButtons = std::to_array<NamedButton>({
    {"a", InputHooks::kButtonA},
    {"b", InputHooks::kButtonB},
    {"x", InputHooks::kButtonX},
    {"y", InputHooks::kButtonY},
    {"l", InputHooks::kButtonL},
    {"r", InputHooks::kButtonR},
    {"zl", InputHooks::kButtonZL},
    {"zr", InputHooks::kButtonZR},
    {"plus", InputHooks::kButtonPlus},
    {"minus", InputHooks::kButtonMinus},
    {"up", InputHooks::kButtonUp},
    {"down", InputHooks::kButtonDown},
    {"left", InputHooks::kButtonLeft},
    {"right", InputHooks::kButtonRight},
});

} // namespace

const NamedButton* InputDriver::buttonNamed(const std::string& name) {
    auto found = std::find_if(kButtons.begin(), kButtons.end(), [&name](const NamedButton& button) {
        return name == button.name;
    });
    return found == kButtons.end() ? nullptr : &*found;
}

bool InputDriver::Poll(size_t playerIndex, InputHooks::Injection& injection) {
    if (playerIndex != kDrivenPlayerIndex) {
        return false;
    }
    std::lock_guard<std::mutex> guard(m_mutex);
    m_pollsSeen += 1;
    if (!m_driving) {
        m_lastMask = 0;
        return false;
    }
    uint32_t mask = 0;
    for (auto& press : m_pressed) {
        mask |= press.mask;
        press.readsRemaining -= 1;
    }
    std::erase_if(m_pressed, [](const HeldPress& press) {
        return press.readsRemaining == 0;
    });
    injection.holdMask = mask;
    injection.leftStickX = m_leftStick.x;
    injection.leftStickY = m_leftStick.y;
    injection.rightStickX = m_rightStick.x;
    injection.rightStickY = m_rightStick.y;
    m_lastMask = mask;
    m_pollsAnswered += 1;
    return true;
}

void InputDriver::press(uint32_t mask, uint32_t reads) {
    if (mask == 0 || reads == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(m_mutex);
    m_pressed.push_back(HeldPress{mask, reads});
    m_pressesQueued += 1;
    m_driving = true;
}

void InputDriver::setLeftStick(StickPosition position) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_leftStick = position;
    m_driving = true;
}

void InputDriver::setRightStick(StickPosition position) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_rightStick = position;
    m_driving = true;
}

void InputDriver::release() {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_pressed.clear();
    m_leftStick = {};
    m_rightStick = {};
    m_driving = false;
    m_lastMask = 0;
}

uint64_t InputDriver::pollsSeen() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_pollsSeen;
}

uint64_t InputDriver::pollsAnswered() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_pollsAnswered;
}

uint64_t InputDriver::pressesQueued() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_pressesQueued;
}

uint32_t InputDriver::currentMask() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_lastMask;
}

} // namespace wiiuport::input
