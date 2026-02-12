cmake_minimum_required(VERSION 3.10)
project(mshost)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Находим и подключаем httplib
#add_subdirectory(src/includes/httplib)

# Исходные файлы
set(SOURCES
   src/main.cpp
   src/MinecraftServerManager.cpp
   src/httpServer.cpp
)

# Исполняемый файл
add_executable(${PROJECT_NAME} ${SOURCES})

# Добавляем пути к include-директориям (для logger.h и httplib)
target_include_directories(${PROJECT_NAME} PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}/src/includes  # Путь к вашим кастомным заголовкам (logger.h)
)

# Подключаем библиотеки для Windows (winsock)
if(WIN32)
   target_link_libraries(${PROJECT_NAME} PRIVATE wsock32 ws2_32)
endif()

# Для Linux/Mac: Добавляем threading, если нужно (хотя httplib редко требует)
if(UNIX)
   target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)
   find_package(Threads REQUIRED)
endif()

# Для удобства: копирование бинарника в bin/
set_target_properties(${PROJECT_NAME} PROPERTIES
   RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Опционально: предустановки для отладки (например, включить адресацию санитайзером, если компилируете в Debug)
if(CMAKE_BUILD_TYPE MATCHES Debug)
   target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)  # Предупреждения
endif()
