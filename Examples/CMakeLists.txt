# Example programs built against the core library:
#   HikStandAlone   - terminal capture tool (needs a connected camera)
#   RingBuffExample - ring-buffer walkthrough with simulated frames (no camera)

function(hik_add_example name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE hikcam_core)

    if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${name} PRIVATE -Wall -Wextra)
    endif()

    # hikcam_core pulls in libMvCameraControl; find it at runtime without
    # MVCAM_COMMON_RUNENV / LD_LIBRARY_PATH having to be exported.
    set_target_properties(${name} PROPERTIES
        BUILD_RPATH   "${HIKCAMTEST_MVS_LIB_DIR}"
        INSTALL_RPATH "${HIKCAMTEST_MVS_LIB_DIR}")
endfunction()

hik_add_example(HikStandAlone   HikStandalone.cpp)
hik_add_example(RingBuffExample RingBuffExample.cpp)

install(TARGETS HikStandAlone RUNTIME DESTINATION bin)
