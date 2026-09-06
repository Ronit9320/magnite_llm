cmake_minimum_required(VERSION 3.20)
project(magnite-app VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

set(MAGNITE_SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/backend_manager.cpp
    src/backend_client.cpp
    src/panels/basepanel.cpp
    src/panels/modelspanel.cpp
    src/panels/trainingpanel.cpp
    src/panels/ragpanel.cpp
    src/panels/adapterspanel.cpp
    src/panels/chatpanel.cpp
    src/panels/settingspanel.cpp
)

add_executable(magnite-app ${MAGNITE_SOURCES})

target_include_directories(magnite-app PRIVATE src)

target_link_libraries(magnite-app PRIVATE
    Qt6::Widgets
    Qt6::Network
)