// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Windows x64 implementation of Microsoft GDK GameInput (GameInput.dll):
// - 100% binary-compatible with official <GameInput.h> COM interfaces:
//   IGameInput, IGameInputReading, IGameInputDevice, IGameInputDispatcher
// - Backed by XInput (4 hardware controller slots) + automatic Virtual Xbox Series X|S Controller
//   fallback over keyboard, Win32 Keyboard polling, Win32 Mouse polling, and UI Navigation.
// - Full GameInputDeviceInfo, Rumble (SetRumbleState -> XInputSetState), BatteryState,
//   Blocking/Async Device Enumeration, Reading History Ring Buffer (Next/Previous/Temporal),
//   wsx.ini configuration ([Input]), and WSX_TRACE=1 API tracing.

#include "wsx_config.h"

#include <Windows.h>
#include <Xinput.h>
#include <GameInput.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr const char* kMod = "GameInput";

uint64_t QueryTimestampMicroseconds() noexcept {
    static const int64_t freq = []() noexcept {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return (f.QuadPart > 0) ? f.QuadPart : 1LL;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>((now.QuadPart * 1000000LL) / freq);
}

float ApplyLinearDeadZone(int16_t raw, int16_t deadZone) noexcept {
    const float v = static_cast<float>(raw);
    const float dz = static_cast<float>(std::clamp<int>(deadZone, 0, 32000));
    if (std::fabs(v) <= dz) return 0.0f;
    const float sign = (v < 0.0f) ? -1.0f : 1.0f;
    return std::clamp(sign * (std::fabs(v) - dz) / (32767.0f - dz), -1.0f, 1.0f);
}

void ApplyRadialDeadZone(int16_t rawX, int16_t rawY, int16_t deadZone, float& outX, float& outY) noexcept {
    const float fx = static_cast<float>(rawX) / 32767.0f;
    const float fy = static_cast<float>(rawY) / 32767.0f;
    const float mag = std::sqrt(fx * fx + fy * fy);
    const float dz = std::clamp(static_cast<float>(deadZone) / 32767.0f, 0.0f, 0.95f);
    if (mag <= dz) {
        outX = 0.0f;
        outY = 0.0f;
        return;
    }
    const float norm_mag = std::clamp((mag - dz) / (1.0f - dz), 0.0f, 1.0f);
    outX = (fx / mag) * norm_mag;
    outY = (fy / mag) * norm_mag;
}

float ApplyTriggerDeadZone(uint8_t raw, uint8_t threshold = XINPUT_GAMEPAD_TRIGGER_THRESHOLD) noexcept {
    const uint8_t th = std::min<uint8_t>(threshold, 250u);
    if (raw <= th) return 0.0f;
    return std::clamp(
        static_cast<float>(raw - th) /
        static_cast<float>(255u - th),
        0.0f, 1.0f);
}

APP_LOCAL_DEVICE_ID MakeDeterministicDeviceId(const char* tag) noexcept {
    APP_LOCAL_DEVICE_ID id{};
    const size_t len = std::min<size_t>(std::strlen(tag), sizeof(id.value));
    std::memcpy(id.value, tag, len);
    return id;
}

bool DeviceIdsEqual(const APP_LOCAL_DEVICE_ID& a, const APP_LOCAL_DEVICE_ID& b) noexcept {
    return std::memcmp(a.value, b.value, sizeof(a.value)) == 0;
}

enum class WsxDeviceRole {
    GamepadSlot0 = 0,
    GamepadSlot1 = 1,
    GamepadSlot2 = 2,
    GamepadSlot3 = 3,
    Keyboard     = 4,
    Mouse        = 5,
    Aggregate    = 6
};

// ============================================================================
// IGameInputDevice Implementation
// ============================================================================
class WsxGameInputDevice final : public IGameInputDevice {
    std::atomic<ULONG> ref_{1};
    WsxDeviceRole role_;
    int xinput_slot_;
    GameInputKind aggregate_kind_;

    std::string display_name_str_;
    GameInputString display_name_{};
    GameInputGamepadInfo gamepad_info_{};
    GameInputKeyboardInfo keyboard_info_{};
    GameInputMouseInfo mouse_info_{};
    GameInputUiNavigationInfo ui_nav_info_{};
    GameInputDeviceInfo info_{};

    std::mutex rumble_mtx_;
    GameInputRumbleParams last_rumble_{};
    std::atomic<float> ffb_motor_gain_{1.0f};
    std::atomic<bool> ffb_motor_powered_{true};

public:
    WsxGameInputDevice(WsxDeviceRole role, int xinput_slot = -1,
                       GameInputKind agg_kind = GameInputKindUnknown) noexcept
        : role_(role), xinput_slot_(xinput_slot), aggregate_kind_(agg_kind) {
        std::memset(&info_, 0, sizeof(info_));
        info_.infoSize = sizeof(GameInputDeviceInfo);
        info_.hardwareVersion = {1, 0, 0, 0};
        info_.firmwareVersion = {5, 17, 3202, 0};

        // Populate standard UI navigation labels
        ui_nav_info_.menuButtonLabel = GameInputLabelXboxMenu;
        ui_nav_info_.viewButtonLabel = GameInputLabelXboxView;
        ui_nav_info_.acceptButtonLabel = GameInputLabelXboxA;
        ui_nav_info_.cancelButtonLabel = GameInputLabelXboxB;
        ui_nav_info_.upButtonLabel = GameInputLabelXboxDPadUp;
        ui_nav_info_.downButtonLabel = GameInputLabelXboxDPadDown;
        ui_nav_info_.leftButtonLabel = GameInputLabelXboxDPadLeft;
        ui_nav_info_.rightButtonLabel = GameInputLabelXboxDPadRight;
        ui_nav_info_.contextButton1Label = GameInputLabelXboxX;
        ui_nav_info_.contextButton2Label = GameInputLabelXboxY;
        ui_nav_info_.pageLeftButtonLabel = GameInputLabelXboxLeftShoulder;
        ui_nav_info_.pageRightButtonLabel = GameInputLabelXboxRightShoulder;

        if (role_ == WsxDeviceRole::Keyboard) {
            display_name_str_ = "Windows Keyboard (WSX GameInput)";
            info_.vendorId = 0x045Eu;
            info_.productId = 0x0750u;
            info_.revisionNumber = 1;
            info_.usage = {0x01, 0x06}; // Generic Desktop / Keyboard
            info_.deviceId = MakeDeterministicDeviceId("WSX-KEYBOARD-0");
            info_.deviceRootId = info_.deviceId;
            info_.deviceFamily = GameInputFamilyHid;
            info_.supportedInput = static_cast<GameInputKind>(
                GameInputKindKeyboard | GameInputKindUiNavigation);
            keyboard_info_.kind = GameInputAnsiKeyboard;
            keyboard_info_.layout = 0x00000409u;
            keyboard_info_.keyCount = 104;
            keyboard_info_.functionKeyCount = 12;
            keyboard_info_.maxSimultaneousKeys = 16;
            info_.keyboardInfo = &keyboard_info_;
            info_.uiNavigationInfo = &ui_nav_info_;
        } else if (role_ == WsxDeviceRole::Mouse) {
            display_name_str_ = "Windows Mouse (WSX GameInput)";
            info_.vendorId = 0x045Eu;
            info_.productId = 0x0040u;
            info_.revisionNumber = 1;
            info_.usage = {0x01, 0x02}; // Generic Desktop / Mouse
            info_.deviceId = MakeDeterministicDeviceId("WSX-MOUSE-0");
            info_.deviceRootId = info_.deviceId;
            info_.deviceFamily = GameInputFamilyHid;
            info_.supportedInput = GameInputKindMouse;
            mouse_info_.supportedButtons = static_cast<GameInputMouseButtons>(
                GameInputMouseLeftButton | GameInputMouseRightButton |
                GameInputMouseMiddleButton | GameInputMouseButton4 | GameInputMouseButton5);
            mouse_info_.sampleRate = 1000;
            mouse_info_.sensorDpi = 1600;
            mouse_info_.hasWheelX = true;
            mouse_info_.hasWheelY = true;
            info_.mouseInfo = &mouse_info_;
        } else if (role_ == WsxDeviceRole::Aggregate) {
            display_name_str_ = "Aggregate Input Device (WSX GameInput)";
            info_.vendorId = 0x045Eu;
            info_.productId = 0x0B12u;
            info_.deviceId = MakeDeterministicDeviceId("WSX-AGGREGATE-0");
            info_.deviceRootId = info_.deviceId;
            info_.deviceFamily = GameInputFamilyAggregate;
            info_.supportedInput = aggregate_kind_;
            info_.supportedRumbleMotors = static_cast<GameInputRumbleMotors>(
                GameInputRumbleLowFrequency | GameInputRumbleHighFrequency |
                GameInputRumbleLeftTrigger | GameInputRumbleRightTrigger);
            info_.uiNavigationInfo = &ui_nav_info_;
        } else {
            char tag[32]{};
            std::snprintf(tag, sizeof(tag), "WSX-GAMEPAD-%d", xinput_slot_);
            char name[96]{};
            std::snprintf(name, sizeof(name), "Xbox Series X|S Wireless Controller #%d (WSX)", xinput_slot_ + 1);
            display_name_str_ = name;

            info_.vendorId = 0x045Eu;  // Microsoft
            info_.productId = 0x0B12u; // Xbox Series X|S Wireless Controller
            info_.revisionNumber = 0x0509u;
            info_.usage = {0x01, 0x05}; // Generic Desktop / Game Pad
            info_.deviceId = MakeDeterministicDeviceId(tag);
            info_.deviceRootId = info_.deviceId;
            info_.deviceFamily = GameInputFamilyXboxOne;
            info_.capabilities = GameInputDeviceCapabilityWireless;
            info_.supportedInput = static_cast<GameInputKind>(
                GameInputKindGamepad | GameInputKindUiNavigation | GameInputKindController);
            info_.supportedRumbleMotors = static_cast<GameInputRumbleMotors>(
                GameInputRumbleLowFrequency | GameInputRumbleHighFrequency |
                GameInputRumbleLeftTrigger | GameInputRumbleRightTrigger);
            info_.supportedSystemButtons = static_cast<GameInputSystemButtons>(
                GameInputSystemButtonGuide | GameInputSystemButtonShare);
            info_.controllerAxisCount = 6;
            info_.controllerButtonCount = 10;
            info_.controllerSwitchCount = 1;

            gamepad_info_.menuButtonLabel = GameInputLabelXboxMenu;
            gamepad_info_.viewButtonLabel = GameInputLabelXboxView;
            gamepad_info_.aButtonLabel = GameInputLabelXboxA;
            gamepad_info_.bButtonLabel = GameInputLabelXboxB;
            gamepad_info_.xButtonLabel = GameInputLabelXboxX;
            gamepad_info_.yButtonLabel = GameInputLabelXboxY;
            gamepad_info_.dpadUpLabel = GameInputLabelXboxDPadUp;
            gamepad_info_.dpadDownLabel = GameInputLabelXboxDPadDown;
            gamepad_info_.dpadLeftLabel = GameInputLabelXboxDPadLeft;
            gamepad_info_.dpadRightLabel = GameInputLabelXboxDPadRight;
            gamepad_info_.leftShoulderButtonLabel = GameInputLabelXboxLeftShoulder;
            gamepad_info_.rightShoulderButtonLabel = GameInputLabelXboxRightShoulder;
            gamepad_info_.leftThumbstickButtonLabel = GameInputLabelXboxLeftStickButton;
            gamepad_info_.rightThumbstickButtonLabel = GameInputLabelXboxRightStickButton;

            info_.gamepadInfo = &gamepad_info_;
            info_.uiNavigationInfo = &ui_nav_info_;
        }

        display_name_.sizeInBytes = static_cast<uint32_t>(display_name_str_.size());
        display_name_.codePointCount = static_cast<uint32_t>(display_name_str_.size());
        display_name_.data = display_name_str_.c_str();
        info_.displayName = &display_name_;
    }

    WsxDeviceRole role() const noexcept { return role_; }
    int xinput_slot() const noexcept { return xinput_slot_; }
    GameInputKind aggregate_kind() const noexcept { return aggregate_kind_; }

    bool is_physically_connected() const noexcept {
        if (role_ == WsxDeviceRole::Keyboard || role_ == WsxDeviceRole::Mouse ||
            role_ == WsxDeviceRole::Aggregate) {
            return true;
        }
        if (xinput_slot_ >= 0 && xinput_slot_ < XUSER_MAX_COUNT) {
            XINPUT_STATE xi{};
            if (XInputGetState(static_cast<DWORD>(xinput_slot_), &xi) == ERROR_SUCCESS) {
                return true;
            }
            if (xinput_slot_ == 0 && wsx::get_config_bool("WSX_VIRTUAL_GAMEPAD", "Input", "virtual_gamepad", true)) {
                return true;
            }
        }
        return false;
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IGameInputDevice)) {
            *ppvObj = static_cast<IGameInputDevice*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    // IGameInputDevice
    GameInputDeviceInfo const* STDMETHODCALLTYPE GetDeviceInfo() override {
        return &info_;
    }

    GameInputDeviceStatus STDMETHODCALLTYPE GetDeviceStatus() override {
        if (!is_physically_connected()) {
            return GameInputDeviceNoStatus;
        }
        return static_cast<GameInputDeviceStatus>(
            GameInputDeviceConnected | GameInputDeviceInputEnabled | GameInputDeviceOutputEnabled);
    }

    void STDMETHODCALLTYPE GetBatteryState(GameInputBatteryState* state) override {
        if (!state) return;
        std::memset(state, 0, sizeof(*state));
        state->remainingCapacity = 1.0f;
        state->fullChargeCapacity = 1.0f;
        state->status = (role_ == WsxDeviceRole::Keyboard || role_ == WsxDeviceRole::Mouse)
            ? GameInputBatteryNotPresent
            : GameInputBatteryIdle;
    }

    HRESULT STDMETHODCALLTYPE CreateForceFeedbackEffect(
        uint32_t motorIndex, GameInputForceFeedbackParams const* params, IGameInputForceFeedbackEffect** effect) override;

    bool STDMETHODCALLTYPE IsForceFeedbackMotorPoweredOn(uint32_t /*motorIndex*/) override {
        return (role_ != WsxDeviceRole::Keyboard && role_ != WsxDeviceRole::Mouse) &&
               ffb_motor_powered_.load(std::memory_order_relaxed);
    }
    void STDMETHODCALLTYPE SetForceFeedbackMotorGain(uint32_t /*motorIndex*/, float masterGain) override {
        ffb_motor_gain_.store(std::clamp(masterGain, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    float ffb_motor_gain() const noexcept {
        return ffb_motor_gain_.load(std::memory_order_relaxed);
    }
    HRESULT STDMETHODCALLTYPE SetHapticMotorState(uint32_t, GameInputHapticFeedbackParams const*) override {
        return S_OK;
    }

    void STDMETHODCALLTYPE SetRumbleState(GameInputRumbleParams const* params) override {
        GameInputRumbleParams effective{};
        if (params) {
            effective = *params;
        }
        {
            std::lock_guard<std::mutex> lock(rumble_mtx_);
            last_rumble_ = effective;
        }

        const float left = std::clamp(effective.lowFrequency + 0.35f * effective.leftTrigger, 0.0f, 1.0f);
        const float right = std::clamp(effective.highFrequency + 0.35f * effective.rightTrigger, 0.0f, 1.0f);
        XINPUT_VIBRATION vib{};
        vib.wLeftMotorSpeed = static_cast<WORD>(left * 65535.0f);
        vib.wRightMotorSpeed = static_cast<WORD>(right * 65535.0f);

        if (xinput_slot_ >= 0 && xinput_slot_ < XUSER_MAX_COUNT) {
            XInputSetState(static_cast<DWORD>(xinput_slot_), &vib);
        } else if (role_ == WsxDeviceRole::Aggregate) {
            for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
                XInputSetState(i, &vib);
            }
        }
        wsx::trace_void(kMod, "IGameInputDevice::SetRumbleState",
                        "slot=%d low=%.2f high=%.2f lt=%.2f rt=%.2f",
                        xinput_slot_, effective.lowFrequency, effective.highFrequency,
                        effective.leftTrigger, effective.rightTrigger);
    }

    void STDMETHODCALLTYPE SetInputSynchronizationState(bool) override {}
    void STDMETHODCALLTYPE SendInputSynchronizationHint() override {}
    void STDMETHODCALLTYPE PowerOff() override {}

    HRESULT STDMETHODCALLTYPE CreateRawDeviceReport(
        uint32_t, GameInputRawDeviceReportKind, IGameInputRawDeviceReport** report) override {
        if (report) *report = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetRawDeviceFeature(uint32_t, IGameInputRawDeviceReport** report) override {
        if (report) *report = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetRawDeviceFeature(IGameInputRawDeviceReport*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SendRawDeviceOutput(IGameInputRawDeviceReport*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SendRawDeviceOutputWithResponse(
        IGameInputRawDeviceReport*, IGameInputRawDeviceReport** responseReport) override {
        if (responseReport) *responseReport = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ExecuteRawDeviceIoControl(
        uint32_t, size_t, void const*, size_t, void*, size_t* outputSize) override {
        if (outputSize) *outputSize = 0;
        return E_NOTIMPL;
    }
    bool STDMETHODCALLTYPE AcquireExclusiveRawDeviceAccess(uint64_t) override { return false; }
    void STDMETHODCALLTYPE ReleaseExclusiveRawDeviceAccess() override {}
};

// ============================================================================
// IGameInputForceFeedbackEffect Implementation
// ============================================================================
class WsxGameInputForceFeedbackEffect final : public IGameInputForceFeedbackEffect {
    std::atomic<ULONG> ref_{1};
    ComPtr<WsxGameInputDevice> device_;
    uint32_t motor_index_ = 0;
    std::mutex mtx_;
    float gain_ = 1.0f;
    GameInputForceFeedbackParams params_{};
    GameInputFeedbackEffectState state_ = GameInputFeedbackStopped;

    void apply_motor_output_locked() noexcept {
        if (!device_) return;
        GameInputRumbleParams rp{};
        if (state_ == GameInputFeedbackRunning) {
            const float mag = std::clamp(gain_ * device_->ffb_motor_gain(), 0.0f, 1.0f);
            rp.lowFrequency = mag;
            rp.highFrequency = mag * 0.75f;
            rp.leftTrigger = mag * 0.5f;
            rp.rightTrigger = mag * 0.5f;
        }
        device_->SetRumbleState(&rp);
    }

public:
    WsxGameInputForceFeedbackEffect(
        WsxGameInputDevice* dev,
        uint32_t motorIndex,
        const GameInputForceFeedbackParams& params) noexcept
        : device_(dev), motor_index_(motorIndex), params_(params) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IGameInputForceFeedbackEffect)) {
            *ppvObj = static_cast<IGameInputForceFeedbackEffect*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    void STDMETHODCALLTYPE GetDevice(IGameInputDevice** device) override {
        if (!device) return;
        *device = device_.Get();
        if (*device) (*device)->AddRef();
    }
    uint32_t STDMETHODCALLTYPE GetMotorIndex() override { return motor_index_; }
    float STDMETHODCALLTYPE GetGain() override {
        std::lock_guard<std::mutex> lk(mtx_);
        return gain_;
    }
    void STDMETHODCALLTYPE SetGain(float gain) override {
        std::lock_guard<std::mutex> lk(mtx_);
        gain_ = std::clamp(gain, 0.0f, 1.0f);
        if (state_ == GameInputFeedbackRunning) apply_motor_output_locked();
    }
    void STDMETHODCALLTYPE GetParams(GameInputForceFeedbackParams* params) override {
        if (!params) return;
        std::lock_guard<std::mutex> lk(mtx_);
        *params = params_;
    }
    bool STDMETHODCALLTYPE SetParams(GameInputForceFeedbackParams const* params) override {
        if (!params) return false;
        std::lock_guard<std::mutex> lk(mtx_);
        params_ = *params;
        if (state_ == GameInputFeedbackRunning) apply_motor_output_locked();
        return true;
    }
    GameInputFeedbackEffectState STDMETHODCALLTYPE GetState() override {
        std::lock_guard<std::mutex> lk(mtx_);
        return state_;
    }
    void STDMETHODCALLTYPE SetState(GameInputFeedbackEffectState state) override {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = state;
        apply_motor_output_locked();
    }
};

HRESULT STDMETHODCALLTYPE WsxGameInputDevice::CreateForceFeedbackEffect(
    uint32_t motorIndex, GameInputForceFeedbackParams const* params, IGameInputForceFeedbackEffect** effect) {
    if (!effect) return E_POINTER;
    *effect = nullptr;
    if (!params) return E_INVALIDARG;
    if (role_ == WsxDeviceRole::Keyboard || role_ == WsxDeviceRole::Mouse) {
        return E_NOTIMPL;
    }
    auto* fx = new (std::nothrow) WsxGameInputForceFeedbackEffect(this, motorIndex, *params);
    if (!fx) return E_OUTOFMEMORY;
    *effect = fx;
    return S_OK;
}

// ============================================================================
// IGameInputReading Implementation
// ============================================================================
class WsxGameInputReading final : public IGameInputReading {
    std::atomic<ULONG> ref_{1};
    GameInputKind kind_;
    uint64_t seq_;
    uint64_t timestamp_;
    ComPtr<WsxGameInputDevice> device_;

    bool has_gamepad_ = false;
    GameInputGamepadState gamepad_{};
    bool has_mouse_ = false;
    GameInputMouseState mouse_{};
    bool has_ui_nav_ = false;
    GameInputUiNavigationState ui_nav_{};
    std::vector<GameInputKeyState> keys_;

public:
    WsxGameInputReading(GameInputKind kind, uint64_t seq, uint64_t ts, WsxGameInputDevice* dev) noexcept
        : kind_(kind), seq_(seq), timestamp_(ts), device_(dev) {}

    void set_gamepad(const GameInputGamepadState& st) noexcept {
        gamepad_ = st;
        has_gamepad_ = true;
    }
    void set_mouse(const GameInputMouseState& st) noexcept {
        mouse_ = st;
        has_mouse_ = true;
    }
    void set_ui_nav(const GameInputUiNavigationState& st) noexcept {
        ui_nav_ = st;
        has_ui_nav_ = true;
    }
    void set_keys(std::vector<GameInputKeyState> keys) noexcept {
        keys_ = std::move(keys);
    }

    uint64_t raw_sequence() const noexcept { return seq_; }
    WsxGameInputDevice* raw_device() const noexcept { return device_.Get(); }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IGameInputReading)) {
            *ppvObj = static_cast<IGameInputReading*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    // IGameInputReading
    GameInputKind STDMETHODCALLTYPE GetInputKind() override { return kind_; }

    uint64_t STDMETHODCALLTYPE GetSequenceNumber(GameInputKind) override { return seq_; }

    uint64_t STDMETHODCALLTYPE GetTimestamp() override { return timestamp_; }

    void STDMETHODCALLTYPE GetDevice(IGameInputDevice** device) override {
        if (!device) return;
        *device = device_.Get();
        if (*device) (*device)->AddRef();
    }

    bool STDMETHODCALLTYPE GetRawReport(IGameInputRawDeviceReport** report) override {
        if (report) *report = nullptr;
        return false;
    }

    uint32_t STDMETHODCALLTYPE GetControllerAxisCount() override {
        return has_gamepad_ ? 6u : 0u;
    }

    uint32_t STDMETHODCALLTYPE GetControllerAxisState(uint32_t stateArrayCount, float* stateArray) override {
        if (!has_gamepad_ || !stateArray || stateArrayCount == 0) return 0;
        const float axes[6] = {
            (gamepad_.leftThumbstickX + 1.0f) * 0.5f,
            (gamepad_.leftThumbstickY + 1.0f) * 0.5f,
            (gamepad_.rightThumbstickX + 1.0f) * 0.5f,
            (gamepad_.rightThumbstickY + 1.0f) * 0.5f,
            gamepad_.leftTrigger,
            gamepad_.rightTrigger
        };
        const uint32_t n = std::min<uint32_t>(stateArrayCount, 6u);
        std::memcpy(stateArray, axes, n * sizeof(float));
        return n;
    }

    uint32_t STDMETHODCALLTYPE GetControllerButtonCount() override {
        return has_gamepad_ ? 10u : 0u;
    }

    uint32_t STDMETHODCALLTYPE GetControllerButtonState(uint32_t stateArrayCount, bool* stateArray) override {
        if (!has_gamepad_ || !stateArray || stateArrayCount == 0) return 0;
        const uint32_t b = static_cast<uint32_t>(gamepad_.buttons);
        const bool btns[10] = {
            (b & GameInputGamepadA) != 0,
            (b & GameInputGamepadB) != 0,
            (b & GameInputGamepadX) != 0,
            (b & GameInputGamepadY) != 0,
            (b & GameInputGamepadLeftShoulder) != 0,
            (b & GameInputGamepadRightShoulder) != 0,
            (b & GameInputGamepadView) != 0,
            (b & GameInputGamepadMenu) != 0,
            (b & GameInputGamepadLeftThumbstick) != 0,
            (b & GameInputGamepadRightThumbstick) != 0
        };
        const uint32_t n = std::min<uint32_t>(stateArrayCount, 10u);
        std::memcpy(stateArray, btns, n * sizeof(bool));
        return n;
    }

    uint32_t STDMETHODCALLTYPE GetControllerSwitchCount() override {
        return has_gamepad_ ? 1u : 0u;
    }

    uint32_t STDMETHODCALLTYPE GetControllerSwitchState(
        uint32_t stateArrayCount, GameInputSwitchPosition* stateArray) override {
        if (!has_gamepad_ || !stateArray || stateArrayCount == 0) return 0;
        const uint32_t b = static_cast<uint32_t>(gamepad_.buttons);
        const bool up = (b & GameInputGamepadDPadUp) != 0;
        const bool dn = (b & GameInputGamepadDPadDown) != 0;
        const bool lt = (b & GameInputGamepadDPadLeft) != 0;
        const bool rt = (b & GameInputGamepadDPadRight) != 0;
        GameInputSwitchPosition pos = GameInputSwitchCenter;
        if (up && rt) pos = GameInputSwitchUpRight;
        else if (up && lt) pos = GameInputSwitchUpLeft;
        else if (dn && rt) pos = GameInputSwitchDownRight;
        else if (dn && lt) pos = GameInputSwitchDownLeft;
        else if (up) pos = GameInputSwitchUp;
        else if (dn) pos = GameInputSwitchDown;
        else if (lt) pos = GameInputSwitchLeft;
        else if (rt) pos = GameInputSwitchRight;
        stateArray[0] = pos;
        return 1u;
    }

    uint32_t STDMETHODCALLTYPE GetKeyCount() override {
        return static_cast<uint32_t>(keys_.size());
    }

    uint32_t STDMETHODCALLTYPE GetKeyState(uint32_t stateArrayCount, GameInputKeyState* stateArray) override {
        if (!stateArray || stateArrayCount == 0 || keys_.empty()) return 0;
        const uint32_t n = std::min<uint32_t>(stateArrayCount, static_cast<uint32_t>(keys_.size()));
        std::memcpy(stateArray, keys_.data(), n * sizeof(GameInputKeyState));
        return n;
    }

    bool STDMETHODCALLTYPE GetMouseState(GameInputMouseState* state) override {
        if (!has_mouse_ || !state) return false;
        *state = mouse_;
        return true;
    }

    uint32_t STDMETHODCALLTYPE GetTouchCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetTouchState(uint32_t, GameInputTouchState*) override { return 0; }
    bool STDMETHODCALLTYPE GetMotionState(GameInputMotionState*) override { return false; }
    bool STDMETHODCALLTYPE GetArcadeStickState(GameInputArcadeStickState*) override { return false; }
    bool STDMETHODCALLTYPE GetFlightStickState(GameInputFlightStickState*) override { return false; }

    bool STDMETHODCALLTYPE GetGamepadState(GameInputGamepadState* state) override {
        if (!has_gamepad_ || !state) return false;
        *state = gamepad_;
        return true;
    }

    bool STDMETHODCALLTYPE GetRacingWheelState(GameInputRacingWheelState*) override { return false; }

    bool STDMETHODCALLTYPE GetUiNavigationState(GameInputUiNavigationState* state) override {
        if (!has_ui_nav_ || !state) return false;
        *state = ui_nav_;
        return true;
    }
};

// ============================================================================
// IGameInputDispatcher Implementation
// ============================================================================
struct PendingDispatchItem {
    GameInputCallbackToken token = 0;
    GameInputDeviceCallback device_cb = nullptr;
    void* context = nullptr;
    ComPtr<IGameInputDevice> device;
    uint64_t timestamp = 0;
    GameInputDeviceStatus cur_status = GameInputDeviceNoStatus;
    GameInputDeviceStatus prev_status = GameInputDeviceNoStatus;
};

class WsxGameInputDispatcher final : public IGameInputDispatcher {
    std::atomic<ULONG> ref_{1};
    std::mutex mtx_;
    std::deque<PendingDispatchItem> queue_;
    HANDLE wait_event_ = nullptr;

public:
    WsxGameInputDispatcher() noexcept {
        wait_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~WsxGameInputDispatcher() {
        if (wait_event_) CloseHandle(wait_event_);
    }

    void enqueue(PendingDispatchItem item) {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push_back(std::move(item));
        if (wait_event_) SetEvent(wait_event_);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IGameInputDispatcher)) {
            *ppvObj = static_cast<IGameInputDispatcher*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    bool STDMETHODCALLTYPE Dispatch(uint64_t quotaInMicroseconds) override {
        const uint64_t deadline = (quotaInMicroseconds > 0)
            ? (QueryTimestampMicroseconds() + quotaInMicroseconds)
            : 0ULL;
        for (;;) {
            PendingDispatchItem item{};
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (queue_.empty()) {
                    if (wait_event_) ResetEvent(wait_event_);
                    return false;
                }
                item = std::move(queue_.front());
                queue_.pop_front();
                if (queue_.empty() && wait_event_) {
                    ResetEvent(wait_event_);
                }
            }
            if (item.device_cb) {
                item.device_cb(item.token, item.context, item.device.Get(),
                               item.timestamp, item.cur_status, item.prev_status);
            }
            if (deadline > 0 && QueryTimestampMicroseconds() >= deadline) {
                std::lock_guard<std::mutex> lock(mtx_);
                return !queue_.empty();
            }
        }
    }

    HRESULT STDMETHODCALLTYPE OpenWaitHandle(HANDLE* waitHandle) override {
        if (!waitHandle) return E_INVALIDARG;
        if (!wait_event_) return E_FAIL;
        if (!DuplicateHandle(GetCurrentProcess(), wait_event_, GetCurrentProcess(),
                             waitHandle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return S_OK;
    }
};

// ============================================================================
// IGameInput Singleton Implementation
// ============================================================================
struct CallbackEntry {
    GameInputCallbackToken token = 0;
    bool active = true;
    bool is_device_cb = false;
    IGameInputDevice* filter_device = nullptr;
    GameInputKind input_kind = GameInputKindUnknown;
    GameInputDeviceStatus status_filter = GameInputDeviceNoStatus;
    void* context = nullptr;
    GameInputDeviceCallback device_cb = nullptr;
};

class WsxGameInput final : public IGameInput {
    std::atomic<ULONG> ref_{1};
    std::atomic<uint64_t> seq_{1};
    std::atomic<uint64_t> next_token_{1};
    GameInputFocusPolicy focus_policy_ = GameInputDefaultFocusPolicy;

    ComPtr<WsxGameInputDevice> gamepads_[XUSER_MAX_COUNT];
    ComPtr<WsxGameInputDevice> keyboard_;
    ComPtr<WsxGameInputDevice> mouse_;
    std::array<GameInputDeviceStatus, XUSER_MAX_COUNT> last_gamepad_status_{};

    std::mutex history_mtx_;
    std::deque<ComPtr<WsxGameInputReading>> history_;
    static constexpr size_t kMaxHistory = 64;

    std::mutex cb_mtx_;
    std::vector<CallbackEntry> callbacks_;
    std::vector<ComPtr<WsxGameInputDispatcher>> dispatchers_;

    void poll_hotplug_transitions() noexcept {
        std::vector<PendingDispatchItem> pending;
        {
            std::lock_guard<std::mutex> lock(cb_mtx_);
            const uint64_t ts = QueryTimestampMicroseconds();
            for (int i = 0; i < XUSER_MAX_COUNT; ++i) {
                WsxGameInputDevice* dev = gamepads_[i].Get();
                if (!dev) continue;
                const GameInputDeviceStatus cur = dev->GetDeviceStatus();
                const GameInputDeviceStatus prev = last_gamepad_status_[i];
                if (cur != prev) {
                    last_gamepad_status_[i] = cur;
                    const uint32_t diff = static_cast<uint32_t>(cur) ^ static_cast<uint32_t>(prev);
                    for (const auto& cb : callbacks_) {
                        if (!cb.active || !cb.is_device_cb || !cb.device_cb) continue;
                        if (cb.filter_device && cb.filter_device != dev) continue;
                        if ((dev->GetDeviceInfo()->supportedInput & cb.input_kind) == 0) continue;
                        if ((static_cast<uint32_t>(cb.status_filter) & diff) == 0) continue;
                        PendingDispatchItem item{};
                        item.token = cb.token;
                        item.device_cb = cb.device_cb;
                        item.context = cb.context;
                        item.device = dev;
                        item.timestamp = ts;
                        item.cur_status = cur;
                        item.prev_status = prev;
                        if (!dispatchers_.empty()) {
                            dispatchers_.back()->enqueue(std::move(item));
                        } else {
                            pending.push_back(std::move(item));
                        }
                    }
                }
            }
        }
        for (const auto& item : pending) {
            if (item.device_cb) {
                item.device_cb(item.token, item.context, item.device.Get(),
                               item.timestamp, item.cur_status, item.prev_status);
            }
        }
    }

    bool poll_gamepad_state(int slot, GameInputGamepadState& out, bool& used_physical) const noexcept {
        std::memset(&out, 0, sizeof(out));
        used_physical = false;
        const DWORD start = (slot < 0) ? 0u : static_cast<DWORD>(slot);
        const DWORD end = (slot < 0) ? XUSER_MAX_COUNT : (start + 1u);

        uint32_t btns = 0;
        for (DWORD i = start; i < end && i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE xi{};
            if (XInputGetState(i, &xi) == ERROR_SUCCESS) {
                used_physical = true;
                const WORD w = xi.Gamepad.wButtons;
                if (w & XINPUT_GAMEPAD_START)          btns |= GameInputGamepadMenu;
                if (w & XINPUT_GAMEPAD_BACK)           btns |= GameInputGamepadView;
                if (w & XINPUT_GAMEPAD_A)              btns |= GameInputGamepadA;
                if (w & XINPUT_GAMEPAD_B)              btns |= GameInputGamepadB;
                if (w & XINPUT_GAMEPAD_X)              btns |= GameInputGamepadX;
                if (w & XINPUT_GAMEPAD_Y)              btns |= GameInputGamepadY;
                if (w & XINPUT_GAMEPAD_DPAD_UP)        btns |= GameInputGamepadDPadUp;
                if (w & XINPUT_GAMEPAD_DPAD_DOWN)      btns |= GameInputGamepadDPadDown;
                if (w & XINPUT_GAMEPAD_DPAD_LEFT)      btns |= GameInputGamepadDPadLeft;
                if (w & XINPUT_GAMEPAD_DPAD_RIGHT)     btns |= GameInputGamepadDPadRight;
                if (w & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns |= GameInputGamepadLeftShoulder;
                if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns |= GameInputGamepadRightShoulder;
                if (w & XINPUT_GAMEPAD_LEFT_THUMB)     btns |= GameInputGamepadLeftThumbstick;
                if (w & XINPUT_GAMEPAD_RIGHT_THUMB)    btns |= GameInputGamepadRightThumbstick;

                const uint8_t trig_dz = static_cast<uint8_t>(std::clamp<int64_t>(
                    wsx::get_config_int("WSX_TRIGGER_DEADZONE", "Input", "trigger_deadzone", XINPUT_GAMEPAD_TRIGGER_THRESHOLD),
                    0, 250));
                const int16_t left_dz = static_cast<int16_t>(std::clamp<int64_t>(
                    wsx::get_config_int("WSX_DEADZONE_LEFT", "Input", "deadzone_left", XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE),
                    0, 32000));
                const int16_t right_dz = static_cast<int16_t>(std::clamp<int64_t>(
                    wsx::get_config_int("WSX_DEADZONE_RIGHT", "Input", "deadzone_right", XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE),
                    0, 32000));
                const bool invert_ry = wsx::get_config_bool("WSX_INVERT_RIGHT_Y", "Input", "invert_right_y", false);

                out.leftTrigger = std::max(out.leftTrigger, ApplyTriggerDeadZone(xi.Gamepad.bLeftTrigger, trig_dz));
                out.rightTrigger = std::max(out.rightTrigger, ApplyTriggerDeadZone(xi.Gamepad.bRightTrigger, trig_dz));

                float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
                ApplyRadialDeadZone(xi.Gamepad.sThumbLX, xi.Gamepad.sThumbLY, left_dz, lx, ly);
                ApplyRadialDeadZone(xi.Gamepad.sThumbRX, xi.Gamepad.sThumbRY, right_dz, rx, ry);
                if (invert_ry) ry = -ry;
                if (std::fabs(lx) > std::fabs(out.leftThumbstickX))  out.leftThumbstickX = lx;
                if (std::fabs(ly) > std::fabs(out.leftThumbstickY))  out.leftThumbstickY = ly;
                if (std::fabs(rx) > std::fabs(out.rightThumbstickX)) out.rightThumbstickX = rx;
                if (std::fabs(ry) > std::fabs(out.rightThumbstickY)) out.rightThumbstickY = ry;
            }
        }

        const bool virtual_enabled =
            wsx::get_config_bool("WSX_VIRTUAL_GAMEPAD", "Input", "virtual_gamepad", true);
        if (!used_physical && (slot == 0 || slot < 0) && virtual_enabled) {
            const bool invert_ry = wsx::get_config_bool("WSX_INVERT_RIGHT_Y", "Input", "invert_right_y", false);
            auto keyDown = [](int vk) noexcept { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
            if (keyDown('A')) out.leftThumbstickX -= 1.0f;
            if (keyDown('D')) out.leftThumbstickX += 1.0f;
            if (keyDown('W')) out.leftThumbstickY += 1.0f;
            if (keyDown('S')) out.leftThumbstickY -= 1.0f;
            if (keyDown('J')) out.rightThumbstickX -= 1.0f;
            if (keyDown('L')) out.rightThumbstickX += 1.0f;
            if (keyDown('I')) out.rightThumbstickY += invert_ry ? -1.0f : 1.0f;
            if (keyDown('K')) out.rightThumbstickY += invert_ry ? 1.0f : -1.0f;
            if (keyDown(VK_UP))     btns |= GameInputGamepadDPadUp;
            if (keyDown(VK_DOWN))   btns |= GameInputGamepadDPadDown;
            if (keyDown(VK_LEFT))   btns |= GameInputGamepadDPadLeft;
            if (keyDown(VK_RIGHT))  btns |= GameInputGamepadDPadRight;
            if (keyDown(VK_SPACE) || keyDown(VK_RETURN)) btns |= GameInputGamepadA;
            if (keyDown(VK_BACK))   btns |= GameInputGamepadB;
            if (keyDown(VK_F5))     btns |= GameInputGamepadX;
            if (keyDown(VK_F9))     btns |= GameInputGamepadY;
            if (keyDown('Q'))       btns |= GameInputGamepadLeftShoulder;
            if (keyDown('E'))       btns |= GameInputGamepadRightShoulder;
            if (keyDown(VK_TAB))    btns |= GameInputGamepadView;
            if (keyDown(VK_ESCAPE)) btns |= GameInputGamepadMenu;
            if (keyDown(VK_LSHIFT)) out.leftTrigger = 1.0f;
            if (keyDown(VK_LCONTROL)) out.rightTrigger = 1.0f;
            out.buttons = static_cast<GameInputGamepadButtons>(btns);
            return true;
        }

        out.buttons = static_cast<GameInputGamepadButtons>(btns);
        return used_physical;
    }

    std::vector<GameInputKeyState> poll_keyboard_keys() const {
        std::vector<GameInputKeyState> keys;
        keys.reserve(8);
        for (int vk = 0x08; vk <= 0xFE && keys.size() < 16; ++vk) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
                vk == VK_XBUTTON1 || vk == VK_XBUTTON2) {
                continue;
            }
            if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
                GameInputKeyState ks{};
                ks.virtualKey = static_cast<uint8_t>(vk);
                ks.scanCode = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
                const UINT ch = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR);
                ks.codePoint = ch & 0xFFFFu;
                ks.isDeadKey = (ch & 0x80000000u) != 0;
                keys.push_back(ks);
            }
        }
        return keys;
    }

    GameInputMouseState poll_mouse_state() const noexcept {
        GameInputMouseState ms{};
        POINT pt{};
        if (GetCursorPos(&pt)) {
            ms.positionX = pt.x;
            ms.positionY = pt.y;
        }
        uint32_t btns = 0;
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)  btns |= GameInputMouseLeftButton;
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)  btns |= GameInputMouseRightButton;
        if (GetAsyncKeyState(VK_MBUTTON) & 0x8000)  btns |= GameInputMouseMiddleButton;
        if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) btns |= GameInputMouseButton4;
        if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) btns |= GameInputMouseButton5;
        ms.buttons = static_cast<GameInputMouseButtons>(btns);
        return ms;
    }

    GameInputUiNavigationState build_ui_nav(const GameInputGamepadState* gp) const noexcept {
        uint32_t nav = 0;
        if (gp) {
            const uint32_t b = static_cast<uint32_t>(gp->buttons);
            if (b & GameInputGamepadMenu) nav |= GameInputUiNavigationMenu;
            if (b & GameInputGamepadView) nav |= GameInputUiNavigationView;
            if (b & GameInputGamepadA) nav |= GameInputUiNavigationAccept;
            if (b & GameInputGamepadB) nav |= GameInputUiNavigationCancel;
            if (b & GameInputGamepadX) nav |= GameInputUiNavigationContext1;
            if (b & GameInputGamepadY) nav |= GameInputUiNavigationContext2;
            if ((b & GameInputGamepadDPadUp) || gp->leftThumbstickY > 0.5f) nav |= GameInputUiNavigationUp;
            if ((b & GameInputGamepadDPadDown) || gp->leftThumbstickY < -0.5f) nav |= GameInputUiNavigationDown;
            if ((b & GameInputGamepadDPadLeft) || gp->leftThumbstickX < -0.5f) nav |= GameInputUiNavigationLeft;
            if ((b & GameInputGamepadDPadRight) || gp->leftThumbstickX > 0.5f) nav |= GameInputUiNavigationRight;
            if (b & GameInputGamepadLeftShoulder) nav |= GameInputUiNavigationPageLeft;
            if (b & GameInputGamepadRightShoulder) nav |= GameInputUiNavigationPageRight;
        }
        GameInputUiNavigationState st{};
        st.buttons = static_cast<GameInputUiNavigationButtons>(nav);
        return st;
    }

    void record_history(WsxGameInputReading* r) {
        std::lock_guard<std::mutex> lock(history_mtx_);
        history_.emplace_back(r);
        while (history_.size() > kMaxHistory) {
            history_.pop_front();
        }
    }

public:
    WsxGameInput() {
        for (int i = 0; i < XUSER_MAX_COUNT; ++i) {
            gamepads_[i].Attach(new WsxGameInputDevice(static_cast<WsxDeviceRole>(i), i));
            last_gamepad_status_[i] = gamepads_[i]->GetDeviceStatus();
        }
        keyboard_.Attach(new WsxGameInputDevice(WsxDeviceRole::Keyboard));
        mouse_.Attach(new WsxGameInputDevice(WsxDeviceRole::Mouse));
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IGameInput)) {
            *ppvObj = static_cast<IGameInput*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    // IGameInput
    uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() override {
        return QueryTimestampMicroseconds();
    }

    HRESULT STDMETHODCALLTYPE GetCurrentReading(
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) override {
        if (!reading) return E_POINTER;
        *reading = nullptr;
        poll_hotplug_transitions();

        auto* wsxDev = static_cast<WsxGameInputDevice*>(device);
        const uint64_t ts = QueryTimestampMicroseconds();
        const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed);

        // 1. Gamepad / Controller / UI Navigation
        if ((inputKind & (GameInputKindGamepad | GameInputKindController | GameInputKindUiNavigation)) != 0 &&
            (!wsxDev || wsxDev->role() != WsxDeviceRole::Mouse)) {
            if (!wsxDev || wsxDev->role() != WsxDeviceRole::Keyboard) {
                int slot = -1;
                WsxGameInputDevice* outDev = gamepads_[0].Get();
                if (wsxDev) {
                    if (wsxDev->role() == WsxDeviceRole::Aggregate) {
                        slot = -1;
                        outDev = wsxDev;
                    } else {
                        slot = wsxDev->xinput_slot();
                        outDev = wsxDev;
                    }
                }
                GameInputGamepadState gp{};
                bool physical = false;
                if (poll_gamepad_state(slot, gp, physical)) {
                    const GameInputKind resolvedKind = static_cast<GameInputKind>(
                        GameInputKindGamepad | GameInputKindController | GameInputKindUiNavigation);
                    auto* r = new (std::nothrow) WsxGameInputReading(resolvedKind, seq, ts, outDev);
                    if (!r) return E_OUTOFMEMORY;
                    r->set_gamepad(gp);
                    r->set_ui_nav(build_ui_nav(&gp));
                    record_history(r);
                    *reading = r;
                    return S_OK;
                }
            }
        }

        // 2. Keyboard
        if ((inputKind & (GameInputKindKeyboard | GameInputKindUiNavigation)) != 0 &&
            (!wsxDev || wsxDev->role() == WsxDeviceRole::Keyboard || wsxDev->role() == WsxDeviceRole::Aggregate)) {
            auto* r = new (std::nothrow) WsxGameInputReading(
                static_cast<GameInputKind>(GameInputKindKeyboard | GameInputKindUiNavigation),
                seq, ts, wsxDev ? wsxDev : keyboard_.Get());
            if (!r) return E_OUTOFMEMORY;
            r->set_keys(poll_keyboard_keys());
            r->set_ui_nav(build_ui_nav(nullptr));
            record_history(r);
            *reading = r;
            return S_OK;
        }

        // 3. Mouse
        if ((inputKind & GameInputKindMouse) != 0 &&
            (!wsxDev || wsxDev->role() == WsxDeviceRole::Mouse || wsxDev->role() == WsxDeviceRole::Aggregate)) {
            auto* r = new (std::nothrow) WsxGameInputReading(
                GameInputKindMouse, seq, ts, wsxDev ? wsxDev : mouse_.Get());
            if (!r) return E_OUTOFMEMORY;
            r->set_mouse(poll_mouse_state());
            record_history(r);
            *reading = r;
            return S_OK;
        }

        return GAMEINPUT_E_READING_NOT_FOUND;
    }

    HRESULT STDMETHODCALLTYPE GetNextReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) override {
        if (!referenceReading || !reading) return E_INVALIDARG;
        *reading = nullptr;
        const uint64_t refSeq = static_cast<WsxGameInputReading*>(referenceReading)->raw_sequence();
        std::lock_guard<std::mutex> lock(history_mtx_);
        for (const auto& item : history_) {
            if (item->raw_sequence() > refSeq &&
                (item->GetInputKind() & inputKind) != 0 &&
                (!device || item->raw_device() == device)) {
                *reading = item.Get();
                (*reading)->AddRef();
                return S_OK;
            }
        }
        return GAMEINPUT_E_READING_NOT_FOUND;
    }

    HRESULT STDMETHODCALLTYPE GetPreviousReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) override {
        if (!referenceReading || !reading) return E_INVALIDARG;
        *reading = nullptr;
        const uint64_t refSeq = static_cast<WsxGameInputReading*>(referenceReading)->raw_sequence();
        std::lock_guard<std::mutex> lock(history_mtx_);
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
            if ((*it)->raw_sequence() < refSeq &&
                ((*it)->GetInputKind() & inputKind) != 0 &&
                (!device || (*it)->raw_device() == device)) {
                *reading = (*it).Get();
                (*reading)->AddRef();
                return S_OK;
            }
        }
        return GAMEINPUT_E_READING_NOT_FOUND;
    }

    HRESULT STDMETHODCALLTYPE GetTemporalReading(
        uint64_t timestamp,
        IGameInputDevice* device,
        IGameInputReading** reading) override {
        if (!reading) return E_POINTER;
        *reading = nullptr;
        std::lock_guard<std::mutex> lock(history_mtx_);
        WsxGameInputReading* best = nullptr;
        uint64_t bestDiff = UINT64_MAX;
        for (const auto& item : history_) {
            if (device && item->raw_device() != device) continue;
            const uint64_t t = item->GetTimestamp();
            const uint64_t diff = (t >= timestamp) ? (t - timestamp) : (timestamp - t);
            if (diff < bestDiff) {
                bestDiff = diff;
                best = item.Get();
            }
        }
        if (!best) return GAMEINPUT_E_READING_NOT_FOUND;
        *reading = best;
        best->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RegisterReadingCallback(
        IGameInputDevice* /*device*/,
        GameInputKind inputKind,
        float /*analogThreshold*/,
        void* /*context*/,
        GameInputReadingCallback callbackFunc,
        GameInputCallbackToken* callbackToken) override {
        if (!callbackFunc) return E_INVALIDARG;
        const GameInputCallbackToken tok = next_token_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(cb_mtx_);
            CallbackEntry entry{};
            entry.token = tok;
            entry.active = true;
            callbacks_.push_back(entry);
        }
        if (callbackToken) *callbackToken = tok;
        return wsx::trace_hr(kMod, "IGameInput::RegisterReadingCallback", S_OK,
                             "kind=0x%X token=%llu", static_cast<unsigned>(inputKind),
                             static_cast<unsigned long long>(tok));
    }

    HRESULT STDMETHODCALLTYPE RegisterDeviceCallback(
        IGameInputDevice* device,
        GameInputKind inputKind,
        GameInputDeviceStatus statusFilter,
        GameInputEnumerationKind enumerationKind,
        void* context,
        GameInputDeviceCallback callbackFunc,
        GameInputCallbackToken* callbackToken) override {
        if (!callbackFunc) return E_INVALIDARG;
        const GameInputCallbackToken tok = next_token_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(cb_mtx_);
            CallbackEntry entry{};
            entry.token = tok;
            entry.active = true;
            entry.is_device_cb = true;
            entry.filter_device = device;
            entry.input_kind = inputKind;
            entry.status_filter = statusFilter;
            entry.context = context;
            entry.device_cb = callbackFunc;
            callbacks_.push_back(entry);
        }
        if (callbackToken) *callbackToken = tok;

        if (enumerationKind != GameInputNoEnumeration && (statusFilter & GameInputDeviceConnected) != 0) {
            std::vector<WsxGameInputDevice*> candidates;
            if (device) {
                candidates.push_back(static_cast<WsxGameInputDevice*>(device));
            } else {
                for (int i = 0; i < XUSER_MAX_COUNT; ++i) candidates.push_back(gamepads_[i].Get());
                candidates.push_back(keyboard_.Get());
                candidates.push_back(mouse_.Get());
            }

            const uint64_t ts = QueryTimestampMicroseconds();
            for (WsxGameInputDevice* dev : candidates) {
                if (!dev || !dev->is_physically_connected()) continue;
                if ((dev->GetDeviceInfo()->supportedInput & inputKind) == 0) continue;
                const GameInputDeviceStatus st = dev->GetDeviceStatus();
                if (enumerationKind == GameInputBlockingEnumeration) {
                    callbackFunc(tok, context, dev, ts, st, GameInputDeviceNoStatus);
                } else {
                    std::lock_guard<std::mutex> lock(cb_mtx_);
                    if (!dispatchers_.empty()) {
                        PendingDispatchItem item{};
                        item.token = tok;
                        item.device_cb = callbackFunc;
                        item.context = context;
                        item.device = dev;
                        item.timestamp = ts;
                        item.cur_status = st;
                        item.prev_status = GameInputDeviceNoStatus;
                        dispatchers_.back()->enqueue(std::move(item));
                    } else {
                        callbackFunc(tok, context, dev, ts, st, GameInputDeviceNoStatus);
                    }
                }
            }
        }
        return wsx::trace_hr(kMod, "IGameInput::RegisterDeviceCallback", S_OK,
                             "kind=0x%X enum=%u token=%llu", static_cast<unsigned>(inputKind),
                             static_cast<unsigned>(enumerationKind),
                             static_cast<unsigned long long>(tok));
    }

    HRESULT STDMETHODCALLTYPE RegisterSystemButtonCallback(
        IGameInputDevice*, GameInputSystemButtons, void*, GameInputSystemButtonCallback callbackFunc,
        GameInputCallbackToken* callbackToken) override {
        if (!callbackFunc) return E_INVALIDARG;
        const GameInputCallbackToken tok = next_token_.fetch_add(1, std::memory_order_relaxed);
        if (callbackToken) *callbackToken = tok;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RegisterKeyboardLayoutCallback(
        IGameInputDevice*, void*, GameInputKeyboardLayoutCallback callbackFunc,
        GameInputCallbackToken* callbackToken) override {
        if (!callbackFunc) return E_INVALIDARG;
        const GameInputCallbackToken tok = next_token_.fetch_add(1, std::memory_order_relaxed);
        if (callbackToken) *callbackToken = tok;
        return S_OK;
    }

    void STDMETHODCALLTYPE StopCallback(GameInputCallbackToken callbackToken) override {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        for (auto& c : callbacks_) {
            if (c.token == callbackToken) c.active = false;
        }
    }

    bool STDMETHODCALLTYPE UnregisterCallback(
        GameInputCallbackToken callbackToken, uint64_t /*timeoutInMicroseconds*/) override {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        auto it = std::remove_if(callbacks_.begin(), callbacks_.end(),
                                 [&](const CallbackEntry& c) { return c.token == callbackToken; });
        if (it == callbacks_.end()) return false;
        callbacks_.erase(it, callbacks_.end());
        return true;
    }

    HRESULT STDMETHODCALLTYPE CreateDispatcher(IGameInputDispatcher** dispatcher) override {
        if (!dispatcher) return E_POINTER;
        auto* d = new (std::nothrow) WsxGameInputDispatcher();
        if (!d) return E_OUTOFMEMORY;
        {
            std::lock_guard<std::mutex> lock(cb_mtx_);
            dispatchers_.emplace_back(d);
        }
        *dispatcher = d;
        return wsx::trace_hr(kMod, "IGameInput::CreateDispatcher", S_OK, "dispatcher=%p", static_cast<void*>(d));
    }

    HRESULT STDMETHODCALLTYPE CreateAggregateDevice(
        GameInputKind inputKind, IGameInputDevice** device) override {
        if (!device) return E_POINTER;
        auto* agg = new (std::nothrow) WsxGameInputDevice(WsxDeviceRole::Aggregate, -1, inputKind);
        if (!agg) return E_OUTOFMEMORY;
        *device = agg;
        return wsx::trace_hr(kMod, "IGameInput::CreateAggregateDevice", S_OK,
                             "kind=0x%X", static_cast<unsigned>(inputKind));
    }

    HRESULT STDMETHODCALLTYPE FindDeviceFromId(
        APP_LOCAL_DEVICE_ID const* value, IGameInputDevice** device) override {
        if (!value || !device) return E_INVALIDARG;
        *device = nullptr;
        WsxGameInputDevice* all[] = {
            gamepads_[0].Get(), gamepads_[1].Get(), gamepads_[2].Get(), gamepads_[3].Get(),
            keyboard_.Get(), mouse_.Get()
        };
        for (WsxGameInputDevice* d : all) {
            if (d && DeviceIdsEqual(d->GetDeviceInfo()->deviceId, *value)) {
                *device = d;
                d->AddRef();
                return S_OK;
            }
        }
        return GAMEINPUT_E_DEVICE_NOT_FOUND;
    }

    HRESULT STDMETHODCALLTYPE FindDeviceFromObject(IUnknown*, IGameInputDevice** device) override {
        if (device) *device = nullptr;
        return GAMEINPUT_E_DEVICE_NOT_FOUND;
    }

    HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformHandle(HANDLE, IGameInputDevice** device) override {
        if (device) *device = nullptr;
        return GAMEINPUT_E_DEVICE_NOT_FOUND;
    }

    HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformString(LPCWSTR value, IGameInputDevice** device) override {
        if (!value || !device) return E_INVALIDARG;
        if (wcsstr(value, L"GAMEPAD") != nullptr || wcsstr(value, L"IG_") != nullptr) {
            *device = gamepads_[0].Get();
            (*device)->AddRef();
            return S_OK;
        }
        *device = nullptr;
        return GAMEINPUT_E_DEVICE_NOT_FOUND;
    }

    HRESULT STDMETHODCALLTYPE EnableOemDeviceSupport(uint16_t, uint16_t, uint8_t, uint8_t) override {
        return S_OK;
    }

    void STDMETHODCALLTYPE SetFocusPolicy(GameInputFocusPolicy policy) override {
        focus_policy_ = policy;
        wsx::trace_void(kMod, "IGameInput::SetFocusPolicy", "policy=0x%X", static_cast<unsigned>(policy));
    }
};

std::mutex g_singleton_mtx;
ComPtr<WsxGameInput> g_singleton;

} // namespace

extern "C" {

STDAPI GameInputCreate(IGameInput** gameInput) {
    if (!gameInput) return E_POINTER;
    std::lock_guard<std::mutex> lock(g_singleton_mtx);
    if (!g_singleton) {
        g_singleton.Attach(new (std::nothrow) WsxGameInput());
        if (!g_singleton) {
            *gameInput = nullptr;
            return wsx::trace_hr(kMod, "GameInputCreate", E_OUTOFMEMORY);
        }
    }
    *gameInput = g_singleton.Get();
    (*gameInput)->AddRef();
    return wsx::trace_hr(kMod, "GameInputCreate", S_OK, "singleton=%p", static_cast<void*>(*gameInput));
}

STDAPI DllCanUnloadNow() {
    return S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID, REFIID, LPVOID* ppv) {
    if (ppv) *ppv = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

} // extern "C"
