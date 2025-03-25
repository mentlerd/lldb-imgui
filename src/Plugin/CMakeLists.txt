set(target Plugin)

add_library(${target} SHARED
	src/Dump.h

	src/Cache.h
	src/Cache.cpp

	src/Functional.h
	src/PathTree.h

	src/LLDB_Private.h
	src/LLDB_Private.cpp

	src/Plugin.cpp
)

set_target_properties(${target} PROPERTIES
	XCODE_GENERATE_SCHEME YES

	# Useful to turn on when debugging LLDB_Private.cpp linkage issues
	XCODE_ATTRIBUTE_LINKER_DISPLAYS_MANGLED_NAMES NO
)

target_link_libraries(${target} PRIVATE
	HostXcode
)
