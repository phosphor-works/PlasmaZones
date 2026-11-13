# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(Qt6 REQUIRED COMPONENTS Test Quick Gui Qml)

# Shared test isolation: a private D-Bus session (so a test that touches D-Bus
# cannot activate the INSTALLED daemon, which then holds the test's stdout pipe
# open and hangs ctest after the test itself passed) plus a per-target XDG
# sandbox (so nothing writes into the developer's real ~/.config/plasmazones).
# CMAKE_CURRENT_LIST_DIR, not CMAKE_SOURCE_DIR: the latter is the repo root
# in-tree but this LIBRARY's directory in a standalone configure, so the include
# looked for <tier>/libs/<lib>/cmake/PhosphorTestIsolation.cmake and hard-failed at
# generate time. The relative walk reaches the repo root from either. The EXISTS
# guard covers the remaining case, a genuine extracted `git subtree split` where
# the repo-root module is not in the subtree at all.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/PhosphorTestIsolation.cmake")
    include(${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/PhosphorTestIsolation.cmake)
else()
    # Stand-ins so the call sites below need no guard of their own.
    # phosphor_append_test_environment is reproduced EXACTLY, because it is
    # self-contained and because callers use it for things like
    # QT_QPA_PLATFORM=offscreen that the tests need to pass at all — a no-op
    # there would break them rather than merely un-isolate them. The isolation
    # helper degrades to nothing: its private D-Bus session and XDG sandbox need
    # module-level state that does not exist without the module.
    function(phosphor_apply_test_isolation _target)
    endfunction()
    function(phosphor_append_test_environment _test_name)
        set_property(TEST ${_test_name} APPEND PROPERTY ENVIRONMENT ${ARGN})
    endfunction()
endif()

# ─── Test plugin fixtures ──────────────────────────────────────────────────────
#
# Every bar-widget test plugin is fake_plugin/fakeplugin.cpp built once per
# variant. A variant differs only in its generated fake_plugin_config.h (IID,
# factory id and the switches below) and metadata.json (embedded by
# Q_PLUGIN_METADATA), both written to a per-variant directory on the target's
# include path, where moc finds the metadata file. Each library lands at
# ${PHOSPHOR_REGISTRY_FIXTURE_DIR}/<variant>.so, and the tests copy it into
# temporary search paths.
set(PHOSPHOR_REGISTRY_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
set(phosphorregistry_fixture_targets "")

# phosphorregistry_add_fake_plugin(<variant> FACTORY_ID <id> METADATA <json body>
#     [IID <iid>] [NULL_FACTORY] [NO_INTERFACE] [UNRESOLVED_SYMBOL])
#
# Without IID the variant declares PhosphorRegistryBarWidgetPlugin_iid itself,
# so the fixtures cannot drift from the header. METADATA must not contain a
# ';', which CMake would split the argument on.
#   NULL_FACTORY       createFactory() returns nullptr.
#   NO_INTERFACE       the root object does not implement IBarWidgetPlugin.
#   UNRESOLVED_SYMBOL  createFactory() calls a function no library defines.
#                      Built with -fplt and linked with -z undefs and -z lazy,
#                      the call goes through a lazily bound PLT slot, so only
#                      an eager bind at load time can catch it. Without -fplt
#                      a -fno-plt in CXXFLAGS would bind it at dlopen anyway.
function(phosphorregistry_add_fake_plugin variant)
    cmake_parse_arguments(ARG "NULL_FACTORY;NO_INTERFACE;UNRESOLVED_SYMBOL" "IID;FACTORY_ID;METADATA" "" ${ARGN})
    set(target phosphor_registry_fixture_${variant})
    set(configDir ${CMAKE_CURRENT_BINARY_DIR}/fixture-config/${variant})
    if(ARG_IID)
        set(FAKE_PLUGIN_IID_EXPR "\"${ARG_IID}\"")
    else()
        set(FAKE_PLUGIN_IID_EXPR "PhosphorRegistryBarWidgetPlugin_iid")
    endif()
    set(FAKE_FACTORY_ID "${ARG_FACTORY_ID}")
    foreach(flag NULL_FACTORY NO_INTERFACE UNRESOLVED_SYMBOL)
        if(ARG_${flag})
            set(FAKE_${flag} 1)
        else()
            set(FAKE_${flag} 0)
        endif()
    endforeach()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/fake_plugin/fake_plugin_config.h.in
                   ${configDir}/fake_plugin_config.h @ONLY)
    # Written to a scratch file and copied only when it differs, so an
    # unchanged metadata.json keeps its timestamp and does not re-run moc.
    file(WRITE ${configDir}/metadata.json.tmp "${ARG_METADATA}\n")
    configure_file(${configDir}/metadata.json.tmp ${configDir}/metadata.json COPYONLY)

    add_library(${target} MODULE ${CMAKE_CURRENT_SOURCE_DIR}/fake_plugin/fakeplugin.cpp)
    target_include_directories(${target} PRIVATE ${configDir})
    target_link_libraries(${target}
        PRIVATE
            PhosphorRegistry::PhosphorRegistry
            Qt6::Core
            Qt6::Gui
            Qt6::Qml
            Qt6::Quick
    )
    if(ARG_UNRESOLVED_SYMBOL)
        target_compile_options(${target} PRIVATE -fplt)
        target_link_options(${target} PRIVATE "LINKER:-z,undefs" "LINKER:-z,lazy")
    endif()
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME ${variant}
        LIBRARY_OUTPUT_DIRECTORY ${PHOSPHOR_REGISTRY_FIXTURE_DIR}
        AUTOMOC ON
    )
    set(phosphorregistry_fixture_targets ${phosphorregistry_fixture_targets} ${target} PARENT_SCOPE)
endfunction()

phosphorregistry_add_fake_plugin(fake-plugin FACTORY_ID "fake-plugin"
    METADATA [=[{ "Id": "fake-plugin", "ApiVersion": 1 }]=])
# fake-plugin again, with one more metadata field.
phosphorregistry_add_fake_plugin(fake-plugin-extra FACTORY_ID "fake-plugin"
    METADATA [=[{ "Id": "fake-plugin", "ApiVersion": 1, "Extra": true }]=])
phosphorregistry_add_fake_plugin(fake-plugin-secondary FACTORY_ID "fake-plugin-secondary"
    METADATA [=[{ "Id": "fake-plugin-secondary", "ApiVersion": 1 }]=])
# The metadata says one id and the factory another.
phosphorregistry_add_fake_plugin(fake-plugin-idmismatch FACTORY_ID "fake-other"
    METADATA [=[{ "Id": "fake-plugin-idmismatch", "ApiVersion": 1 }]=])
phosphorregistry_add_fake_plugin(fake-plugin-nullfactory FACTORY_ID "fake-plugin-nullfactory" NULL_FACTORY
    METADATA [=[{ "Id": "fake-plugin-nullfactory", "ApiVersion": 1 }]=])
phosphorregistry_add_fake_plugin(fake-plugin-nointerface FACTORY_ID "fake-plugin-nointerface" NO_INTERFACE
    METADATA [=[{ "Id": "fake-plugin-nointerface", "ApiVersion": 1 }]=])
# A broken build of fake-plugin-retry: same id, but it cannot be loaded. The
# retry test replaces it with the working fake-plugin-retry.
phosphorregistry_add_fake_plugin(fake-plugin-unresolved FACTORY_ID "fake-plugin-retry" UNRESOLVED_SYMBOL
    METADATA [=[{ "Id": "fake-plugin-retry", "ApiVersion": 1 }]=])
phosphorregistry_add_fake_plugin(fake-plugin-retry FACTORY_ID "fake-plugin-retry"
    METADATA [=[{ "Id": "fake-plugin-retry", "ApiVersion": 1 }]=])
phosphorregistry_add_fake_plugin(fake-plugin-wrongapi FACTORY_ID "fake-plugin-wrongapi"
    METADATA [=[{ "Id": "fake-plugin-wrongapi", "ApiVersion": 2 }]=])
phosphorregistry_add_fake_plugin(fake-plugin-noapi FACTORY_ID "fake-plugin-noapi"
    METADATA [=[{ "Id": "fake-plugin-noapi" }]=])
phosphorregistry_add_fake_plugin(fake-plugin-stringapi FACTORY_ID "fake-plugin-stringapi"
    METADATA [=[{ "Id": "fake-plugin-stringapi", "ApiVersion": "1" }]=])
phosphorregistry_add_fake_plugin(fake-plugin-wrongiid FACTORY_ID "fake-plugin-wrongiid"
    IID "org.phosphor_works.Test.NotABarWidget/1"
    METADATA [=[{ "Id": "fake-plugin-wrongiid", "ApiVersion": 1 }]=])
phosphorregistry_add_fake_plugin(fake-plugin-noid FACTORY_ID "fake-plugin-noid"
    METADATA [=[{ "ApiVersion": 1 }]=])
phosphorregistry_add_fake_plugin(fake-plugin-badid FACTORY_ID "../escape"
    METADATA [=[{ "Id": "../escape", "ApiVersion": 1 }]=])
# The JSON escape decodes to a real newline at the end of the id.
phosphorregistry_add_fake_plugin(fake-plugin-newline FACTORY_ID "fake-plugin-newline"
    METADATA [=[{ "Id": "fake-plugin-newline\n", "ApiVersion": 1 }]=])

# A shared library that is not a Qt plugin.
add_library(phosphor_registry_fixture_plain MODULE ${CMAKE_CURRENT_SOURCE_DIR}/fake_plugin/plain.cpp)
set_target_properties(phosphor_registry_fixture_plain PROPERTIES
    PREFIX ""
    OUTPUT_NAME plain
    LIBRARY_OUTPUT_DIRECTORY ${PHOSPHOR_REGISTRY_FIXTURE_DIR}
)
list(APPEND phosphorregistry_fixture_targets phosphor_registry_fixture_plain)

# ─── Tests ─────────────────────────────────────────────────────────────────────

function(phosphorregistry_add_test name)
    add_executable(${name} ${name}.cpp)
    # Core / Gui / Qml propagate PUBLIC from PhosphorRegistry. Qt6::Test
    # is test-specific. Qt6::Quick is needed by the QQuickItem helpers
    # used in test_registry_helpers.h.
    target_link_libraries(${name}
        PRIVATE
            PhosphorRegistry::PhosphorRegistry
            Qt6::Test
            Qt6::Quick
    )
    add_test(NAME ${name} COMMAND ${name})
    phosphor_apply_test_isolation(${name})
    set_property(TEST ${name} APPEND PROPERTY LABELS "phosphorregistry")
    phosphor_append_test_environment(${name} "QT_QPA_PLATFORM=offscreen")
endfunction()

phosphorregistry_add_test(test_phosphor_registry)
# The shared baseline-plus-keyed-overrides container the four shader-assignment
# trees hold. Tested here rather than through any one tree: all four used to
# hand-write it, and the copies drifted, so the contract belongs where the one
# copy lives. One of its four consumers (OverlayShaderTree) has no unit test at
# all, which left the empty-key refusal and clearOverride's return value unpinned
# for that payload entirely.
phosphorregistry_add_test(test_path_keyed_overrides)
phosphorregistry_add_test(test_phosphor_registry_metadatapack_loader)
phosphorregistry_add_test(test_phosphor_registry_plugincatalog)
phosphorregistry_add_test(test_phosphor_registry_pluginloader)

# The plugin tests stage the fixtures, check with dlopen(RTLD_NOLOAD) whether a
# library is mapped, and assert on the catalog's info lines, which a
# developer's QT_LOGGING_RULES could otherwise switch off. One rule only: ctest
# splits an ENVIRONMENT value on ';'.
foreach(test test_phosphor_registry_plugincatalog test_phosphor_registry_pluginloader)
    target_compile_definitions(${test}
        PRIVATE PHOSPHOR_REGISTRY_FIXTURE_DIR="${PHOSPHOR_REGISTRY_FIXTURE_DIR}")
    target_link_libraries(${test} PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(${test} ${phosphorregistry_fixture_targets})
    phosphor_append_test_environment(${test} "QT_LOGGING_RULES=phosphorregistry.*=true")
endforeach()
