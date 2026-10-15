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

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Svg)

set(MAGNITE_SOURCES
    src/testing/uicheck.cpp
    src/testing/uicheck.h
    src/main.cpp
    src/mainwindow.cpp
    src/theme.cpp
    src/backend_manager.cpp
    src/backend_client.cpp
    src/panels/basepanel.cpp
    src/panels/modelspanel.cpp
    src/panels/modelcard.cpp
    src/panels/adaptercard.cpp
    src/widgets/flowlayout.cpp
    src/widgets/combobox.cpp
    src/widgets/combobox.h
    src/widgets/clickfeedback.cpp
    src/widgets/clickfeedback.h
    src/widgets/brandmark.cpp
    src/widgets/brandmark.h
    src/widgets/studiocanvas.cpp
    src/widgets/studiocanvas.h
    src/widgets/overviewtile.cpp
    src/widgets/overviewtile.h
    src/widgets/confirmation.h
    src/widgets/surface.h
    src/panels/trainingpanel.cpp
    src/panels/ragpanel.cpp
    src/panels/ragdoccard.cpp
    src/panels/adapterspanel.cpp
    src/panels/chatpanel.cpp
    src/panels/chatbubble.cpp
    src/panels/settingspanel.cpp
    resources.qrc
)

add_executable(magnite-app ${MAGNITE_SOURCES})

set(MAGNITE_BUILD_LABEL "${PROJECT_VERSION}" CACHE STRING "Version displayed by this build")
target_compile_definitions(magnite-app PRIVATE MAGNITE_BUILD_LABEL="${MAGNITE_BUILD_LABEL}")
if(WIN32)
    set_target_properties(magnite-app PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

install(TARGETS magnite-app RUNTIME DESTINATION bin)

target_include_directories(magnite-app PRIVATE src)

target_link_libraries(magnite-app PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Svg
)
