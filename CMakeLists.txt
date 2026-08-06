# Warthog — top-level ESP-IDF CMake stub. PlatformIO drives the build via
# `framework = espidf`; this file is the canonical ESP-IDF entry point that
# both PlatformIO and raw `idf.py` understand.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(warthog)
