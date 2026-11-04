cmake_minimum_required(VERSION 3.28...3.30)

if(MSVC)
  set_source_files_properties(
    pixels.cpp PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:/O2>;$<$<CONFIG:Debug>:/Ob2>"
                          VS_SETTINGS "BasicRuntimeChecks=Default"
  )
endif()

add_library(
  dvc-filter MODULE
  filter.cpp
  formats.cpp
  pixels.cpp
  scheduler.cpp
  module.cpp
  ipc.cpp
  registration.cpp
  filter.def
)
target_compile_features(dvc-filter PRIVATE cxx_std_17)
target_compile_definitions(dvc-filter PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(dvc-filter PRIVATE ole32 oleaut32 strmiids uuid advapi32)
if(CMAKE_VS_PLATFORM_NAME STREQUAL Win32)
  set(dvc_bits 32)
else()
  set(dvc_bits 64)
endif()
set_target_properties(
  dvc-filter
  PROPERTIES FOLDER plugins/dynamic-virtualcam
             OUTPUT_NAME dvc-filter${dvc_bits}
             MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

add_executable(
  dvc-device-check EXCLUDE_FROM_ALL
  "${CMAKE_SOURCE_DIR}/test/test_dynamic_virtualcam_device.cpp"
  ipc.cpp
  registration.cpp
)
target_include_directories(dvc-device-check PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dvc-device-check PRIVATE cxx_std_17)
target_compile_definitions(dvc-device-check PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(dvc-device-check PRIVATE ole32 oleaut32 strmiids uuid advapi32 shell32)
set_target_properties(dvc-device-check PROPERTIES FOLDER tests)

add_executable(dvc-pixel-tests EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/test/test_dynamic_virtualcam_pixels.cpp" pixels.cpp)
target_include_directories(dvc-pixel-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dvc-pixel-tests PRIVATE cxx_std_17)
target_compile_definitions(dvc-pixel-tests PRIVATE UNICODE _UNICODE NOMINMAX)
set_target_properties(dvc-pixel-tests PROPERTIES FOLDER tests)

add_executable(dvc-buffer-tests EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/test/test_dynamic_virtualcam_buffer.cpp" ipc.cpp)
target_include_directories(dvc-buffer-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dvc-buffer-tests PRIVATE cxx_std_17)
target_compile_definitions(dvc-buffer-tests PRIVATE UNICODE _UNICODE NOMINMAX DVC_BUFFER_TEST_MAIN)
target_link_libraries(dvc-buffer-tests PRIVATE ole32)
set_target_properties(dvc-buffer-tests PROPERTIES FOLDER tests)

if(NOT OBS_PARENT_ARCHITECTURE STREQUAL CMAKE_VS_PLATFORM_NAME)
  return()
endif()

_target_install_obs(dvc-filter DESTINATION "${OBS_PLUGIN_DESTINATION}/dynamic-virtualcam/data")

add_custom_command(
  TARGET dvc-filter POST_BUILD
  COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_SOURCE_DIR}/build_x86" --config $<CONFIG> -t dvc-filter -- /nr:false
  COMMENT "Build x86 dynamic virtual camera"
  VERBATIM
)
_target_install_obs(dvc-filter DESTINATION "${OBS_PLUGIN_DESTINATION}/dynamic-virtualcam/data" x86)

add_executable(dvc-watchdog WIN32 watchdog.cpp ipc.cpp registration.cpp)
target_compile_features(dvc-watchdog PRIVATE cxx_std_17)
target_compile_definitions(dvc-watchdog PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(dvc-watchdog PRIVATE ole32 strmiids uuid advapi32 shell32)
set_target_properties(
  dvc-watchdog PROPERTIES FOLDER plugins/dynamic-virtualcam
                         MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
_target_install_obs(dvc-watchdog DESTINATION "${OBS_PLUGIN_DESTINATION}/dynamic-virtualcam/data")

if(NOT ENABLE_FRONTEND)
  return()
endif()

find_package(Qt6 REQUIRED Widgets)
add_library(
  dynamic-virtualcam MODULE
  plugin.cpp
  manager.cpp
  ui.cpp
  output.cpp
  pixels.cpp
  ipc.cpp
  registration.cpp
  test-card.cpp
)
target_compile_features(dynamic-virtualcam PRIVATE cxx_std_17)
target_compile_definitions(dynamic-virtualcam PRIVATE UNICODE _UNICODE NOMINMAX QT_NO_KEYWORDS)
target_link_libraries(dynamic-virtualcam PRIVATE OBS::libobs OBS::frontend-api Qt::Widgets
                                               ole32 strmiids uuid advapi32 gdi32 user32 shell32)
add_dependencies(dynamic-virtualcam dvc-filter dvc-watchdog)
set_target_properties_obs(dynamic-virtualcam PROPERTIES FOLDER plugins PREFIX "")

add_executable(
  dvc-tests EXCLUDE_FROM_ALL
  "${CMAKE_SOURCE_DIR}/test/test_dynamic_virtualcam.cpp"
  "${CMAKE_SOURCE_DIR}/test/test_dynamic_virtualcam_filter.cpp"
  "${CMAKE_SOURCE_DIR}/test/test_dynamic_virtualcam_buffer.cpp"
  filter.cpp
  formats.cpp
  pixels.cpp
  scheduler.cpp
  ipc.cpp
  registration.cpp
  test-card.cpp
)
target_include_directories(dvc-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dvc-tests PRIVATE cxx_std_17)
target_compile_definitions(dvc-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(dvc-tests PRIVATE ole32 oleaut32 strmiids uuid advapi32 gdi32 user32)
set_target_properties(dvc-tests PROPERTIES FOLDER tests)

add_executable(
  dvc-output-tests EXCLUDE_FROM_ALL
  "${CMAKE_SOURCE_DIR}/test/test_dynamic_virtualcam_output.cpp"
  output.cpp
  pixels.cpp
  ipc.cpp
  test-card.cpp
)
target_include_directories(dvc-output-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dvc-output-tests PRIVATE cxx_std_17)
target_compile_definitions(dvc-output-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(dvc-output-tests PRIVATE OBS::libobs ole32 gdi32 user32)
set_target_properties(dvc-output-tests PROPERTIES FOLDER tests)

add_executable(dvc-ui-tests EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/test/test_dynamic_virtualcam_ui.cpp")
target_include_directories(dvc-ui-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dvc-ui-tests PRIVATE cxx_std_17)
target_compile_definitions(dvc-ui-tests PRIVATE QT_NO_KEYWORDS)
target_link_libraries(dvc-ui-tests PRIVATE Qt::Widgets)
set_target_properties(dvc-ui-tests PROPERTIES FOLDER tests)
