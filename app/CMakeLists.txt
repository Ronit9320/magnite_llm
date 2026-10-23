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
    src/widgets/logoanimation.cpp
    src/widgets/logoanimation.h
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

# macOS packaging only: produce Magnite.app. Off by default so the documented
# ./build/magnite-app dev/CI workflow is unchanged. See docs/release/macos/adr/0007.
option(MAGNITE_MACOS_BUNDLE "Build a Magnite.app bundle (macOS packaging only)" OFF)
if(APPLE AND MAGNITE_MACOS_BUNDLE)
    set(MAGNITE_MACOS_MIN_VERSION "14.0" CACHE STRING "LSMinimumSystemVersion written to Info.plist")
    set(MAGNITE_MACOS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../build/packaging/macos/magnite.icns"
        CACHE FILEPATH "Bundle icon; create it with packaging/macos/make_icon.sh")
    if(NOT EXISTS "${MAGNITE_MACOS_ICON}")
        message(FATAL_ERROR "Missing ${MAGNITE_MACOS_ICON}: run packaging/macos/make_icon.sh first")
    endif()
    get_filename_component(MAGNITE_MACOS_ICON_NAME "${MAGNITE_MACOS_ICON}" NAME)
    set_source_files_properties("${MAGNITE_MACOS_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(magnite-app PRIVATE "${MAGNITE_MACOS_ICON}")
    set_target_properties(magnite-app PROPERTIES
        OUTPUT_NAME Magnite   # bundle directory and executable become Magnite.app / Magnite
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/macos/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Magnite"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.magnite.magnite-llm"
        MACOSX_BUNDLE_ICON_FILE "${MAGNITE_MACOS_ICON_NAME}"
        # CFBundleShortVersionString must be numeric; the full label goes in MagniteBuildLabel.
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    install(TARGETS magnite-app BUNDLE DESTINATION .)
else()
    install(TARGETS magnite-app RUNTIME DESTINATION bin)
endif()

target_include_directories(magnite-app PRIVATE src)

target_link_libraries(magnite-app PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Svg
)
