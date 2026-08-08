add_library(imguiImpl
	imgui_extension.cpp
	imgui_extension.h
)

if (ENABLE_VULKAN)
	target_sources(imguiImpl PRIVATE
		imgui_impl_vulkan.cpp
		imgui_impl_vulkan.h
	)
endif()

if (ENABLE_METAL)
    target_sources(imguiImpl PRIVATE
        imgui_impl_metal.mm
        imgui_impl_metal.h
    )

    target_compile_definitions(imguiImpl PRIVATE IMGUI_IMPL_METAL_CPP)
endif ()

set_property(TARGET imguiImpl PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

cemu_use_precompiled_header(imguiImpl)

target_include_directories(imguiImpl PUBLIC "../")

target_link_libraries(imguiImpl
	PUBLIC
		spatial::foundation_imgui
	PRIVATE
	CemuCommon
	CemuGui
	CemuInput
	pugixml::pugixml
)
