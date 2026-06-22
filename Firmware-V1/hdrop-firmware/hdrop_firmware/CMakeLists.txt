# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Aplica sdkconfig.defaults (partições OTA + flash 4 MB) em builds limpos.
# Se sdkconfig já existir com configuração antiga, apagar antes de compilar.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hdrop_firmware)
