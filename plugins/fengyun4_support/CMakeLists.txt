cmake_minimum_required(VERSION 3.12)
project(fy4_lora_support)

file(GLOB fy4_lora_support_CPPS *.cpp)
list(FILTER fy4_lora_support_CPPS EXCLUDE REGEX "/main\\.cpp$")
add_library(fy4_lora_support SHARED ${fy4_lora_support_CPPS})
target_link_libraries(fy4_lora_support PUBLIC satdump_core)
target_include_directories(fy4_lora_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS fy4_lora_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/satdump/plugins)