# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PlasmaZones Test Suite

# Enable testing
enable_testing()

# Find Qt Test module
find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)

# Include directories for tests
include_directories(
    ${PROJECT_SOURCE_DIR}/plasmazones/src
    ${PROJECT_BINARY_DIR}/plasmazones/src  # For generated headers like plasmazones_export.h
    # Tests include shared fixtures as "helpers/X.h" from any partition depth.
    ${PROJECT_SOURCE_DIR}/plasmazones/tests/unit
)

# The surface decoration packs are not in this tree: the tests that load them
# read the pinned phosphor-surface-packs tree (plasmazones/CMakeLists.txt).
add_compile_definitions(PHOSPHOR_SURFACE_PACKS_DIR="${PHOSPHOR_SURFACE_PACKS_DIR}")

# Add subdirectories
add_subdirectory(unit)
# add_subdirectory(integration)  # Enable when integration tests are ready
