set(target Plugin)

find_package(llvm REQUIRED config LLVMSupport)

add_library(${target} SHARED
	src/PathTree.h
	src/Plugin.cpp
)

set_target_properties(${target} PROPERTIES
	XCODE_GENERATE_SCHEME YES
	XCODE_ATTRIBUTE_LINKER_DISPLAYS_MANGLED_NAMES YES
)

target_link_libraries(${target} PRIVATE
	HostXcode
	LLVMSupport
)
