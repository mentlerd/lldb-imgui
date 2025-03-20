
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
	"${ImGui_ROOT}/backends/imgui_impl_osx.h"
	"${ImGui_ROOT}/backends/imgui_impl_osx.mm"
	"${ImGui_ROOT}/backends/imgui_impl_metal.h"	
	"${ImGui_ROOT}/backends/imgui_impl_metal.mm"
)
target_include_directories(ImGui_Backend SYSTEM PUBLIC
	"${ImGui_ROOT}/backends"
)
target_link_libraries(ImGui_Backend PUBLIC
	ImGui
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
get_property(targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)

foreach(target ${targets})
	set_target_properties(${target} PROPERTIES FOLDER "Libs")
endforeach()
