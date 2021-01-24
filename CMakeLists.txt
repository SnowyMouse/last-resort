# SPDX-License-Identifier: GPL-3.0-only

# Require at least version 3.12
cmake_minimum_required(VERSION 3.12)

# Project name - You should change this if you're making a fork
project("Last Resort"
    VERSION 0.1.2
    DESCRIPTION "Convince tags to work on the Xbox version of the game. Use this when nothing else works"
    LANGUAGES C CXX
)

# Use C++17
set(CMAKE_CXX_STANDARD 17)

add_executable(last-resort
    src/main.cpp
)

option(INVADER_STATIC_LINKED_LIBS "Use static-linked Invader")

target_link_libraries(last-resort invader)

if(${INVADER_STATIC_LINKED_LIBS})
    target_link_libraries(last-resort squish ogg vorbis vorbisenc vorbisfile ogg zstd z gomp invader-bitmap-p8-palette)
endif()
