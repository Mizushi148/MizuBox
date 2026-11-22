// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Automated unit and integration test for clean-room GameInput.dll (IGameInput):
// Tests GameInputCreate, Blocking & Async Device Enumeration, FindDeviceFromId,
// Gamepad + Keyboard + Mouse + UI Navigation + Raw Controller readings, Reading History
// (GetPreviousReading / GetNextReading / GetTemporalReading), SetRumbleState, BatteryState,
// CreateAggregateDevice, and IGameInputDispatcher (Dispatch + OpenWaitHandle).

#include <Windows.h>
#include <GameInput.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK_HR(expr)                                                                 \
    do {                                                                               \
        HRESULT _hr = (expr);                                                          \
        if (FAILED(_hr)) {                                                             \
            std::fprintf(stderr, "FAIL: %s returned 0x%08X at line %d\n",              \
                         #expr, static_cast<unsigned>(_hr), __LINE__);                 \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

#define CHECK_COND(cond, msg)                                                          \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL: %s (%s) at line %d\n", msg, #cond, __LINE__);  \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

struct EnumStats {
    int gamepads = 0;
    int keyboards = 0;
    int mice = 0;
    APP_LOCAL_DEVICE_ID firstGamepadId{};
};

int main() {
    ComPtr<IGameInput> gameInput;
    CHECK_HR(GameInputCreate(gameInput.GetAddressOf()));
    CHECK_COND(gameInput != nullptr, "GameInputCreate returned null");

    const uint64_t ts0 = gameInput->GetCurrentTimestamp();
    CHECK_COND(ts0 > 0, "GetCurrentTimestamp returned 0");

    // 1. Blocking device enumeration (Gamepad + Keyboard + Mouse)
    EnumStats stats{};
    GameInputCallbackToken devToken = 0;
    CHECK_HR(gameInput->RegisterDeviceCallback(
        nullptr,
        static_cast<GameInputKind>(GameInputKindGamepad | GameInputKindKeyboard | GameInputKindMouse),
        GameInputDeviceConnected,
        GameInputBlockingEnumeration,
        &stats,
        [](GameInputCallbackToken, void* ctx, IGameInputDevice* dev, uint64_t,
           GameInputDeviceStatus cur, GameInputDeviceStatus) noexcept {
            if (!dev || (cur & GameInputDeviceConnected) == 0) return;
            auto* s = static_cast<EnumStats*>(ctx);
            const GameInputDeviceInfo* info = dev->GetDeviceInfo();
            if (!info) return;
            if ((info->supportedInput & GameInputKindGamepad) != 0) {
                if (s->gamepads == 0) s->firstGamepadId = info->deviceId;
                ++s->gamepads;
            }
            if ((info->supportedInput & GameInputKindKeyboard) != 0) ++s->keyboards;
            if ((info->supportedInput & GameInputKindMouse) != 0) ++s->mice;
        },
        &devToken));
    CHECK_COND(devToken != 0, "RegisterDeviceCallback token is 0");
    CHECK_COND(stats.gamepads >= 1, "Expected at least 1 gamepad (physical or virtual)");
    CHECK_COND(stats.keyboards >= 1, "Expected keyboard device");
    CHECK_COND(stats.mice >= 1, "Expected mouse device");

    // 2. FindDeviceFromId + DeviceInfo + BatteryState + SetRumbleState
    ComPtr<IGameInputDevice> padDevice;
    CHECK_HR(gameInput->FindDeviceFromId(&stats.firstGamepadId, padDevice.GetAddressOf()));
    CHECK_COND(padDevice != nullptr, "FindDeviceFromId returned null");

    const GameInputDeviceInfo* padInfo = padDevice->GetDeviceInfo();
    CHECK_COND(padInfo != nullptr && padInfo->vendorId == 0x045Eu && padInfo->productId == 0x0B12u,
               "Unexpected Xbox Series gamepad VID/PID");
    CHECK_COND((padDevice->GetDeviceStatus() & GameInputDeviceConnected) != 0,
               "Gamepad device not marked connected");

    GameInputBatteryState bat{};
    padDevice->GetBatteryState(&bat);
    CHECK_COND(bat.remainingCapacity > 0.0f && bat.fullChargeCapacity > 0.0f,
               "Invalid battery state");

    GameInputRumbleParams rumble{0.25f, 0.15f, 0.10f, 0.10f};
    padDevice->SetRumbleState(&rumble);
    padDevice->SetRumbleState(nullptr); // Stop rumble

    // 3. Gamepad + Controller + UI Navigation readings
    ComPtr<IGameInputReading> r1, r2;
    CHECK_HR(gameInput->GetCurrentReading(GameInputKindGamepad, padDevice.Get(), r1.GetAddressOf()));
    CHECK_HR(gameInput->GetCurrentReading(GameInputKindGamepad, padDevice.Get(), r2.GetAddressOf()));
    CHECK_COND(r2->GetSequenceNumber(GameInputKindGamepad) > r1->GetSequenceNumber(GameInputKindGamepad),
               "Reading sequence numbers must increase monotonically");

    GameInputGamepadState gpState{};
    CHECK_COND(r2->GetGamepadState(&gpState), "GetGamepadState returned false");

    GameInputUiNavigationState uiState{};
    CHECK_COND(r2->GetUiNavigationState(&uiState), "GetUiNavigationState returned false");

    CHECK_COND(r2->GetControllerAxisCount() == 6u, "Expected 6 controller axes");
    float axes[6]{};
    CHECK_COND(r2->GetControllerAxisState(6, axes) == 6u, "GetControllerAxisState count mismatch");
    CHECK_COND(r2->GetControllerButtonCount() == 10u, "Expected 10 controller buttons");
    CHECK_COND(r2->GetControllerSwitchCount() == 1u, "Expected 1 controller switch");

    // 4. Reading History (GetPreviousReading / GetNextReading / GetTemporalReading)
    ComPtr<IGameInputReading> prevR, nextR, tempR;
    CHECK_HR(gameInput->GetPreviousReading(r2.Get(), GameInputKindGamepad, padDevice.Get(), prevR.GetAddressOf()));
    CHECK_COND(prevR->GetSequenceNumber(GameInputKindGamepad) == r1->GetSequenceNumber(GameInputKindGamepad),
               "GetPreviousReading mismatch");
    CHECK_HR(gameInput->GetNextReading(r1.Get(), GameInputKindGamepad, padDevice.Get(), nextR.GetAddressOf()));
    CHECK_COND(nextR->GetSequenceNumber(GameInputKindGamepad) == r2->GetSequenceNumber(GameInputKindGamepad),
               "GetNextReading mismatch");
    CHECK_HR(gameInput->GetTemporalReading(r1->GetTimestamp(), padDevice.Get(), tempR.GetAddressOf()));
    CHECK_COND(tempR != nullptr, "GetTemporalReading returned null");

    // 5. Keyboard & Mouse readings
    ComPtr<IGameInputReading> kbReading, mouseReading;
    CHECK_HR(gameInput->GetCurrentReading(GameInputKindKeyboard, nullptr, kbReading.GetAddressOf()));
    const uint32_t keyCount = kbReading->GetKeyCount();
    if (keyCount > 0) {
        std::vector<GameInputKeyState> keys(keyCount);
        CHECK_COND(kbReading->GetKeyState(keyCount, keys.data()) == keyCount, "GetKeyState mismatch");
    }

    CHECK_HR(gameInput->GetCurrentReading(GameInputKindMouse, nullptr, mouseReading.GetAddressOf()));
    GameInputMouseState mouseState{};
    CHECK_COND(mouseReading->GetMouseState(&mouseState), "GetMouseState returned false");

    // 6. Aggregate Device
    ComPtr<IGameInputDevice> aggPad;
    CHECK_HR(gameInput->CreateAggregateDevice(GameInputKindGamepad, aggPad.GetAddressOf()));
    ComPtr<IGameInputReading> aggReading;
    CHECK_HR(gameInput->GetCurrentReading(GameInputKindGamepad, aggPad.Get(), aggReading.GetAddressOf()));
    CHECK_COND(aggReading->GetGamepadState(&gpState), "Aggregate gamepad reading failed");

    // 7. Dispatcher + Async Device Enumeration
    ComPtr<IGameInputDispatcher> dispatcher;
    CHECK_HR(gameInput->CreateDispatcher(dispatcher.GetAddressOf()));
    HANDLE waitHandle = nullptr;
    CHECK_HR(dispatcher->OpenWaitHandle(&waitHandle));
    CHECK_COND(waitHandle != nullptr, "OpenWaitHandle returned null");

    std::atomic<int> asyncEnumCount{0};
    GameInputCallbackToken asyncToken = 0;
    CHECK_HR(gameInput->RegisterDeviceCallback(
        nullptr, GameInputKindGamepad, GameInputDeviceConnected, GameInputAsyncEnumeration,
        &asyncEnumCount,
        [](GameInputCallbackToken, void* ctx, IGameInputDevice*, uint64_t,
           GameInputDeviceStatus, GameInputDeviceStatus) noexcept {
            static_cast<std::atomic<int>*>(ctx)->fetch_add(1);
        },
        &asyncToken));
    CHECK_COND(WaitForSingleObject(waitHandle, 100) == WAIT_OBJECT_0,
               "Dispatcher wait handle should be signaled after async device enum");
    dispatcher->Dispatch(0);
    CHECK_COND(asyncEnumCount.load() >= 1, "Dispatcher did not deliver async device callbacks");

    CloseHandle(waitHandle);
    CHECK_COND(gameInput->UnregisterCallback(devToken, 5000), "UnregisterCallback(devToken) failed");
    CHECK_COND(gameInput->UnregisterCallback(asyncToken, 5000), "UnregisterCallback(asyncToken) failed");

    std::printf(
        "PASS: GameInput.dll test (Devices[Pads:%d KB:%d Mouse:%d] VID=0x%04X PID=0x%04X Mouse=(%lld,%lld) History+Rumble+Dispatcher OK)\n",
        stats.gamepads, stats.keyboards, stats.mice,
        padInfo->vendorId, padInfo->productId,
        static_cast<long long>(mouseState.positionX),
        static_cast<long long>(mouseState.positionY));
    return 0;
}
