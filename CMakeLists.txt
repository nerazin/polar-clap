cmake_minimum_required(VERSION 3.16)

project(PolarReconstruction
        VERSION 0.3.0
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
        MACOSX_BUNDLE_BUNDLE_VERSION "0.3.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.3.0"
        MACOSX_BUNDLE_COPYRIGHT "Reconstructed DSP; see THIRD_PARTY_NOTICES.md")
else()
    set_target_properties(polar_reconstruction PROPERTIES SUFFIX ".clap")
endif()

# --------------------------------------------------------------- tests
# polar_clap_test loads the built plugin through the plain CLAP C ABI and cross-checks it
# against a direct engine instance; polar_whitebox_test drives the C++ layer; polar_golden_test
# pins the original binary's math and tables. Build with -DBUILD_TESTING=ON (macOS or Linux).
if(BUILD_TESTING)
    enable_testing()
    set(TEST_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/engine
                      ${CMAKE_CURRENT_SOURCE_DIR}/clap-sdk/include)

    add_executable(polar_clap_test tests/host_smoke.cpp src/params.cpp)
    target_include_directories(polar_clap_test PRIVATE ${TEST_INCLUDES})
    target_compile_options(polar_clap_test PRIVATE -O2 -Wall -Wextra -Wno-format-truncation ${KNOWN_FLOAT_FLAGS})
    target_link_libraries(polar_clap_test PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME host_smoke COMMAND polar_clap_test $<TARGET_FILE:polar_reconstruction>)

    add_executable(polar_whitebox_test tests/whitebox.cpp src/plugin.cpp src/params.cpp src/entry.cpp)
    target_include_directories(polar_whitebox_test PRIVATE ${TEST_INCLUDES})
    target_compile_options(polar_whitebox_test PRIVATE -O2 -Wall -Wextra ${KNOWN_FLOAT_FLAGS})
    if(APPLE)
        target_sources(polar_whitebox_test PRIVATE src/gui/polar_gui.mm)
        target_link_libraries(polar_whitebox_test PRIVATE "-framework Cocoa")
    endif()
    add_test(NAME whitebox COMMAND polar_whitebox_test)

    add_executable(polar_golden_test tests/golden.cpp)
    target_include_directories(polar_golden_test PRIVATE ${TEST_INCLUDES})
    target_compile_options(polar_golden_test PRIVATE -O2 -Wall -Wextra ${KNOWN_FLOAT_FLAGS})
    add_test(NAME golden COMMAND polar_golden_test)

    if(APPLE)  # renders the GUI to a PNG for review: polar_gui_snapshot out.png
        add_executable(polar_gui_snapshot tests/gui_snapshot.mm src/plugin.cpp src/params.cpp src/entry.cpp src/gui/polar_gui.mm)
        target_include_directories(polar_gui_snapshot PRIVATE ${TEST_INCLUDES})
        target_compile_options(polar_gui_snapshot PRIVATE -O2 ${KNOWN_FLOAT_FLAGS})
        target_link_libraries(polar_gui_snapshot PRIVATE "-framework Cocoa")
    endif()
endif()
