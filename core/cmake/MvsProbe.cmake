# ---------------------------------------------------------------------------
# Does the MVS SDK found by core/CMakeLists.txt fit this code?
#
#   hik_mvs_probe(<out>)   sets <out> to "" if it does, else to the first error
#
# Header and library being there is not enough: an SDK of another version may
# lack functions the code uses, or the library may be built for another
# processor. Two try_compile probes find out at configure time:
#   1. compile the four sources that use the SDK (exactly what the build will
#      compile, so the probe cannot go stale) as a static library,
#   2. link a tiny program against libMvCameraControl.
# The result is cached under a signature of SDK, sources and compiler, so an
# unchanged tree is not probed again (each probe takes a few seconds).
#
# Expects MVS_ROOT, MVS_INCLUDE_DIR, MVS_LIB_DIR and MVS_CAMERA_CONTROL_LIB.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

get_filename_component(_HIK_MVS_PROBE_CORE "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_HIK_MVS_PROBE_FILE "${CMAKE_CURRENT_LIST_FILE}")

function(hik_mvs_probe out)
    set(core "${_HIK_MVS_PROBE_CORE}")
    set(sources src/MvsCamera.cpp src/MvsError.cpp src/CaptureApp.cpp src/HikCamera.cpp)
    set(headers include/MvsCamera.h include/MvsError.h include/CaptureApp.h include/HikCamera.h
                include/DeviceSummary.h)

    # Signature: which SDK, which code, which compiler - and this check itself.
    file(TIMESTAMP "${MVS_INCLUDE_DIR}/MvCameraControl.h" header_time "%s")
    file(TIMESTAMP "${MVS_CAMERA_CONTROL_LIB}" lib_time "%s")
    set(sig "${MVS_ROOT}|${MVS_CAMERA_CONTROL_LIB}|${header_time}|${lib_time}")
    string(APPEND sig "|${CMAKE_CXX_COMPILER}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_CXX_FLAGS}")
    foreach(f IN LISTS sources headers)
        file(SHA1 "${core}/${f}" hash)
        string(APPEND sig "|${hash}")
    endforeach()
    file(SHA1 "${_HIK_MVS_PROBE_FILE}" hash)
    string(APPEND sig "|${hash}")
    string(SHA1 sig "${sig}")
    if(DEFINED HIK_MVS_PROBE_SIGNATURE AND HIK_MVS_PROBE_SIGNATURE STREQUAL sig)
        set(${out} "${HIK_MVS_PROBE_ERROR}" PARENT_SCOPE)
        return()
    endif()

    message(STATUS "Checking that the MVS SDK in ${MVS_ROOT} fits this code")
    set(bin "${CMAKE_CURRENT_BINARY_DIR}/mvs_probe")
    set(error "")

    # 1. Compile the SDK users.
    list(TRANSFORM sources PREPEND "${core}/")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    try_compile(_hik_mvs_compiles "${bin}/compile"
        SOURCES ${sources}
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${core}/include;${MVS_INCLUDE_DIR}"
        COMPILE_DEFINITIONS -DHIK_HAVE_MVS=1
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        OUTPUT_VARIABLE log)
    set(ok ${_hik_mvs_compiles})
    unset(_hik_mvs_compiles CACHE)

    # 2. Link against the library.
    if(ok)
        file(WRITE "${bin}/link_probe.cpp"
            "#include \"MvCameraControl.h\"\n"
            "int main()\n"
            "{\n"
            "    const int rc = MV_CC_Initialize();\n"
            "    const unsigned int version = MV_CC_GetSDKVersion();\n"
            "    MV_CC_Finalize();\n"
            "    return (rc == MV_OK && version != 0) ? 0 : 1;\n"
            "}\n")
        set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
        try_compile(_hik_mvs_links "${bin}/link"
            SOURCES "${bin}/link_probe.cpp"
            CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MVS_INCLUDE_DIR}"
            LINK_LIBRARIES "${MVS_CAMERA_CONTROL_LIB}"
            LINK_OPTIONS "LINKER:-rpath,${MVS_LIB_DIR}"
            CXX_STANDARD 17
            OUTPUT_VARIABLE log)
        set(ok ${_hik_mvs_links})
        unset(_hik_mvs_links CACHE)
    endif()

    if(NOT ok)
        # The first line that names the problem: a compiler diagnostic (some
        # locales translate "error:"), or a linker complaint.
        string(REPLACE "\n" ";" lines "${log}")
        foreach(line IN LISTS lines)
            if(line MATCHES "[Ee]rror:|Fehler:|undefined reference|cannot find|incompatible|file format not recognized")
                string(STRIP "${line}" error)
                break()
            endif()
        endforeach()
        if(NOT error)
            set(error "the probe build failed (see CMakeFiles/CMakeConfigureLog.yaml)")
        endif()
    endif()

    set(HIK_MVS_PROBE_SIGNATURE "${sig}" CACHE INTERNAL "Inputs of the last MVS SDK check")
    set(HIK_MVS_PROBE_ERROR "${error}" CACHE INTERNAL "Result of the last MVS SDK check")
    set(${out} "${error}" PARENT_SCOPE)
endfunction()
