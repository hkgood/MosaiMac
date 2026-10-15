# ESP-IDF project for the ESP32-S31 / ESP-Mosaico QuickDraw port.
#
#   . $IDF_PATH/export.sh
#   idf.py set-target esp32s31
#   idf.py build flash monitor
#
# The target is still a preview part, so if `set-target` does not know it yet,
# point IDF_PATH at an Espressif preview branch. The graphics core is plain
# portable C and does not care which one.
#
# Alternatively, drop this directory into an esp-mosaico-vibe workspace at
# projects/qd_esp32s31 -- the sibling project mosaico-film does exactly that,
# and going through the workspace gets you the board's pinned IDF version
# and the `mosaico.py project sim` PC simulator for free. See README.md.

cmake_minimum_required(VERSION 3.16)

set(QD_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(qd_esp32s31)
