#-------------------------------------------------------------------------------
# Zephyr Example Application
#
# Copyright (c) 2021 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)

zephyr_include_directories(src)

target_sources(app PRIVATE src/main.c)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/bg_image.c)
	target_sources(app PRIVATE src/bg_image.c)
endif()
