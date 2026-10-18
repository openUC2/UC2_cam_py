# ---------------------------------------------------------------------------
# Analysis tooling: sanitizers and profiling-friendly code generation.
#
# Included from the top-level CMakeLists.txt (before add_subdirectory(core))
# and from core/CMakeLists.txt, so both configuration paths get the same
# switches. include_guard keeps the flags from being added twice.
#
#   HIK_SANITIZERS  ""                 plain build (default)
#                   "address;undefined" ASan + UBSan (LSan is part of ASan)
#                   "thread"           TSan
#   HIK_PROFILING   ON                 frame pointers for perf / hotspot
#
# The flags are applied directory-wide (add_compile_options/add_link_options)
# so the core, the examples, the tests and Catch2 are all instrumented: TSan in
# particular only works reliably when the whole program is. Presets for the
# common combinations live in CMakePresets.json; see Docs/Tooling.md.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

set(HIK_SANITIZERS "" CACHE STRING
    "Sanitizers to build with: empty, 'address;undefined' or 'thread'")
option(HIK_PROFILING "Keep frame pointers for perf/hotspot call graphs" OFF)

if(HIK_SANITIZERS)
    if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        message(FATAL_ERROR "HIK_SANITIZERS needs GCC or Clang (got ${CMAKE_CXX_COMPILER_ID})")
    endif()

    set(_hik_allowed address undefined leak thread)
    foreach(_san IN LISTS HIK_SANITIZERS)
        if(NOT _san IN_LIST _hik_allowed)
            message(FATAL_ERROR "HIK_SANITIZERS: unknown sanitizer '${_san}' "
                                "(allowed: ${_hik_allowed})")
        endif()
    endforeach()
    if("thread" IN_LIST HIK_SANITIZERS AND
       ("address" IN_LIST HIK_SANITIZERS OR "leak" IN_LIST HIK_SANITIZERS))
        message(FATAL_ERROR "HIK_SANITIZERS: 'thread' cannot be combined with "
                            "'address' or 'leak' - build them in separate trees")
    endif()

    list(JOIN HIK_SANITIZERS "," _hik_san_list)
    message(STATUS "HIK: building with -fsanitize=${_hik_san_list}")

    # -O1 keeps sanitized code fast enough for the MT tests while still giving
    # accurate stack traces; frame pointers make those traces cheap to unwind.
    add_compile_options(-fsanitize=${_hik_san_list} -O1 -g
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${_hik_san_list})
endif()

if(HIK_PROFILING)
    message(STATUS "HIK: profiling build (frame pointers kept)")
    # With frame pointers perf can walk the stack itself (--call-graph fp),
    # which is far cheaper than recording DWARF stack copies. Both flags are
    # accepted by GCC and Clang on x86-64 and aarch64 alike.
    add_compile_options(-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer)
endif()
