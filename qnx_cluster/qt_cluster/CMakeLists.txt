cmake_minimum_required(VERSION 3.21)
project(qt_cluster LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick)
qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0001 NEW)   # QML resources under /qt/qml, which is where loadFromModule() looks

qt_add_executable(qt_cluster
    main.cpp
    receiver.cpp
    receiver.h
    diaglink.cpp
    diaglink.h
    clustermodel.cpp
    clustermodel.h
)

qt_add_qml_module(qt_cluster
    URI Cluster
    VERSION 1.0
    QML_FILES Main.qml Dial.qml LampIcon.qml
)

# cluster_msg.h and cluster_e2e.h are shared with the C programs one directory up
target_include_directories(qt_cluster PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(qt_cluster PRIVATE Qt6::Quick)
