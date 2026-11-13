cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" OFF)

# Third-party headers (cpp-httplib) are not warning-clean on every compiler.
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

# The OBS 30.1.2 dependency step installs only the Debug libobs/frontend import
# libraries. Import libraries just point at obs.dll / obs-frontend-api.dll, so
# release builds can safely link against them.
foreach(_cfg RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_MAP_IMPORTED_CONFIG_${_cfg} "${_cfg};Release;RelWithDebInfo;Debug")
endforeach()

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE RPG_HAVE_FRONTEND=1)
endif()

# FFmpeg ships with OBS (obs-deps); the plugin decodes replay files with it.
find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h REQUIRED)
foreach(_lib avformat avcodec avutil avfilter)
  find_library(FFMPEG_${_lib}_LIBRARY NAMES ${_lib} REQUIRED)
  list(APPEND FFMPEG_LIBRARIES ${FFMPEG_${_lib}_LIBRARY})
endforeach()
message(STATUS "RPG Replay Engine: FFmpeg headers in ${FFMPEG_INCLUDE_DIR}")
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIR})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES})

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/deps")

if(WIN32)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 shell32)
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/engine.cpp
    src/recorder.cpp
    src/player.cpp
    src/api-server.cpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
