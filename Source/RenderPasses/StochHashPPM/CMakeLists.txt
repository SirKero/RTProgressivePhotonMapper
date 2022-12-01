add_renderpass(StochHashPPM)

target_sources(StochHashPPM PRIVATE
    StochHashPPM.cpp
    StochHashPPM.h
	PhotonMapperStochasticHashCollect.cs.slang
	PhotonMapperStochasticHashFunctions.slang
	PhotonMapperStochasticHashGenerate.rt.slang
)

target_copy_shaders(StochHashPPM RenderPasses/StochHashPPM)

target_source_group(StochHashPPM "RenderPasses")
