cmake_minimum_required(VERSION 3.20)
# Read the same identity used by the backend and release scripts.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../backend/src/backend/_version.py"
    MAGNITE_VERSION_LINE REGEX "^RELEASE_VERSION = ")
string(REGEX MATCH "v[0-9]+\\.[0-9]+(\\.[0-9]+)?-preview(\\.[0-9]+)?" MAGNITE_VERSION "${MAGNITE_VERSION_LINE}")
if(NOT MAGNITE_VERSION)
    message(FATAL_ERROR "Invalid canonical release identity")
endif()
string(REGEX MATCH "[0-9]+\\.[0-9]+(\\.[0-9]+)?" MAGNITE_NUMERIC_VERSION "${MAGNITE_VERSION}")
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
    set_target_properties(magnite-app PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

install(TARGETS magnite-app RUNTIME DESTINATION bin)

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
