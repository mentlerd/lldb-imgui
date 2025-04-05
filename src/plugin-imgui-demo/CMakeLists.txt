set(target plugin-imgui-demo)

add_library(${target} MODULE
	src/Plugin.cpp
)
target_link_libraries(${target} PRIVATE
	lldb-imgui
)

# TODO: Figure out what to do about this.. When loading a plugin into the RPC
#  server it doesn't find lldb-imgui symbols because of the symbol ordinal
#  settings, which specify to look for them in the main executable specifically:
#
# > dyld_info -fixups plugin-imgui-demo/Debug/libplugin-imgui-demo.so | grep _ZN10ImDrawList10AddEllipseERK6ImVec2S2
# >  __DATA_CONST    __got            0x00048000           bind  <main-executable>/__ZN10ImDrawList10AddEllipseERK6ImVec2S2_jfif
#
# Flat namespace is claimed to be slower, and more fragile. One option would be to
#  divide the main executable into a separate app, and code dylib so plugins can
#  link against the core, but this could result in plugins listing the core as a
#  dependency.. Not sure if that is a problem?
#
# Either way, this works in the short term to allow the RPC to load plugins built
#  against the standalone executable
target_link_options(${target} PRIVATE
    "-flat_namespace"
)

set_target_properties(${target} PROPERTIES
	XCODE_GENERATE_SCHEME YES
)
