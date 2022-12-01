add_renderpass(HashPPM)

target_sources(HashPPM PRIVATE
    HashPPM.cpp
    HashPPM.h
	PhotonMapperHashCollect.cs.slang
	PhotonMapperHashFunctions.slang
	PhotonMapperHashGenerate.rt.slang
)

target_copy_shaders(HashPPM RenderPasses/HashPPM)

target_source_group(HashPPM "RenderPasses")
