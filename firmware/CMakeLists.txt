# Wincode node firmware — ESP-IDF v5.5
#   winch:       idf.py build
#   pixel node:  idf.py -B build-pixel -DSDKCONFIG=sdkconfig.pixel "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pixel" build
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wincode_node)
