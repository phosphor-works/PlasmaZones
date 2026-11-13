# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

find_package(Qt6 REQUIRED COMPONENTS Test DBus Gui)

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
    # Qt6::Gui is linked here on purpose. QTEST_MAIN builds a
    # QGuiApplication exactly when the test is compiled against Qt6::Gui
    # (its QT_GUI_LIB definition), and Qt6::Test does not bring it: its link
    # interface is Qt6::Core alone. Gui would reach the tests anyway, through
    # PhosphorControl's PUBLIC PhosphorConfig link, but naming it here makes
    # the application type of the QTEST_MAIN tests a decision of this file
    # instead of a side effect of that library's dependencies. A
    # QGuiApplication needs a platform plugin, hence
    # QT_QPA_PLATFORM=offscreen below. Of the Facet tests, test_facet_slot
    # and test_page_host, which build QML items, use QTEST_MAIN, as do most of
    # the others here. A test that uses QTEST_GUILESS_MAIN needs no plugin. No
    # test creates its own application object or touches widgets.
    target_link_libraries(${name}
        PRIVATE
            PhosphorControl::PhosphorControl
            Qt6::Core
            Qt6::DBus
            Qt6::Gui
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
# Runs the build tool on files of its own, so it needs no fixture module
# either.
phosphorcontrol_add_test(test_facet_metadata_tool)
target_compile_definitions(test_facet_metadata_tool PRIVATE
    PHOSPHOR_FACET_METADATA_TOOL="$<TARGET_FILE:phosphor-facet-metadata>")
add_dependencies(test_facet_metadata_tool phosphor-facet-metadata)

# ─── Facet module fixtures ─────────────────────────────────────────────────────
#
# A STATIC build cannot host Facet modules (see PHOSPHOR_CONTROL_BUILD_SHARED):
# each fixture would carry its own copy of the library, so the host and the
# module would not even share StagingDomain's metaobject. The fixtures and the
# tests that load them need the shared library. test_facet_slot and
# test_page_host below are left out of a static build too: each links the
# library together with the static QML module, which compiles the same
# sources, and two static archives of one set of objects in one executable is
# not a combination this build supports.
if(NOT PHOSPHOR_CONTROL_BUILD_SHARED)
    message(STATUS "phosphor-control: static build, so the Facet module tests are not built")
    return()
endif()
#
# Every test Facet module but notamodule is fake_module/fakemodule.cpp built
# once per variant. A variant differs in its generated fake_module_config.h
# (FAKE_MODE, see the source), its metadata.json, both in a per-variant
# directory on the target's include path, and the catalogs it translates its
# metadata from (TS_FILES). The metadata goes through
# phosphor_add_facet_module_metadata, and the plugin embeds the
# facet_metadata.json it generates. Each library lands at
# ${PHOSPHOR_CONTROL_FIXTURE_DIR}/<variant>.so.
set(PHOSPHOR_CONTROL_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
# The tests copy fixtures here before loading them, not under /tmp, which may
# be mounted noexec.
set(PHOSPHOR_CONTROL_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/facet-staging)
file(MAKE_DIRECTORY ${PHOSPHOR_CONTROL_STAGING_DIR})
set(phosphorcontrol_fixture_targets "")

# phosphorcontrol_add_fake_module(<variant> MODE <n> METADATA <json> [SOURCE <file>]
#                                [TS_FILES <ts>...] [INVALID])
#
# SOURCE replaces fake_module/fakemodule.cpp, for a fixture that is not a
# FakeFacetModule at all.
#
# The metadata goes through phosphor_add_facet_module_metadata like a real
# module's, with TS_FILES as its translations. INVALID lets it through when it
# is invalid on purpose.
#
# Each variant compiles the source through a symbolic link in its own
# directory. phosphor_add_facet_module_metadata makes the plugin source
# depend on the generated metadata, and a source file's dependencies are kept
# per directory, so with one shared path every variant would be rebuilt
# whenever any variant's metadata changed. The function refuses that.
#
# METADATA is one argument. A ';' inside a JSON array's [...] does not split
# it, because CMake keeps a bracketed run in one list element. A ';' anywhere
# else does, and so does a misspelt keyword. Either leaves arguments that no
# keyword claims, which is an error here rather than a variant built from
# half its metadata. A keyword misspelt after TS_FILES is taken as one of its
# catalogs instead, which phosphor_add_facet_module_metadata refuses for not
# being a .ts file.
#
# MODE 44 is the variant whose library cannot be loaded, because it calls a
# function no library defines (see fakemodule.cpp). It is compiled with -fplt
# and linked with -z undefs and -z lazy, so the call goes through a lazily
# bound PLT slot and only the eager bind PluginCatalog::load asks for reports
# it. Without -fplt a -fno-plt in CXXFLAGS would bind it at dlopen anyway.
function(phosphorcontrol_add_fake_module variant)
    cmake_parse_arguments(ARG "INVALID" "MODE;METADATA;SOURCE" "TS_FILES" ${ARGN})
    if(DEFINED ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "phosphorcontrol_add_fake_module(${variant}): unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
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
    set(invalid "")
    if(ARG_INVALID)
        set(invalid ALLOW_INVALID)
    endif()
    # Not in the generated <target>_facet directory: a link there would put
    # the generated facet_metadata.json beside the source, which the function
    # refuses as a stray copy from the second configure on. No copy as a
    # fallback either, since a copy would go stale when the source changes.
    get_filename_component(sourceName ${ARG_SOURCE} NAME)
    file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCE} ${configDir}/${sourceName} SYMBOLIC)
    add_library(${target} MODULE ${configDir}/${sourceName}
        ${CMAKE_CURRENT_SOURCE_DIR}/fake_module/fake.qrc)
    target_include_directories(${target} PRIVATE ${configDir})
    # PhosphorConfig by name: fakemodule.cpp makes PhosphorConfig::JsonBackend
    # objects itself, which is more than PhosphorControl's link interface
    # promises to keep bringing.
    target_link_libraries(${target}
        PRIVATE PhosphorControl::PhosphorControl PhosphorConfig::PhosphorConfig Qt6::Core)
    if(ARG_MODE EQUAL 44)
        target_compile_options(${target} PRIVATE -fplt)
        target_link_options(${target} PRIVATE "LINKER:-z,undefs" "LINKER:-z,lazy")
    endif()
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME ${variant}
        LIBRARY_OUTPUT_DIRECTORY ${PHOSPHOR_CONTROL_FIXTURE_DIR}
        AUTOMOC ON
        AUTORCC ON
        UNITY_BUILD OFF
    )
    phosphor_add_facet_module_metadata(${target} METADATA ${configDir}/metadata.json TS_FILES ${ARG_TS_FILES} ${invalid})
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
# A module that defines a category of its own. Its search entry has the title
# of translated's: "owncat" sorts before "translated" by id, while its games
# category comes after apps, where translated goes, in the sidebar.
phosphorcontrol_add_fake_module(owncat MODE 0 METADATA [=[{
    "Id": "owncat", "ApiVersion": 1, "Name": "Own Category", "Category": "games",
    "Categories": [ { "Id": "games", "Name": "Games", "Icon": "games-icon", "Order": 55 } ],
    "Pages": [ { "Id": "page", "Name": "Page", "Anchors": ["shared"] } ],
    "Search": [ { "Page": "page", "Anchor": "shared", "Title": "Pointer speed" } ] }]=])
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
_fake_with_good_pages(discardbroken 50 "Discard Broken")
phosphorcontrol_add_fake_module(notamodule MODE 0 SOURCE fake_module/notamodule.cpp METADATA [=[{
    "Id": "notamodule", "ApiVersion": 1, "Name": "Not A Module", "Pages": [ { "Id": "page", "Name": "Page" } ] }]=])
_fake_with_good_pages(noqml 16 "No QML")
_fake_with_good_pages(dirtyatload 17 "Dirty At Load")
_fake_with_good_pages(silent 18 "Silent")
_fake_with_good_pages(missingqml 19 "Missing QML")
_fake_with_good_pages(slowresult 20 "Slow Result")
_fake_with_good_pages(relativeqml 21 "Relative QML")
_fake_with_good_pages(pagethread 32 "Page Thread")
_fake_with_good_pages(pageasdomain 33 "Page As Domain")
_fake_with_good_pages(domainthread 34 "Domain Thread")
_fake_with_good_pages(deletespage 36 "Deletes Page")
_fake_with_good_pages(deletesdomain 37 "Deletes Domain")
_fake_with_good_pages(foreignpage 38 "Foreign Page")
_fake_with_good_pages(foreigndomain 39 "Foreign Domain")
_fake_with_good_pages(headlessgone 54 "Headless Gone")
_fake_with_good_pages(deletesroot 56 "Deletes Root")
# The same module id as "good" with different metadata: a module that changed
# on disk between start() and its first load.
phosphorcontrol_add_fake_module(goodv2 MODE 0 METADATA
    "{ \"Id\": \"good\", \"ApiVersion\": 1, \"Name\": \"Good Two\", \"Category\": \"input\",${_good_pages} }")
# The next release of "good": the speed anchor of its page general is renamed,
# so the page's anchors version goes up to 2. Its panel point is unchanged.
phosphorcontrol_add_fake_module(goodnext MODE 0 METADATA [=[{
    "Id": "good", "ApiVersion": 1, "Name": "Good Next", "Category": "input", "Order": 20,
    "Pages": [
        { "Id": "general/advanced", "Name": "Advanced", "Parent": "general" },
        { "Id": "general", "Name": "General", "Keywords": ["basic"], "Anchors": ["speed-new"] },
        { "Id": "about", "Name": "About" }
    ],
    "ExtensionPoints": [ { "Page": "general", "Id": "panel", "Version": 2 },
                         { "Page": "general", "Id": "anchors", "Version": 2 } ] }]=])
# Its metadata is valid: parse() reads ApiVersion without judging it, and the
# catalog is what refuses the module.
phosphorcontrol_add_fake_module(wrongapi MODE 0 METADATA [=[{
    "Id": "wrongapi", "ApiVersion": 2, "Name": "Wrong Api", "Pages": [ { "Id": "page", "Name": "Page" } ] }]=])
phosphorcontrol_add_fake_module(badmetadata MODE 0 INVALID METADATA [=[{
    "Id": "badmetadata", "ApiVersion": 1,
    "Pages": [ { "Id": "a:b", "Name": "Colon" } ] }]=])
# A key no Facet knows, which loads with a warning. The tool prints that
# warning when it generates this fixture's metadata, on purpose.
phosphorcontrol_add_fake_module(typokey MODE 0 METADATA [=[{
    "Id": "typokey", "ApiVersion": 1, "Name": "Typo Key",
    "Pages": [ { "Id": "page", "Name": "Page", "Keywrods": ["typo"] } ] }]=])
# The same module id as "badmetadata" with valid metadata: the module after
# its author fixed it.
phosphorcontrol_add_fake_module(fixedmetadata MODE 0 METADATA [=[{
    "Id": "badmetadata", "ApiVersion": 1, "Name": "Fixed Metadata",
    "Pages": [ { "Id": "page", "Name": "Page" } ] }]=])
# Two modules that each put a page under the other's page, both placed outside
# their module: a cycle no order of placing them can satisfy.
phosphorcontrol_add_fake_module(pagecyclea MODE 0 METADATA [=[{
    "Id": "pagecyclea", "ApiVersion": 1, "Name": "Page Cycle A",
    "Pages": [ { "Id": "leaf", "Name": "Leaf", "Parent": "pagecycleb:leaf" } ] }]=])
phosphorcontrol_add_fake_module(pagecycleb MODE 0 METADATA [=[{
    "Id": "pagecycleb", "ApiVersion": 1, "Name": "Page Cycle B",
    "Pages": [ { "Id": "leaf", "Name": "Leaf", "Parent": "pagecyclea:leaf" } ] }]=])
# A page under a page of that cycle, from outside it. Its module sorts ahead
# of both cycle members, so it is the first page to wait.
phosphorcontrol_add_fake_module(pagechain MODE 0 METADATA [=[{
    "Id": "pagechain", "ApiVersion": 1, "Name": "Page Chain",
    "Pages": [ { "Id": "leaf", "Name": "Leaf", "Parent": "pagecyclea:leaf" } ] }]=])

# Modules that add sections to good's pages. extender's first four land on
# good:general: two at the top, one after the speed anchor, one at the panel
# point. Its "ghost", aimed at a module that is not installed, stays inactive.
# Its top two sort by Order against their id order, and extender2's, at Order
# 0 with an id that sorts first, lands after extender's Order 0 ones, so both
# sort keys are pinned. badextender's are all refused.
set(_extender_metadata [=[{
    "Id": "extender", "ApiVersion": 1, "Name": "Extender",
    "Pages": [ { "Id": "own", "Name": "Own" } ],
    "Extensions": [
        { "Id": "top-a", "Target": "good:general", "Slot": "top", "Name": "Top A", "Order": 10 },
        { "Id": "top-b", "Target": "good:general", "Slot": "top", "Name": "Top B" },
        { "Id": "after-speed", "Target": "good:general", "Slot": "after:speed", "Name": "After speed" },
        { "Id": "panel", "Target": "good:general", "Slot": "panel", "TargetVersion": 2, "Name": "Panel" },
        { "Id": "ghost", "Target": "absent:page", "Slot": "top", "Name": "Ghost" } ] }]=])
phosphorcontrol_add_fake_module(extender MODE 0 METADATA "${_extender_metadata}")
# A broken build of extender: the same metadata, so the host takes the two for
# one module, in a library that cannot be loaded. The live-reload test
# replaces it with the working extender.
phosphorcontrol_add_fake_module(extenderbroken MODE 44 METADATA "${_extender_metadata}")
# Its one extension is aimed at badmetadata, a module whose metadata is
# invalid.
phosphorcontrol_add_fake_module(targetsbad MODE 0 METADATA [=[{
    "Id": "targetsbad", "ApiVersion": 1, "Name": "Targets Bad",
    "Pages": [ { "Id": "own", "Name": "Own" } ],
    "Extensions": [ { "Id": "top", "Target": "badmetadata:page", "Slot": "top", "Name": "Top" } ] }]=])
phosphorcontrol_add_fake_module(extender2 MODE 0 METADATA [=[{
    "Id": "extender2", "ApiVersion": 1, "Name": "Extender Two",
    "Pages": [ { "Id": "own", "Name": "Own" } ],
    "Extensions": [ { "Id": "aaa", "Target": "good:general", "Slot": "top", "Name": "Aaa",
                      "Keywords": ["ext-word"] } ] }]=])
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
macro(_fake_with_two_extensions variant mode name)
    phosphorcontrol_add_fake_module(${variant} MODE ${mode} METADATA
        "{ \"Id\": \"${variant}\", \"ApiVersion\": 1, \"Name\": \"${name}\",${_two_extensions} }")
endmacro()
_fake_with_two_extensions(missingextension 22 "Missing Extension")
_fake_with_two_extensions(extraextension 23 "Extra Extension")
_fake_with_two_extensions(extensiondouble 4 "Extension Double")
_fake_with_two_extensions(extensionnodomain 24 "Extension No Domain")
_fake_with_two_extensions(extensionshared 25 "Extension Shared")
_fake_with_two_extensions(extensionrelative 26 "Extension Relative")
_fake_with_two_extensions(extensionmissingqml 27 "Extension Missing QML")
_fake_with_two_extensions(extensionthread 28 "Extension Thread")
_fake_with_two_extensions(deletesduringload 29 "Deletes During Load")
_fake_with_two_extensions(extensionnoqml 35 "Extension No QML")
_fake_with_two_extensions(deleteslater 52 "Deletes Later")
_fake_with_two_extensions(removedwhileloading 55 "Removed While Loading")

# Modules that open a config file. Each has the one page "general", like the
# page-contract variants, so the same keeps-nothing checks cover the ones that
# break the contract. Each opens the file $FAKE_CONFIG_PATH names at the moment
# it loads, so a test decides which of them share one.
macro(_fake_with_config variant mode name)
    phosphorcontrol_add_fake_module(${variant} MODE ${mode} METADATA
        "{ \"Id\": \"${variant}\", \"ApiVersion\": 1, \"Name\": \"${name}\", \"Pages\": [ { \"Id\": \"general\", \"Name\": \"General\" } ] }")
endmacro()
_fake_with_config(sharera 30 "Sharer A")
_fake_with_config(sharerb 30 "Sharer B")
_fake_with_config(sharerc 30 "Sharer C")
_fake_with_config(relativeconfig 31 "Relative Config")
_fake_with_config(confignull 40 "Config Null")
_fake_with_config(configempty 41 "Config Empty")
_fake_with_config(confignested 42 "Config Nested")
_fake_with_config(configthenbreaks 43 "Config Then Breaks")

# Metadata localized from a catalog at build time, with a setting and an
# advanced-only section to search for on its first page. Its second page has
# a setting of its own, and the module's keywords and name find it as they
# find the first. It takes its place after the first, so a pageRegistered slot
# for the first runs while the module is only partly in the sidebar.
phosphorcontrol_add_fake_module(translated MODE 0
    TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/fake_module/translated_de.ts
    METADATA [=[{
    "Id": "translated", "ApiVersion": 1, "Name": "Translated", "Keywords": ["module-word"],
    "Pages": [ { "Id": "speed", "Name": "Speed", "Keywords": ["fast", "quick"], "Anchors": ["pointer", "accel"] },
               { "Id": "extra", "Name": "Extra", "Anchors": ["more"] } ],
    "Search": [ { "Page": "speed", "Anchor": "pointer", "Title": "Pointer speed", "Keywords": ["fast"] },
                { "Page": "speed", "Anchor": "accel", "Kind": "Section", "Title": "Acceleration",
                  "AdvancedOnly": true },
                { "Page": "extra", "Anchor": "more", "Title": "More options" } ] }]=])

# Dynamic search providers: well-behaved ones (providers, providerstwo),
# strayprovider pointing elsewhere, undeclaredprovider registered without the
# metadata flag, providersbroken in a registration that breaks the contract
# otherwise, badproviders registering a null provider and one twice, and the
# providers module again in a library that cannot be loaded
# (providersunloadable).
# Their one page is "general", like the page-contract variants, so the same
# keeps-nothing checks cover the ones that break the contract.
set(_provider_pages [=[ "Pages": [ { "Id": "general", "Name": "General", "Anchors": ["list"] } ] ]=])
set(_providers_metadata
    "{ \"Id\": \"providers\", \"ApiVersion\": 1, \"Name\": \"Providers\", \"SearchProviders\": true,${_provider_pages} }")
phosphorcontrol_add_fake_module(providers MODE 45 METADATA "${_providers_metadata}")
phosphorcontrol_add_fake_module(providersunloadable MODE 44 METADATA "${_providers_metadata}")
phosphorcontrol_add_fake_module(providerstwo MODE 45 METADATA
    "{ \"Id\": \"providerstwo\", \"ApiVersion\": 1, \"Name\": \"Providers Two\", \"SearchProviders\": true,${_provider_pages} }")
phosphorcontrol_add_fake_module(strayprovider MODE 46 METADATA
    "{ \"Id\": \"strayprovider\", \"ApiVersion\": 1, \"Name\": \"Stray\", \"SearchProviders\": true,${_provider_pages} }")
phosphorcontrol_add_fake_module(undeclaredprovider MODE 45 METADATA
    "{ \"Id\": \"undeclaredprovider\", \"ApiVersion\": 1, \"Name\": \"Undeclared\",${_provider_pages} }")
phosphorcontrol_add_fake_module(providersbroken MODE 47 METADATA
    "{ \"Id\": \"providersbroken\", \"ApiVersion\": 1, \"Name\": \"Providers Broken\", \"SearchProviders\": true,${_provider_pages} }")
phosphorcontrol_add_fake_module(badproviders MODE 49 METADATA
    "{ \"Id\": \"badproviders\", \"ApiVersion\": 1, \"Name\": \"Bad Providers\", \"SearchProviders\": true,${_provider_pages} }")
# Providers that are QObjects: two the context refuses, and one a test deletes.
phosphorcontrol_add_fake_module(providerobjects MODE 51 METADATA
    "{ \"Id\": \"providerobjects\", \"ApiVersion\": 1, \"Name\": \"Provider Objects\", \"SearchProviders\": true,${_provider_pages} }")
phosphorcontrol_add_fake_module(providerobject MODE 53 METADATA
    "{ \"Id\": \"providerobject\", \"ApiVersion\": 1, \"Name\": \"Provider Object\", \"SearchProviders\": true,${_provider_pages} }")
# Says it has providers and registers none.
phosphorcontrol_add_fake_module(providersnone MODE 0 METADATA
    "{ \"Id\": \"providersnone\", \"ApiVersion\": 1, \"Name\": \"Providers None\", \"SearchProviders\": true,${_provider_pages} }")
# A provider whose entries FacetTestHost checks one by one: on its own page,
# on the page its one extension targets, and an action with no page.
phosphorcontrol_add_fake_module(providerchecks MODE 48 METADATA [=[{
    "Id": "providerchecks", "ApiVersion": 1, "Name": "Provider Checks", "SearchProviders": true,
    "Pages": [ { "Id": "general", "Name": "General", "Anchors": ["list"] } ],
    "Extensions": [ { "Id": "first", "Target": "good:general", "Slot": "top", "Name": "First" } ] }]=])

phosphorcontrol_add_test(test_facet_host)
# Its page controller stub is simple_page.h, which declares Q_OBJECT in a
# header: AUTOMOC runs moc on it only when it is one of the target's sources.
target_sources(test_facet_host PRIVATE simple_page.h)
phosphorcontrol_add_test(test_facet_test_host)
target_link_libraries(test_facet_test_host PRIVATE PhosphorControl::PhosphorControlTest)
# The anchors files phosphor_add_facet_module_metadata generates, so the test
# checks a release against what the tool really writes.
target_compile_definitions(test_facet_test_host PRIVATE
    PHOSPHOR_CONTROL_GOOD_ANCHORS="$<TARGET_PROPERTY:phosphor_control_fixture_good,FACET_ANCHORS_FILE>"
    PHOSPHOR_CONTROL_GOODNEXT_ANCHORS="$<TARGET_PROPERTY:phosphor_control_fixture_goodnext,FACET_ANCHORS_FILE>")

# FacetSlot and FacetExtensionFrame, through the static org.phosphor.control
# module the in-tree apps link. They import org.kde.kirigami, and so does
# PageHost, so those tests need Kirigami's QML module at run time and are left
# out without it.
find_package(KF6Kirigami QUIET)
if(KF6Kirigami_FOUND)
    phosphorcontrol_add_test(test_facet_slot)
    target_link_libraries(test_facet_slot PRIVATE PhosphorControl::PhosphorControlQml PhosphorControlQmlplugin
                                                  Qt6::Quick)
    phosphorcontrol_add_test(test_page_host)
    target_link_libraries(test_page_host PRIVATE
        PhosphorControl::PhosphorControlQml PhosphorControlQmlplugin PhosphorAnimationQmlplugin Qt6::Quick)
    # Their shared page controller stub declares Q_OBJECT in a header, which
    # AUTOMOC runs moc on only when the header is one of the target's sources.
    foreach(test test_facet_slot test_page_host)
        target_sources(${test} PRIVATE simple_page.h)
    endforeach()
else()
    message(STATUS "phosphor-control: KF6 Kirigami not found, so test_facet_slot and test_page_host are not built")
endif()
foreach(test test_facet_host test_facet_test_host)
    target_compile_definitions(${test} PRIVATE
        PHOSPHOR_CONTROL_FIXTURE_DIR="${PHOSPHOR_CONTROL_FIXTURE_DIR}"
        PHOSPHOR_CONTROL_STAGING_DIR="${PHOSPHOR_CONTROL_STAGING_DIR}")
    add_dependencies(${test} ${phosphorcontrol_fixture_targets})
endforeach()
