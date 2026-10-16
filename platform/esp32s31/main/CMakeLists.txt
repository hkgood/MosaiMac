idf_component_register(
    SRCS
        "main.c"
        "qd_bsp.c"
        # The graphics core, compiled straight from the portable tree rather
        # than copied, so there is one source of truth for the ROP engine.
        "${CMAKE_CURRENT_LIST_DIR}/../../../src/qd_rop.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../../src/qd_region.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../../src/qd_expand.c"
        # The window manager is shared with the desktop simulator, so a drag
        # that looks right on a Mac is the same drag that runs on the panel.
        "${CMAKE_CURRENT_LIST_DIR}/../../../src/qd_win.c"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_LIST_DIR}/../../../include"
    REQUIRES
        esp_lcd
        esp-mosaico-bsp
        # bsp/display.h includes <esp_lcd_touch.h> but does not declare it as a
        # dependency of its own, so consumers have to. Without this the build
        # dies on a missing header with no hint that a dependency is the cause.
        esp_lcd_touch
)

# Board bring-up comes from esp-mosaico-bsp, which already owns the CO5300 and
# CST9220 pin mapping, the glass mounting orientation and the power sequence.
# Its dependencies pull in the panel and touch drivers, so there is nothing
# extra to declare here.
#
# For a standalone (non-workspace) build:
#   idf.py add-dependency "espressif/esp_lcd_co53000"
#   idf.py add-dependency "espressif/esp_lcd_touch_cst9220"
