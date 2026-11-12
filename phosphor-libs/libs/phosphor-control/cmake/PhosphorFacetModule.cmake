# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# phosphor_add_facet_module_metadata(<target> METADATA <metadata.json>
#                                    [TS_FILES <file.ts>...] [ALLOW_INVALID])
#
# Generates a Facet module's metadata for the plugin <target>, with the
# phosphor-facet-metadata tool, into ${CMAKE_CURRENT_BINARY_DIR}/<target>_facet/:
#
#   facet_metadata.json  the metadata with `Key[<lang>]` added for every string
#                        a catalog translates. The plugin embeds it with
#                        Q_PLUGIN_METADATA(IID PhosphorFacetModule_iid
#                        FILE "facet_metadata.json"). The directory goes first
#                        on the target's include path, where moc looks after
#                        the directory of the file that names it.
#   anchors.json         each page's anchors and anchors version, and the
#                        version of each of its named extension points. Attach
#                        it to the module's releases: FacetTestHost checks the
#                        next release against it (setPreviousAnchors).
#   facet_strings.cpp    the translatable strings, in context "FacetModule",
#                        for lupdate. Not compiled.
#
# Call it after the target's sources are added. The paths are the target's
# FACET_METADATA_FILE, FACET_ANCHORS_FILE and FACET_STRINGS_FILE properties,
# and the custom target <target>_facet_metadata writes them (an lupdate target
# that reads FACET_STRINGS_FILE depends on it).
#
# Invalid metadata fails the build, unless ALLOW_INVALID is given (for tests
# that load invalid modules on purpose). A warning, such as a key this Facet
# does not know, is printed once and never fails it, so metadata written for
# a newer Facet still builds.
#
# Translations: each catalog's strings in context "FacetModule" go under the
# catalog's language, with '-' turned into '_' and a regional catalog (de_DE)
# keyed by the bare language (de) unless that would be ambiguous. A
# `Key[<lang>]` already in METADATA was written by hand and wins. Only
# finished translations are used, so a module whose pages and sidebar entry
# should agree passes -nounfinished to lrelease.
#
# The function refuses a setup in which moc could embed some other file
# named facet_metadata.json: METADATA itself under that name, or such a file
# beside a source that names it or in the target's source or binary
# directory. A plugin source that names facet_metadata.json gets a
# dependency on the generated file, so it is rebuilt in the same build when
# the metadata changes. That dependency is kept per directory, so two
# modules in one directory cannot compile the same plugin source file: give
# each its own path to it, for example a symbolic link.
#
# PHOSPHOR_FACET_METADATA_EXECUTABLE, when set, names a phosphor-facet-metadata
# that runs on the build machine, used in place of PhosphorControl's own, for a
# cross build.

set(PHOSPHOR_FACET_METADATA_EXECUTABLE "" CACHE FILEPATH
    "A phosphor-facet-metadata that runs on the build machine, used in place of PhosphorControl's own")

function(phosphor_add_facet_module_metadata target)
    set(fn "phosphor_add_facet_module_metadata(${target})")
    # PARSE_ARGV keeps an empty argument, which the caller may pass for an
    # empty TS_FILES list, and does not split one that holds a ';'.
    cmake_parse_arguments(PARSE_ARGV 1 ARG "ALLOW_INVALID" "METADATA" "TS_FILES")
    if(DEFINED ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "${fn}: unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR "${fn}: there is no target named ${target}. Create the module's target first.")
    endif()
    get_target_property(aliasedTarget ${target} ALIASED_TARGET)
    if(aliasedTarget)
        message(FATAL_ERROR "${fn}: ${target} is an alias. Pass the target it names, ${aliasedTarget}.")
    endif()
    get_target_property(importedTarget ${target} IMPORTED)
    if(importedTarget)
        message(FATAL_ERROR "${fn}: ${target} is an imported target. Pass the module target this project builds.")
    endif()
    if(NOT DEFINED ARG_METADATA OR ARG_METADATA STREQUAL "")
        message(FATAL_ERROR "${fn}: METADATA is required")
    endif()
    get_filename_component(metadataName "${ARG_METADATA}" NAME)
    if(metadataName STREQUAL "facet_metadata.json")
        message(FATAL_ERROR "${fn}: METADATA must not be called facet_metadata.json, the name of the generated "
                            "file the plugin embeds, or moc may embed it instead. Rename it, for example to "
                            "metadata.json.")
    endif()
    if(PHOSPHOR_FACET_METADATA_EXECUTABLE)
        if(NOT EXISTS "${PHOSPHOR_FACET_METADATA_EXECUTABLE}" OR IS_DIRECTORY "${PHOSPHOR_FACET_METADATA_EXECUTABLE}")
            message(FATAL_ERROR "${fn}: PHOSPHOR_FACET_METADATA_EXECUTABLE is ${PHOSPHOR_FACET_METADATA_EXECUTABLE}, "
                                "which is not a file")
        endif()
        set(tool "${PHOSPHOR_FACET_METADATA_EXECUTABLE}")
    else()
        # One name in the tree that builds PhosphorControl (an alias) and in a
        # project that finds it installed (an imported target, visible only in
        # the directory that called find_package and below it).
        set(tool PhosphorControl::phosphor-facet-metadata)
        if(NOT TARGET ${tool})
            message(FATAL_ERROR "${fn}: there is no target named ${tool} in this directory or a parent. "
                                "Call find_package(PhosphorControl) here or in a parent directory first.")
        endif()
    endif()
    get_filename_component(source "${ARG_METADATA}" ABSOLUTE)
    set(catalogs "")
    foreach(ts IN LISTS ARG_TS_FILES)
        if(ts STREQUAL "")
            continue()
        endif()
        get_filename_component(ts "${ts}" ABSOLUTE)
        # A keyword misspelt after TS_FILES lands in this list, so it is
        # reported here rather than handed to the tool as a catalog.
        if(NOT ts MATCHES "\\.ts$" OR NOT EXISTS "${ts}" OR IS_DIRECTORY "${ts}")
            message(FATAL_ERROR "${fn}: TS_FILES entry ${ts} is not an existing .ts file")
        endif()
        list(APPEND catalogs "${ts}")
    endforeach()
    set(flags "")
    if(ARG_ALLOW_INVALID)
        set(flags --allow-invalid)
    endif()

    set(outDir "${CMAKE_CURRENT_BINARY_DIR}/${target}_facet")
    set(metadataOut "${outDir}/facet_metadata.json")
    set(anchorsOut "${outDir}/anchors.json")
    set(stringsOut "${outDir}/facet_strings.cpp")
    # One command, so invalid metadata stops it at the first run and is
    # reported once. The tool rewrites a file only when its content changed,
    # and the stamp is the declared output, so a rerun forced by a rebuilt
    # tool leaves everything downstream alone on either generator.
    set(stamp "${outDir}/facet_metadata.stamp")
    add_custom_command(OUTPUT "${stamp}"
        BYPRODUCTS "${metadataOut}" "${anchorsOut}" "${stringsOut}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${outDir}"
        COMMAND ${tool} ${flags} localize "${source}" "${metadataOut}" ${catalogs}
        COMMAND ${tool} ${flags} anchors "${source}" "${anchorsOut}"
        COMMAND ${tool} ${flags} strings "${source}" "${stringsOut}"
        COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
        DEPENDS "${source}" ${catalogs} ${tool}
        COMMENT "Generating Facet module metadata for ${target}"
        VERBATIM)
    add_custom_target(${target}_facet_metadata DEPENDS "${stamp}")
    add_dependencies(${target} ${target}_facet_metadata)
    # moc reads the file while generating the plugin's metadata section, so
    # it must exist before AUTOMOC runs, and a change to it must re-run moc.
    set_property(TARGET ${target} APPEND PROPERTY AUTOGEN_TARGET_DEPENDS ${target}_facet_metadata "${metadataOut}")
    set_target_properties(${target} PROPERTIES
        FACET_METADATA_FILE "${metadataOut}"
        FACET_ANCHORS_FILE "${anchorsOut}"
        FACET_STRINGS_FILE "${stringsOut}")
    target_include_directories(${target} BEFORE PRIVATE "${outDir}")

    # CMAKE_INCLUDE_CURRENT_DIR puts the target's own directories ahead of
    # even a BEFORE include directory, so a stray copy there would win.
    get_target_property(targetSourceDir ${target} SOURCE_DIR)
    get_target_property(targetBinaryDir ${target} BINARY_DIR)
    foreach(dir IN ITEMS "${targetSourceDir}" "${targetBinaryDir}")
        if(EXISTS "${dir}/facet_metadata.json")
            message(FATAL_ERROR "${fn}: ${dir}/facet_metadata.json could be embedded instead of the generated "
                                "metadata. Remove it.")
        endif()
    endforeach()

    # A plugin class declared in a .cpp includes its moc output (<name>.moc),
    # which is not a declared build output, so without help ninja sees the
    # regenerated file only on the next build. The .cpp that names
    # facet_metadata.json therefore depends on the generated file. A class
    # declared in a header needs nothing: its moc output reaches the build
    # through mocs_compilation.cpp, which AUTOGEN declares.
    #
    # Relative paths resolve against the target's directory, which need not
    # be the caller's. A unity build compiles the file inside a unity source
    # that does not carry its OBJECT_DEPENDS, so it is kept out of the batch.
    # A generated source is never read, on any configure, since whether it
    # exists yet depends on whether the build has run.
    get_target_property(sources ${target} SOURCES)
    set(embedders "")
    foreach(sourceFile IN LISTS sources)
        if(sourceFile MATCHES "\\$<")
            continue()
        endif()
        if(NOT IS_ABSOLUTE "${sourceFile}")
            if(EXISTS "${targetSourceDir}/${sourceFile}")
                set(sourceFile "${targetSourceDir}/${sourceFile}")
            else()
                set(sourceFile "${targetBinaryDir}/${sourceFile}")
            endif()
        endif()
        get_property(generated SOURCE "${sourceFile}" TARGET_DIRECTORY ${target} PROPERTY GENERATED)
        if(generated OR NOT EXISTS "${sourceFile}" OR IS_DIRECTORY "${sourceFile}")
            continue()
        endif()
        set(isCpp FALSE)
        if(sourceFile MATCHES "\\.(cpp|cc|cxx|c\\+\\+)$")
            set(isCpp TRUE)
        endif()
        set(candidates "${sourceFile}")
        if(isCpp)
            # AUTOMOC also reads the header of the same name next to a .cpp
            # when it is not listed. A class declared there needs no
            # dependency (see above), only to count as the embedder.
            get_filename_component(stem "${sourceFile}" NAME_WLE)
            get_filename_component(sourceDir "${sourceFile}" DIRECTORY)
            foreach(ext IN ITEMS h hh hpp hxx)
                if(EXISTS "${sourceDir}/${stem}.${ext}")
                    list(APPEND candidates "${sourceDir}/${stem}.${ext}")
                endif()
            endforeach()
        endif()
        foreach(candidate IN LISTS candidates)
            file(STRINGS "${candidate}" mentions REGEX "\"facet_metadata\\.json\"")
            if(NOT mentions)
                continue()
            endif()
            list(APPEND embedders "${candidate}")
            # moc looks next to the file before the include path, so a copy
            # there would be embedded instead of the generated one.
            get_filename_component(candidateDir "${candidate}" DIRECTORY)
            if(EXISTS "${candidateDir}/facet_metadata.json")
                message(FATAL_ERROR "${fn}: ${candidateDir}/facet_metadata.json would be embedded instead of the "
                                    "generated metadata. Remove it.")
            endif()
            if(candidate STREQUAL sourceFile AND isCpp)
                get_property(owner SOURCE "${sourceFile}" TARGET_DIRECTORY ${target} PROPERTY PHOSPHOR_FACET_MODULE)
                if(owner AND NOT owner STREQUAL target)
                    message(FATAL_ERROR "${fn}: ${sourceFile} also builds the Facet module ${owner} in the same "
                                        "directory. Its dependency on the generated metadata is kept per directory, "
                                        "so each module would be rebuilt whenever the other's metadata changes. "
                                        "Give each module its own path to the file, for example a symbolic link "
                                        "made with file(CREATE_LINK ... SYMBOLIC).")
                endif()
                set_property(SOURCE "${sourceFile}" TARGET_DIRECTORY ${target} PROPERTY PHOSPHOR_FACET_MODULE ${target})
                set_property(SOURCE "${sourceFile}" TARGET_DIRECTORY ${target} APPEND PROPERTY OBJECT_DEPENDS "${metadataOut}")
                set_property(SOURCE "${sourceFile}" TARGET_DIRECTORY ${target} PROPERTY SKIP_UNITY_BUILD_INCLUSION ON)
            endif()
        endforeach()
    endforeach()
    if(NOT embedders)
        message(AUTHOR_WARNING "${fn}: none of the target's sources names \"facet_metadata.json\", so the plugin "
                               "does not embed the generated metadata. Add the sources before calling this "
                               "function. Generated sources are not read.")
    endif()
endfunction()
