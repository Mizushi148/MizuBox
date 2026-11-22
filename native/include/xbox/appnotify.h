// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox PLM app state notification shim (<appnotify.h>).

#pragma once
#include <Windows.h>

struct _APPSTATE_REGISTRATION;
using PAPPSTATE_REGISTRATION = _APPSTATE_REGISTRATION*;

struct _APPCONSTRAIN_REGISTRATION;
using PAPPCONSTRAIN_REGISTRATION = _APPCONSTRAIN_REGISTRATION*;

using PAPPSTATE_CHANGE_ROUTINE = VOID(NTAPI*)(BOOLEAN Quiesced, PVOID Context);
using PAPPCONSTRAIN_CHANGE_ROUTINE = VOID(NTAPI*)(BOOLEAN Constrained, PVOID Context);

extern "C" {
ULONG NTAPI WsxRegisterAppStateChangeNotification(
    PAPPSTATE_CHANGE_ROUTINE Routine,
    PVOID Context,
    PAPPSTATE_REGISTRATION* Registration) noexcept;

VOID NTAPI WsxUnregisterAppStateChangeNotification(
    PAPPSTATE_REGISTRATION Registration) noexcept;

ULONG NTAPI WsxRegisterAppConstrainedChangeNotification(
    PAPPCONSTRAIN_CHANGE_ROUTINE Routine,
    PVOID Context,
    PAPPCONSTRAIN_REGISTRATION* Registration) noexcept;

VOID NTAPI WsxUnregisterAppConstrainedChangeNotification(
    PAPPCONSTRAIN_REGISTRATION Registration) noexcept;
}

#define RegisterAppStateChangeNotification WsxRegisterAppStateChangeNotification
#define UnregisterAppStateChangeNotification WsxUnregisterAppStateChangeNotification
#define RegisterAppConstrainedChangeNotification WsxRegisterAppConstrainedChangeNotification
#define UnregisterAppConstrainedChangeNotification WsxUnregisterAppConstrainedChangeNotification
