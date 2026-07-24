cmake_minimum_required(VERSION 3.16)
project(recorder_ui C)

set(CMAKE_C_STANDARD 11)

# PI_BUILD=OFF -> PC simulator (SDL2 window, 800x480)
# PI_BUILD=ON  -> Raspberry Pi (DRM display + evdev touch, no desktop needed)
option(PI_BUILD "Build for Raspberry Pi (DRM/evdev)" OFF)

if(PI_BUILD)
    add_compile_definitions(RECORDER_PI=1)
else()
    add_compile_definitions(RECORDER_SIM=1)
endif()
add_compile_definitions(LV_LVGL_H_INCLUDE_SIMPLE)

# LVGL v9 fetched automatically, configured by our lv_conf.h
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE STRING "" FORCE)
include(FetchContent)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v9.2.2
)
FetchContent_MakeAvailable(lvgl)

add_executable(recorder_ui
    src/main.c
    src/data_model.c
    src/config.c
    src/alarm.c
    src/logger.c
    src/comm_sim.c
    src/comm_modbus.c
    src/modbus_tcp.c
    src/diag.c
    src/users.c
    src/update.c
    src/webserver.c
    src/export.c
    src/pdf.c
    src/events.c
    src/history.c
    src/ui/ui.c
    src/ui/img_jetpace.c
    src/ui/font_units_12.c
    src/ui/font_units_14.c
    src/ui/font_units_16.c
    src/ui/font_units_28.c
    src/ui/scr_digital.c
    src/ui/scr_trend.c
    src/ui/scr_polar.c
    src/ui/scr_bar.c
    src/ui/scr_alarm.c
    src/ui/scr_menu.c
)
target_include_directories(recorder_ui PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(recorder_ui PRIVATE lvgl Threads::Threads m)
if(WIN32)
    target_link_libraries(recorder_ui PRIVATE ws2_32)
endif()

if(PI_BUILD)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DRM REQUIRED libdrm)
    target_include_directories(recorder_ui PRIVATE ${DRM_INCLUDE_DIRS})
    target_link_libraries(recorder_ui PRIVATE ${DRM_LIBRARIES})
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(recorder_ui PRIVATE SDL2::SDL2)
    if(WIN32)
        # GUI app: no console window opens alongside the UI
        set_target_properties(recorder_ui PROPERTIES WIN32_EXECUTABLE TRUE)
        # copy runtime DLLs next to the exe so double-click launch works
        foreach(dll SDL2.dll libwinpthread-1.dll)
            add_custom_command(TARGET recorder_ui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        C:/msys64/ucrt64/bin/${dll}
                        $<TARGET_FILE_DIR:recorder_ui>)
        endforeach()
    endif()
endif()

# ship the web dashboard next to the binary; webserver.c serves this file
# from disk (edit-and-refresh, no rebuild) and only falls back to the
# compiled-in page if it is missing.
if(EXISTS ${CMAKE_SOURCE_DIR}/dashboard.html)
    add_custom_command(TARGET recorder_ui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_SOURCE_DIR}/dashboard.html
                $<TARGET_FILE_DIR:recorder_ui>)
endif()
