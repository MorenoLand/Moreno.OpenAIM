cmake_minimum_required(VERSION 3.24)
project(OpenAIM VERSION 4.7.2480 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(AIM_QT_PREFIX "" CACHE PATH "Path to a static Qt 6.8 installation")
if(NOT AIM_QT_PREFIX AND DEFINED ENV{AIM_QT_PREFIX})
  set(AIM_QT_PREFIX "$ENV{AIM_QT_PREFIX}" CACHE PATH "Path to a static Qt 6.8 installation" FORCE)
endif()
if(AIM_QT_PREFIX)
  list(PREPEND CMAKE_PREFIX_PATH "${AIM_QT_PREFIX}")
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network)
foreach(_module Core Gui Network)
  get_target_property(_qt_type Qt6::${_module} TYPE)
  if(NOT _qt_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "A statically built Qt 6.8 Core/Gui/Network kit is required")
  endif()
endforeach()
set(AIM_SOURCES
  src/main.cpp
  src/oscar/protocol.cpp
  src/oscar/protocol.h
  src/oscar/client.cpp
  src/oscar/client.h
  src/ui/window_base.cpp
  src/ui/window_base.h
  src/ui/connection_window.cpp
  src/ui/connection_window.h
  src/ui/preferences_window.cpp
  src/ui/preferences_window.h
  src/ui/native_preferences.cpp
  src/ui/native_preferences.h
  src/ui/system_tray.cpp
  src/ui/system_tray.h
  src/ui/sign_on_window.cpp
  src/ui/sign_on_window.h
  src/ui/buddy_list_window.cpp
  src/ui/buddy_list_window.h
  src/ui/messaging_window.cpp
  src/ui/messaging_window.h
  src/ui/menu_template.cpp
  src/ui/menu_template.h
  src/ui/menu_bar.cpp
  src/ui/menu_bar.h
  src/ui/sounds.cpp
  src/ui/sounds.h
  src/ui/ctl_group.cpp
  src/ui/ctl_group.h
  src/ui/native_dialog.cpp
  src/ui/native_dialog.h
  src/ui/away_dialog.cpp
  src/ui/away_dialog.h
  resources/aim.qrc
  resources/dialogs.qrc
  resources/ui.qrc
)
if(WIN32)
  qt_add_executable(openaim WIN32 ${AIM_SOURCES})
else()
  qt_add_executable(openaim ${AIM_SOURCES})
endif()
target_link_libraries(openaim PRIVATE Qt6::Gui Qt6::Network)
if(WIN32)
  target_link_libraries(openaim PRIVATE comctl32 comdlg32 shell32 ole32 crypt32 dwmapi winmm)
  if(MSVC)
    target_link_options(openaim PRIVATE /MANIFEST:NO)
  endif()
  configure_file(resources/openaim.rc.in "${CMAKE_CURRENT_BINARY_DIR}/openaim.rc" @ONLY)
  target_sources(openaim PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/openaim.rc")
endif()
qt_import_plugins(openaim INCLUDE Qt6::QGifPlugin Qt6::QICOPlugin)
foreach(_plugin QGifPlugin QICOPlugin)
  if(NOT TARGET Qt6::${_plugin})
    message(FATAL_ERROR "Static Qt image plugin Qt6::${_plugin} is required")
  endif()
  get_target_property(_plugin_type Qt6::${_plugin} TYPE)
  if(NOT _plugin_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "Qt image plugin Qt6::${_plugin} must be static")
  endif()
endforeach()
set_target_properties(openaim PROPERTIES OUTPUT_NAME OpenAIM RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
foreach(_config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${_config}" _config_upper)
  set_target_properties(openaim PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${_config_upper} "${PROJECT_SOURCE_DIR}/bin")
endforeach()
if(MSVC)
  target_link_options(openaim PRIVATE "/MAP:${PROJECT_BINARY_DIR}/OpenAIM.map")
  target_compile_options(openaim PRIVATE "$<$<CONFIG:Release>:/O1>" "$<$<CONFIG:Release>:/Gy>")
  target_link_options(openaim PRIVATE "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
elseif(APPLE)
  target_compile_options(openaim PRIVATE "$<$<CONFIG:Release>:-Os>" "$<$<CONFIG:Release>:-ffunction-sections>" "$<$<CONFIG:Release>:-fdata-sections>")
  target_link_options(openaim PRIVATE "$<$<CONFIG:Release>:-Wl,-dead_strip>")
else()
  target_compile_options(openaim PRIVATE "$<$<CONFIG:Release>:-Os>" "$<$<CONFIG:Release>:-ffunction-sections>" "$<$<CONFIG:Release>:-fdata-sections>")
  target_link_options(openaim PRIVATE "$<$<CONFIG:Release>:-Wl,--gc-sections>")
endif()
option(AIM_ENABLE_UPX "Compress Release executables with UPX" ON)
if(AIM_ENABLE_UPX)
  find_program(AIM_UPX_EXECUTABLE NAMES upx)
  if(AIM_UPX_EXECUTABLE)
    add_custom_command(TARGET openaim POST_BUILD
      COMMAND "${CMAKE_COMMAND}" "-DAIM_UPX_EXECUTABLE=${AIM_UPX_EXECUTABLE}" "-DAIM_OUTPUT_FILE=$<TARGET_FILE:openaim>" "-DAIM_BUILD_CONFIG=$<CONFIG>" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/upx_release.cmake"
      COMMENT "Packing OpenAIM Release executable with UPX"
      VERBATIM
    )
  else()
    message(WARNING "UPX was not found; OpenAIM will be built without executable packing")
  endif()
endif()

option(AIM_BUILD_SELFTEST "Build the OSCAR self-test" OFF)
if(AIM_BUILD_SELFTEST)
  add_executable(openaim_selftest tests/selftest.cpp src/oscar/protocol.cpp src/oscar/protocol.h src/oscar/client.cpp src/oscar/client.h)
  target_include_directories(openaim_selftest PRIVATE src/oscar)
  target_link_libraries(openaim_selftest PRIVATE Qt6::Core Qt6::Network)
  if(MSVC)
    set_target_properties(openaim_selftest PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_executable(openaim_layouttest tests/layouttest.cpp src/ui/ctl_group.cpp src/ui/ctl_group.h resources/ui.qrc)
  target_link_libraries(openaim_layouttest PRIVATE Qt6::Gui)
  if(MSVC)
    set_target_properties(openaim_layouttest PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  set_target_properties(openaim_layouttest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build-selftest/bin")
  foreach(_config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_config}" _config_upper)
    set_target_properties(openaim_layouttest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${_config_upper} "${PROJECT_SOURCE_DIR}/build-selftest/bin")
  endforeach()
  set_target_properties(openaim_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build-selftest/bin")
  foreach(_config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_config}" _config_upper)
    set_target_properties(openaim_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${_config_upper} "${PROJECT_SOURCE_DIR}/build-selftest/bin")
  endforeach()
endif()
