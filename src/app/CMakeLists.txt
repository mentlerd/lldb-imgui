set(target app)

add_executable(${target}
	dummy.cpp

    AppIcon.svg
)
target_link_libraries(${target} PRIVATE
    lldb-imgui
)
set_target_properties(${target} PROPERTIES
	MACOSX_BUNDLE TRUE

    MACOSX_BUNDLE_GUI_IDENTIFIER "com.mentlerd.lldb-imgui.app"
	MACOSX_BUNDLE_BUNDLE_VERSION "0.1"

    MACOSX_BUNDLE_BUNDLE_NAME "LLDB ImGui"
    MACOSX_BUNDLE_COPYRIGHT "Copyright © 2025 David Mentler"

    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.mentlerd.lldb-imgui.app"
	XCODE_GENERATE_SCHEME YES
)

#
# MacOS bundles require a special icon format. The OS caches these images, so if you
# edit the icon you'll probably need to flush the cache, and restart the Dock:
#
# > sudo rm -r /Library/Caches/com.apple.iconservices.store
# > killall Dock
#
if (CMAKE_SYSTEM_NAME MATCHES "Darwin")
    find_program(inkscape "inkscape" REQUIRED)
    find_program(iconutil "iconutil" REQUIRED)

    set(icns "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns")
    set(iconset "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.iconset")

    set(icon_sizes
        16,16x16
        32,16x16@2x
        32,32x32
        64,32x32@2x
        128,128x128
        256,128x128@2x
        256,256x256
        512,256x256@2x
        512,512x512
        1024,512x512@2x
    )
    foreach(icon IN LISTS icon_sizes)
        string(REPLACE "," ";" icon ${icon})

        list(GET icon 0 icon_size)
        list(GET icon 1 icon_name)

        set(icon_svg "${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.svg")
        set(icon_png "${iconset}/icon_${icon_name}.png")

        add_custom_command(
            MAIN_DEPENDENCY "${icon_svg}"
            OUTPUT "${icon_png}"
            COMMAND "${inkscape}" 
                --export-type png
                --export-filename "${icon_png}"
                --export-area-page 
                --export-width "${icon_size}"
                --export-height "${icon_size}"
                "${icon_svg}" 
            VERBATIM
        )
        list(APPEND icon_pngs "${icon_png}")
    endforeach()

    add_custom_command(
        MAIN_DEPENDENCY "${icon_svg}"
        DEPENDS ${icon_pngs}
        OUTPUT "${icns}"
        COMMAND "${iconutil}" -c icns -o "${icns}" "${iconset}"
        VERBATIM
    )

    target_sources(${target} PRIVATE 
        "${icns}"
    )
    set_target_properties(${target} PROPERTIES 
        MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
        RESOURCE "${icns}"
    )
endif()
