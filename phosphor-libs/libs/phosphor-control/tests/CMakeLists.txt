# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(Qt6 REQUIRED COMPONENTS Test DBus)

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
function(phosphorcontrol_add_test name)
    add_executable(${name} ${name}.cpp)
    # Qt6::Gui is pulled in transitively by Qt6::Test (which links
    # Qt6::Gui in its INTERFACE), so we don't need it explicitly. A test
    # using QTEST_MAIN gets a QGuiApplication (the QT_GUI_LIB variant),
    # which needs a platform plugin, hence QT_QPA_PLATFORM=offscreen
    # below. test_facet_slot, which builds QML items, is one of those; the
    # other Facet tests use QTEST_GUILESS_MAIN and need none. No test
    # creates its own application object or touches widgets.
    target_link_libraries(${name}
        PRIVATE
            PhosphorControl::PhosphorControl
            Qt6::Core
            Qt6::DBus
            Qt6::Test
    )
    # LABELS lets `ctest -L phosphorcontrol` filter to just this
    # lib's tests in a workspace that runs many ctest suites.
    add_test(NAME ${name} COMMAND ${name})
    phosphor_apply_test_isolation(${name})
    # APPEND, never a bare SET: `set_tests_properties(... PROPERTIES
    # ENVIRONMENT ...)` REPLACES the whole list and would drop the XDG
    # sandbox `phosphor_apply_test_isolation` just installed.
    phosphor_append_test_environment(${name} "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(${name} PROPERTIES
        LABELS "phosphorcontrol"
    )
endfunction()

phosphorcontrol_add_test(test_page_registry)
phosphorcontrol_add_test(test_application_controller)
phosphorcontrol_add_test(test_application_controller_history)
phosphorcontrol_add_test(test_localized_context)
phosphorcontrol_add_test(test_dbus_bridge)
phosphorcontrol_add_test(test_search_ranker)
phosphorcontrol_add_test(test_search_controller)
phosphorcontrol_add_test(test_sidebar_rows)
# Parses metadata only, so it needs no fixture module.
phosphorcontrol_add_test(test_facet_module_metadata)

# ─── Facet module fixtures ─────────────────────────────────────────────────────
#
# A STATIC build cannot host Facet modules (see PHOSPHOR_CONTROL_BUILD_SHARED):
# each fixture would carry its own copy of the library, so the host and the
# module would not even share StagingDomain's metaobject. The fixtures and the
# tests that load them need the shared library. test_facet_slot below is left
# out of a static build too: it links the library together with the static
# QML module, which compiles the same sources, and two static archives of one
# set of objects in one executable is not a combination this build supports.
if(NOT PHOSPHOR_CONTROL_BUILD_SHARED)
    message(STATUS "phosphor-control: static build, so the Facet module tests are not built")
    return()
endif()
#
# Every test Facet module is fake_module/fakemodule.cpp built once per variant.
# A variant differs only in its generated fake_module_config.h (FAKE_MODE, see
# the source) and metadata.json, both in a per-variant directory on the
# target's include path, where moc finds the metadata file. Each library lands
# at ${PHOSPHOR_CONTROL_FIXTURE_DIR}/<variant>.so.
set(PHOSPHOR_CONTROL_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
# The tests copy fixtures here before loading them, not under /tmp, which may
# be mounted noexec.
set(PHOSPHOR_CONTROL_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/facet-staging)
file(MAKE_DIRECTORY ${PHOSPHOR_CONTROL_STAGING_DIR})
set(phosphorcontrol_fixture_targets "")

# phosphorcontrol_add_fake_module(<variant> MODE <n> METADATA <json> [SOURCE <file>])
#
# SOURCE replaces fake_module/fakemodule.cpp, for a fixture that is not a
# FakeFacetModule at all.
#
# METADATA must not contain ';': cmake_parse_arguments splits a
# one-value argument on it, and the variant would silently get only the part
# before the first one.
function(phosphorcontrol_add_fake_module variant)
    cmake_parse_arguments(ARG "" "MODE;METADATA;SOURCE" "" ${ARGN})
    if(NOT DEFINED ARG_SOURCE)
        set(ARG_SOURCE fake_module/fakemodule.cpp)
    endif()
    if(NOT DEFINED ARG_MODE OR NOT ARG_MODE MATCHES "^[0-9]+$")
        message(FATAL_ERROR "phosphorcontrol_add_fake_module(${variant}): MODE must be a number, see fakemodule.cpp")
    endif()
    if(NOT DEFINED ARG_METADATA)
        message(FATAL_ERROR "phosphorcontrol_add_fake_module(${variant}): METADATA is required")
    endif()
    set(target phosphor_control_fixture_${variant})
    set(configDir ${CMAKE_CURRENT_BINARY_DIR}/fixture-config/${variant})
    set(FAKE_MODE ${ARG_MODE})
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/fake_module/fake_module_config.h.in
                   ${configDir}/fake_module_config.h @ONLY)
    file(WRITE ${configDir}/metadata.json.tmp "${ARG_METADATA}\n")
    configure_file(${configDir}/metadata.json.tmp ${configDir}/metadata.json COPYONLY)
    add_library(${target} MODULE ${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCE}
        ${CMAKE_CURRENT_SOURCE_DIR}/fake_module/fake.qrc)
    target_include_directories(${target} PRIVATE ${configDir})
    target_link_libraries(${target} PRIVATE PhosphorControl::PhosphorControl Qt6::Core)
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME ${variant}
        LIBRARY_OUTPUT_DIRECTORY ${PHOSPHOR_CONTROL_FIXTURE_DIR}
        AUTOMOC ON
        AUTORCC ON
        UNITY_BUILD OFF
    )
    set(phosphorcontrol_fixture_targets ${phosphorcontrol_fixture_targets} ${target} PARENT_SCOPE)
endfunction()

# Declared parent-first except for one child, so the parse's parents-first
# order and its keeping of sibling order are both exercised.
set(_good_pages [=[
    "Pages": [
        { "Id": "general/advanced", "Name": "Advanced", "Parent": "general" },
        { "Id": "general", "Name": "General", "Name[de]": "Allgemein", "Keywords": ["basic"],
          "Anchors": ["speed"] },
        { "Id": "about", "Name": "About" }
    ],
    "ExtensionPoints": [ { "Page": "general", "Id": "panel", "Version": 2 } ]]=])
macro(_fake_with_good_pages variant mode name)
    phosphorcontrol_add_fake_module(${variant} MODE ${mode} METADATA
        "{ \"Id\": \"${variant}\", \"ApiVersion\": 1, \"Name\": \"${name}\",${_good_pages} }")
endmacro()

phosphorcontrol_add_fake_module(good MODE 0 METADATA
    "{ \"Id\": \"good\", \"ApiVersion\": 1, \"Name\": \"Good\", \"Category\": \"input\", \"Icon\": \"good-icon\", \"Order\": 20,${_good_pages} }")
# Same category as good, sorted ahead of it by Order. Its page "simple" has a
# divider and a tier, and its page "pinned" goes into the appearance category
# directly, with a tier.
phosphorcontrol_add_fake_module(early MODE 0 METADATA [=[{
    "Id": "early", "ApiVersion": 1, "Name": "Early", "Category": "input", "Order": 10,
    "Pages": [ { "Id": "main", "Name": "Main", "Counterpart": "simple" },
               { "Id": "simple", "Name": "Simple", "Visibility": "SimpleOnly", "Counterpart": "main",
                 "DividerAfter": true },
               { "Id": "pinned", "Name": "Pinned", "Category": "appearance", "Visibility": "AdvancedOnly" } ] }]=])
phosphorcontrol_add_fake_module(child MODE 0 METADATA [=[{
    "Id": "child", "ApiVersion": 1, "Name": "Child", "Parent": "good",
    "Pages": [ { "Id": "extra", "Name": "Extra", "Parent": "good:general" } ] }]=])
# A page under another module's page that is itself placed outside its
# module: "aaa" sorts first by id, so it is found and placed before "zzz".
phosphorcontrol_add_fake_module(aaa MODE 0 METADATA [=[{
    "Id": "aaa", "ApiVersion": 1, "Name": "Aaa",
    "Pages": [ { "Id": "leaf", "Name": "Leaf", "Parent": "zzz:hub" } ] }]=])
phosphorcontrol_add_fake_module(zzz MODE 0 METADATA [=[{
    "Id": "zzz", "ApiVersion": 1, "Name": "Zzz",
    "Pages": [ { "Id": "hub", "Name": "Hub", "Category": "system" } ] }]=])
# Two modules nested under each other, and one nested under a member of that
# cycle: only the cycle is broken.
phosphorcontrol_add_fake_module(loopa MODE 0 METADATA [=[{
    "Id": "loopa", "ApiVersion": 1, "Name": "Loop A", "Parent": "loopb", "Category": "desktop", "Order": 1,
    "Pages": [ { "Id": "page", "Name": "Page" } ] }]=])
phosphorcontrol_add_fake_module(loopb MODE 0 METADATA [=[{
    "Id": "loopb", "ApiVersion": 1, "Name": "Loop B", "Parent": "loopa", "Category": "desktop", "Order": 2,
    "Pages": [ { "Id": "page", "Name": "Page" } ] }]=])
phosphorcontrol_add_fake_module(looptail MODE 0 METADATA [=[{
    "Id": "looptail", "ApiVersion": 1, "Name": "Loop Tail", "Parent": "loopa",
    "Pages": [ { "Id": "page", "Name": "Page" } ] }]=])
# An absent parent module, with a known category: the node goes to that
# category, and its page under the absent module's page stays under its own
# module.
phosphorcontrol_add_fake_module(orphan MODE 0 METADATA [=[{
    "Id": "orphan", "ApiVersion": 1, "Name": "Orphan", "Parent": "absent", "Category": "system",
    "Pages": [ { "Id": "page", "Name": "Page", "Parent": "absent:somewhere" } ] }]=])
# Nested under orphan, whose page fell back under it: that page stays ahead of
# this module, with the module's own pages.
phosphorcontrol_add_fake_module(orphankid MODE 0 METADATA [=[{
    "Id": "orphankid", "ApiVersion": 1, "Name": "Orphan Kid", "Parent": "orphan",
    "Pages": [ { "Id": "page", "Name": "Page" } ] }]=])
# An unknown category: the node goes to apps.
phosphorcontrol_add_fake_module(stray MODE 0 METADATA [=[{
    "Id": "stray", "ApiVersion": 1, "Name": "Stray", "Category": "no-such-category",
    "Pages": [ { "Id": "page", "Name": "Page" } ] }]=])
# A module that defines a category of its own.
phosphorcontrol_add_fake_module(owncat MODE 0 METADATA [=[{
    "Id": "owncat", "ApiVersion": 1, "Name": "Own Category", "Category": "games",
    "Categories": [ { "Id": "games", "Name": "Games", "Icon": "games-icon", "Order": 55 } ],
    "Pages": [ { "Id": "page", "Name": "Page" } ] }]=])
_fake_with_good_pages(missingpage 1 "Missing Page")
_fake_with_good_pages(extrapage 2 "Extra Page")
_fake_with_good_pages(wrongid 3 "Wrong Id")
_fake_with_good_pages(doubleresult 4 "Double Result")
_fake_with_good_pages(twicepage 5 "Twice Page")
_fake_with_good_pages(nullcontroller 6 "Null Controller")
_fake_with_good_pages(twicedomain 7 "Twice Domain")
_fake_with_good_pages(nulldomain 8 "Null Domain")
_fake_with_good_pages(doublediscard 9 "Double Discard")
_fake_with_good_pages(selfdelete 10 "Self Delete")
_fake_with_good_pages(recorder 12 "Recorder")
_fake_with_good_pages(lateresult 13 "Late Result")
_fake_with_good_pages(failingapply 14 "Failing Apply")
phosphorcontrol_add_fake_module(notamodule MODE 0 SOURCE fake_module/notamodule.cpp METADATA [=[{
    "Id": "notamodule", "ApiVersion": 1, "Name": "Not A Module", "Pages": [ { "Id": "page", "Name": "Page" } ] }]=])
_fake_with_good_pages(noqml 16 "No QML")
_fake_with_good_pages(dirtyatload 17 "Dirty At Load")
_fake_with_good_pages(silent 18 "Silent")
_fake_with_good_pages(missingqml 19 "Missing QML")
_fake_with_good_pages(slowresult 20 "Slow Result")
_fake_with_good_pages(relativeqml 21 "Relative QML")
# The same module id as "good" with different metadata: a module that changed
# on disk between start() and its first load.
phosphorcontrol_add_fake_module(goodv2 MODE 0 METADATA
    "{ \"Id\": \"good\", \"ApiVersion\": 1, \"Name\": \"Good Two\", \"Category\": \"input\",${_good_pages} }")
phosphorcontrol_add_fake_module(wrongapi MODE 0 METADATA [=[{
    "Id": "wrongapi", "ApiVersion": 2, "Name": "Wrong Api", "Pages": [ { "Id": "page", "Name": "Page" } ] }]=])
phosphorcontrol_add_fake_module(badmetadata MODE 0 METADATA [=[{
    "Id": "badmetadata", "ApiVersion": 1,
    "Pages": [ { "Id": "a:b", "Name": "Colon" } ] }]=])

# Modules that add sections to good's pages. extender's first four land on
# good:general: two at the top, one after the speed anchor, one at the panel
# point. Its "ghost", aimed at a module that is not installed, stays inactive.
# Its top two sort by Order against their id order, and extender2's, at Order
# 0 with an id that sorts first, lands after extender's Order 0 ones, so both
# sort keys are pinned. badextender's are all refused.
phosphorcontrol_add_fake_module(extender MODE 0 METADATA [=[{
    "Id": "extender", "ApiVersion": 1, "Name": "Extender",
    "Pages": [ { "Id": "own", "Name": "Own" } ],
    "Extensions": [
        { "Id": "top-a", "Target": "good:general", "Slot": "top", "Name": "Top A", "Order": 10 },
        { "Id": "top-b", "Target": "good:general", "Slot": "top", "Name": "Top B" },
        { "Id": "after-speed", "Target": "good:general", "Slot": "after:speed", "Name": "After speed" },
        { "Id": "panel", "Target": "good:general", "Slot": "panel", "TargetVersion": 2, "Name": "Panel" },
        { "Id": "ghost", "Target": "absent:page", "Slot": "top", "Name": "Ghost" } ] }]=])
phosphorcontrol_add_fake_module(extender2 MODE 0 METADATA [=[{
    "Id": "extender2", "ApiVersion": 1, "Name": "Extender Two",
    "Pages": [ { "Id": "own", "Name": "Own" } ],
    "Extensions": [ { "Id": "aaa", "Target": "good:general", "Slot": "top", "Name": "Aaa" } ] }]=])
# Its id is taken by an application page in the test, so it is never placed,
# and its extension must not reach good:general.
phosphorcontrol_add_fake_module(clashext MODE 0 METADATA [=[{
    "Id": "clashext", "ApiVersion": 1, "Name": "Clash Extender",
    "Pages": [ { "Id": "own", "Name": "Own" } ],
    "Extensions": [ { "Id": "top", "Target": "good:general", "Slot": "top", "Name": "Top" } ] }]=])
phosphorcontrol_add_fake_module(badextender MODE 0 METADATA [=[{
    "Id": "badextender", "ApiVersion": 1, "Name": "Bad Extender",
    "Pages": [ { "Id": "own", "Name": "Own" } ],
    "Extensions": [
        { "Id": "old-panel", "Target": "good:general", "Slot": "panel", "Name": "Old panel" },
        { "Id": "new-anchors", "Target": "good:general", "Slot": "top", "TargetVersion": 2, "Name": "New anchors" },
        { "Id": "no-anchor", "Target": "good:general", "Slot": "before:nope", "Name": "No anchor" },
        { "Id": "no-point", "Target": "good:general/advanced", "Slot": "panel", "Name": "No point" },
        { "Id": "no-page", "Target": "good:nowhere", "Slot": "top", "Name": "No page" } ] }]=])
# Their own page is "general", like the page-contract variants, so the same
# keeps-nothing checks cover them.
set(_two_extensions [=[
    "Pages": [ { "Id": "general", "Name": "General" } ],
    "Extensions": [
        { "Id": "first", "Target": "good:general", "Slot": "bottom", "Name": "First" },
        { "Id": "second", "Target": "good:general", "Slot": "bottom", "Name": "Second" } ]]=])
phosphorcontrol_add_fake_module(missingextension MODE 22 METADATA
    "{ \"Id\": \"missingextension\", \"ApiVersion\": 1, \"Name\": \"Missing Extension\",${_two_extensions} }")
phosphorcontrol_add_fake_module(extraextension MODE 23 METADATA
    "{ \"Id\": \"extraextension\", \"ApiVersion\": 1, \"Name\": \"Extra Extension\",${_two_extensions} }")
macro(_fake_with_two_extensions variant mode name)
    phosphorcontrol_add_fake_module(${variant} MODE ${mode} METADATA
        "{ \"Id\": \"${variant}\", \"ApiVersion\": 1, \"Name\": \"${name}\",${_two_extensions} }")
endmacro()
_fake_with_two_extensions(extensiondouble 4 "Extension Double")
_fake_with_two_extensions(extensionnodomain 24 "Extension No Domain")
_fake_with_two_extensions(extensionshared 25 "Extension Shared")
_fake_with_two_extensions(extensionrelative 26 "Extension Relative")
_fake_with_two_extensions(extensionmissingqml 27 "Extension Missing QML")
_fake_with_two_extensions(extensionthread 28 "Extension Thread")
_fake_with_two_extensions(deletesduringload 29 "Deletes During Load")

# Two modules writing one config file, and one opening it by a relative path.
phosphorcontrol_add_fake_module(sharera MODE 30 METADATA [=[{
    "Id": "sharera", "ApiVersion": 1, "Name": "Sharer A", "Pages": [ { "Id": "general", "Name": "General" } ] }]=])
phosphorcontrol_add_fake_module(sharerb MODE 30 METADATA [=[{
    "Id": "sharerb", "ApiVersion": 1, "Name": "Sharer B", "Pages": [ { "Id": "general", "Name": "General" } ] }]=])
phosphorcontrol_add_fake_module(relativeconfig MODE 31 METADATA [=[{
    "Id": "relativeconfig", "ApiVersion": 1, "Name": "Relative Config",
    "Pages": [ { "Id": "general", "Name": "General" } ] }]=])

phosphorcontrol_add_test(test_facet_host)
phosphorcontrol_add_test(test_facet_test_host)
target_link_libraries(test_facet_test_host PRIVATE PhosphorControl::PhosphorControlTest)

# FacetSlot and FacetExtensionFrame, through the static org.phosphor.control
# module the in-tree apps link. They import org.kde.kirigami, so the test
# needs Kirigami's QML module at run time and is left out without it.
find_package(KF6Kirigami QUIET)
if(KF6Kirigami_FOUND)
    phosphorcontrol_add_test(test_facet_slot)
    target_link_libraries(test_facet_slot PRIVATE PhosphorControl::PhosphorControlQml PhosphorControlQmlplugin
                                                  Qt6::Quick)
else()
    message(STATUS "phosphor-control: KF6 Kirigami not found, so test_facet_slot is not built")
endif()
foreach(test test_facet_host test_facet_test_host)
    target_compile_definitions(${test} PRIVATE
        PHOSPHOR_CONTROL_FIXTURE_DIR="${PHOSPHOR_CONTROL_FIXTURE_DIR}"
        PHOSPHOR_CONTROL_STAGING_DIR="${PHOSPHOR_CONTROL_STAGING_DIR}")
    add_dependencies(${test} ${phosphorcontrol_fixture_targets})
endforeach()
