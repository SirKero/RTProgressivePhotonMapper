from falcor import *

def render_graph_StochPPM():
    g = RenderGraph('StochPPM')
    loadRenderPassLibrary('VBufferPM.dll')
    loadRenderPassLibrary('StochHashPPM.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    ToneMapper = createPass('ToneMapper', {'outputSize': IOSize.Default, 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': ToneMapOp.Aces, 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': ExposureMode.AperturePriority})
    g.addPass(ToneMapper, 'ToneMapper')
    VBufferPM = createPass('VBufferPM', {'outputSize': IOSize.Default, 'samplePattern': 4, 'sampleCount': 64, 'useAlphaTest': True, 'adjustShadingNormals': True})
    g.addPass(VBufferPM, 'VBufferPM')
    StochHashPPM = createPass('StochHashPPM')
    g.addPass(StochHashPPM, 'StochHashPPM')
    g.addEdge('StochHashPPM.PhotonImage', 'ToneMapper.src')
    g.addEdge('VBufferPM.vbuffer', 'StochHashPPM.vbuffer')
    g.addEdge('VBufferPM.viewW', 'StochHashPPM.viewW')
    g.addEdge('VBufferPM.throughput', 'StochHashPPM.thp')
    g.addEdge('VBufferPM.emissive', 'StochHashPPM.emissive')
    g.markOutput('ToneMapper.dst')
    return g

StochPPM = render_graph_StochPPM()
try: m.addGraph(StochPPM)
except NameError: None
