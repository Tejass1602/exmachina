# Board CMake for RP2350A Pico 2 W

board_set_debugger_iface(picoprobe)
board_set_debugger_iface(jlink)

board_runner_args(picoprobe "--frequency=5000")
board_runner_args(jlink "--speed=4000")

# Flash using picotool or openocd
board_runner_args(picotool "--update")

include(${ZEPHYR_BASE}/boards/common/picoprobe/board.cmake)