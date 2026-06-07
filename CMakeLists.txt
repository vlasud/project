cmake_minimum_required(VERSION 3.15)
project(gamemode CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
set(CMAKE_SHARED_LIBRARY_PREFIX "")

add_subdirectory(ThirdParty/open.mp-sdk)
add_subdirectory(ThirdParty/fmt-12.1.0)

file(GLOB_RECURSE SOURCE_FILES "Src/*.h" "Src/*.cpp")
add_library(${PROJECT_NAME} SHARED ${SOURCE_FILES})

target_link_libraries(${PROJECT_NAME} PUBLIC OMP-SDK fmt::fmt)

set(MYSQL_CONNECTOR_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/mysql-connector-c++-8.0.33-win32")
target_include_directories(${PROJECT_NAME} PRIVATE
    "${MYSQL_CONNECTOR_DIR}/include"
    "${MYSQL_CONNECTOR_DIR}/include/mysql"
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "${MYSQL_CONNECTOR_DIR}/lib/vs14/mysqlcppconn8.lib"
        ws2_32
    )
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        mysqlcppconn
        pthread
    )
endif()

set(LIBSODIUM_CONNECTOR_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/libsodium-1.0.22")
target_include_directories(${PROJECT_NAME} PRIVATE
    "${LIBSODIUM_CONNECTOR_DIR}/include"
)
target_link_libraries(${PROJECT_NAME} PRIVATE
        "${LIBSODIUM_CONNECTOR_DIR}/lib/libsodium.lib"
)