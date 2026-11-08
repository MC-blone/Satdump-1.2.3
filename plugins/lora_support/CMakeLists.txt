cmake_minimum_required(VERSION 3.12)
project(lora_support)

file(GLOB_RECURSE lora_support_CPPS *.cpp)
add_library(lora_support SHARED ${lora_support_CPPS})
target_link_libraries(lora_support PUBLIC satdump_core)
target_include_directories(lora_support PUBLIC src .)
install(TARGETS lora_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/satdump/plugins)