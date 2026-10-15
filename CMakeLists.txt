cmake_minimum_required(VERSION 3.16)

# Per-machine secrets (e.g. CONFIG_ENVIRO8_OTA_TOKEN) live in a git-ignored file.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
endif()

set(PROJECT_VER "1.2.2")  # keep in sync with APP_VERSION in main/app_config.h

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(enviro8)
