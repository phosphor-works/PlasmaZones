// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Every unit test runs as a PlasmaZones executable would: initialized as a
// PlasmaZones host (its app directory name and its settings extension) before
// main, where the executables do it at the top of theirs.
// tests/unit/CMakeLists.txt adds this source to every test that links
// plasmazones_core.

#include "common/hostsettings.h"

namespace {
const bool s_hostInitialized = [] {
    PlasmaZones::initPlasmaZonesHost();
    return true;
}();
} // namespace
