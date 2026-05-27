cmake_minimum_required(VERSION 3.20)

project(RootACS LANGUAGES CXX)

add_executable(RootACS-Service
    rootacs-service/rootacs-service.cpp
)

add_executable(rootacs
    rootacs/rootacs.cpp
)

target_compile_features(RootACS-Service PRIVATE cxx_std_17)
target_compile_features(rootacs PRIVATE cxx_std_17)

target_compile_definitions(RootACS-Service PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(rootacs PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(RootACS-Service PRIVATE $<$<CONFIG:Debug>:ROOTACS_DEBUG_BUILD>)
target_compile_definitions(rootacs PRIVATE $<$<CONFIG:Debug>:ROOTACS_DEBUG_BUILD>)

target_link_libraries(RootACS-Service PRIVATE advapi32)
target_link_libraries(rootacs PRIVATE advapi32)
