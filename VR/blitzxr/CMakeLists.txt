# BlitzXR userlib. Must be built as 32-bit (x86 / Win32) to match BlitzX3D.
#   Visual Studio:  cmake -S . -B build -A Win32 && cmake --build build --config Release
#   MinGW (Linux):  ./build_mingw.sh
cmake_minimum_required(VERSION 3.16)
project(BlitzXR CXX)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "BlitzXR must be built as 32-bit (use -A Win32)")
endif()
add_library(BlitzXR SHARED src/BlitzXR.cpp BlitzXR.def)
target_include_directories(BlitzXR PRIVATE include)
target_compile_features(BlitzXR PRIVATE cxx_std_17)
target_link_libraries(BlitzXR PRIVATE d3d9 d3d11 dxgi)
set_target_properties(BlitzXR PROPERTIES PREFIX "")
if(MSVC)
  target_compile_options(BlitzXR PRIVATE /W3)
  set_property(TARGET BlitzXR PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
  target_link_options(BlitzXR PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
endif()
