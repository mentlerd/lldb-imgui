set(target Plugin)

add_library(${target} SHARED
	src/Cache.h
	src/Functional.h
	src/PathTree.h

	src/LLDB_Private.h
	src/LLDB_Private.cpp

	src/Plugin.cpp
)

set_target_properties(${target} PROPERTIES
	XCODE_GENERATE_SCHEME YES
	XCODE_ATTRIBUTE_LINKER_DISPLAYS_MANGLED_NAMES YES
)

target_link_libraries(${target} PRIVATE
	HostXcode
)
