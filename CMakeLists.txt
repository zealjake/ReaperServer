cmake_minimum_required(VERSION 3.20)

project(ZealReaperServer VERSION 0.1 LANGUAGES CXX)

# Path to your REAPER SDK folder (where sdk/ and WDL/ are)
set(REAPER_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/reaper_sdk")

add_library(ZealReaperServer SHARED
    main.cpp
)

target_include_directories(ZealReaperServer PRIVATE
    "${REAPER_SDK_DIR}/sdk"
    "${REAPER_SDK_DIR}/WDL"
)

target_compile_features(ZealReaperServer PRIVATE cxx_std_17)

if(APPLE)
    set_target_properties(ZealReaperServer PROPERTIES
        PREFIX ""
        OUTPUT_NAME "ZealReaperServer"
        SUFFIX ".dylib"
    )
endif()
