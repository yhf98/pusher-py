cmake_minimum_required(VERSION 3.16)

project(pusher VERSION 0.1.9 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PUSHER_BUILD_SHARED "Build pusher as a shared library" ON)
option(PUSHER_BUILD_EXAMPLES "Build C and C++ examples" ON)
set(PUSHER_WHIP "AUTO" CACHE STRING "WHIP support: AUTO, ON, or OFF")
set_property(CACHE PUSHER_WHIP PROPERTY STRINGS AUTO ON OFF)

set(PUSHER_SDK_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include" CACHE PATH "pusher and FFmpeg include directory")
set(PUSHER_SDK_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib" CACHE PATH "FFmpeg library directory")

set(PUSHER_LIBRARY_TYPE STATIC)
if(PUSHER_BUILD_SHARED)
    set(PUSHER_LIBRARY_TYPE SHARED)
endif()

add_library(pusher ${PUSHER_LIBRARY_TYPE}
    src/pusher/camera_source.cpp
    src/pusher/pusher.cpp
    src/pusher/pusher_c.cpp
    src/pusher/stream_push/whip.cpp
    src/pusher/url_utils.c
)

target_compile_features(pusher PUBLIC cxx_std_17)
target_include_directories(pusher
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PUSHER_SDK_INCLUDE_DIR}>"
        "$<INSTALL_INTERFACE:include>"
)
target_compile_definitions(pusher PRIVATE PUSHER_BUILDING_LIBRARY)
if(PUSHER_BUILD_SHARED)
    target_compile_definitions(pusher PUBLIC PUSHER_SHARED)
endif()

find_package(Threads REQUIRED)

foreach(component IN ITEMS avformat avcodec avutil avdevice avfilter swscale swresample)
    string(TOUPPER "${component}" component_upper)
    find_library(PUSHER_FFMPEG_${component_upper}_LIBRARY
        NAMES "${component}" "lib${component}"
        PATHS "${PUSHER_SDK_LIB_DIR}"
        NO_DEFAULT_PATH
    )
    if(NOT PUSHER_FFMPEG_${component_upper}_LIBRARY)
        message(FATAL_ERROR "Missing FFmpeg ${component} library in ${PUSHER_SDK_LIB_DIR}. Run scripts/build_ffmpeg.sh first or pass -DPUSHER_SDK_LIB_DIR=/path/to/lib.")
    endif()
    target_link_libraries(pusher PRIVATE "${PUSHER_FFMPEG_${component_upper}_LIBRARY}")
endforeach()

target_link_libraries(pusher PRIVATE Threads::Threads)

if(NOT WIN32)
    target_link_options(pusher PRIVATE
        "-Wl,--disable-new-dtags"
    )
endif()

if(PUSHER_WHIP STREQUAL "ON" OR PUSHER_WHIP STREQUAL "AUTO")
    find_package(CURL QUIET)
    find_path(PUSHER_RTC_INCLUDE_DIR rtc/rtc.hpp PATHS /usr/local/include /usr/include)
    find_library(PUSHER_DATACHANNEL_LIBRARY NAMES datachannel PATHS /usr/local/lib /usr/lib /usr/lib64)

    if(CURL_FOUND AND PUSHER_RTC_INCLUDE_DIR AND PUSHER_DATACHANNEL_LIBRARY)
        target_compile_definitions(pusher PRIVATE PUSHER_ENABLE_WHIP=1)
        target_include_directories(pusher PRIVATE "${PUSHER_RTC_INCLUDE_DIR}")
        target_link_libraries(pusher PRIVATE CURL::libcurl "${PUSHER_DATACHANNEL_LIBRARY}")
        if(NOT WIN32)
            get_filename_component(PUSHER_DATACHANNEL_LIB_DIR "${PUSHER_DATACHANNEL_LIBRARY}" DIRECTORY)
        endif()
        message(STATUS "Embedded WHIP/WebRTC support: enabled")
    elseif(PUSHER_WHIP STREQUAL "ON")
        message(FATAL_ERROR "PUSHER_WHIP=ON requires libcurl and libdatachannel headers/libraries.")
    else()
        message(STATUS "Embedded WHIP/WebRTC support: disabled; libcurl or libdatachannel was not found")
    endif()
endif()

if(MSVC)
    target_compile_options(pusher PRIVATE /O2 /EHsc)
else()
    target_compile_options(pusher PRIVATE -O3 -Wall -Wextra)
endif()

set_target_properties(pusher PROPERTIES
    OUTPUT_NAME pusher
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)

if(NOT WIN32)
    set(PUSHER_BUILD_RPATH "${PUSHER_SDK_LIB_DIR};$ORIGIN")
    if(PUSHER_DATACHANNEL_LIB_DIR)
        list(APPEND PUSHER_BUILD_RPATH "${PUSHER_DATACHANNEL_LIB_DIR}")
    endif()
    set_target_properties(pusher PROPERTIES
        BUILD_RPATH "${PUSHER_BUILD_RPATH}"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

if(PUSHER_BUILD_EXAMPLES)
    add_executable(pusher_c_demo examples/c_demo.c)
    target_link_libraries(pusher_c_demo PRIVATE pusher)

    add_executable(pusher_cpp_demo examples/cpp_demo.cpp)
    target_link_libraries(pusher_cpp_demo PRIVATE pusher)

    if(NOT WIN32)
        set_target_properties(pusher_c_demo pusher_cpp_demo PROPERTIES
            BUILD_RPATH "${PUSHER_SDK_LIB_DIR};$ORIGIN"
        )
    endif()
endif()

install(TARGETS pusher
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(DIRECTORY include/pusher DESTINATION include)
