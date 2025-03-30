cmake_minimum_required(VERSION 3.31)

project(lldb-imgui LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

# LLVM's tablegen targets can get really out-of-control
set(CMAKE_XCODE_GENERATE_SCHEME NO)

# Order targets neatly into folders in IDEs which support it
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

add_subdirectory(libs)

add_subdirectory(src/lldb-imgui)

add_subdirectory(src/AppDummy)
add_subdirectory(src/HostXcode)
add_subdirectory(src/Plugin)

# While technically not related, it is very useful to keep the host
# and plugin libs up-to-date for each dummy app launch
add_dependencies(AppDummy HostXcode Plugin)
