# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later

# PhosphorChannel unit tests. Headless: sockets, pipes, memfds, no QPA. Qt
# Test only drives them; the library itself links no Qt.

find_package(Qt6 REQUIRED COMPONENTS Test)

# The shared test isolation, as in phosphor-geometry: the relative walk
# reaches the repo root in-tree and in a standalone configure, and the EXISTS
# guard covers an extracted subtree without the module.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/PhosphorTestIsolation.cmake")
    include(${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/PhosphorTestIsolation.cmake)
else()
    function(phosphor_apply_test_isolation _target)
    endfunction()
endif()

find_package(Threads REQUIRED)

function(phosphorchannel_add_test _name)
    add_executable(${_name} ${ARGN})
    set_target_properties(${_name} PROPERTIES AUTOMOC ON)
    target_compile_features(${_name} PRIVATE cxx_std_20)
    target_link_libraries(${_name}
        PRIVATE
            Qt6::Test
            Qt6::Core
            Threads::Threads
            PhosphorChannel::PhosphorChannel
    )
    add_test(NAME ${_name} COMMAND ${_name})
    phosphor_apply_test_isolation(${_name})
    # The timeout again, because the stand-in above sets none, and these
    # tests block on sockets and pipes.
    set_tests_properties(${_name} PROPERTIES LABELS "phosphorchannel" TIMEOUT 120)
endfunction()

phosphorchannel_add_test(phosphorchannel_test_ring test_ring.cpp)
phosphorchannel_add_test(phosphorchannel_test_handshake test_handshake.cpp)
phosphorchannel_add_test(phosphorchannel_test_stream test_stream.cpp)

# The stream test throws from a callback, which the library supports, so it
# needs exceptions, which the tree's KDECompilerSettings turn off. The tree's
# shared test PCH is built without them and would not fit, so this target
# compiles without it.
target_compile_options(phosphorchannel_test_stream PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fexceptions>)
set_target_properties(phosphorchannel_test_stream PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
