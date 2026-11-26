#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Install the packages of .github/ci/arch-packages.txt (or the list given as
# the first argument) into an Arch CI container, for the jobs of ci.yml that
# do not come from the shared C++ CI. A full -Syu rather than -S, because an
# install against a stale sync database risks Arch's unsupported
# partial-upgrade state.
set -euo pipefail

list="${1:-.github/ci/arch-packages.txt}"
mapfile -t packages < <(sed -e 's/#.*//' -e 's/[[:space:]]//g' -e '/^$/d' "$list")
# Retried, so one flaky mirror does not fail the job.
for try in 1 2 3; do
    pacman -Syu --noconfirm --needed "${packages[@]}" && exit 0
    [ "$try" = 3 ] && exit 1
    sleep 10
done
