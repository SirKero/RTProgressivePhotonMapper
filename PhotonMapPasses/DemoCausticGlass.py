# Graphs
from falcor import *

def render_graph_RTPM():
    g = RenderGraph('RTPM')
    loadRenderPassLibrary('VBufferPM.dll')
    loadRenderPassLibrary('ToneMapper.dll')
    loadRenderPassLibrary('RTPhotonMapper.dll')
    VBufferPM = createPass('VBufferPM', {'outputSize': IOSize.Default, 'samplePattern': 4, 'specRoughCutoff': 0.5, 'sampleCount': 64, 'useAlphaTest': True, 'adjustShadingNormals': True})
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
m.addGraph(render_graph_RTPM())

# Scene
m.loadScene('../Scenes/caustic-glass/caustic_glass.pyscene')
m.scene.renderSettings = SceneRenderSettings(useEnvLight=True, useAnalyticLights=True, useEmissiveLights=True, useGridVolumes=True)
m.scene.camera.position = float3(-5.500000,7.000000,-5.500000)
m.scene.camera.target = float3(-4.750000,2.250000,0.000000)
m.scene.camera.up = float3(0.000000,1.000000,0.000000)
m.scene.cameraSpeed = 1.0

# Window Configuration
m.resizeSwapChain(1920, 1080)
m.ui = True

# Clock Settings
m.clock.time = 0
m.clock.framerate = 0
# If framerate is not zero, you can use the frame property to set the start frame
# m.clock.frame = 0

# Frame Capture
m.frameCapture.outputDir = '.'
m.frameCapture.baseFilename = 'Mogwai'

