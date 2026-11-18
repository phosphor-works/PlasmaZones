// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Every unit test runs as a PlasmaZones executable would: with the app
// directory name set before main, where the executables set it at the top of
// theirs. tests/unit/CMakeLists.txt adds this source to every test that links
// plasmazones_core.

#include "core/platform/appdirectoryname.h"
#include "core/types/constants.h"

namespace {
const bool s_appDirectoryNameSet = [] {
    PlasmaZones::setAppDirectoryName(PlasmaZones::AppDirectoryName);
    return true;
}();
} // namespace
