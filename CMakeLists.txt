cmake_minimum_required(VERSION 3.5)
project(speedplay)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE "Debug")


set(CMAKE_EXE_LINKER_FLAGS " -framework Security -framework AudioToolbox -framework CoreVideo -framework VideoToolbox -framework CoreMedia -framework Cocoa")

include_directories(third_party/ffmpeg/include)
include_directories(third_party/gflags/include)
include_directories(third_party/glog/include)
include_directories(third_party/x264/include)
include_directories(third_party/glew/include)

set(PROCESS x86_64)
IF(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm")
    message(STATUS "this is arm cpu")
    set(PROCESS arm)
ELSEIF(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64")
    message(STATUS "this is x86_64 cpu")
    set(PROCESS x86_64)
ENDIF(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm")

link_directories(third_party/ffmpeg/lib/${PROCESS})
link_directories(third_party/glog/lib/${PROCESS})
link_directories(third_party/x264/lib/${PROCESS})

set(COMMON_LIB
    -lx264 -lz -lbz2 -liconv
    -lavformat -lavutil -lavcodec -lswresample -lavfilter -lswscale -lpostproc
    -lglog
    #-lstdc++ -lpthread -lm -lvpx -lmp3lame -lopenjp2 -llzma -gflags
    )
include_directories(src)
aux_source_directory(src stream)

add_executable(test main.cpp ${stream})
target_link_libraries(test ${COMMON_LIB})

