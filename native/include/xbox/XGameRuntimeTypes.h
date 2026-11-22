// Copyright (c) 2026 edupode (Original AnyXboxSeries) and local contributors
// SPDX-License-Identifier: CPAL-1.0
//
// Clean-room Xbox GDK common runtime types (<XGameRuntimeTypes.h>).

#pragma once

#include <Windows.h>
#include <cstdint>

struct XVersion {
    union {
        struct {
            uint16_t major;
            uint16_t minor;
            uint16_t build;
            uint16_t revision;
        };
        uint64_t Value;
    };
};
