# PlasmaZones About sub-KCM
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Minimal KCM: version info, update checks, settings launcher.
# All settings configuration is in the standalone plasmazones-settings app.

set(kcm_about_SRCS
    kcmabout.cpp
    kcmabout.h
)

# kcmutils_add_qml_kcm auto-discovers QML under ui/ via a hardcoded
# GLOB_RECURSE inside the macro — no QML_FILES arg needed (verify with
# KCMUtilsMacros.cmake __kcmutils_target_qml_sources). Just keep ui/main.qml
# in place.
kcmutils_add_qml_kcm(kcm_plasmazones_about
    SOURCES ${kcm_about_SRCS}
)
# System Settings loads the KCM from its plugin directory; PhosphorPaths is
# PlasmaZones' embedded copy, in the private directory.
plasmazones_private_rpath(_plasmazones_kcm_rpath "${KDE_INSTALL_PLUGINDIR}/plasma/kcms/systemsettings")
set_target_properties(kcm_plasmazones_about PROPERTIES INSTALL_RPATH "${_plasmazones_kcm_rpath}")
phosphor_install_embedded(FOR kcm_plasmazones_about)

target_compile_definitions(kcm_plasmazones_about PRIVATE
    PLASMAZONES_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(kcm_plasmazones_about
    PRIVATE
        KF6::KCMUtils
        # PlasmaZones' catalog, composed from the suite folder's name: the KCM
        # runs in System Settings and names no folder.
        PhosphorPaths::PhosphorPaths
)
