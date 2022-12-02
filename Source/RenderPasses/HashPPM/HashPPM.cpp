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
#include "HashPPM.h"
 //Needed from Falcor to load the lib
#include "RenderGraph/RenderPassLibrary.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

//for random seed generation
#include <random>
#include <ctime>
#include <limits>
#include <fstream>

constexpr float kUint32tMaxF = float((uint32_t)-1);

const RenderPass::Info PhotonMapperHash::kInfo{"HashPPM", "Progressive Photon Mapper based on a Hash Grid" };

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary & lib)
{
    lib.registerPass(PhotonMapperHash::kInfo, PhotonMapperHash::create);
}

namespace
{
    const char kShaderGeneratePhoton[] = "RenderPasses/HashPPM/PhotonMapperHashGenerate.rt.slang";
    const char kShaderCollectPhoton[] = "RenderPasses/HashPPM/PhotonMapperHashCollect.cs.slang";

    // Ray tracing settings that affect the traversal stack size.
   // These should be set as small as possible.
   //TODO: set them later to the right vals
    const uint32_t kMaxPayloadSizeBytes = 64u;
    const uint32_t kMaxPayloadSizeBytesCollect = 32u;
    const uint32_t kMaxAttributeSizeBytes = 8u;
    const uint32_t kMaxRecursionDepth = 2u;

    const ChannelList kInputChannels =
    {
        {"vbuffer",             "gVBuffer",                 "V Buffer to get the intersected triangle",         false},
        {"viewW",               "gViewWorld",               "World View Direction",                             false},
        {"thp",                 "gThp",                     "Throughput",                                       false},
        {"emissive",            "gEmissive",                "Emissive",                                         false},
    };

    const ChannelList kOutputChannels =
    {
        { "PhotonImage",          "gPhotonImage",               "An image that shows the caustics and indirect light from global photons" , false , ResourceFormat::RGBA32Float }
    };

    const Gui::DropdownList kInfoTexDropdownList{
        //{(uint)PhotonMapperHash::TextureFormat::_8Bit , "8Bits"},
        {(uint)PhotonMapperHash::TextureFormat::_16Bit , "16Bits"},
        {(uint)PhotonMapperHash::TextureFormat::_32Bit , "32Bits"}
    };

    const Gui::DropdownList kLightTexModeList{
        {PhotonMapperHash::LightTexMode::power , "Power"},
        {PhotonMapperHash::LightTexMode::area , "Area"}
    };
}

PhotonMapperHash::SharedPtr PhotonMapperHash::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new PhotonMapperHash);
    return pPass;
}

PhotonMapperHash::PhotonMapperHash():
    RenderPass(kInfo)
{
    mpSampleGenerator = SampleGenerator::create(SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);
}

Dictionary PhotonMapperHash::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection PhotonMapperHash::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;

    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);


    return reflector;
}

void PhotonMapperHash::compile(RenderContext* pContext, const CompileData& compileData)
{
    // put reflector outputs here and create again if needed
    
}

void PhotonMapperHash::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    /// Update refresh flag if options that affect the output have changed.
    auto& dict = renderData.getDictionary();
    if (mOptionsChanged) {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        mResetIterations = true;
        mSetConstantBuffers = true;
        mOptionsChanged = false;
        mResetTimer = true;
    }

    //If we have no scene just return
    if (!mpScene)
    {
        return;
    }

    //Reset Frame Count if conditions are met
    if (mResetIterations || mAlwaysResetIterations || is_set(mpScene->getUpdates(), Scene::UpdateFlags::CameraMoved)) {
        mFrameCount = 0;
        mResetIterations = false;
        mResetTimer = true;
    }

    //Check timer and return if render time is over
    checkTimer();
    if (mUseTimer && mTimerStopRenderer) return;

    //Copy Photon Counter for UI
    copyPhotonCounter(pRenderContext);

    if (mNumPhotonsChanged) {
        changeNumPhotons();
        mNumPhotonsChanged = false;
    }
        
    //reset radius
    if (mFrameCount == 0) {
        mCausticRadius = mCausticRadiusStart;
        mGlobalRadius = mGlobalRadiusStart;
    }

    if (is_set(mpScene->getUpdates(), Scene::UpdateFlags::GeometryChanged))
    {
        throw std::runtime_error("This render pass does not support scene geometry changes. Aborting.");
    }

    // Request the light collection if emissive lights are enabled.
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        mpScene->getLightCollection(pRenderContext);
    }

    if (mResizePhotonBuffers) {
        if (mFitBuffersToPhotonShot) {
            //if size of conter is 0 wait till next iteration
            if (mPhotonCount[0] > 0 && mPhotonCount[1] > 0) {
                mCausticBufferSizeUI = static_cast<uint>(mPhotonCount[0] * 1.1);
                mGlobalBufferSizeUI = static_cast<uint>(mPhotonCount[1] * 1.1);
            }
            mFitBuffersToPhotonShot = false;
        }
        //put in new size with info tex2D height in mind
        uint causticWidth = static_cast<uint>(std::ceil(mCausticBufferSizeUI / static_cast<float>(kInfoTexHeight)));
        mCausticBuffers.maxSize = causticWidth * kInfoTexHeight;
        uint globalWidth = static_cast<uint>(std::ceil(mGlobalBufferSizeUI / static_cast<float>(kInfoTexHeight)));
        mGlobalBuffers.maxSize = globalWidth * kInfoTexHeight;

        //refresh UI to new variable
        mCausticBufferSizeUI = mCausticBuffers.maxSize; mGlobalBufferSizeUI = mGlobalBuffers.maxSize;
        mResizePhotonBuffers = false;
        mPhotonBuffersReady = false;
        mRebuildAS = true;
    }

    //Only change format if we dont rebuild the buffers
    if (mPhotonBuffersReady && mPhotonInfoFormatChanged) {
        preparePhotonInfoTexture();
        mPhotonInfoFormatChanged = false;
    }

    if (!mPhotonBuffersReady) {
        mPhotonBuffersReady = preparePhotonBuffers();
    }

    if (!mRandNumSeedBuffer) {
        prepareRandomSeedBuffer(renderData.getDefaultTextureDims());
    }

    if (mRebuildLightTex) {
        mLightSampleTex.reset();
        mRebuildLightTex = false;
    }

    //Create light sample tex if empty
    if (!mLightSampleTex) {
        createLightSampleTexture(pRenderContext);
    }

    if (mResetCS) {
        mpCSCollect.reset();
        prepareHashBuffer();
        mResetCS = false;
    }

    //
    // Generate Ray Pass
    //

    generatePhotons(pRenderContext, renderData);
    
    //Gather the photons with short rays
    collectPhotons(pRenderContext, renderData);
    mFrameCount++;

    if (mUseStatisticProgressivePM) {
        float itF = static_cast<float>(mFrameCount);
        mGlobalRadius *= sqrt((itF + mSPPMAlphaGlobal) / (itF + 1.0f));
        mCausticRadius *= sqrt((itF + mSPPMAlphaCaustic) / (itF + 1.0f));

        //Clamp to min radius

        mGlobalRadius = std::max(mGlobalRadius, kMinPhotonRadius);
        mCausticRadius = std::max(mCausticRadius, kMinPhotonRadius);
    }

    if (mSetConstantBuffers)
        mSetConstantBuffers = false;
}

void PhotonMapperHash::generatePhotons(RenderContext* pRenderContext, const RenderData& renderData)
{

    //Reset counter Buffers
    pRenderContext->copyBufferRegion(mPhotonCounterBuffer.counter.get(), 0, mPhotonCounterBuffer.reset.get(), 0, sizeof(uint64_t));
    pRenderContext->resourceBarrier(mPhotonCounterBuffer.counter.get(), Resource::State::ShaderResource);

    //Clear the photon Buffers
    pRenderContext->clearTexture(mGlobalBuffers.position.get(), float4(0, 0, 0, 0));
    pRenderContext->clearTexture(mGlobalBuffers.infoFlux.get(), float4(0, 0, 0, 0));
    pRenderContext->clearTexture(mGlobalBuffers.infoDir.get(), float4(0, 0, 0, 0));
    pRenderContext->clearTexture(mCausticBuffers.position.get(), float4(0, 0, 0, 0));
    pRenderContext->clearTexture(mCausticBuffers.infoFlux.get(), float4(0, 0, 0, 0));
    pRenderContext->clearTexture(mCausticBuffers.infoDir.get(), float4(0, 0, 0, 0));
    pRenderContext->clearUAV(mpGlobalBuckets->getUAV().get(), uint4(0, 0, 0, 0));
    pRenderContext->clearUAV(mpCausticBuckets->getUAV().get(), uint4(0, 0, 0, 0));
    

    auto lights = mpScene->getLights();
    auto lightCollection = mpScene->getLightCollection(pRenderContext);

    // Specialize the Generate program.
    // These defines should not modify the program vars. Do not trigger program vars re-creation.
    mTracerGenerate.pProgram->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    mTracerGenerate.pProgram->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    mTracerGenerate.pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    mTracerGenerate.pProgram->addDefine("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
    mTracerGenerate.pProgram->addDefine("MAX_PHOTON_INDEX_GLOBAL", std::to_string(mGlobalBuffers.maxSize));
    mTracerGenerate.pProgram->addDefine("MAX_PHOTON_INDEX_CAUSTIC", std::to_string(mCausticBuffers.maxSize));
    mTracerGenerate.pProgram->addDefine("ANALYTIC_INV_PDF", std::to_string(mAnalyticInvPdf));
    mTracerGenerate.pProgram->addDefine("INFO_TEXTURE_HEIGHT", std::to_string(kInfoTexHeight));
    mTracerGenerate.pProgram->addDefine("NUM_PHOTONS_PER_BUCKET", std::to_string(mNumPhotonsPerBucket));
    mTracerGenerate.pProgram->addDefine("NUM_BUCKETS", std::to_string(mNumBuckets));
    mTracerGenerate.pProgram->addDefine("PHOTON_FACE_NORMAL", mEnableFaceNormalRejection ? "1" : "0");
    
    // Prepare program vars. This may trigger shader compilation.
    // The program should have all necessary defines set at this point.

    if (!mTracerGenerate.pVars) prepareVars();
    FALCOR_ASSERT(mTracerGenerate.pVars);

    auto& dict = renderData.getDictionary();

    //Calculate cell size depending on the radius


    // Set buffers
    auto var = mTracerGenerate.pVars->getRootVar();

    //PerFrame Constant Buffer
    std::string nameBuf = "PerFrame";
    var[nameBuf]["gFrameCount"] = mFrameCount;
    var[nameBuf]["gCausticRadius"] = mCausticRadius;
    var[nameBuf]["gGlobalRadius"] = mGlobalRadius;
    var[nameBuf]["gCausticHashScaleFactor"] = 1.f / mCausticRadius;
    var[nameBuf]["gGlobalHashScaleFactor"] = 1.f / mGlobalRadius;

    //Constant Buffer is only set when options changed
    if (mSetConstantBuffers) {
        nameBuf = "CB";
        var[nameBuf]["gPRNGDimension"] = dict.keyExists(kRenderPassPRNGDimension) ? dict[kRenderPassPRNGDimension] : 0u;
        var[nameBuf]["gGlobalRejection"] = mRussianRoulette;
        var[nameBuf]["gEmissiveScale"] = mIntensityScalar;
        var[nameBuf]["gSpecRoughCutoff"] = mSpecRoughCutoff;

        var[nameBuf]["gMaxRecursion"] = mMaxBounces;
        var[nameBuf]["gUseAlphaTest"] = mUseAlphaTest;
        var[nameBuf]["gAdjustShadingNormals"] = mAdjustShadingNormals;
        var[nameBuf]["gQuadProbeIt"] = mQuadraticProbeIterations;
    }
    
    //set the buffers
    var["gCausticPos"] = mCausticBuffers.position;
    var["gCausticFlux"] = mCausticBuffers.infoFlux;
    var["gCausticDir"] = mCausticBuffers.infoDir;
    var["gGlobalPos"] = mGlobalBuffers.position;
    var["gGlobalFlux"] = mGlobalBuffers.infoFlux;
    var["gGlobalDir"] = mGlobalBuffers.infoDir;
    var["gRndSeedBuffer"] = mRandNumSeedBuffer;

    var["gGlobalHashBucket"] = mpGlobalBuckets;
    var["gCausticHashBucket"] = mpCausticBuckets;

    var["gPhotonCounter"] = mPhotonCounterBuffer.counter;

    //Bind light sample tex
    var["gLightSample"] = mLightSampleTex;
    var["gNumPhotonsPerEmissive"] = mPhotonsPerTriangle;

    // Get dimensions of ray dispatch.
    const uint2 targetDim = uint2(mPGDispatchX, mMaxDispatchY);
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    // Trace the photons
    mpScene->raytrace(pRenderContext, mTracerGenerate.pProgram.get(), mTracerGenerate.pVars, uint3(targetDim, 1));
}

void PhotonMapperHash::collectPhotons(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Trace the photons
    FALCOR_PROFILE("collect photons");


    if (!mpCSCollect) {
        Program::Desc desc;
        desc.addShaderLibrary(kShaderCollectPhoton).csEntry("main").setShaderModel("6_5");
        desc.addTypeConformances(mpScene->getTypeConformances());

        Program::DefineList defines;
        defines.add(mpScene->getSceneDefines());
        defines.add(mpSampleGenerator->getDefines());

        defines.add("INFO_TEXTURE_HEIGHT", std::to_string(kInfoTexHeight));
        defines.add("NUM_PHOTONS_PER_BUCKET", std::to_string(mNumPhotonsPerBucket));
        defines.add("NUM_BUCKETS", std::to_string(mNumBuckets));
        defines.add("PHOTON_FACE_NORMAL", mEnableFaceNormalRejection ? "1" : "0");

        mpCSCollect = ComputePass::create(desc, defines, true);
    }
    
    // Prepare program vars. This may trigger shader compilation.

    // Set constants.
    auto var = mpCSCollect->getRootVar();
    //Set Scene data. Is needed for shading
    mpScene->setRaytracingShaderData(pRenderContext, var, 1);
    mpSampleGenerator->setShaderData(var);

    std::string nameBuf = "PerFrame";
    var[nameBuf]["gFrameCount"] = mFrameCount;
    var[nameBuf]["gCausticRadius"] = mCausticRadius;
    var[nameBuf]["gGlobalRadius"] = mGlobalRadius;
    var[nameBuf]["gCausticHashScaleFactor"] = 1.f / mCausticRadius;
    var[nameBuf]["gGlobalHashScaleFactor"] = 1.f / mGlobalRadius;

    //Set constant buffer only if changes where made
    if (mSetConstantBuffers) {
        nameBuf = "CB";
        var[nameBuf]["gEmissiveScale"] = mIntensityScalar;
        var[nameBuf]["gCollectGlobalPhotons"] = !mDisableGlobalCollection;
        var[nameBuf]["gCollectCausticPhotons"] = !mDisableCausticCollection;
        var[nameBuf]["gQuadProbeIt"] = mQuadraticProbeIterations;
        var[nameBuf]["gEnableStochasicGathering"] = mEnableStochasticCollection;
        var[nameBuf]["gCollectProbability"] = mStochasticCollectProbability;
    }


    var["gGlobalHashBucket"] = mpGlobalBuckets;
    var["gCausticHashBucket"] = mpCausticBuckets;

    //set the buffers
    var["gCausticPos"] = mCausticBuffers.position;
    var["gCausticFlux"] = mCausticBuffers.infoFlux;
    var["gCausticDir"] = mCausticBuffers.infoDir;
    var["gGlobalPos"] = mGlobalBuffers.position;
    var["gGlobalFlux"] = mGlobalBuffers.infoFlux;
    var["gGlobalDir"] = mGlobalBuffers.infoDir;

    // Lamda for binding textures. These needs to be done per-frame as the buffers may change anytime.
    auto bindAsTex = [&](const ChannelDesc& desc)
    {
        if (!desc.texname.empty())
        {
            var[desc.texname] = renderData[desc.name]->asTexture();
        }
    };
    //Bind input and output textures
    for (auto& channel : kInputChannels) bindAsTex(channel);
    bindAsTex(kOutputChannels[0]);

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    

    FALCOR_ASSERT(pRenderContext  && mpCSCollect);

    //pRenderContext->raytrace(mTracerCollect.pProgram.get(), mTracerCollect.pVars.get(), targetDim.x, targetDim.y, 1);
    mpCSCollect->execute(pRenderContext, uint3(targetDim, 1));
}

void PhotonMapperHash::renderUI(Gui::Widgets& widget)
{
    float2 dummySpacing = float2(0, 10);
    bool dirty = false;

    //Info
    widget.text("Iterations: " + std::to_string(mFrameCount));
    widget.text("Caustic Photons: " + std::to_string(mPhotonCount[0]) + " / " + std::to_string(mCausticBuffers.maxSize));
    widget.tooltip("Photons for current Iteration / Buffer Size");
    widget.text("Global Photons: " + std::to_string(mPhotonCount[1]) + " / " + std::to_string(mGlobalBuffers.maxSize));
    widget.tooltip("Photons for current Iteration / Buffer Size");

    widget.text("Current Global Radius: " + std::to_string(mGlobalRadius));
    widget.text("Current Caustic Radius: " + std::to_string(mCausticRadius));

    widget.dummy("", dummySpacing);
    widget.var("Number Photons", mNumPhotonsUI, 1000u, UINT_MAX, 1000u);
    widget.tooltip("The number of photons that are shot per iteration. Press \"Apply\" to apply the change");
    widget.var("Size Caustic Buffer", mCausticBufferSizeUI, 1000u, UINT_MAX, 1000u);
    widget.var("Size Global Buffer", mGlobalBufferSizeUI, 1000u, UINT_MAX, 1000u);
    mNumPhotonsChanged |= widget.button("Apply");
    widget.dummy("", float2(15,0), true);
    mFitBuffersToPhotonShot |= widget.button("Fit Buffers", true);
    widget.tooltip("Fitts the Caustic and Global Buffer to current number of photons shot + 10 %");
    widget.dummy("", dummySpacing);

    //If fit buffers is triggered, also trigger the photon change routine
    mNumPhotonsChanged |= mFitBuffersToPhotonShot;  

    //Progressive PM
    dirty |= widget.checkbox("Use SPPM", mUseStatisticProgressivePM);
    widget.tooltip("Activate Statistically Progressive Photon Mapping");

    if (mUseStatisticProgressivePM) {
        dirty |= widget.var("Global Alpha", mSPPMAlphaGlobal, 0.1f, 1.0f, 0.001f);
        widget.tooltip("Sets the Alpha in SPPM for the Global Photons");
        dirty |= widget.var("Caustic Alpha", mSPPMAlphaCaustic, 0.1f, 1.0f, 0.001f);
        widget.tooltip("Sets the Alpha in SPPM for the Caustic Photons");
    }
    
    widget.dummy("", dummySpacing);
    //miscellaneous
    dirty |= widget.slider("Max Recursion Depth", mMaxBounces, 1u, 32u);
    widget.tooltip("Maximum path length for Photon Bounces");
    mResetCS |= widget.checkbox("Use Photon Face Normal Rejection", mEnableFaceNormalRejection);
    widget.tooltip("Uses encoded Face Normal to reject photon hits on different surfaces (corners / other side of wall).");
    dirty |= mResetCS;

    widget.dummy("", dummySpacing);

    //Timer
    if (auto group = widget.group("Timer")) {
        bool resetTimer = false;
        resetTimer |= widget.checkbox("Enable Timer", mUseTimer);
        widget.tooltip("Enables the timer");
        if (mUseTimer) {
            uint sec = static_cast<uint>(mTimerDurationSec);
            if (sec != 0) widget.text("Elapsed seconds: " + std::to_string(mCurrentElapsedTime) + " / " + std::to_string(sec));
            if (mTimerMaxIterations != 0) widget.text("Iterations: " + std::to_string(mFrameCount) + " / " + std::to_string(mTimerMaxIterations));
            resetTimer |= widget.var("Timer Seconds", sec, 0u, UINT_MAX, 1u);
            widget.tooltip("Time in seconds needed to stop rendering. When 0 time is not used");
            resetTimer |= widget.var("Max Iterations", mTimerMaxIterations, 0u, UINT_MAX, 1u);
            widget.tooltip("Max iterations until stop. When 0 iterations are not used");
            mTimerDurationSec = static_cast<double>(sec);
            resetTimer |= widget.checkbox("Record Times", mTimerRecordTimes);
            resetTimer |= widget.button("Reset Timer");
            if (mTimerRecordTimes) {
                if (widget.button("Store Times", true)) {
                    FileDialogFilterVec filters;
                    filters.push_back({ "csv", "CSV Files" });
                    std::filesystem::path path;
                    if (saveFileDialog(filters, path))
                    {
                        mTimesOutputFilePath = path.string();
                        outputTimes();
                    }
                }
            }
        }
        mResetTimer |= resetTimer;
        dirty |= resetTimer;
    }

    //Radius settings
    if (auto group = widget.group("Radius Options")) {
        dirty |= widget.var("Caustic Radius Start", mCausticRadiusStart, kMinPhotonRadius, FLT_MAX, 0.001f);
        widget.tooltip("The start value for the radius of caustic Photons");
        dirty |= widget.var("Global Radius Start", mGlobalRadiusStart, kMinPhotonRadius, FLT_MAX, 0.001f);
        widget.tooltip("The start value for the radius of global Photons");
        dirty |= widget.var("Russian Roulette", mRussianRoulette, 0.001f, 1.f, 0.001f);
        widget.tooltip("Probabilty that a Global Photon is saved");
    }
    //Material Settings
    if (auto group = widget.group("Material Options")) {
        dirty |= widget.var("Emissive Scalar", mIntensityScalar, 0.0f, FLT_MAX, 0.001f);
        widget.tooltip("Scales the intensity of all emissive Light Sources");
        dirty |= widget.var("SpecRoughCutoff", mSpecRoughCutoff, 0.0f, 1.0f, 0.01f);
        widget.tooltip("The cutoff for Specular Materials. All Reflections above this threshold are considered Diffuse");
        dirty |= widget.checkbox("Alpha Test", mUseAlphaTest);
        widget.tooltip("Enables Alpha Test for Photon Generation");
        dirty |= widget.checkbox("Adjust Shading Normals", mAdjustShadingNormals);
        widget.tooltip("Adjusts the shading normals in the Photon Generation");
    }
    //Hash Settings
    if (auto group = widget.group("Hash Options")) {
        dirty |= widget.var("Quadradic Probe Iterations", mQuadraticProbeIterations, 0u, 100u, 1u);
        widget.tooltip("Max iterations that are used for quadratic probe");
        mResetCS |= widget.slider("Num Photons per bucket", mNumPhotonsPerBucket, 2u, 32u);
        widget.tooltip("Max number of photons that can be saved in a hash grid");
        mResetCS |= widget.slider("Bucket size (bits)", mNumBucketBits, 2u, 32u);
        widget.tooltip("Bucket size in 2^x. One bucket takes 16Byte + Num photons per bucket * 4 Byte");

        dirty |= mResetCS;
    }

    if (auto group = widget.group("Light Sample Tex")) {
        mRebuildLightTex |= widget.dropdown("Sample mode", kLightTexModeList, (uint32_t&)mLightTexMode);
        widget.tooltip("Changes photon distribution for the light sampling texture. Also rebuilds the texture.");
        mRebuildLightTex |= widget.button("Rebuild Light Tex");
        dirty |= mRebuildLightTex;
    }

    mPhotonInfoFormatChanged |= widget.dropdown("Photon Info size", kInfoTexDropdownList, mInfoTexFormat);
    widget.tooltip("Determines the resolution of each element of the photon info struct.");

    dirty |= mPhotonInfoFormatChanged;  //Reset iterations if format is changed

    //Disable Photon Collecion
    if (auto group = widget.group("Collect Options")) {
        dirty |= widget.checkbox("Disable Global Photons", mDisableGlobalCollection);
        widget.tooltip("Disables the collection of Global Photons. However they will still be generated");
        dirty |= widget.checkbox("Disable Caustic Photons", mDisableCausticCollection);
        widget.tooltip("Disables the collection of Caustic Photons. However they will still be generated");
        dirty |= widget.checkbox("Stochastic Collection", mEnableStochasticCollection);
        widget.tooltip("Enables stochastic collection. A geometrically distributed random step is used for that");
        if (mEnableStochasticCollection) {
            dirty |= widget.slider("Stochastic Collection Probability", mStochasticCollectProbability, 0.0001f, 1.0f);
            widget.tooltip("Probability for the geometrically distributed random step");
        }
    }
    widget.dummy("", dummySpacing);
    //Reset Iterations
    widget.checkbox("Always Reset Iterations", mAlwaysResetIterations);
    widget.tooltip("Always Resets the Iterations, currently good for moving the camera");
    mResetIterations |= widget.button("Reset Iterations");
    widget.tooltip("Resets the iterations");
    dirty |= mResetIterations;

    //set flag to indicate that settings have changed and the pass has to be rebuild
    if (dirty)
        mOptionsChanged = true;
}

void PhotonMapperHash::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{


    // Clear data for previous scene.
    resetPhotonMapper();

    // After changing scene, the raytracing program should to be recreated.
    mTracerGenerate = RayTraceProgramHelper::create();
    mpCSCollect.reset();
    mSetConstantBuffers = true;
    
    // Set new scene.
    mpScene = pScene;

    if (mpScene)
    {
        if (mpScene->hasGeometryType(Scene::GeometryType::Custom))
        {
            logWarning("This render pass only supports triangles. Other types of geometry will be ignored.");
        }

        // Create ray tracing program.
        {
            RtProgram::Desc desc;
            desc.addShaderLibrary(kShaderGeneratePhoton);
            desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
            desc.setMaxAttributeSize(kMaxAttributeSizeBytes);
            desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);
            //desc.addDefines(mpScene->getSceneDefines());
            


            mTracerGenerate.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
            auto& sbt = mTracerGenerate.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen"));
            sbt->setMiss(0, desc.addMiss("miss"));
            if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh)) {
                sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
            }

            mTracerGenerate.pProgram = RtProgram::create(desc, mpScene->getSceneDefines());
        }
    }

    //init the photon counters
    preparePhotonCounters();
}

void PhotonMapperHash::getActiveEmissiveTriangles(RenderContext* pRenderContext)
{
    auto lightCollection = mpScene->getLightCollection(pRenderContext);

    auto meshLightTriangles = lightCollection->getMeshLightTriangles();

    mActiveEmissiveTriangles.clear();
    mActiveEmissiveTriangles.reserve(meshLightTriangles.size());

    for (uint32_t triIdx = 0; triIdx < (uint32_t)meshLightTriangles.size(); triIdx++)
    {
        if (meshLightTriangles[triIdx].flux > 0.f)
        {
            mActiveEmissiveTriangles.push_back(triIdx);
        }
    }
}

void PhotonMapperHash::createLightSampleTexture(RenderContext* pRenderContext)
{
    if (mPhotonsPerTriangle) mPhotonsPerTriangle.reset();
    if (mLightSampleTex) mLightSampleTex.reset();

    FALCOR_ASSERT(mpScene);    //Scene has to be set

    auto analyticLights = mpScene->getActiveLights();
    auto lightCollection = mpScene->getLightCollection(pRenderContext);

    uint analyticPhotons = 0;
    uint numEmissivePhotons = 0;
    //If there are analytic Lights split number of Photons even between the analytic light and the number of emissive models (approximation of the number of emissive lights)
    if (analyticLights.size() != 0){
        uint lightsTotal = static_cast<uint>(analyticLights.size() + lightCollection->getMeshLights().size());
        float percentAnalytic = static_cast<float>(analyticLights.size()) / static_cast<float>(lightsTotal);
        analyticPhotons = static_cast<uint>(mNumPhotons * percentAnalytic);
        analyticPhotons += (uint)analyticLights.size() - (analyticPhotons % (uint) analyticLights.size());  //add it up so every light gets the same number of photons
        numEmissivePhotons = mNumPhotons - analyticPhotons;
    }
    else
        numEmissivePhotons = mNumPhotons;

    std::vector<uint> numPhotonsPerTriangle;    //only filled when there are emissive

    if (numEmissivePhotons > 0) {
        getActiveEmissiveTriangles(pRenderContext);
        auto meshLightTriangles = lightCollection->getMeshLightTriangles();
        //Get total area to distribute to get the number of photons per area.
        float totalMode = 0;
        for (uint i = 0; i < (uint)mActiveEmissiveTriangles.size(); i++) {
            uint triIdx = mActiveEmissiveTriangles[i];

            switch (mLightTexMode) {
            case LightTexMode::power:
                totalMode += meshLightTriangles[triIdx].flux;
                break;
            case LightTexMode::area:
                totalMode += meshLightTriangles[triIdx].area;
                break;
            default:
                totalMode += meshLightTriangles[triIdx].flux;
            }

        }
        float photonsPerMode = numEmissivePhotons / totalMode;

        //Calculate photons on a per triangle base
        uint tmpNumEmissivePhotons = 0; //Real count will change due to rounding
        numPhotonsPerTriangle.reserve(mActiveEmissiveTriangles.size());
        for (uint i = 0; i < (uint)mActiveEmissiveTriangles.size(); i++) {
            uint triIdx = mActiveEmissiveTriangles[i];
            uint photons = 0;

            switch (mLightTexMode) {
            case LightTexMode::power:
                photons = static_cast<uint>(std::ceil(meshLightTriangles[triIdx].flux * photonsPerMode));
                break;
            case LightTexMode::area:
                photons = static_cast<uint>(std::ceil(meshLightTriangles[triIdx].area * photonsPerMode));
                break;
            default:
                photons = static_cast<uint>(std::ceil(meshLightTriangles[triIdx].flux * photonsPerMode));
            }


            if (photons == 0) photons = 1;  //shoot at least one photon
            tmpNumEmissivePhotons += photons;
            numPhotonsPerTriangle.push_back(photons);
        }
        numEmissivePhotons = tmpNumEmissivePhotons;     //get real photon count
    }

    uint totalNumPhotons = numEmissivePhotons + analyticPhotons;

    //calculate the pdf for analytic and emissive light
    if (analyticPhotons > 0 && analyticLights.size() > 0) {
        mAnalyticInvPdf = (static_cast<float>(totalNumPhotons) * static_cast<float>(analyticLights.size())) / static_cast<float>(analyticPhotons);
    }   

    const uint blockSize = 16;
    const uint blockSizeSq = blockSize * blockSize;

    //Create texture. The texture fills 16x16 fields with information
    
    uint xPhotons = (totalNumPhotons / mMaxDispatchY) + 1;
    xPhotons += (xPhotons % blockSize == 0 && analyticPhotons > 0) ? blockSize : blockSize - (xPhotons % blockSize);  //Fill up so x to 16x16 block with at least 1 block extra when mixed

    //Init the texture with the invalid index (zero)
    //Negative indices are analytic and postivie indices are emissive
    std::vector<int32_t> lightIdxTex(xPhotons * mMaxDispatchY, 0);

    //Helper functions
    auto getIndex = [&](uint2 idx) {
        return idx.x + idx.y * xPhotons;
    };

    auto getBlockStartingIndex = [&](uint blockIdx) {
        blockIdx = blockIdx * blockSize;          //Multiply by the expansion of the box in x
        uint x = blockIdx % xPhotons;
        uint y = (blockIdx / xPhotons) * blockSize;
        return uint2(x, y);
    };

    //Fill analytic lights
    if (analyticLights.size() > 0) {
        uint numCurrentLight = 0;
        uint step = analyticPhotons / static_cast<uint>(analyticLights.size());
        bool stop = false;
        for (uint i = 0; i <= analyticPhotons / blockSizeSq; i++) {
            if (stop) break;
            for (uint y = 0; y < blockSize; y++) {
                if (stop) break;
                for (uint x = 0; x < blockSize; x++) {
                    if (numCurrentLight >= analyticPhotons) {
                        stop = true;
                        break;
                    }
                    uint2 idx = getBlockStartingIndex(i);
                    idx += uint2(x, y);
                    int32_t lightIdx = static_cast<int32_t>((numCurrentLight / step) + 1);  //current light index + 1
                    lightIdx *= -1;                                                         //turn it negative as it is a analytic light
                    lightIdxTex[getIndex(idx)] = lightIdx;
                    numCurrentLight++;
                }
            }
        }
    }
    

    //Fill emissive lights
    if (numEmissivePhotons > 0) {
        uint analyticEndBlock = analyticPhotons > 0 ? (analyticPhotons / blockSizeSq) + 1 : 0;    //we have guaranteed an extra block
        uint currentActiveTri = 0;
        uint lightInActiveTri = 0;
        bool stop = false;
        for (uint i = 0; i <= numEmissivePhotons / blockSizeSq; i++) {
            if (stop) break;
            for (uint y = 0; y < blockSize; y++) {
                if (stop) break;
                for (uint x = 0; x < blockSize; x++) {
                    if (currentActiveTri >= static_cast<uint>(numPhotonsPerTriangle.size())) {
                        stop = true;
                        break;
                    }
                    uint2 idx = getBlockStartingIndex(i + analyticEndBlock);
                    idx += uint2(x, y);
                    int32_t lightIdx = static_cast<int32_t>(currentActiveTri + 1);      //emissive has the positive index
                    lightIdxTex[getIndex(idx)] = lightIdx;

                    //Check if the number of photons exeed that of the current active triangle
                    lightInActiveTri++;
                    if (lightInActiveTri >= numPhotonsPerTriangle[currentActiveTri]) {
                        currentActiveTri++;
                        lightInActiveTri = 0;
                    }
                }
            }
        }
    }
    


    //Create texture and Pdf buffer
    mLightSampleTex = Texture::create2D(xPhotons, mMaxDispatchY, ResourceFormat::R32Int, 1, 1, lightIdxTex.data());
    mLightSampleTex->setName("PhotonMapperHash::LightSampleTex");

    //Create a buffer for num photons per triangle
    if (numPhotonsPerTriangle.size() == 0) {
        numPhotonsPerTriangle.push_back(0);
    }
    mPhotonsPerTriangle = Buffer::createStructured(sizeof(uint), static_cast<uint32_t>(numPhotonsPerTriangle.size()), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, numPhotonsPerTriangle.data());
    mPhotonsPerTriangle->setName("PhotonMapperHash::mPhotonsPerTriangleEmissive");


    //Set numPhoton variable
    mPGDispatchX = xPhotons;

    mNumPhotons = mPGDispatchX * mMaxDispatchY;
    mNumPhotonsUI = mNumPhotons;
}

void PhotonMapperHash::resetPhotonMapper()
{
    mFrameCount = 0;

    //For Photon Buffers and resize
    mResizePhotonBuffers = true; mPhotonBuffersReady = false;
    mCausticBuffers.maxSize = 0; mGlobalBuffers.maxSize = 0;
    mPhotonCount[0] = 0; mPhotonCount[1] = 0;

    mResetCS = true;
    mSetConstantBuffers = true;

    //reset light sample tex
    mLightSampleTex = nullptr;
}

void PhotonMapperHash::changeNumPhotons()
{
    //If photon number differ reset the light sample texture
    if (mNumPhotonsUI != mNumPhotons) {
        //Reset light sample tex and frame counter
        mNumPhotons = mNumPhotonsUI;
        mLightSampleTex = nullptr;
        mFrameCount = 0;
    }

    

    if (mGlobalBuffers.maxSize != mGlobalBufferSizeUI || mCausticBuffers.maxSize != mCausticBufferSizeUI || mFitBuffersToPhotonShot) {
        mResizePhotonBuffers = true; mPhotonBuffersReady = false;
        mCausticBuffers.maxSize = 0; mGlobalBuffers.maxSize = 0;
    }

}

void PhotonMapperHash::copyPhotonCounter(RenderContext* pRenderContext)
{
    //Copy the photonConter to a CPU Buffer
    pRenderContext->copyBufferRegion(mPhotonCounterBuffer.cpuCopy.get(), 0, mPhotonCounterBuffer.counter.get(), 0, sizeof(uint32_t) * 2);

    void* data = mPhotonCounterBuffer.cpuCopy->map(Buffer::MapType::Read);
    std::memcpy(mPhotonCount.data(), data, sizeof(uint) * 2);
    mPhotonCounterBuffer.cpuCopy->unmap();
}

void PhotonMapperHash::prepareVars()
{
    FALCOR_ASSERT(mTracerGenerate.pProgram);

    // Configure program.
    mTracerGenerate.pProgram->addDefines(mpSampleGenerator->getDefines());
    mTracerGenerate.pProgram->setTypeConformances(mpScene->getTypeConformances());
    // Create program variables for the current program.
    // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
    mTracerGenerate.pVars = RtProgramVars::create(mTracerGenerate.pProgram, mTracerGenerate.pBindingTable);

    // Bind utility classes into shared data.
    auto var = mTracerGenerate.pVars->getRootVar();
    mpSampleGenerator->setShaderData(var);
}

ResourceFormat inline getFormatRGBA(uint format, bool flux = true)
{
    switch (format) {
    case static_cast<uint>(PhotonMapperHash::TextureFormat::_8Bit):
        if (flux) 
            return ResourceFormat::RGBA8Unorm;
        else
            return ResourceFormat::RGBA8Snorm;
    case static_cast<uint>(PhotonMapperHash::TextureFormat::_16Bit):
        return ResourceFormat::RGBA16Float;
    case static_cast<uint>(PhotonMapperHash::TextureFormat::_32Bit):
        return ResourceFormat::RGBA32Float;
    }

    //If invalid format return highest possible format
    return ResourceFormat::RGBA32Float;
}

void PhotonMapperHash::preparePhotonInfoTexture()
{
    FALCOR_ASSERT(mCausticBuffers.maxSize > 0 || mGlobalBuffers.maxSize > 0);
    //clean tex
    mCausticBuffers.infoFlux.reset(); mCausticBuffers.infoDir.reset();  mCausticBuffers.position.reset();
    mGlobalBuffers.infoFlux.reset(); mGlobalBuffers.infoDir.reset(); mGlobalBuffers.position.reset();
   
    //Caustic
    mCausticBuffers.infoFlux = Texture::create2D(mCausticBuffers.maxSize / kInfoTexHeight, kInfoTexHeight, getFormatRGBA(mInfoTexFormat, true), 1, 1, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    mCausticBuffers.infoFlux->setName("PhotonMapperHash::mCausticBuffers.fluxInfo");
    mCausticBuffers.infoDir = Texture::create2D(mCausticBuffers.maxSize / kInfoTexHeight, kInfoTexHeight, getFormatRGBA(mInfoTexFormat, false), 1, 1, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    mCausticBuffers.infoDir->setName("PhotonMapperHash::mCausticBuffers.dirInfo");
    mCausticBuffers.position = Texture::create2D(mCausticBuffers.maxSize / kInfoTexHeight, kInfoTexHeight, ResourceFormat::RGBA32Float, 1, 1, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    mCausticBuffers.position->setName("PhotonMapperHash::mCausticBuffers.position");

    FALCOR_ASSERT(mCausticBuffers.infoFlux); FALCOR_ASSERT(mCausticBuffers.infoDir); FALCOR_ASSERT(mCausticBuffers.position);

    mGlobalBuffers.infoFlux = Texture::create2D(mGlobalBuffers.maxSize / kInfoTexHeight, kInfoTexHeight, getFormatRGBA(mInfoTexFormat, true), 1, 1, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    mGlobalBuffers.infoFlux->setName("PhotonMapperHash::mGlobalBuffers.fluxInfo");
    mGlobalBuffers.infoDir = Texture::create2D(mGlobalBuffers.maxSize / kInfoTexHeight, kInfoTexHeight, getFormatRGBA(mInfoTexFormat, false), 1, 1, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    mGlobalBuffers.infoDir->setName("PhotonMapperHash::mGlobalBuffers.dirInfo");
    mGlobalBuffers.position = Texture::create2D(mGlobalBuffers.maxSize / kInfoTexHeight, kInfoTexHeight, ResourceFormat::RGBA32Float, 1, 1, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    mGlobalBuffers.position->setName("PhotonMapperHash::mGlobalBuffers.position");

    FALCOR_ASSERT(mGlobalBuffers.infoFlux); FALCOR_ASSERT(mGlobalBuffers.infoDir); FALCOR_ASSERT(mGlobalBuffers.position);
}

void PhotonMapperHash::prepareHashBuffer()
{
    //reset buffers if already set
    if (mpGlobalBuckets) {
        mpGlobalBuckets.reset();
        mpCausticBuckets.reset();
    }

    //Build buffers
    mNumBuckets = 1 << mNumBucketBits;
    mpGlobalBuckets = Buffer::createStructured(sizeof(uint32_t) * (mNumPhotonsPerBucket + 4), mNumBuckets);
    mpGlobalBuckets->setName("PhotonMapperHash::BucketGlobal");
    mpCausticBuckets = Buffer::createStructured(sizeof(uint32_t) * (mNumPhotonsPerBucket + 4), mNumBuckets);
    mpCausticBuckets->setName("PhotonMapperHash::BucketCaustic");

}

bool PhotonMapperHash::preparePhotonBuffers()
{
    FALCOR_ASSERT(mCausticBuffers.maxSize > 0 || mGlobalBuffers.maxSize > 0);

    //Create the hash buffers
    prepareHashBuffer();

    //Create/recreate the textures
    preparePhotonInfoTexture();
    
    return true;
}

void PhotonMapperHash::preparePhotonCounters()
{
    //photon counter
    mPhotonCounterBuffer.counter = Buffer::createStructured(sizeof(uint), 2);
    mPhotonCounterBuffer.counter->setName("PhotonMapperHash::PhotonCounter");
    uint64_t zeroInit = 0;
    mPhotonCounterBuffer.reset = Buffer::create(sizeof(uint64_t), ResourceBindFlags::None, Buffer::CpuAccess::None, &zeroInit);
    mPhotonCounterBuffer.reset->setName("PhotonMapperHash::PhotonCounterReset");
    uint32_t oneInit[2] = { 1,1 };
    mPhotonCounterBuffer.cpuCopy = Buffer::create(sizeof(uint64_t), ResourceBindFlags::None, Buffer::CpuAccess::Read, oneInit);
    mPhotonCounterBuffer.cpuCopy->setName("PhotonMapperHash::PhotonCounterCPU");
}

void PhotonMapperHash::prepareRandomSeedBuffer(const uint2 screenDimensions)
{
    FALCOR_ASSERT(screenDimensions.x > 0 && screenDimensions.y > 0);

    //fill a std vector with random seed from the seed_seq
    std::seed_seq seq{ time(0) };
    std::vector<uint32_t> cpuSeeds(screenDimensions.x * screenDimensions.y);
    seq.generate(cpuSeeds.begin(), cpuSeeds.end());

    //create the gpu buffer
    mRandNumSeedBuffer = Texture::create2D(screenDimensions.x, screenDimensions.y, ResourceFormat::R32Uint, 1, 1, cpuSeeds.data());
    mRandNumSeedBuffer->setName("PhotonMapperHash::RandomSeedBuffer");

    FALCOR_ASSERT(mRandNumSeedBuffer);
}

void PhotonMapperHash::checkTimer()
{
    if (!mUseTimer) return;

    //reset timer
    if (mResetTimer) {
        mCurrentElapsedTime = 0.0;
        mTimerStartTime = std::chrono::steady_clock::now();
        mTimerStopRenderer = false;
        mResetTimer = false;
        if (mTimerRecordTimes) {
            mTimesList.clear();
            mTimesList.reserve(10000);
        }
        return;
    }

    if (mTimerStopRenderer) return;

    //check time
    if (mTimerDurationSec != 0) {
        auto currentTime = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsedSec = currentTime - mTimerStartTime;
        mCurrentElapsedTime = elapsedSec.count();

        if (mTimerDurationSec <= mCurrentElapsedTime) {
            mTimerStopRenderer = true;
        }
    }

    //check iterations
    if (mTimerMaxIterations != 0) {
        if (mTimerMaxIterations <= mFrameCount) {
            mTimerStopRenderer = true;
        }
    }

    //Add to times list
    if (mTimerRecordTimes) {
        mTimesList.push_back(mCurrentElapsedTime);
    }
}

void PhotonMapperHash::outputTimes()
{
    if (mTimesOutputFilePath.empty() || mTimesList.empty()) return;

    std::ofstream file = std::ofstream(mTimesOutputFilePath, std::ios::trunc);

    if (!file) {
        reportError(fmt::format("Failed to open file '{}'.", mTimesOutputFilePath));
        mTimesOutputFilePath.clear();
        return;
    }

    //Write into file
    file << "Hash_Times" << std::endl;
    file << std::fixed << std::setprecision(16);
    for (size_t i = 0; i < mTimesList.size(); i++) {
        file << mTimesList[i];
        file << std::endl;
    }
    file.close();
}
