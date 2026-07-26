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

# Src — корень внутренних include: все #include пишутся от него
# (например "Services/Core/GridService/GridService.h"), без относительных "../../".
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/Src")

# nlohmann/json — header-only single-header в ThirdParty/nlohmann; подключается как
# #include <nlohmann/json.hpp> (корень include — ThirdParty, неймспейс nlohmann/).
# SYSTEM: предупреждения из чужих заголовков не наши и только прятали бы свои.
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty")

# Предупреждения компилятора — на своём коде по максимуму. Без них молча проходят
# знаковые сравнения, неинициализированные члены, потерянные возвраты и срезка.
# /WX (warnings as errors) НЕ включаем: сборка не должна падать из-за нового
# предупреждения в обновлённом тулчейне — но список должен оставаться пустым.
# Дев-бенчмарк (/bench: микрозамеры и нагрузочные прогоны на ботах). В боевой
# сборке не нужен: его зонды висят на диспатчере апдейтов, а команда даёт
# возможность подвесить сервер на секунды. Включать только для замеров:
#   cmake -S . -B build -DGAMEMODE_BENCHMARK=ON
option(GAMEMODE_BENCHMARK "Собрать дев-бенчмарк (/bench)" OFF)
if(GAMEMODE_BENCHMARK)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GAMEMODE_BENCHMARK)
endif()

#
# C4100 (неиспользуемый формальный параметр) выключен осознанно: подавляющее
# большинство таких мест — обязательные override'ы обработчиков SDK, где сигнатуру
# диктует интерфейс (onPlayerDisconnect(player, reason) и т.п.). Убирать имена
# параметров ради тишины — потерять документацию сигнатуры.
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /wd4100)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

target_link_libraries(${PROJECT_NAME} PUBLIC OMP-SDK fmt::fmt)

set(MYSQL_CONNECTOR_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/mysql-connector-c++-8.0.33-win32")
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
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
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    "${LIBSODIUM_CONNECTOR_DIR}/include"
)
target_link_libraries(${PROJECT_NAME} PRIVATE
        "${LIBSODIUM_CONNECTOR_DIR}/lib/libsodium.lib"
)