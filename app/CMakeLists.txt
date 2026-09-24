cmake_minimum_required(VERSION 3.22.1)
project(danzku_shader)

add_library(danzku_shader SHARED native/danzku_shader.cpp)

target_include_directories(danzku_shader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/jni
)

find_library(log-lib log)
find_library(dl-lib dl)
find_library(glesv2-lib GLESv2)
find_library(egl-lib EGL)

target_link_libraries(danzku_shader
    ${log-lib}
    ${dl-lib}
    ${glesv2-lib}
    ${egl-lib}
)
