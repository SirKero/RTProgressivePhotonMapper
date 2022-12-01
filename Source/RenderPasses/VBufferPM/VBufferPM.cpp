/***************************************************************************
 # Copyright (c) 2015-21, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "VBufferPM.h"
 //Needed from Falcor to load the lib
#include "RenderGraph/RenderPassStandardFlags.h"

//Sample Generators
#include "Utils/SampleGenerators/DxSamplePattern.h"
#include "Utils/SampleGenerators/HaltonSamplePattern.h"
#include "Utils/SampleGenerators/StratifiedSamplePattern.h"

const RenderPass::Info VBufferPM::kInfo{ "VBufferPM", "VBuffer for the Photon Mapper passes. Like a regular VBuffer but it traces the path until a diffuse surface was hit." };

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary & lib)
{
    lib.registerPass(VBufferPM::kInfo, VBufferPM::create);
}

namespace
{
    //Metadata
    const char kShader[] = "RenderPasses/VBufferPM/VBufferPM.rt.slang";

    //Ray Tracing Program data
    const uint32_t kMaxPayloadSizeBytes = 64u;
    const uint32_t kMaxRecursionDepth = 2u;


    //Output channels
    const std::string kVBufferName = "vbuffer";
    const std::string kVBufferShaderName = "gVBuffer";
    const std::string kVBufferDesc = "V-Buffer in packed format (indices + barycentrics)";

    const ChannelList kOutputChannels = {
        { "viewW",          "gViewWorld",       "World View Direction",                     false , ResourceFormat::RGBA32Float },
        { "throughput",     "gThp",             "Throughput for transparent materials",     false , ResourceFormat::RGBA32Float },
        { "emissive",       "gEmissive",        "Emissive color",                           false , ResourceFormat::RGBA32Float },
    };

    //Additional output channels
    const ChannelList kExtraOutputChannels =
    {
        { "linearDepth",          "gLinDepth",           "Distance from camera to hitpoint",  true /* optional */, ResourceFormat::R32Float    },
        { "mvec",                 "gMVec",    "Motion vector",                    true /* optional */, ResourceFormat::RG32Float   },
    };

    // UI variables.
    const Gui::DropdownList kSamplePatternList =
    {
        { VBufferPM::SamplePattern::Center, "Center" },
        { VBufferPM::SamplePattern::DirectX, "DirectX" },
        { VBufferPM::SamplePattern::Halton, "Halton" },
        { VBufferPM::SamplePattern::Stratified, "Stratified" },
        { VBufferPM::SamplePattern::RandomUniform, "RandomUniform"}
    };

    // Scripting options.
    const char kOutputSize[] = "outputSize";
    const char kFixedOutputSize[] = "fixedOutputSize";
    const char kSamplePattern[] = "samplePattern";
    const char kSampleCount[] = "sampleCount";
    const char kUseAlphaTest[] = "useAlphaTest";
    const char kAdjustShadingNormals[] = "adjustShadingNormals";
    const char kSpecRoughCutoff[] = "specRoughCutoff";
}

VBufferPM::SharedPtr VBufferPM::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new VBufferPM(dict));
    return pPass;
}

VBufferPM::VBufferPM(const Dictionary& dict)
    : RenderPass(kInfo)
{
    parseDictionary(dict);
    mpSampleGenerator = SampleGenerator::create(SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);
}

void VBufferPM::parseDictionary(const Dictionary& dict)
{
    for (const auto& [key, value] : dict)
    {
        if (key == kOutputSize) mOutputSizeSelection = value;
        else if (key == kFixedOutputSize) mFixedOutputSize = value;
        else if (key == kSamplePattern) mSamplePattern = value;
        else if (key == kSampleCount) mSampleCount = value;
        else if (key == kUseAlphaTest) mUseAlphaTest = value;
        else if (key == kAdjustShadingNormals) mAdjustShadingNormals = value;
        else if (key == kSpecRoughCutoff) mSpecRoughCutoff = value;
        // TODO: Check for unparsed fields
    }
}

Dictionary VBufferPM::getScriptingDictionary()
{
    Dictionary dict;
    dict[kOutputSize] = mOutputSizeSelection;
    if (mOutputSizeSelection == RenderPassHelpers::IOSize::Fixed) dict[kFixedOutputSize] = mFixedOutputSize;
    dict[kSamplePattern] = mSamplePattern;
    dict[kSpecRoughCutoff] = mSpecRoughCutoff;
    dict[kSampleCount] = mSampleCount;
    dict[kUseAlphaTest] = mUseAlphaTest;
    dict[kAdjustShadingNormals] = mAdjustShadingNormals;

    return dict;
}

RenderPassReflection VBufferPM::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;

    //output for vBuffer
    const uint2 sz = RenderPassHelpers::calculateIOSize(mOutputSizeSelection, mFixedOutputSize, compileData.defaultTexDims);
    // Add the required output. This always exists.
    reflector.addOutput(kVBufferName, kVBufferDesc).bindFlags(Resource::BindFlags::UnorderedAccess).format(mVBufferFormat).texture2D(sz.x, sz.y);
    addRenderPassOutputs(reflector, kOutputChannels);
    //add the extra outputs
    addRenderPassOutputs(reflector, kExtraOutputChannels);

    return reflector;
}

void VBufferPM::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    /// Update refresh flag if options that affect the output have changed.
    auto& dict = renderData.getDictionary();
    if (mOptionsChanged) {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionsChanged = false;
        mResetConstantBuffers = true;
    }
    //Get VBuffer
    auto pVBuff = renderData[kVBufferName]->asTexture();
    FALCOR_ASSERT(pVBuff);

    //clear all output images
    auto clear = [&](const ChannelDesc& channel)
    {
        auto pTex = renderData.getTexture(channel.name);
        if (pTex) pRenderContext->clearUAV(pTex->getUAV().get(), float4(0.f));
    };
    pRenderContext->clearUAV(pVBuff->getUAV().get(), float4(0.f));
    for (const auto& channel : kOutputChannels) clear(channel);
    for (const auto& channel : kExtraOutputChannels) clear(channel);


    //If we have no scene just return
    if (!mpScene) {

        return;
    }


    if (is_set(mpScene->getUpdates(), Scene::UpdateFlags::GeometryChanged))
        throw std::runtime_error("This render pass does not support scene geometry changes. Aborting.");

    //On start set the jitters sample generator
    if (mFrameCount == 0)
        updateSamplePattern();

    //Check if camera jitter sample gen needs to be set
    if (mFrameDim != renderData.getDefaultTextureDims() || mJitterGenChanged) {
        setCameraJitter(renderData.getDefaultTextureDims());
        mJitterGenChanged = false;
    }

    // Specialize the program
    // These defines should not modify the program vars. Do not trigger program vars re-creation.
    mTracer.pProgram->addDefines(getValidResourceDefines(kExtraOutputChannels, renderData));    //Valid defines for extra channels
    mTracer.pProgram->addDefine("COMPUTE_DEPTH_OF_FIELD", mComputeDOF ? "1" : "0");
    // Prepare program vars. This may trigger shader compilation.
    // The program should have all necessary defines set at this point.

    if (!mTracer.pVars)
        prepareVars();
    FALCOR_ASSERT(mTracer.pVars);

    // Set constants.
    auto var = mTracer.pVars->getRootVar();
    std::string bufName = "PerFrame";
    var[bufName]["gFrameCount"] = mFrameCount;

    if (mResetConstantBuffers) {
        bufName = "CB";
        var[bufName]["gMaxRecursion"] = mRecursionDepth;
        var[bufName]["gSpecularRougnessCutoff"] = mSpecRoughCutoff;
        var[bufName]["gAdjustShadingNormals"] = mAdjustShadingNormals;
        var[bufName]["gUseAlphaTest"] = mUseAlphaTest;
        var[bufName]["gUseRandomPixelPosCamera"] = mCameraUseRandomSample;
    }

    // Bind Output Textures. These needs to be done per-frame as the buffers may change anytime.
    auto bindAsTex = [&](const ChannelDesc& desc)
    {
        if (!desc.texname.empty())
        {
            var[desc.texname] = renderData.getTexture(desc.name);
        }
    };

    var[kVBufferShaderName] = renderData[kVBufferName]->asTexture();
    for (auto& output : kOutputChannels) bindAsTex(output);
    for (auto& output : kExtraOutputChannels) bindAsTex(output);

    // Get dimensions of ray dispatch.
    FALCOR_ASSERT(mFrameDim.x > 0 && mFrameDim.y > 0);

    // Trace the Scene
    mpScene->raytrace(pRenderContext, mTracer.pProgram.get(), mTracer.pVars, uint3(mFrameDim, 1));

    mFrameCount++;
    if (mResetConstantBuffers) mResetConstantBuffers = false;
}

void VBufferPM::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    //Clear data from previous scenne
    //Rt program should be recreated
    mTracer = RayTraceProgramHelper::create();
    mResetConstantBuffers = true;
    mpScene = pScene;
    mFrameCount = 0;

    if (mpScene) {
        if (mpScene->hasGeometryType(Scene::GeometryType::Custom))
        {
            logWarning("This render pass only supports triangles. Other types of geometry will be ignored.");
        }

        //Create Ray Tracing Program
        RtProgram::Desc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShader);
        desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
        desc.addTypeConformances(mpScene->getTypeConformances());
        desc.setMaxAttributeSize(pScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);

        mTracer.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
        auto& sbt = mTracer.pBindingTable;
        //sbt->setRayGen(desc.addRayGen("rayGen", mpScene->getMaterialSystem()->getTypeConformances()));
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("miss"));

        //TODO: Support more of Falcors geometry types

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh)) {
            sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
        }



        mTracer.pProgram = RtProgram::create(desc, mpScene->getSceneDefines());
    }
}

void VBufferPM::renderUI(Gui::Widgets& widget)
{
    // Controls for output size.
    // When output size requirements change, we'll trigger a graph recompile to update the render pass I/O sizes.
    if (widget.dropdown("Output size", RenderPassHelpers::kIOSizeList, (uint32_t&)mOutputSizeSelection)) requestRecompile();
    if (mOutputSizeSelection == RenderPassHelpers::IOSize::Fixed)
    {
        if (widget.var("Size in pixels", mFixedOutputSize, 32u, 16384u)) requestRecompile();
    }

    //Recursion Settings
    mOptionsChanged |= widget.slider("Max Recursion Depth", mRecursionDepth, 1u, 32u);
    widget.tooltip("Maximum path length for Photon Bounces");

    mOptionsChanged |= widget.var("SpecRoughCutoff", mSpecRoughCutoff, 0.0f, 1.0f, 0.01f);
    widget.tooltip("The cutoff for Diffuse amd Specular Materials. All Reflections above this threshold are considered diffuse, does not apply to transmissive materials.\n 0 - always take first hit (except for transmissive materials). \n 1 - Only use bsdf lobes");

    // Sample pattern controls.
    bool updatePattern = widget.dropdown("Sample pattern", kSamplePatternList, (uint32_t&)mSamplePattern);
    widget.tooltip("Selects sample pattern for anti-aliasing over multiple frames.\n\n"
                   "The camera jitter is set at the start of each frame based on the chosen pattern. All render passes should see the same jitter.\n"
                   "\'Center\' disables anti-aliasing by always sampling at the center of the pixel.\n"
                   "\'Random Uniform\' does not support Motion Vectors", true);
    if (mSamplePattern != SamplePattern::Center && mSamplePattern != SamplePattern::RandomUniform)
    {
        updatePattern |= widget.var("Sample count", mSampleCount, 1u);
        widget.tooltip("Number of samples in the anti-aliasing sample pattern.", true);
    }
    if (updatePattern)
    {
        updateSamplePattern();
    }

    mOptionsChanged |= widget.checkbox("Use Alpha Test", mUseAlphaTest);
    widget.tooltip("Enables Alpha Test for the VBuffer");

    mOptionsChanged |= widget.checkbox("Adjust Shading Normals", mAdjustShadingNormals);
    widget.tooltip("Adjusts the shading normals to prevent invalid pixels at the edge of specular/transparent materials");
}

void VBufferPM::prepareVars()
{
    FALCOR_ASSERT(mTracer.pProgram);

    //Configure Program
    mTracer.pProgram->addDefines(mpSampleGenerator->getDefines());
    mTracer.pProgram->setTypeConformances(mpScene->getTypeConformances());

    // Create program variables for the current program.
    // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
    mTracer.pVars = RtProgramVars::create(mTracer.pProgram, mTracer.pBindingTable);

    // Bind utility classes into shared data.
    auto var = mTracer.pVars->getRootVar();
    mpSampleGenerator->setShaderData(var);
}

static CPUSampleGenerator::SharedPtr createSamplePattern(uint32_t type, uint32_t sampleCount)
{
    switch (type)
    {
    case VBufferPM::SamplePattern::Center:
    case VBufferPM::SamplePattern::RandomUniform:
        return nullptr;
    case VBufferPM::SamplePattern::DirectX:
        return DxSamplePattern::create(sampleCount);
    case VBufferPM::SamplePattern::Halton:
        return HaltonSamplePattern::create(sampleCount);
    case VBufferPM::SamplePattern::Stratified:
        return StratifiedSamplePattern::create(sampleCount);
    default:
        FALCOR_UNREACHABLE();
        return nullptr;
    }
}

void VBufferPM::setCameraJitter(const uint2 frameDim)
{
    FALCOR_ASSERT(frameDim.x > 0 && frameDim.y > 0);
    mFrameDim = frameDim;
    float2 mInvFrameDim = 1.f / float2(frameDim);

    // Update sample generator for camera jitter.
    if (mpScene) mpScene->getCamera()->setPatternGenerator(mpCameraJitterSampleGenerator, mInvFrameDim);
}

void VBufferPM::updateSamplePattern()
{
    mpCameraJitterSampleGenerator = createSamplePattern(mSamplePattern, mSampleCount);
    if (mpCameraJitterSampleGenerator) mSampleCount = mpCameraJitterSampleGenerator->getSampleCount();

    //Set the random sample boolean
    mCameraUseRandomSample = (mSamplePattern == SamplePattern::RandomUniform);
    mResetConstantBuffers = true;
    mJitterGenChanged = true;
}
