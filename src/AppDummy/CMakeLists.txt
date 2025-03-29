set(target AppDummy)

add_executable(${target}
	main.cpp
)
set_target_properties(${target} PROPERTIES
	XCODE_GENERATE_SCHEME YES
)
