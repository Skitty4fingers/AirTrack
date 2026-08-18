cmake_minimum_required(VERSION 3.16)

# AirTrack is board-specific; selecting the target here keeps a fresh build from
# silently producing an image for another ESP32 family.
set(IDF_TARGET "esp32c6" CACHE STRING "ESP-IDF target")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(_airtrack_idf_version
    "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
if(_airtrack_idf_version VERSION_LESS "5.5.0"
   OR NOT _airtrack_idf_version VERSION_LESS "5.6.0")
    message(FATAL_ERROR
        "AirTrack requires ESP-IDF >=5.5.0 and <5.6.0; found ${_airtrack_idf_version}")
endif()

project(airtrack VERSION 1.6.2)
