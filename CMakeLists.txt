cmake_minimum_required(VERSION 3.10)

project(SharedMemoryJSON)

set(CMAKE_CXX_STANDARD 11)
set(JSON_INCLUDE ${CMAKE_SOURCE_DIR}/thirdparty/json/single_include/nlohmann)
set(PULSEAUDIO_INCLUDE /usr/include)
set(CMAKE_BUILD_TYPE Debug)

# 使用find_library自动查找库路径
find_library(RT_LIB rt)
find_library(PTHREAD_LIB pthread)
find_library(PULSEAUDIO_LIB pulse)

add_executable(send send.cpp)
# add_executable(receive receive.cpp)

# 设置头文件路径
target_include_directories(send PRIVATE ${JSON_INCLUDE} ${PULSEAUDIO_INCLUDE})
target_link_libraries(send PRIVATE ${RT_LIB} ${PTHREAD_LIB} ${PULSEAUDIO_LIB})

# target_include_directories(receive PRIVATE ${JSON_INCLUDE} ${PULSEAUDIO_INCLUDE})
# target_link_libraries(receive PRIVATE ${RT_LIB} ${PTHREAD_LIB} ${PULSEAUDIO_LIB})
