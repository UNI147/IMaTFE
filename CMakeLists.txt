cmake_minimum_required(VERSION 3.25)

project(IMaTFE
    VERSION 0.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(IMaTFE
    src/main.cpp
    src/engine/Engine.cpp
)

target_include_directories(IMaTFE
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (MSVC)
    target_compile_options(IMaTFE PRIVATE
        /W4
        /permissive-
    )
else()
    target_compile_options(IMaTFE PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()