# NucleoOS Recovery - the ota_1 app of flash layout v2 (partitions.csv, docs/OTA.md).
# Built as a sub-project of the firmware (top-level CMakeLists.txt) and flashed to `recovery`.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/nv_fwup")
set(COMPONENTS main)   # only what main pulls in: keeps the image small
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nucleo-recovery VERSION 1.0.0)

# sdkconfig drift guard: IDF applies sdkconfig.defaults* only to options an existing sdkconfig lacks,
# so a stale hand-edited sdkconfig silently overrides the committed defaults (it once built with NVS
# encryption on, which burns eFuse KEY5 on first boot). Stop the build instead; see the script.
idf_build_get_property(_nv_sdkconfig SDKCONFIG)
idf_build_get_property(_nv_defaults SDKCONFIG_DEFAULTS)
idf_build_get_property(_nv_python PYTHON)
if(NOT _nv_defaults)
    set(_nv_defaults "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")
endif()
set(_nv_all_defaults "")
foreach(_d ${_nv_defaults})
    get_filename_component(_d "${_d}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND _nv_all_defaults "${_d}")
    if(EXISTS "${_d}.${IDF_TARGET}")
        list(APPEND _nv_all_defaults "${_d}.${IDF_TARGET}")
    endif()
endforeach()
execute_process(COMMAND ${_nv_python} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_sdkconfig.py" "${_nv_sdkconfig}" ${_nv_all_defaults}
                RESULT_VARIABLE _nv_cfg_rc)
if(NOT _nv_cfg_rc EQUAL 0)
    message(FATAL_ERROR "sdkconfig contradicts sdkconfig.defaults* (details above)")
endif()
