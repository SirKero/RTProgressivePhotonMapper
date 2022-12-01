add_renderpass(RTPhotonMapper)

target_sources(RTPhotonMapper PRIVATE
    RTPhotonMapper.cpp
    RTPhotonMapper.h
	PhotonCulling.cs.slang
	PhotonCullingHash.slang
	PhotonMapperCollect.rt.slang
	PhotonMapperGenerate.rt.slang
	PhotonMapperStochasticCollect.rt.slang
	showPhotonAccelerationStructure.rt.slang
)

target_copy_shaders(PathTracer RenderPasses/RTPhotonMapper)

target_source_group(RTPhotonMapper "RenderPasses")
