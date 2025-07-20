cmake_minimum_required(VERSION 3.12)
project(IOCPDocumentServer 
    VERSION 1.0.0
    DESCRIPTION "High-performance IOCP-based document server"
    LANGUAGES C)

# Set C standard
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Platform check
if(NOT WIN32)
    message(FATAL_ERROR "This project requires Windows (IOCP is Windows-specific)")
endif()

# Compiler-specific options
if(MSVC)
    # Visual Studio
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W4")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /Od /Zi")
else()
    # MinGW/GCC
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0")
endif()

# Find required libraries
find_library(WS2_32_LIB ws2_32 REQUIRED)
find_library(MSWSOCK_LIB mswsock REQUIRED)

# Server executable
add_executable(server_iocp server_iocp.c)
target_link_libraries(server_iocp ${WS2_32_LIB} ${MSWSOCK_LIB})

# Client executable
add_executable(client_iocp client_iocp.c)
target_link_libraries(client_iocp ${WS2_32_LIB})

# Set output directory
set_target_properties(server_iocp client_iocp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Copy config file to build directory
configure_file(${CMAKE_SOURCE_DIR}/config.txt ${CMAKE_BINARY_DIR}/bin/config.txt COPYONLY)

# Debug versions
add_executable(server_iocp_debug server_iocp.c)
target_link_libraries(server_iocp_debug ${WS2_32_LIB} ${MSWSOCK_LIB})
target_compile_definitions(server_iocp_debug PRIVATE DEBUG)
set_target_properties(server_iocp_debug PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/debug
)

add_executable(client_iocp_debug client_iocp.c)
target_link_libraries(client_iocp_debug ${WS2_32_LIB})
target_compile_definitions(client_iocp_debug PRIVATE DEBUG)
set_target_properties(client_iocp_debug PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/debug
)

# Installation
install(TARGETS server_iocp client_iocp
    RUNTIME DESTINATION bin
)

install(FILES config.txt
    DESTINATION bin
)

# Package configuration
set(CPACK_PACKAGE_NAME "IOCPDocumentServer")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "High-performance IOCP-based document server")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

include(CPack)

# Custom targets
add_custom_target(run-server
    COMMAND ${CMAKE_BINARY_DIR}/bin/server_iocp.exe 127.0.0.1 8080
    DEPENDS server_iocp
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    COMMENT "Running IOCP server on localhost:8080"
)

add_custom_target(run-client
    COMMAND ${CMAKE_BINARY_DIR}/bin/client_iocp.exe
    DEPENDS client_iocp
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    COMMENT "Running IOCP client"
)

# Print build information
message(STATUS "")
message(STATUS "IOCP Document Server Build Configuration:")
message(STATUS "  Project Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  C Flags: ${CMAKE_C_FLAGS}")
message(STATUS "  Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
message(STATUS "Available targets:")
message(STATUS "  server_iocp       - Build server")
message(STATUS "  client_iocp       - Build client") 
message(STATUS "  server_iocp_debug - Build server (debug)")
message(STATUS "  client_iocp_debug - Build client (debug)")
message(STATUS "  run-server        - Build and run server")
message(STATUS "  run-client        - Build and run client")
message(STATUS "")
