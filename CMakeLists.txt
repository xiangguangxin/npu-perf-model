cmake_minimum_required(VERSION 3.16)
project(npu_perf_model CXX)

# ---- 语言标准（与技术栈一致：C++17）----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- 定位随项目自带的 SystemC（含 TLM2.0）----
# 允许外部通过 -DSYSTEMC_HOME 覆盖；默认用 third_party 里编好的那份。
if(NOT DEFINED SYSTEMC_HOME)
    set(SYSTEMC_HOME "${CMAKE_SOURCE_DIR}/third_party/systemc")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${SYSTEMC_HOME}")

# SystemCLanguage 会带出 TLM，提供 imported target SystemC::systemc
find_package(SystemCLanguage CONFIG REQUIRED)

message(STATUS "Using SystemC ${SystemCLanguage_VERSION} from ${SYSTEMC_HOME}")

# SystemC 用什么 C++ 标准编的，使用方也要对齐，否则 ABI 会冲突
if(DEFINED SystemC_CXX_STANDARD AND SystemC_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD ${SystemC_CXX_STANDARD})
endif()

# ---- 核心库：各模块实现编译一次，供可执行文件复用 ----
add_library(npu_core STATIC
    src/common.cpp
    src/dma_engine.cpp
    src/hbm.cpp
    src/memory_controller.cpp
    src/interconnect.cpp
    src/arbiter.cpp
    src/npu_system.cpp
    src/onchip_buffer.cpp
    src/pe_array.cpp
    src/perf_monitor.cpp
    src/workload_driver.cpp)
target_include_directories(npu_core PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(npu_core PUBLIC SystemC::systemc)
target_compile_features(npu_core PUBLIC cxx_std_17)

# ---- 主仿真可执行文件 ----
add_executable(npu_sim src/main.cpp)
target_link_libraries(npu_sim PRIVATE npu_core)

# ---- Sanity 测试 ----
enable_testing()
add_executable(sanity_tests tests/sanity_tests.cpp)
target_link_libraries(sanity_tests PRIVATE npu_core)
add_test(NAME sanity_tests COMMAND sanity_tests)
