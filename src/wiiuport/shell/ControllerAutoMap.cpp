#include "wiiuport/shell/ControllerAutoMap.h"

#include "wiiuport/shell/PadRoster.h"

#include "input/InputManager.h"
#include "input/api/Controller.h"
#include "input/api/SDL/SDLControllerProvider.h"
#include "input/emulated/ProController.h"

#include <algorithm>
#include <array>
#include <utility>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>

#include <lucent/log.h>

namespace wiiuport::shell {
namespace {

// The player this host maps a pad to: WPAD channel 0, the only one the title keeps a Pro
// Controller on.
constexpr size_t kPlayerIndex = 0;

// By position: the Pro Controller's A is on the right of the diamond, so it is the
// button on the right of the pad in the player's hands, whatever letter is
// printed on it. The stick directions carry SDL's sign convention, where Y
// grows downwards.
const std::array<ButtonBinding, 25> kLayout{{
    {ProController::kButtonId_A, SDL_GAMEPAD_BUTTON_EAST},
    {ProController::kButtonId_B, SDL_GAMEPAD_BUTTON_SOUTH},
    {ProController::kButtonId_X, SDL_GAMEPAD_BUTTON_NORTH},
    {ProController::kButtonId_Y, SDL_GAMEPAD_BUTTON_WEST},
    {ProController::kButtonId_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {ProController::kButtonId_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {ProController::kButtonId_ZL, kTriggerXP},
    {ProController::kButtonId_ZR, kTriggerYP},
    {ProController::kButtonId_Plus, SDL_GAMEPAD_BUTTON_START},
    {ProController::kButtonId_Minus, SDL_GAMEPAD_BUTTON_BACK},
    {ProController::kButtonId_Home, SDL_GAMEPAD_BUTTON_GUIDE},
    {ProController::kButtonId_Up, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {ProController::kButtonId_Down, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {ProController::kButtonId_Left, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {ProController::kButtonId_Right, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {ProController::kButtonId_StickL, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {ProController::kButtonId_StickR, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {ProController::kButtonId_StickL_Up, kAxisYN},
    {ProController::kButtonId_StickL_Down, kAxisYP},
    {ProController::kButtonId_StickL_Left, kAxisXN},
    {ProController::kButtonId_StickL_Right, kAxisXP},
    {ProController::kButtonId_StickR_Up, kRotationYN},
    {ProController::kButtonId_StickR_Down, kRotationYP},
    {ProController::kButtonId_StickR_Left, kRotationXN},
    {ProController::kButtonId_StickR_Right, kRotationXP},
}};

} // namespace

std::span<const ButtonBinding> ControllerAutoMap::layout() {
    return kLayout;
}

void ControllerAutoMap::start() {
    // The Pro Controller is there with no pad plugged in, so the title finds it and a tool
    // can drive it.
    if (!InputManager::instance().set_controller(kPlayerIndex, EmulatedController::Type::Pro)) {
        lucent::error("input", "player {}'s Pro Controller could not be created", kPlayerIndex + 1);
    }
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

    auto emulated = InputManager::instance().get_wpad_controller(kPlayerIndex);
    if (emulated) {
        std::vector<std::string> present;
        present.reserve(controllers.size());
        for (const auto& controller : controllers) {
            present.push_back(controller->uuid());
        }
        const std::vector<std::shared_ptr<ControllerBase>> mappedPads = emulated->get_controllers();
        std::vector<std::string> mapped;
        mapped.reserve(mappedPads.size());
        for (const auto& pad : mappedPads) {
            mapped.push_back(pad->uuid());
        }
        const PadRoster::Change change = PadRoster(std::move(mapped)).against(present);
        for (const auto& pad : mappedPads) {
            if (std::find(change.lost.begin(), change.lost.end(), pad->uuid()) !=
                change.lost.end()) {
                emulated->remove_controller(pad);
                ++m_devicesLost;
                lucent::info("input", "{} was unplugged", pad->display_name());
            }
        }
        if (change.keep) {
            return;
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
        kPlayerIndex, EmulatedController::Type::Pro, controller);
    if (!emulated) {
        return false;
    }
    for (const ButtonBinding& binding : layout()) {
        emulated->set_mapping(binding.emulated, controller, binding.physical);
    }
    m_attachedDevice = controller->display_name();
    ++m_devicesAttached;
    lucent::info("input", "{} is now player {}'s Pro Controller, {} bindings applied",
                 m_attachedDevice, kPlayerIndex + 1, layout().size());
    return true;
}

} // namespace wiiuport::shell
