cmake_minimum_required(VERSION 3.16)

project(Quire VERSION 0.3.13 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Ubuntu ships Qt6 Core/Widgets without WebEngine. Honor a user-local prefix
# (extracted debs) so we still find WebEngineWidgets the way Monastery links it.
if(DEFINED ENV{HOME})
    list(PREPEND CMAKE_PREFIX_PATH
        "$ENV{HOME}/.local/opt/qt6-webengine/usr"
        "$ENV{HOME}/.local/opt/qt6-webengine/usr/lib/x86_64-linux-gnu")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Widgets PrintSupport)

find_package(Qt6WebEngineWidgets QUIET)
if(NOT Qt6WebEngineWidgets_FOUND)
    find_package(Qt6 REQUIRED COMPONENTS WebEngineWidgets)
endif()

find_path(HUNSPELL_INCLUDE_DIR hunspell/hunspell.h)
find_library(HUNSPELL_LIBRARY hunspell-1.7)

include_directories(${HUNSPELL_INCLUDE_DIR})

qt_standard_project_setup()

qt_add_executable(Quire
    main.cpp
    QuireFrame.cpp
    MonasteryEditor.cpp
    Theme.cpp
    EpubWriter.cpp
    DocumentIo.cpp
    ScrivenerImport.cpp
)

target_sources(Quire PRIVATE resources.qrc)

find_library(MINIZIP_LIBRARY minizip HINTS "$ENV{HOME}/.local/opt/qt6-webengine/usr/lib/x86_64-linux-gnu")
find_library(ZLIB_LIBRARY z)
link_directories("$ENV{HOME}/.local/opt/qt6-webengine/usr/lib/x86_64-linux-gnu")

target_include_directories(Quire PRIVATE "$ENV{HOME}/.local/opt/qt6-webengine/usr/include")

target_link_libraries(Quire PRIVATE Qt6::Core Qt6::Widgets Qt6::PrintSupport Qt6::WebEngineWidgets ${HUNSPELL_LIBRARY} ${MINIZIP_LIBRARY} ${ZLIB_LIBRARY})
target_link_options(Quire PRIVATE
    "-Wl,-rpath-link,$ENV{HOME}/.local/opt/qt6-webengine/usr/lib/x86_64-linux-gnu"
    "-Wl,-rpath,$ENV{HOME}/.local/opt/qt6-webengine/usr/lib/x86_64-linux-gnu")


set_target_properties(Quire PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)
