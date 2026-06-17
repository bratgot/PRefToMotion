cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)              # MSVC runtime via MSVC_RUNTIME_LIBRARY

project(PRefToMotion LANGUAGES CXX)

# ---- Nuke, from the rez-resolved 'nuke' package ----
# The nuke package is expected to export NUKE_INSTALL_PATH (its Nuke root).
# For a non-rez build, pass -DCMAKE_PREFIX_PATH=<Nuke root> instead.
if (NOT NUKE_INSTALL_PATH AND DEFINED ENV{NUKE_INSTALL_PATH})
    set(NUKE_INSTALL_PATH "$ENV{NUKE_INSTALL_PATH}")
endif()

find_package(Nuke QUIET)
if (NOT TARGET Nuke::NDK)
    if (NOT NUKE_INSTALL_PATH)
        message(FATAL_ERROR "Nuke not found. The 'nuke' rez package should export "
                "NUKE_INSTALL_PATH, or pass -DCMAKE_PREFIX_PATH=<Nuke root>.")
    endif()
    add_library(Nuke::NDK SHARED IMPORTED)
    if (WIN32)
        set_target_properties(Nuke::NDK PROPERTIES
            IMPORTED_IMPLIB              "${NUKE_INSTALL_PATH}/DDImage.lib"
            IMPORTED_LOCATION            "${NUKE_INSTALL_PATH}/DDImage.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${NUKE_INSTALL_PATH}/include")
    elseif (APPLE)
        set_target_properties(Nuke::NDK PROPERTIES
            IMPORTED_LOCATION            "${NUKE_INSTALL_PATH}/libDDImage.dylib"
            INTERFACE_INCLUDE_DIRECTORIES "${NUKE_INSTALL_PATH}/include")
    else()
        set_target_properties(Nuke::NDK PROPERTIES
            IMPORTED_LOCATION            "${NUKE_INSTALL_PATH}/libDDImage.so"
            INTERFACE_INCLUDE_DIRECTORIES "${NUKE_INSTALL_PATH}/include")
    endif()
endif()

add_library(PRefToMotion MODULE "src/PRefToMotion.cpp")
target_link_libraries(PRefToMotion PRIVATE Nuke::NDK)

# nanoflann is header-only, vendored in this repo
target_include_directories(PRefToMotion PRIVATE "${CMAKE_SOURCE_DIR}/third_party")
set_target_properties(PRefToMotion PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME           "PRefToMotion"     # must match the Op::Description name
    PREFIX                "")

if (APPLE)
    set_target_properties(PRefToMotion PROPERTIES SUFFIX ".dylib")
endif()

if (MSVC)
    set_target_properties(PRefToMotion PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    target_compile_definitions(PRefToMotion PRIVATE
        NOMINMAX _USE_MATH_DEFINES WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(PRefToMotion PRIVATE /W3 /EHsc /wd4251)
endif()

# rez sets CMAKE_INSTALL_PREFIX to the per-variant payload; land the .dll in nuke/
install(TARGETS PRefToMotion LIBRARY DESTINATION nuke RUNTIME DESTINATION nuke)
