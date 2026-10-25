cmake_minimum_required(VERSION 3.20)

project(FreeFirstPersonCameraBML VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# BML 0.3.40 is the legacy, non-Plus loader.  It ships its own Virtools
# headers under include/BML/virtools and only provides BML.lib.
set(BML_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/BML-dev-0340"
        CACHE PATH "BML 0.3.40 SDK root")
set(VT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/Virtools-SDK-2.1"
        CACHE PATH "Virtools 2.1 SDK root")

if (NOT EXISTS "${BML_SDK_DIR}/include/BML/IMod.h")
    message(FATAL_ERROR "BML 0.3.40 SDK not found at ${BML_SDK_DIR}")
endif ()
if (NOT EXISTS "${BML_SDK_DIR}/lib/Release/BML.lib")
    message(FATAL_ERROR "BML 0.3.40 import library not found at ${BML_SDK_DIR}/lib/Release/BML.lib")
endif ()
if (NOT EXISTS "${VT_SDK_DIR}/Include/CKAll.h")
    message(FATAL_ERROR "Virtools SDK not found at ${VT_SDK_DIR}")
endif ()

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
            "Ballance and BML are 32-bit. Configure with -A Win32 (Visual Studio generator).")
endif ()

add_library(FreeFirstPersonCameraBML SHARED
        ../src/bml-old/Mod.cpp
)

target_include_directories(FreeFirstPersonCameraBML PRIVATE
        "${BML_SDK_DIR}/include"
        "${VT_SDK_DIR}/Include"
)

target_link_libraries(FreeFirstPersonCameraBML PRIVATE
        "${BML_SDK_DIR}/lib/Release/BML.lib"
        "${VT_SDK_DIR}/Lib/CK2.lib"
        "${VT_SDK_DIR}/Lib/VxMath.lib"
)

target_compile_definitions(FreeFirstPersonCameraBML PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        FFPC_BML_LEGACY=1
        FFPC_VERSION="${PROJECT_VERSION}"
)

target_compile_options(FreeFirstPersonCameraBML PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
        $<$<CXX_COMPILER_ID:MSVC>:/W3>
        $<$<CXX_COMPILER_ID:MSVC>:/Zc:strictStrings->
        $<$<CXX_COMPILER_ID:MSVC>:/Zc:char8_t->
)

set_target_properties(FreeFirstPersonCameraBML PROPERTIES
        OUTPUT_NAME "FreeFirstPersonCamera"
        PREFIX ""
        SUFFIX ".bmod"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
