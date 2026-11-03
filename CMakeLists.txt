cmake_minimum_required(VERSION 3.16)

project(PolarReconstruction
        VERSION 0.2.0
        DESCRIPTION "Polar Reconstruction CLAP plugin"
        LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The recovered engine depends on the tested float operation order:
# no fast-math, no FMA contraction.
set(KNOWN_FLOAT_FLAGS
    -fno-fast-math -ffp-contract=off)

add_library(polar_reconstruction MODULE
    src/plugin.cpp
    src/params.cpp
    src/entry.cpp)

set_target_properties(polar_reconstruction PROPERTIES
    OUTPUT_NAME "PolarReconstruction"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

target_include_directories(polar_reconstruction PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/clap-sdk/include)

target_compile_options(polar_reconstruction PRIVATE
    -O2 -Wall -Wextra
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:${KNOWN_FLOAT_FLAGS}>)

if(APPLE)
    target_sources(polar_reconstruction PRIVATE src/gui/polar_gui.mm)
    target_link_libraries(polar_reconstruction PRIVATE "-framework Cocoa")
    set_target_properties(polar_reconstruction PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION clap
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Polar Reconstruction"
        MACOSX_BUNDLE_EXECUTABLE_NAME "PolarReconstruction"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.polar-reconstruction.polar"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_VERSION "0.2.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.2.0"
        MACOSX_BUNDLE_COPYRIGHT "Reconstructed DSP; see THIRD_PARTY_NOTICES.md")
else()
    set_target_properties(polar_reconstruction PROPERTIES SUFFIX ".clap")
endif()

# --------------------------------------------------------------- tests
# The Linux build additionally produces a validation harness that loads the
# plugin through the plain CLAP C ABI and cross-checks it against a direct
# instance of the engine. Not built on macOS (where the plugin links Cocoa).
if(NOT APPLE AND BUILD_TESTING)
    enable_testing()

    add_executable(polar_clap_test tests/host_smoke.cpp src/params.cpp)
    target_include_directories(polar_clap_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
        ${CMAKE_CURRENT_SOURCE_DIR}/clap-sdk/include)
    target_compile_options(polar_clap_test PRIVATE -O2 -Wall -Wextra -Wno-format-truncation ${KNOWN_FLOAT_FLAGS})
    target_link_libraries(polar_clap_test PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME host_smoke COMMAND polar_clap_test
             $<TARGET_FILE:polar_reconstruction>)

    add_executable(polar_whitebox_test tests/whitebox.cpp src/plugin.cpp src/params.cpp src/entry.cpp)
    target_include_directories(polar_whitebox_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/engine
        ${CMAKE_CURRENT_SOURCE_DIR}/clap-sdk/include)
    target_compile_options(polar_whitebox_test PRIVATE -O2 -Wall -Wextra ${KNOWN_FLOAT_FLAGS})
    add_test(NAME whitebox COMMAND polar_whitebox_test)
endif()
