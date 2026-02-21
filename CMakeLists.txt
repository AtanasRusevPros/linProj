cmake_minimum_required(VERSION 3.14)
project(ipc_client_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# All build outputs go into the build directory root
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# RPATH: executables find libipc.so at runtime without LD_LIBRARY_PATH
set(CMAKE_BUILD_RPATH ${CMAKE_BINARY_DIR})
set(CMAKE_INSTALL_RPATH "$ORIGIN")

# Compiler warnings
add_compile_options(-Wall -Wextra -Wpedantic)

# Optional sanitizers (cmake -DENABLE_SANITIZERS=ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Threading (resolves to -lpthread on Linux)
find_package(Threads REQUIRED)

# --- Shared library: libipc.so ---
add_library(ipc SHARED src/libipc.cpp)
target_include_directories(ipc PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ipc PRIVATE rt pthread)
set_target_properties(ipc PROPERTIES
    OUTPUT_NAME "ipc"
    VERSION 1.0.0
    SOVERSION 1
)

# --- Server executable ---
add_executable(server src/server.cpp)
target_include_directories(server PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(server PRIVATE Threads::Threads rt)

# --- Client 1: links libipc.so directly ---
add_executable(client1 src/client1.cpp)
target_include_directories(client1 PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(client1 PRIVATE ipc)

# --- Client 2: uses dlopen/dlsym (links only dl) ---
add_executable(client2 src/client2.cpp)
target_include_directories(client2 PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(client2 PRIVATE dl)

# --- Doxygen documentation ---
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    configure_file(
        ${CMAKE_SOURCE_DIR}/docs/Doxyfile.in
        ${CMAKE_BINARY_DIR}/Doxyfile
        @ONLY
    )
    add_custom_target(docs-doxygen
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/docs/doxygen
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating Doxygen documentation"
        VERBATIM
    )
else()
    message(STATUS "Doxygen not found -- docs-doxygen target disabled")
endif()

# --- Sphinx documentation (depends on Doxygen XML output) ---
find_program(SPHINX_BUILD
    NAMES sphinx-build
    PATHS ${CMAKE_SOURCE_DIR}/.venv/bin
    NO_DEFAULT_PATH
)
if(NOT SPHINX_BUILD)
    find_program(SPHINX_BUILD sphinx-build)
endif()
if(SPHINX_BUILD AND DOXYGEN_FOUND)
    add_custom_target(docs-sphinx
        COMMAND ${SPHINX_BUILD} -b html
            ${CMAKE_SOURCE_DIR}/docs/sphinx
            ${CMAKE_BINARY_DIR}/docs/sphinx
        DEPENDS docs-doxygen
        COMMENT "Generating Sphinx documentation"
        VERBATIM
    )
else()
    message(STATUS "sphinx-build not found -- docs-sphinx target disabled")
endif()

# --- cppcheck static analysis ---
find_program(CPPCHECK cppcheck)
if(CPPCHECK)
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK}
            --enable=all
            --std=c++17
            --suppress=missingIncludeSystem
            --quiet
            -I ${CMAKE_SOURCE_DIR}/include
            -I ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src
        COMMENT "Running cppcheck static analysis"
        VERBATIM
    )
else()
    message(STATUS "cppcheck not found -- cppcheck target disabled")
endif()

# --- Test target (pytest) ---
# Look for pytest in a project-local venv first, then system-wide
find_program(PYTEST
    NAMES pytest pytest-3
    PATHS ${CMAKE_SOURCE_DIR}/.venv/bin
    NO_DEFAULT_PATH
)
if(NOT PYTEST)
    find_program(PYTEST NAMES pytest pytest-3)
endif()
if(PYTEST)
    add_custom_target(test
        COMMAND ${PYTEST} ${CMAKE_SOURCE_DIR}/tests -v
            --tb=short
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS server client1 client2 ipc
        COMMENT "Running pytest integration tests"
        VERBATIM
    )
else()
    message(STATUS "pytest not found -- test target disabled. "
        "Create a venv: python3 -m venv .venv && .venv/bin/pip install pytest")
endif()
