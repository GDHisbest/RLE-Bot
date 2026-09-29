cmake_minimum_required(VERSION 3.25)

project(RLE-Bot)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Проверяем, что переменная GEODE_SDK задана
if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

# Создаём библиотеку мода
add_library(${PROJECT_NAME} SHARED
    src/main.cpp
)

# Подключаем Geode SDK
add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Настраиваем мод (парсит mod.json, пакует .geode и т.д.)
setup_geode_mod(${PROJECT_NAME})
