set(target HostXcode)

find_package(llvm REQUIRED config LLVMSupport)

add_library(${target} SHARED
	include/CocoaWrapper.h
	include/DebugWindow.h
	include/LLDB_Exposer.h
	include/Logging.h
	include/Registry.h

	include/Debuggable.h
	src/Debuggable.cpp

	src/CocoaWrapper.mm
	src/LLDB_Exposer.cpp
	src/Logging.cpp

	src/MainThreadHijacker.h
	src/MainThreadHijacker.cpp

	src/HostXcode.mm
)
target_include_directories(${target} PUBLIC
	"include"
)

set_target_properties(${target} PROPERTIES
	XCODE_GENERATE_SCHEME YES
)

target_link_libraries(${target} 
	PUBLIC
		XcodeLLDB
		ImGui
		LLVMSupport

	PRIVATE
		ImGui_Backend

		"-framework Cocoa"
		"-framework GameController"
		"-framework Metal"
		"-framework MetalKit"
)
