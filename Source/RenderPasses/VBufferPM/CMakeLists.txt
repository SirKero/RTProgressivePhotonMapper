add_renderpass(VBufferPM)

target_sources(VBufferPM PRIVATE
    VBufferPM.cpp
    VBufferPM.h
	VBufferPM.rt.slang
)
target_copy_shaders(VBufferPM RenderPasses/VBufferPM)

target_source_group(VBufferPM "RenderPasses")
