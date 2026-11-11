# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# CPU meter plugin for phosphor-registry-plugin-demo: a bar-widget Qt
# plugin whose id and ApiVersion live in metadata.json, embedded by
# Q_PLUGIN_METADATA. It builds into the bar-widget directory of the
# per-build plugin root the demo searches first. "bar-widget" is the name
# PhosphorRegistry::PluginLoader::barWidgetSeam() gives that directory.

qt_add_plugin(phosphor_registry_demo_cpu_meter_plugin SHARED
    cpumeter.cpp
)

target_link_libraries(phosphor_registry_demo_cpu_meter_plugin
    PRIVATE
        PhosphorRegistry::PhosphorRegistry
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
)

set_target_properties(phosphor_registry_demo_cpu_meter_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "cpu-meter"
    LIBRARY_OUTPUT_DIRECTORY ${PHOSPHOR_REGISTRY_PLUGIN_DEMO_ROOT}/bar-widget
)

# The plugin catalog refuses a plugin that is group-writable, or that sits
# in a group-writable directory, and a umask of 002 leaves both that way.
# The linker writes a new file on every relink, so clear the bits after
# each one. A demo running during a relink may briefly see the new file
# before this runs and withdraw cpu-meter, then offer it again once the
# mode changes.
add_custom_command(TARGET phosphor_registry_demo_cpu_meter_plugin POST_BUILD
    COMMAND chmod go-w $<TARGET_FILE:phosphor_registry_demo_cpu_meter_plugin>
                       $<TARGET_FILE_DIR:phosphor_registry_demo_cpu_meter_plugin>
    VERBATIM
)
