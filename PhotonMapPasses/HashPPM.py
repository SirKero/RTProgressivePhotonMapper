from falcor import *

def render_graph_HashPPM():
    g = RenderGraph('HashPPM')
    loadRenderPassLibrary('VBufferPM.dll')
    loadRenderPassLibrary('HashPPM.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    VBufferPM = createPass('VBufferPM', {'outputSize': IOSize.Default, 'samplePattern': 4, 'sampleCount': 64, 'useAlphaTest': True, 'adjustShadingNormals': True})
    g.addPass(VBufferPM, 'VBufferPM')
    HashPPM = createPass('HashPPM')
    g.addPass(HashPPM, 'HashPPM')
    ToneMapper = createPass('ToneMapper', {'outputSize': IOSize.Default, 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': ToneMapOp.Aces, 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': ExposureMode.AperturePriority})
    g.addPass(ToneMapper, 'ToneMapper')
    g.addEdge('VBufferPM.vbuffer', 'HashPPM.vbuffer')
    g.addEdge('VBufferPM.viewW', 'HashPPM.viewW')
    g.addEdge('VBufferPM.throughput', 'HashPPM.thp')
    g.addEdge('VBufferPM.emissive', 'HashPPM.emissive')
    g.addEdge('HashPPM.PhotonImage', 'ToneMapper.src')
    g.markOutput('ToneMapper.dst')
    return g

HashPPM = render_graph_HashPPM()
try: m.addGraph(HashPPM)
except NameError: None
