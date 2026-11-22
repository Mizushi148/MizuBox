// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDKX compatibility header (<gxdk.h>).

#pragma once

#define _GXDK_VER 0x65F40752 /* GDK Edition 241000 (October 2024) */

#ifndef _GAMING_XBOX
#define _GAMING_XBOX 1
#endif

#ifndef _GAMING_XBOX_SCARLETT
#define _GAMING_XBOX_SCARLETT 1
#endif

// Microsoft GDKX guarantees UTF-8 as the active code page on console.
#include <Windows.h>
#undef GetACP
inline UINT WsxGetACP() noexcept { return CP_UTF8; }
#define GetACP() WsxGetACP()
