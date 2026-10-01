# NucleoOS Recovery - the ota_1 app of flash layout v2 (partitions.csv, docs/OTA.md).
# Built as a sub-project of the firmware (top-level CMakeLists.txt) and flashed to `recovery`.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/nv_fwup")
set(COMPONENTS main)   # only what main pulls in: keeps the image small
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nucleo-recovery VERSION 1.0.0)
