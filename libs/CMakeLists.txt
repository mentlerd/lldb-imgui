
find_package(SDL3 REQUIRED)

# =====/ spdlog /========================================
add_subdirectory(spdlog)

# =====/ ImGui /========================================

set(ImGui_ROOT "${CMAKE_SOURCE_DIR}/libs/imgui")

file(GLOB ImGui_FILES CONFIGURE_DEPENDS 
	"${ImGui_ROOT}/*.h"
	"${ImGui_ROOT}/*.cpp"
)

add_library(ImGui STATIC
	${ImGui_FILES}
)
target_include_directories(ImGui SYSTEM PUBLIC 
	"${ImGui_ROOT}"
)

add_library(ImGui_Backend STATIC
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_sdl3.h"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_sdl3.cpp"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_sdlgpu3.h"
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_sdlgpu3.cpp"
)
target_include_directories(ImGui_Backend PUBLIC
    "${CMAKE_SOURCE_DIR}/libs/imgui/backends"
)
target_link_libraries(ImGui_Backend PUBLIC
    ImGui
    SDL3::SDL3
)

# =====/ Xcode's LLDB.framework /========================================

# LLDB.framework no longer contains public headers for the SB API, but the symbols are still exported. 
# Use the installed LLVM headers, and be very careful :)
find_package(llvm REQUIRED config)

add_library(XcodeLLDB INTERFACE)

target_include_directories(XcodeLLDB INTERFACE
	"${LLVM_INCLUDE_DIRS}"
)
target_link_libraries(XcodeLLDB INTERFACE 
	"/Applications/Xcode_16.2.app/Contents/SharedFrameworks/LLDB.framework/Versions/A/LLDB"
)


# Move all targets added into a separate IDE directory
macro(_tweakTargetsRecursive dir)
    get_property(subdirectories DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(subdir IN LISTS subdirectories)
        _tweakTargetsRecursive("${subdir}")
    endforeach()

    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    file(RELATIVE_PATH rdir "${CMAKE_CURRENT_SOURCE_DIR}" "${dir}/..")

    foreach(target ${targets})
    	set_target_properties(${target} PROPERTIES FOLDER "Libs")
    endforeach()
endmacro()

_tweakTargetsRecursive("${CMAKE_CURRENT_SOURCE_DIR}")
