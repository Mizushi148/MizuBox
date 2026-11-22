// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room DirectXTK12 GamePad compatibility wrapper powered by GameInput.dll (IGameInput)
// with seamless XInput fallback.

#pragma once
#include <Windows.h>
#include <Xinput.h>
#include <GameInput.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

namespace DirectX {

class GamePad {
public:
    static constexpr int c_MergedInput = -1;
    static constexpr int MAX_PLAYER_COUNT = 8;

    struct Buttons {
        bool a = false;
        bool b = false;
        bool x = false;
        bool y = false;
        bool leftStick = false;
        bool rightStick = false;
        bool leftShoulder = false;
        bool rightShoulder = false;
        bool back = false;
        bool view = false;
        bool start = false;
        bool menu = false;
    };

    struct DPad {
        bool up = false;
        bool down = false;
        bool right = false;
        bool left = false;
    };

    struct ThumbSticks {
        float leftX = 0.0f;
        float leftY = 0.0f;
        float rightX = 0.0f;
        float rightY = 0.0f;
    };

    struct Triggers {
        float left = 0.0f;
        float right = 0.0f;
    };

    struct State {
        bool connected = false;
        uint64_t packet = 0;
        Buttons buttons{};
        DPad dpad{};
        ThumbSticks thumbSticks{};
        Triggers triggers{};

        bool IsConnected() const noexcept { return connected; }
        bool IsViewPressed() const noexcept { return buttons.view; }
        bool IsMenuPressed() const noexcept { return buttons.menu; }
        bool IsAPressed() const noexcept { return buttons.a; }
        bool IsBPressed() const noexcept { return buttons.b; }
        bool IsXPressed() const noexcept { return buttons.x; }
        bool IsYPressed() const noexcept { return buttons.y; }
        bool IsLeftShoulderPressed() const noexcept { return buttons.leftShoulder; }
        bool IsRightShoulderPressed() const noexcept { return buttons.rightShoulder; }
        bool IsDPadUpPressed() const noexcept { return dpad.up; }
        bool IsDPadDownPressed() const noexcept { return dpad.down; }
        bool IsDPadLeftPressed() const noexcept { return dpad.left; }
        bool IsDPadRightPressed() const noexcept { return dpad.right; }
    };

    struct ButtonStateTracker {
        enum ButtonState {
            UP = 0,
            HELD = 1,
            RELEASED = 2,
            PRESSED = 3
        };

        ButtonState a = UP;
        ButtonState b = UP;
        ButtonState x = UP;
        ButtonState y = UP;
        ButtonState leftStick = UP;
        ButtonState rightStick = UP;
        ButtonState leftShoulder = UP;
        ButtonState rightShoulder = UP;
        ButtonState back = UP;
        ButtonState view = UP;
        ButtonState start = UP;
        ButtonState menu = UP;
        ButtonState dpadUp = UP;
        ButtonState dpadDown = UP;
        ButtonState dpadLeft = UP;
        ButtonState dpadRight = UP;

        State lastState{};

        static ButtonState Track(bool current, bool last) noexcept {
            if (current) return last ? HELD : PRESSED;
            return last ? RELEASED : UP;
        }

        void Update(const State& state) noexcept {
            a = Track(state.buttons.a, lastState.buttons.a);
            b = Track(state.buttons.b, lastState.buttons.b);
            x = Track(state.buttons.x, lastState.buttons.x);
            y = Track(state.buttons.y, lastState.buttons.y);
            leftStick = Track(state.buttons.leftStick, lastState.buttons.leftStick);
            rightStick = Track(state.buttons.rightStick, lastState.buttons.rightStick);
            leftShoulder = Track(state.buttons.leftShoulder, lastState.buttons.leftShoulder);
            rightShoulder = Track(state.buttons.rightShoulder, lastState.buttons.rightShoulder);
            back = Track(state.buttons.back, lastState.buttons.back);
            view = Track(state.buttons.view, lastState.buttons.view);
            start = Track(state.buttons.start, lastState.buttons.start);
            menu = Track(state.buttons.menu, lastState.buttons.menu);
            dpadUp = Track(state.dpad.up, lastState.dpad.up);
            dpadDown = Track(state.dpad.down, lastState.dpad.down);
            dpadLeft = Track(state.dpad.left, lastState.dpad.left);
            dpadRight = Track(state.dpad.right, lastState.dpad.right);
            lastState = state;
        }

        void Reset() noexcept {
            *this = ButtonStateTracker{};
        }

        State GetLastState() const noexcept { return lastState; }
    };

    GamePad() noexcept {
        HMODULE mod = nullptr;
        wchar_t binDir[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"WSX_BIN_DIR", binDir, MAX_PATH) > 0 && binDir[0] != L'\0') {
            wchar_t fullPath[MAX_PATH]{};
            swprintf_s(fullPath, L"%s\\GameInput.dll", binDir);
            mod = LoadLibraryW(fullPath);
        }
        if (!mod) {
            mod = LoadLibraryW(L"GameInput.dll");
        }
        if (mod) {
            using PFN_GameInputCreate = HRESULT(STDAPICALLTYPE*)(IGameInput**);
            auto fn = reinterpret_cast<PFN_GameInputCreate>(GetProcAddress(mod, "GameInputCreate"));
            if (fn) {
                fn(m_gameInput.GetAddressOf());
            }
        }
    }
    ~GamePad() = default;

    static float ApplyLinearDeadZone(int16_t value, int16_t deadZone) noexcept {
        const float v = static_cast<float>(value);
        const float dz = static_cast<float>(deadZone);
        if (std::fabs(v) <= dz) return 0.0f;
        const float sign = (v < 0.0f) ? -1.0f : 1.0f;
        return std::clamp(sign * (std::fabs(v) - dz) / (32767.0f - dz), -1.0f, 1.0f);
    }

    State GetState(int player) noexcept {
        State st{};
        // 1. Prefer modern GDK GameInput (IGameInput) when available
        if (m_gameInput) {
            Microsoft::WRL::ComPtr<IGameInputReading> reading;
            if (SUCCEEDED(m_gameInput->GetCurrentReading(GameInputKindGamepad, nullptr, reading.GetAddressOf())) &&
                reading) {
                GameInputGamepadState gp{};
                if (reading->GetGamepadState(&gp)) {
                    st.connected = true;
                    st.packet = reading->GetSequenceNumber(GameInputKindGamepad);
                    const uint32_t b = static_cast<uint32_t>(gp.buttons);
                    st.buttons.a = (b & GameInputGamepadA) != 0;
                    st.buttons.b = (b & GameInputGamepadB) != 0;
                    st.buttons.x = (b & GameInputGamepadX) != 0;
                    st.buttons.y = (b & GameInputGamepadY) != 0;
                    st.buttons.leftStick = (b & GameInputGamepadLeftThumbstick) != 0;
                    st.buttons.rightStick = (b & GameInputGamepadRightThumbstick) != 0;
                    st.buttons.leftShoulder = (b & GameInputGamepadLeftShoulder) != 0;
                    st.buttons.rightShoulder = (b & GameInputGamepadRightShoulder) != 0;
                    st.buttons.view = (b & GameInputGamepadView) != 0;
                    st.buttons.back = st.buttons.view;
                    st.buttons.menu = (b & GameInputGamepadMenu) != 0;
                    st.buttons.start = st.buttons.menu;
                    st.dpad.up = (b & GameInputGamepadDPadUp) != 0;
                    st.dpad.down = (b & GameInputGamepadDPadDown) != 0;
                    st.dpad.left = (b & GameInputGamepadDPadLeft) != 0;
                    st.dpad.right = (b & GameInputGamepadDPadRight) != 0;
                    st.triggers.left = gp.leftTrigger;
                    st.triggers.right = gp.rightTrigger;
                    st.thumbSticks.leftX = gp.leftThumbstickX;
                    st.thumbSticks.leftY = gp.leftThumbstickY;
                    st.thumbSticks.rightX = gp.rightThumbstickX;
                    st.thumbSticks.rightY = gp.rightThumbstickY;
                    return st;
                }
            }
        }

        // 2. Fallback to direct XInput
        const DWORD startIdx = (player == c_MergedInput) ? 0u : static_cast<DWORD>(player);
        const DWORD endIdx = (player == c_MergedInput) ? XUSER_MAX_COUNT : (startIdx + 1u);
        for (DWORD i = startIdx; i < endIdx && i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE xi{};
            if (XInputGetState(i, &xi) == ERROR_SUCCESS) {
                st.connected = true;
                st.packet = xi.dwPacketNumber;
                const WORD w = xi.Gamepad.wButtons;
                st.buttons.a |= (w & XINPUT_GAMEPAD_A) != 0;
                st.buttons.b |= (w & XINPUT_GAMEPAD_B) != 0;
                st.buttons.x |= (w & XINPUT_GAMEPAD_X) != 0;
                st.buttons.y |= (w & XINPUT_GAMEPAD_Y) != 0;
                st.buttons.leftStick |= (w & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
                st.buttons.rightStick |= (w & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
                st.buttons.leftShoulder |= (w & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
                st.buttons.rightShoulder |= (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
                st.buttons.view |= (w & XINPUT_GAMEPAD_BACK) != 0;
                st.buttons.back |= (w & XINPUT_GAMEPAD_BACK) != 0;
                st.buttons.menu |= (w & XINPUT_GAMEPAD_START) != 0;
                st.buttons.start |= (w & XINPUT_GAMEPAD_START) != 0;
                st.dpad.up |= (w & XINPUT_GAMEPAD_DPAD_UP) != 0;
                st.dpad.down |= (w & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
                st.dpad.left |= (w & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
                st.dpad.right |= (w & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;

                st.triggers.left = std::max(st.triggers.left, xi.Gamepad.bLeftTrigger / 255.0f);
                st.triggers.right = std::max(st.triggers.right, xi.Gamepad.bRightTrigger / 255.0f);

                const float lx = ApplyLinearDeadZone(xi.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                const float ly = ApplyLinearDeadZone(xi.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                const float rx = ApplyLinearDeadZone(xi.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
                const float ry = ApplyLinearDeadZone(xi.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
                if (std::fabs(lx) > std::fabs(st.thumbSticks.leftX)) st.thumbSticks.leftX = lx;
                if (std::fabs(ly) > std::fabs(st.thumbSticks.leftY)) st.thumbSticks.leftY = ly;
                if (std::fabs(rx) > std::fabs(st.thumbSticks.rightX)) st.thumbSticks.rightX = rx;
                if (std::fabs(ry) > std::fabs(st.thumbSticks.rightY)) st.thumbSticks.rightY = ry;
            }
        }
        return st;
    }

    bool SetVibration(int player, float leftMotor, float rightMotor,
                      float leftTrigger = 0.0f, float rightTrigger = 0.0f) noexcept {
        if (m_gameInput) {
            Microsoft::WRL::ComPtr<IGameInputReading> reading;
            if (SUCCEEDED(m_gameInput->GetCurrentReading(GameInputKindGamepad, nullptr, reading.GetAddressOf())) &&
                reading) {
                Microsoft::WRL::ComPtr<IGameInputDevice> dev;
                reading->GetDevice(dev.GetAddressOf());
                if (dev) {
                    GameInputRumbleParams params{leftMotor, rightMotor, leftTrigger, rightTrigger};
                    dev->SetRumbleState(&params);
                    return true;
                }
            }
        }
        const DWORD idx = (player == c_MergedInput) ? 0u : static_cast<DWORD>(player);
        if (idx >= XUSER_MAX_COUNT) return false;
        XINPUT_VIBRATION vib{};
        vib.wLeftMotorSpeed = static_cast<WORD>(std::clamp(leftMotor, 0.0f, 1.0f) * 65535.0f);
        vib.wRightMotorSpeed = static_cast<WORD>(std::clamp(rightMotor, 0.0f, 1.0f) * 65535.0f);
        return XInputSetState(idx, &vib) == ERROR_SUCCESS;
    }

    void Suspend() noexcept {}
    void Resume() noexcept {}

private:
    Microsoft::WRL::ComPtr<IGameInput> m_gameInput;
};

} // namespace DirectX
