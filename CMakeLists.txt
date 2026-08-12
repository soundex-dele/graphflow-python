cmake_minimum_required(VERSION 3.16)
project(graphflow_python VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    add_compile_options(/FS /utf-8)
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/graphflow-cpp/CMakeLists.txt")
    message(FATAL_ERROR
        "graphflow-cpp submodule is missing. Run: git submodule update --init --recursive")
endif()
set(GRAPHFLOW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GRAPHFLOW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/graphflow-cpp)

find_package(pybind11 CONFIG REQUIRED)
pybind11_add_module(_graphflow_native src/graphflow_bindings.cpp)
target_link_libraries(_graphflow_native PRIVATE graphflow_engine)

set(_GRAPHFLOW_PYTHON_PACKAGE "${CMAKE_CURRENT_SOURCE_DIR}/graphflow")
set_target_properties(_graphflow_native PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${_GRAPHFLOW_PYTHON_PACKAGE}"
    RUNTIME_OUTPUT_DIRECTORY "${_GRAPHFLOW_PYTHON_PACKAGE}"
)
foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(_graphflow_native PROPERTIES
        "LIBRARY_OUTPUT_DIRECTORY_${config}" "${_GRAPHFLOW_PYTHON_PACKAGE}"
        "RUNTIME_OUTPUT_DIRECTORY_${config}" "${_GRAPHFLOW_PYTHON_PACKAGE}"
    )
endforeach()
