from falcor import *

def render_graph_RTPM():
    g = RenderGraph('RTPM')
    loadRenderPassLibrary('VBufferPM.dll')
    loadRenderPassLibrary('RTPhotonMapper.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    VBufferPM = createPass('VBufferPM', {'outputSize': IOSize.Default, 'samplePattern': 4, 'sampleCount': 64, 'useAlphaTest': True, 'adjustShadingNormals': True})
    g.addPass(VBufferPM, 'VBufferPM')
    RTPhotonMapper = createPass('RTPhotonMapper')
    g.addPass(RTPhotonMapper, 'RTPhotonMapper')
    ToneMapper = createPass('ToneMapper', {'outputSize': IOSize.Default, 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': ToneMapOp.Aces, 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': ExposureMode.AperturePriority})
    g.addPass(ToneMapper, 'ToneMapper')
    g.addEdge('RTPhotonMapper.PhotonImage', 'ToneMapper.src')
    g.addEdge('VBufferPM.vbuffer', 'RTPhotonMapper.vbuffer')
    g.addEdge('VBufferPM.throughput', 'RTPhotonMapper.thp')
    g.addEdge('VBufferPM.emissive', 'RTPhotonMapper.emissive')
    g.addEdge('VBufferPM.viewW', 'RTPhotonMapper.viewW')
    g.markOutput('ToneMapper.dst')
    return g

RTPM = render_graph_RTPM()
try: m.addGraph(RTPM)
except NameError: None
