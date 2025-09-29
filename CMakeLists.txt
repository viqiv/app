cmake_minimum_required(VERSION 3.16)
project(ZipCombiner)

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
target_sources(${PROJECT_NAME} PRIVATE icon.rc)
else()
add_executable(${PROJECT_NAME} main.cpp)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "ZipCombiner")

target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Widgets)
target_link_libraries(${PROJECT_NAME} PRIVATE minizip)


if(APPLE)
    set(MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/mac/Info.plist")
    set(APP_ICON icons/ZipCombiner.icns)

    set_source_files_properties(${APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

    target_sources(${PROJECT_NAME} PRIVATE ${APP_ICON})

    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE "ZipCombiner.icns"
    )
endif()


# mkdir -p icons/Icon.iconset
# sips -z 16 16     icons/icon.png --out icons/Icon.iconset/icon_16x16.png
# sips -z 32 32     icons/icon.png --out icons/Icon.iconset/icon_16x16@2x.png
# sips -z 32 32     icons/icon.png --out icons/Icon.iconset/icon_32x32.png
# sips -z 64 64     icons/icon.png --out icons/Icon.iconset/icon_32x32@2x.png
# sips -z 128 128   icons/icon.png --out icons/Icon.iconset/icon_128x128.png
# sips -z 256 256   icons/icon.png --out icons/Icon.iconset/icon_128x128@2x.png
# sips -z 256 256   icons/icon.png --out icons/Icon.iconset/icon_256x256.png
# sips -z 512 512   icons/icon.png --out icons/Icon.iconset/icon_256x256@2x.png
# sips -z 512 512   icons/icon.png --out icons/Icon.iconset/icon_512x512.png
# cp icons/icon.png icons/Icon.iconset/icon_512x512@2x.png
# iconutil -c icns icons/Icon.iconset -o icons/ZipCombiner.icns

