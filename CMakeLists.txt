cmake_minimum_required(VERSION 3.10)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PROJECT_VERSION_TEXT)
string(STRIP "${PROJECT_VERSION_TEXT}" PROJECT_VERSION_TEXT)
project(livox_mid360_diagnostics VERSION "${PROJECT_VERSION_TEXT}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(mid360_config_tool src/cpp/mid360_config_tool.cpp)
target_include_directories(mid360_config_tool PRIVATE external/Livox-SDK2/3rdparty)
target_link_libraries(mid360_config_tool PRIVATE Threads::Threads)

add_executable(livox_mid360_diagnostics src/cpp/mid360_diagnostics.cpp)
target_compile_definitions(livox_mid360_diagnostics PRIVATE
  LIVOX_MID360_DIAGNOSTICS_VERSION="${PROJECT_VERSION}"
)

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

if(LIVOX_SDK2_ROOT AND EXISTS "${LIVOX_SDK2_ROOT}/build/sdk_core/liblivox_lidar_sdk_static.a")
  set(LIVOX_SDK2_LIBRARY "${LIVOX_SDK2_ROOT}/build/sdk_core/liblivox_lidar_sdk_static.a")
  add_livox_sdk2_tool(mid360_sdk_monitor src/cpp/mid360_sdk_monitor.cpp "${LIVOX_SDK2_LIBRARY}")
  add_livox_sdk2_tool(mid360_sdk_dump src/cpp/mid360_sdk_dump.cpp "${LIVOX_SDK2_LIBRARY}")
elseif(LIVOX_SDK2_ROOT AND EXISTS "${LIVOX_SDK2_ROOT}/build/sdk_core/liblivox_lidar_sdk_shared.so")
  set(LIVOX_SDK2_LIBRARY "${LIVOX_SDK2_ROOT}/build/sdk_core/liblivox_lidar_sdk_shared.so")
  add_livox_sdk2_tool(mid360_sdk_monitor src/cpp/mid360_sdk_monitor.cpp "${LIVOX_SDK2_LIBRARY}")
  add_livox_sdk2_tool(mid360_sdk_dump src/cpp/mid360_sdk_dump.cpp "${LIVOX_SDK2_LIBRARY}")
elseif(LIVOX_SDK2_ROOT)
  message(STATUS "Livox-SDK2 found at ${LIVOX_SDK2_ROOT}, but its library is not built; run scripts/build_cpp_with_sdk2.sh to build SDK tools.")
else()
  message(STATUS "LIVOX_SDK2_ROOT not set; SDK tools will not be built.")
endif()


install(TARGETS mid360_config_tool livox_mid360_diagnostics
  RUNTIME DESTINATION bin
)
foreach(sdk_target mid360_sdk_monitor mid360_sdk_dump)
  if(TARGET ${sdk_target})
    install(TARGETS ${sdk_target}
      RUNTIME DESTINATION bin
    )
  endif()
endforeach()
