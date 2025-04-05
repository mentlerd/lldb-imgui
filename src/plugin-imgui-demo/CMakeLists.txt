set(target plugin-imgui-demo)

add_library(${target} MODULE
	src/Plugin.cpp
)
target_link_libraries(${target} PRIVATE
	lldb-imgui
)

set_target_properties(${target} PROPERTIES
	XCODE_GENERATE_SCHEME YES
)
