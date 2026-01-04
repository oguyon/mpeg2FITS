cmake_minimum_required(VERSION 3.10)
project(mp4-to-FITS)

set(CMAKE_C_STANDARD 99)

find_package(PkgConfig REQUIRED)

pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(CFITSIO REQUIRED cfitsio)

# Debug prints to check what was found
message(STATUS "AVCODEC_INCLUDE_DIRS: ${AVCODEC_INCLUDE_DIRS}")
message(STATUS "AVCODEC_LIBRARIES: ${AVCODEC_LIBRARIES}")
message(STATUS "CFITSIO_INCLUDE_DIRS: ${CFITSIO_INCLUDE_DIRS}")
message(STATUS "CFITSIO_LIBRARIES: ${CFITSIO_LIBRARIES}")

include_directories(
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${CFITSIO_INCLUDE_DIRS}
)

link_directories(
    ${AVCODEC_LIBRARY_DIRS}
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
    ${CFITSIO_LIBRARY_DIRS}
)

add_executable(mp4-to-FITS src/mp4-to-FITS.c)

target_link_libraries(mp4-to-FITS
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${CFITSIO_LIBRARIES}
    m
)
