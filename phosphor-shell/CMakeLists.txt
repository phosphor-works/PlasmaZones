# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Phosphor shell tier. Entered from the root CMakeLists only when
# BUILD_PHOSPHOR_SHELL is ON. It carries the phosphorctl IPC CLI; the shell
# binary and its bundled QML tree are being rebuilt on the shell libraries in
# phosphor-shell-libs/.
#
# This tier links nothing from the plasmazones tier, which is what lets the
# moon workspace enforce the tier order.

add_subdirectory(cli/phosphorctl)       # phosphorctl call / list / schema / subscribe

# Aggregate target for the moon workspace: `cmake --build build --target phosphor-shell-tier`.
include(PhosphorTier)
phosphor_tier_target(phosphor-shell-tier)
