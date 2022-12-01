#
# Copyright (c) 2021 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(NONE)

# NORDIC SDK APP START
target_sources(app PRIVATE
  src/main.c
  src/shared_event.c
)
# NORDIC SDK APP END

# Add include directory for board specific CAF def files
zephyr_include_directories(
  src
  configuration/${BOARD}
)

add_subdirectory(drivers)
