# SPDX-License-Identifier: BSD-3-Clause
# ndk-sw tools-debug CMake build file
#
# Copyright (c) 2022 CESNET
#
# Author(s):
#   Martin Spinler <spinler@cesnet.cz
#
# Modified by Richard Hyroš
# for the purpose of bachelor thesis submission

cmake_minimum_required(VERSION 3.20)
cmake_policy(VERSION 3.20)
project(nfb-framework LANGUAGES C)
include(${CMAKE_CURRENT_LIST_DIR}/functions.cmake)

get_git_version()

set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "Main")

set(PACKAGE_NAME "${PROJECT_NAME}")
set(PACKAGE_VERSION "${GIT_VERSION}")
set(RELEASE "${GIT_VERSION_RELEASE}")

add_subdirectory(drivers)
add_subdirectory(libnfb)
add_subdirectory(tools)