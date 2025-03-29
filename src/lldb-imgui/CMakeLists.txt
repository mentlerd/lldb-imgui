set(target lldb-imgui)

find_package(OpenGL REQUIRED)
find_package(SDL3 REQUIRED)

add_library(${target} SHARED
    src/App.h
    src/App.cpp

    src/MainSDL.cpp
)
target_link_libraries(${target} 
	PRIVATE
        spdlog
		ImGui

		SDL3::SDL3
)

# Include support to act as an LLDB plugin in Xcode's lldb-rpc-server
if (CMAKE_SYSTEM_NAME MATCHES "Darwin")
	target_sources(${target} PRIVATE src/MainRPCServer.cpp)
endif()

# ImGui backend
target_include_directories(${target} PRIVATE
	"${CMAKE_SOURCE_DIR}/libs/imgui/backends"
)
target_sources(${target} PRIVATE
	"${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_sdl3.h"
	"${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_sdl3.cpp"
	"${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_sdlgpu3.h"
	"${CMAKE_SOURCE_DIR}/libs/imgui/backends/imgui_impl_sdlgpu3.cpp"
)

set_target_properties(${target} PROPERTIES
	XCODE_GENERATE_SCHEME YES
)
