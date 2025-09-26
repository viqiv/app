cmake_minimum_required(VERSION 3.16)
project(app)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)


find_package(Qt6 REQUIRED COMPONENTS Widgets)

include(FetchContent)

FetchContent_Declare(
  minizip
  GIT_REPOSITORY https://github.com/zlib-ng/minizip-ng.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(minizip)
# add_subdirectory(external/minizip-ng)
set(MZ_FETCH_DEPENDENCIES OFF)
set(MZ_BUILD_TESTS OFF)
set(MZ_BUILD_EXAMPLES OFF)
set(MZ_BUILD_UNIT_TESTS OFF)

if(WIN32)
add_executable(${PROJECT_NAME} WIN32 main.cpp)
else()
add_executable(${PROJECT_NAME} main.cpp)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Widgets)
target_link_libraries(${PROJECT_NAME} PRIVATE minizip)
