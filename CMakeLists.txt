cmake_minimum_required(VERSION 3.10)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PROJECT_VERSION_TEXT)
string(STRIP "${PROJECT_VERSION_TEXT}" PROJECT_VERSION_TEXT)
project(livox_mid360_diagnostics VERSION "${PROJECT_VERSION_TEXT}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
set(CURSES_NEED_WIDE TRUE)
find_package(Curses)
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(NCURSESW QUIET ncursesw)
endif()

add_executable(mid360_config_tool src/cpp/mid360_config_tool.cpp)
target_include_directories(mid360_config_tool PRIVATE external/Livox-SDK2/3rdparty)
target_link_libraries(mid360_config_tool PRIVATE Threads::Threads)

add_library(mid360_config_tool_obj OBJECT src/cpp/mid360_config_tool.cpp)
target_compile_definitions(mid360_config_tool_obj PRIVATE main=run_mid360_config_tool)
target_include_directories(mid360_config_tool_obj PRIVATE external/Livox-SDK2/3rdparty)

add_executable(livox_mid360_diagnostics
  src/cpp/mid360_diagnostics.cpp
  $<TARGET_OBJECTS:mid360_config_tool_obj>
)
target_compile_definitions(livox_mid360_diagnostics PRIVATE
  LIVOX_MID360_DIAGNOSTICS_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(livox_mid360_diagnostics PRIVATE Threads::Threads)

set(DEFAULT_LIVOX_SDK2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/Livox-SDK2")
if(EXISTS "${DEFAULT_LIVOX_SDK2_ROOT}/include/livox_lidar_api.h")
  set(LIVOX_SDK2_ROOT "${DEFAULT_LIVOX_SDK2_ROOT}" CACHE PATH "Path to Livox-SDK2 root containing include/livox_lidar_api.h")
else()
  set(LIVOX_SDK2_ROOT "" CACHE PATH "Path to Livox-SDK2 root containing include/livox_lidar_api.h")
endif()

function(add_livox_sdk2_tool target source sdk_library)
  add_executable(${target} ${source})
  target_include_directories(${target} PRIVATE "${LIVOX_SDK2_ROOT}/include")
  target_link_libraries(${target} PRIVATE
    "${sdk_library}"
    Threads::Threads
    ${CMAKE_DL_LIBS}
  )
endfunction()

function(add_livox_sdk2_tool_object target source run_symbol)
  add_library(${target} OBJECT ${source})
  target_compile_definitions(${target} PRIVATE main=${run_symbol})
  target_include_directories(${target} PRIVATE "${LIVOX_SDK2_ROOT}/include")
endfunction()

if(LIVOX_SDK2_ROOT AND EXISTS "${LIVOX_SDK2_ROOT}/build/sdk_core/liblivox_lidar_sdk_static.a")
  set(LIVOX_SDK2_LIBRARY "${LIVOX_SDK2_ROOT}/build/sdk_core/liblivox_lidar_sdk_static.a")
  add_livox_sdk2_tool(mid360_sdk_monitor src/cpp/mid360_sdk_monitor.cpp "${LIVOX_SDK2_LIBRARY}")
  add_livox_sdk2_tool(mid360_sdk_dump src/cpp/mid360_sdk_dump.cpp "${LIVOX_SDK2_LIBRARY}")
  add_livox_sdk2_tool(mid360_sdk_preview src/cpp/mid360_sdk_preview.cpp "${LIVOX_SDK2_LIBRARY}")
  add_livox_sdk2_tool_object(mid360_sdk_monitor_obj src/cpp/mid360_sdk_monitor.cpp run_mid360_sdk_monitor)
  add_livox_sdk2_tool_object(mid360_sdk_dump_obj src/cpp/mid360_sdk_dump.cpp run_mid360_sdk_dump)
  add_livox_sdk2_tool_object(mid360_sdk_preview_obj src/cpp/mid360_sdk_preview.cpp run_mid360_sdk_preview)
elseif(LIVOX_SDK2_ROOT AND EXISTS "${LIVOX_SDK2_ROOT}/build/sdk_core/liblivox_lidar_sdk_shared.so")
  set(LIVOX_SDK2_LIBRARY "${LIVOX_SDK2_ROOT}/build/sdk_core/liblivox_lidar_sdk_shared.so")
  add_livox_sdk2_tool(mid360_sdk_monitor src/cpp/mid360_sdk_monitor.cpp "${LIVOX_SDK2_LIBRARY}")
  add_livox_sdk2_tool(mid360_sdk_dump src/cpp/mid360_sdk_dump.cpp "${LIVOX_SDK2_LIBRARY}")
  add_livox_sdk2_tool(mid360_sdk_preview src/cpp/mid360_sdk_preview.cpp "${LIVOX_SDK2_LIBRARY}")
  add_livox_sdk2_tool_object(mid360_sdk_monitor_obj src/cpp/mid360_sdk_monitor.cpp run_mid360_sdk_monitor)
  add_livox_sdk2_tool_object(mid360_sdk_dump_obj src/cpp/mid360_sdk_dump.cpp run_mid360_sdk_dump)
  add_livox_sdk2_tool_object(mid360_sdk_preview_obj src/cpp/mid360_sdk_preview.cpp run_mid360_sdk_preview)
elseif(LIVOX_SDK2_ROOT)
  message(STATUS "Livox-SDK2 found at ${LIVOX_SDK2_ROOT}, but its library is not built; run scripts/build_cpp_with_sdk2.sh to build SDK tools.")
else()
  message(STATUS "LIVOX_SDK2_ROOT not set; SDK tools will not be built.")
endif()

if(TARGET mid360_sdk_monitor_obj AND TARGET mid360_sdk_dump_obj AND TARGET mid360_sdk_preview_obj)
  target_sources(livox_mid360_diagnostics PRIVATE
    $<TARGET_OBJECTS:mid360_sdk_monitor_obj>
    $<TARGET_OBJECTS:mid360_sdk_dump_obj>
    $<TARGET_OBJECTS:mid360_sdk_preview_obj>
  )
  target_include_directories(livox_mid360_diagnostics PRIVATE "${LIVOX_SDK2_ROOT}/include")
  target_link_libraries(livox_mid360_diagnostics PRIVATE
    "${LIVOX_SDK2_LIBRARY}"
    ${CMAKE_DL_LIBS}
  )
  target_compile_definitions(livox_mid360_diagnostics PRIVATE LIVOX_MID360_HAS_SDK_TOOLS=1)
endif()

if(TARGET mid360_sdk_monitor)
  if(NCURSESW_FOUND)
    target_compile_options(mid360_sdk_monitor PRIVATE ${NCURSESW_CFLAGS_OTHER})
    target_link_options(mid360_sdk_monitor PRIVATE ${NCURSESW_LDFLAGS_OTHER})
    target_link_libraries(mid360_sdk_monitor PRIVATE ${NCURSESW_LIBRARIES})
    target_compile_definitions(mid360_sdk_monitor PRIVATE LIVOX_MID360_HAS_NCURSES=1)
  elseif(CURSES_FOUND)
    target_include_directories(mid360_sdk_monitor PRIVATE ${CURSES_INCLUDE_DIRS})
    target_link_libraries(mid360_sdk_monitor PRIVATE ${CURSES_LIBRARIES})
    target_compile_definitions(mid360_sdk_monitor PRIVATE LIVOX_MID360_HAS_NCURSES=1)
  else()
    message(STATUS "ncurses not found; mid360_sdk_monitor will use the plain ANSI dashboard.")
  endif()
endif()

if(TARGET mid360_sdk_monitor_obj)
  if(NCURSESW_FOUND)
    target_compile_options(mid360_sdk_monitor_obj PRIVATE ${NCURSESW_CFLAGS_OTHER})
    target_link_options(livox_mid360_diagnostics PRIVATE ${NCURSESW_LDFLAGS_OTHER})
    target_link_libraries(livox_mid360_diagnostics PRIVATE ${NCURSESW_LIBRARIES})
    target_compile_definitions(mid360_sdk_monitor_obj PRIVATE LIVOX_MID360_HAS_NCURSES=1)
    target_compile_definitions(livox_mid360_diagnostics PRIVATE LIVOX_MID360_HAS_NCURSES=1)
  elseif(CURSES_FOUND)
    target_include_directories(mid360_sdk_monitor_obj PRIVATE ${CURSES_INCLUDE_DIRS})
    target_include_directories(livox_mid360_diagnostics PRIVATE ${CURSES_INCLUDE_DIRS})
    target_link_libraries(livox_mid360_diagnostics PRIVATE ${CURSES_LIBRARIES})
    target_compile_definitions(mid360_sdk_monitor_obj PRIVATE LIVOX_MID360_HAS_NCURSES=1)
    target_compile_definitions(livox_mid360_diagnostics PRIVATE LIVOX_MID360_HAS_NCURSES=1)
  endif()
endif()


install(TARGETS livox_mid360_diagnostics
  RUNTIME DESTINATION bin
)
