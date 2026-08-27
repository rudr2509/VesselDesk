cmake_minimum_required(VERSION 3.16)

project(VesselDesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
)

find_package(X11 REQUIRED)

add_executable(VesselDesk
    src/main.cpp
)

target_include_directories(VesselDesk PRIVATE
    ${X11_INCLUDE_DIR}
)

target_link_libraries(VesselDesk PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    ${X11_LIBRARIES}
    Xext
    Xtst
)
