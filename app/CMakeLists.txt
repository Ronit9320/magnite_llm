cmake_minimum_required(VERSION 3.20)
# Read the same identity used by the backend and release scripts.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../backend/src/backend/_version.py"
    MAGNITE_VERSION_LINE REGEX "^RELEASE_VERSION = ")
string(REGEX MATCH "v[0-9]+\\.[0-9]+(\\.[0-9]+)?-preview(\\.[0-9]+)?" MAGNITE_VERSION "${MAGNITE_VERSION_LINE}")
if(NOT MAGNITE_VERSION)
    message(FATAL_ERROR "Invalid canonical release identity")
endif()
string(REGEX MATCH "[0-9]+\\.[0-9]+(\\.[0-9]+)?" MAGNITE_NUMERIC_VERSION "${MAGNITE_VERSION}")
if(MAGNITE_NUMERIC_VERSION MATCHES "^[0-9]+\\.[0-9]+$")
    string(APPEND MAGNITE_NUMERIC_VERSION ".0")
endif()
project(magnite-app VERSION ${MAGNITE_NUMERIC_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Svg)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../backend/src/backend/_version.py" MAGNITE_SEQUENCE_LINE REGEX "^UPDATE_SEQUENCE = ")
string(REGEX MATCH "[0-9]+" MAGNITE_UPDATE_SEQUENCE "${MAGNITE_SEQUENCE_LINE}")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../backend/src/backend/_version.py" MAGNITE_SCHEMA_LINE REGEX "^DATA_SCHEMA = ")
string(REGEX MATCH "[0-9]+" MAGNITE_DATA_SCHEMA "${MAGNITE_SCHEMA_LINE}")
set(MAGNITE_RELEASE_REPOSITORY "Ronit9320/magnite_llm" CACHE STRING "Public GitHub release repository")
if(NOT MAGNITE_RELEASE_REPOSITORY MATCHES "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR "Invalid GitHub release repository")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/keys/update-public.pem" MAGNITE_UPDATE_PUBLIC_KEY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../backend/src/backend/_version.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/keys/update-public.pem")
configure_file(src/update_config.h.in generated/update_config.h @ONLY)

set(MAGNITE_SOURCES
    src/update_manager.cpp
    src/update_manager.h
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

unset(MAGNITE_BUILD_LABEL CACHE) # Discard labels left by older packaging builds.
set(MAGNITE_BUILD_LABEL "${MAGNITE_VERSION}")
target_compile_definitions(magnite-app PRIVATE MAGNITE_BUILD_LABEL="${MAGNITE_BUILD_LABEL}")
if(WIN32)
    enable_language(RC)
    foreach(field PROJECT_VERSION_MAJOR PROJECT_VERSION_MINOR PROJECT_VERSION_PATCH MAGNITE_UPDATE_SEQUENCE)
        if(${field} GREATER 65535)
            message(FATAL_ERROR "${field} exceeds Windows version resource field")
        endif()
    endforeach()
    configure_file(src/windows.rc.in generated/windows.rc @ONLY)
    target_sources(magnite-app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/windows.rc")
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
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${MAGNITE_UPDATE_SEQUENCE}"
        # The installed bundle loads Qt from Contents/Frameworks (filled by macdeployqt,
        # which only rewrites existing rpaths and adds none of its own).
        INSTALL_RPATH "@executable_path/../Frameworks")
    install(TARGETS magnite-app BUNDLE DESTINATION .)
else()
    install(TARGETS magnite-app RUNTIME DESTINATION bin)
endif()

target_include_directories(magnite-app PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(magnite-app PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Svg
    OpenSSL::Crypto
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(magnite-update-helper ../packaging/update_helper.cpp)
    set_target_properties(magnite-update-helper PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(magnite-update-helper PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    install(TARGETS magnite-update-helper RUNTIME DESTINATION bin)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(magnite-update-tests src/testing/update_tests.cpp src/update_manager.cpp src/update_manager.h)
    target_include_directories(magnite-update-tests PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(magnite-update-tests PRIVATE Qt6::Network OpenSSL::Crypto)
    add_test(NAME update-manifest COMMAND magnite-update-tests)
endif()
