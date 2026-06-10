cmake_minimum_required(VERSION 3.21)
project(ObjectDisable VERSION 0.0.1 LANGUAGES CXX)

add_compile_definitions(SKYRIM)
set(BUILD_TESTS OFF)                                

# CommonLibSSE-NG from the submodule (built from source)
add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)

find_package(nlohmann_json CONFIG REQUIRED)
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h")
find_library(DETOURS_LIBRARY detours REQUIRED)

add_library(${PROJECT_NAME} SHARED main.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE ${DETOURS_INCLUDE_DIRS})

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        CommonLibSSE::CommonLibSSE
        nlohmann_json::nlohmann_json
        ${DETOURS_LIBRARY})
