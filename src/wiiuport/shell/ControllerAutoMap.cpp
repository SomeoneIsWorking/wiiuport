#include "wiiuport/shell/ControllerAutoMap.h"

#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "input/api/SDL/SDLControllerProvider.h"
#include "input/emulated/VPADController.h"

#include <algorithm>
#include <array>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

#include <lucent/log.h>

namespace wiiuport::shell {
namespace {

// The player this host maps a pad to. One GamePad is what the title asks for.
constexpr size_t kPlayerIndex = 0;

// By position: the Wii U's A is on the right of the diamond, so it is the
// button on the right of the pad in the player's hands, whatever letter is
// printed on it. The stick directions carry SDL's sign convention, where Y
// grows downwards.
const std::array<ButtonBinding, 25> kLayout{{
    {VPADController::kButtonId_A, SDL_GAMEPAD_BUTTON_EAST},
    {VPADController::kButtonId_B, SDL_GAMEPAD_BUTTON_SOUTH},
    {VPADController::kButtonId_X, SDL_GAMEPAD_BUTTON_NORTH},
    {VPADController::kButtonId_Y, SDL_GAMEPAD_BUTTON_WEST},
    {VPADController::kButtonId_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {VPADController::kButtonId_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {VPADController::kButtonId_ZL, kTriggerXP},
    {VPADController::kButtonId_ZR, kTriggerYP},
    {VPADController::kButtonId_Plus, SDL_GAMEPAD_BUTTON_START},
    {VPADController::kButtonId_Minus, SDL_GAMEPAD_BUTTON_BACK},
    {VPADController::kButtonId_Home, SDL_GAMEPAD_BUTTON_GUIDE},
    {VPADController::kButtonId_Up, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {VPADController::kButtonId_Down, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {VPADController::kButtonId_Left, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {VPADController::kButtonId_Right, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {VPADController::kButtonId_StickL, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {VPADController::kButtonId_StickR, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {VPADController::kButtonId_StickL_Up, kAxisYN},
    {VPADController::kButtonId_StickL_Down, kAxisYP},
    {VPADController::kButtonId_StickL_Left, kAxisXN},
    {VPADController::kButtonId_StickL_Right, kAxisXP},
    {VPADController::kButtonId_StickR_Up, kRotationYN},
    {VPADController::kButtonId_StickR_Down, kRotationYP},
    {VPADController::kButtonId_StickR_Left, kRotationXN},
    {VPADController::kButtonId_StickR_Right, kRotationXP},
}};

} // namespace

std::span<const ButtonBinding> ControllerAutoMap::layout() {
    return kLayout;
}

void ControllerAutoMap::start() {
    reconcile();
}

void ControllerAutoMap::handleEvent(const SDL_Event& event) {
    if (event.type != SDL_EVENT_GAMEPAD_ADDED && event.type != SDL_EVENT_GAMEPAD_REMOVED) {
        return;
    }
    InputManager::instance().on_device_changed();
    reconcile();
}

void ControllerAutoMap::reconcile() {
    auto provider = InputManager::instance().get_api_provider(InputAPI::SDLController);
    if (!provider) {
        lucent::error("input", "no SDL controller provider exists, so no pad can be attached");
        return;
    }
    std::vector<std::shared_ptr<ControllerBase>> controllers = provider->get_controllers();

    auto emulated = InputManager::instance().get_vpad_controller(kPlayerIndex);
    if (emulated) {
        for (const auto& attached : emulated->get_controllers()) {
            bool stillPresent =
                std::any_of(controllers.begin(), controllers.end(),
                            [&attached](const std::shared_ptr<ControllerBase>& candidate) {
                                return candidate->uuid() == attached->uuid();
                            });
            if (stillPresent) {
                // What the player is holding is still what is mapped.
                return;
            }
            ++m_devicesLost;
            lucent::info("input", "{} was unplugged", attached->display_name());
        }
    }
    m_attachedDevice.clear();
    if (controllers.empty()) {
        lucent::info("input", "no gamepad is plugged in; {} attached, {} lost so far",
                     m_devicesAttached, m_devicesLost);
        return;
    }
    if (!attach(controllers.front())) {
        lucent::error("input", "{} could not be attached to player {}",
                      controllers.front()->display_name(), kPlayerIndex + 1);
    }
}

bool ControllerAutoMap::attach(const std::shared_ptr<ControllerBase>& controller) {
    if (!controller->connect()) {
        return false;
    }
    auto emulated = InputManager::instance().set_controller(
        kPlayerIndex, EmulatedController::Type::VPAD, controller);
    if (!emulated) {
        return false;
    }
    for (const ButtonBinding& binding : layout()) {
        emulated->set_mapping(binding.emulated, controller, binding.physical);
    }
    m_attachedDevice = controller->display_name();
    ++m_devicesAttached;
    lucent::info("input", "{} is now player {}'s GamePad, {} bindings applied", m_attachedDevice,
                 kPlayerIndex + 1, layout().size());
    return true;
}

} // namespace wiiuport::shell
