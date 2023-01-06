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
#include "StochHashPPM.h"
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

const RenderPass::Info PhotonMapperStochasticHash::kInfo{"StochHashPPM", "Progressive Photon Mapper based on a Stochastic Hash Grid" };

// Don't remove this. it's required for hot-reload to function properly
extern "C" FALCOR_API_EXPORT const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" FALCOR_API_EXPORT void getPasses(Falcor::RenderPassLibrary & lib)
{
    lib.registerPass(PhotonMapperStochasticHash::kInfo, PhotonMapperStochasticHash::create);
}

namespace
{
    const char kShaderGeneratePhoton[] = "RenderPasses/StochHashPPM/PhotonMapperStochasticHashGenerate.rt.slang";
    const char kShaderCollectPhoton[] = "RenderPasses/StochHashPPM/PhotonMapperStochasticHashCollect.cs.slang";

    // Ray tracing settings that affect the traversal stack size.
   // These should be set as small as possible.
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
        //{(uint)PhotonMapperStochasticHash::TextureFormat::_8Bit , "8Bits"},
        {(uint)PhotonMapperStochasticHash::TextureFormat::_16Bit , "16Bits"},
        {(uint)PhotonMapperStochasticHash::TextureFormat::_32Bit , "32Bits"}
    };

    const Gui::DropdownList kLightTexModeList{
        {PhotonMapperStochasticHash::LightTexMode::power , "Power"},
        {PhotonMapperStochasticHash::LightTexMode::area , "Area"}
    };

    const Gui::DropdownList kCausticMapModes{
        {0, "LS+D"},
        {1, "L(S|D)*SD"}
    };
}

PhotonMapperStochasticHash::SharedPtr PhotonMapperStochasticHash::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new PhotonMapperStochasticHash);
    return pPass;
}

PhotonMapperStochasticHash::PhotonMapperStochasticHash():
    RenderPass(kInfo)
{
    mpSampleGenerator = SampleGenerator::create(SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);
}

Dictionary PhotonMapperStochasticHash::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection PhotonMapperStochasticHash::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;

    // Define our input/output channels.
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);


    return reflector;
}

void PhotonMapperStochasticHash::compile(RenderContext* pContext, const CompileData& compileData)
{
    // put reflector outputs here and create again if needed
    
}

void PhotonMapperStochasticHash::execute(RenderContext* pRenderContext, const RenderData& renderData)
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
        preparePhotonBuffers();
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

void PhotonMapperStochasticHash::generatePhotons(RenderContext* pRenderContext, const RenderData& renderData)
{
    //Clear the photon Buffers
    pRenderContext->clearTexture(mpGlobalPosBucket.get());
    pRenderContext->clearTexture(mpGlobalDirBucket.get());
    pRenderContext->clearTexture(mpGlobalFluxBucket.get());
    pRenderContext->clearTexture(mpCausticPosBucket.get());
    pRenderContext->clearTexture(mpCausticDirBucket.get());
    pRenderContext->clearTexture(mpCausticFluxBucket.get());
    pRenderContext->clearUAV(mpGlobalHashPhotonCounter->getUAV().get(), uint4(0, 0, 0, 0));
    pRenderContext->clearUAV(mpCausticHashPhotonCounter->getUAV().get(), uint4(0, 0, 0, 0));
    

    auto lights = mpScene->getLights();
    auto lightCollection = mpScene->getLightCollection(pRenderContext);

    // Specialize the Generate program.
    // These defines should not modify the program vars. Do not trigger program vars re-creation.
    mTracerGenerate.pProgram->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    mTracerGenerate.pProgram->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    mTracerGenerate.pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    mTracerGenerate.pProgram->addDefine("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");
    mTracerGenerate.pProgram->addDefine("ANALYTIC_INV_PDF", std::to_string(mAnalyticInvPdf));
    mTracerGenerate.pProgram->addDefine("INFO_TEXTURE_HEIGHT", std::to_string(kInfoTexHeight));
    mTracerGenerate.pProgram->addDefine("NUM_BUCKETS", std::to_string(mNumBuckets));
    mTracerGenerate.pProgram->addDefine("PHOTON_FACE_NORMAL", mEnableFaceNormalRejection ? "1" : "0");
    mTracerGenerate.pProgram->addDefine("MULTI_DIFFHIT_CAUSTIC_MAP", mCausticMapMultipleDiffuseHits == 0 ? "0" : "1");
    
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
        var[nameBuf]["gBucketYExtent"] = mBucketFixedYExtend;
    }
    
    //set the buffers
    var["gRndSeedBuffer"] = mRandNumSeedBuffer;

    for (uint32_t i = 0; i <= 1; i++)
    {
        var["gHashBucketPos"][i] = i == 0 ? mpCausticPosBucket : mpGlobalPosBucket;
        var["gHashBucketDir"][i] = i == 0 ? mpCausticDirBucket : mpGlobalDirBucket;
        var["gHashBucketFlux"][i] = i == 0 ? mpCausticFluxBucket : mpGlobalFluxBucket;
        var["gHashCounter"][i] = i == 0 ? mpCausticHashPhotonCounter : mpGlobalHashPhotonCounter;
    }

    //Bind light sample tex
    var["gLightSample"] = mLightSampleTex;
    var["gNumPhotonsPerEmissive"] = mPhotonsPerTriangle;

    // Get dimensions of ray dispatch.
    const uint2 targetDim = uint2(mPGDispatchX, mMaxDispatchY);
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    // Trace the photons
    mpScene->raytrace(pRenderContext, mTracerGenerate.pProgram.get(), mTracerGenerate.pVars, uint3(targetDim, 1));
}

void PhotonMapperStochasticHash::collectPhotons(RenderContext* pRenderContext, const RenderData& renderData)
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
        var[nameBuf]["gBucketYExtent"] = mBucketFixedYExtend;
    }

    for (uint32_t i = 0; i <= 1; i++)
    {
        var["gHashBucketPos"][i] = i == 0 ? mpCausticPosBucket : mpGlobalPosBucket;
        var["gHashBucketDir"][i] = i == 0 ? mpCausticDirBucket : mpGlobalDirBucket;
        var["gHashBucketFlux"][i] = i == 0 ? mpCausticFluxBucket : mpGlobalFluxBucket;
        var["gHashCounter"][i] = i == 0 ? mpCausticHashPhotonCounter : mpGlobalHashPhotonCounter;
    }

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

void PhotonMapperStochasticHash::renderUI(Gui::Widgets& widget)
{
    float2 dummySpacing = float2(0, 10);
    bool dirty = false;

    widget.text("Iterations: " + std::to_string(mFrameCount));
    widget.text("Current Global Radius: " + std::to_string(mGlobalRadius));
    widget.text("Current Caustic Radius: " + std::to_string(mCausticRadius));

    widget.dummy("", dummySpacing);
    widget.var("Number Photons", mNumPhotonsUI, 1000u, UINT_MAX, 1000u);
    widget.tooltip("The number of photons that are shot per iteration. Press \"Apply\" to apply the change");
    mNumPhotonsChanged |= widget.button("Apply");
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
    dirty |= widget.dropdown("Caustic Map Definition", kCausticMapModes, mCausticMapMultipleDiffuseHits);
    widget.tooltip("Changes definition of the caustic photons map. L(S|D)SD path will store way more stray caustic photons, but allows caustics from indirect illuminated surfaces");


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
        mResetCS |= widget.slider("Bucket size (bits)", mNumBucketBits, 2u, 32u);
        widget.tooltip("Bucket size in 2^x. One bucket takes 48Byte. Total Size = 2^x * 48B. There are two buckets total");

        dirty |= mResetCS;
    }

    if (auto group = widget.group("Light Sample Tex")) {
        mRebuildLightTex |= widget.dropdown("Sample mode", kLightTexModeList, (uint32_t&)mLightTexMode);
        widget.tooltip("Changes photon distribution for the light sampling texture. Also rebuilds the texture.");
        mRebuildLightTex |= widget.button("Rebuild Light Tex");
        dirty |= mRebuildLightTex;
    }

    //Disable Photon Collecion
    if (auto group = widget.group("Collect Options")) {
        dirty |= widget.checkbox("Disable Global Photons", mDisableGlobalCollection);
        widget.tooltip("Disables the collection of Global Photons. However they will still be generated");
        dirty |= widget.checkbox("Disable Caustic Photons", mDisableCausticCollection);
        widget.tooltip("Disables the collection of Caustic Photons. However they will still be generated");
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

void PhotonMapperStochasticHash::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    // Clear data for previous scene.
    resetPhotonMapper();

    // After changing scene, the raytracing program should to be recreated.
    mTracerGenerate = RayTraceProgramHelper::create();
    mpCSCollect.reset();
    mSetConstantBuffers = true;
    
    // Set new scene.
    mpScene = pScene;
    mpScene->setIsAnimated(false); //Disable animations

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
}

void PhotonMapperStochasticHash::getActiveEmissiveTriangles(RenderContext* pRenderContext)
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

void PhotonMapperStochasticHash::createLightSampleTexture(RenderContext* pRenderContext)
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
    mLightSampleTex->setName("PhotonMapperStochasticHash::LightSampleTex");

    //Create a buffer for num photons per triangle
    if (numPhotonsPerTriangle.size() == 0) {
        numPhotonsPerTriangle.push_back(0);
    }
    mPhotonsPerTriangle = Buffer::createStructured(sizeof(uint), static_cast<uint32_t>(numPhotonsPerTriangle.size()), ResourceBindFlags::ShaderResource, Buffer::CpuAccess::None, numPhotonsPerTriangle.data());
    mPhotonsPerTriangle->setName("PhotonMapperStochasticHash::mPhotonsPerTriangleEmissive");


    //Set numPhoton variable
    mPGDispatchX = xPhotons;

    mNumPhotons = mPGDispatchX * mMaxDispatchY;
    mNumPhotonsUI = mNumPhotons;
}

void PhotonMapperStochasticHash::resetPhotonMapper()
{
    mFrameCount = 0;

    //For Photon Buffers and resize
    mResizePhotonBuffers = true; mPhotonBuffersReady = false;

    mResetCS = true;
    mSetConstantBuffers = true;

    //reset light sample tex
    mLightSampleTex = nullptr;
}

void PhotonMapperStochasticHash::changeNumPhotons()
{
    //If photon number differ reset the light sample texture
    if (mNumPhotonsUI != mNumPhotons) {
        //Reset light sample tex and frame counter
        mNumPhotons = mNumPhotonsUI;
        mLightSampleTex = nullptr;
        mFrameCount = 0;
    }

}

void PhotonMapperStochasticHash::prepareVars()
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

bool PhotonMapperStochasticHash::preparePhotonBuffers()
{
    //reset buffers if already set
    if (mpGlobalPosBucket) {
        mpGlobalPosBucket.reset(); mpGlobalDirBucket.reset(); mpGlobalFluxBucket.reset();
        mpCausticPosBucket.reset(); mpCausticDirBucket.reset(); mpCausticFluxBucket.reset();
    }

    //Build buffers
    mNumBuckets = 1 << mNumBucketBits;
    //Get xy extend
    uint2 bucketExtend = uint2(mNumBuckets % mBucketFixedYExtend == 0 ? mNumBuckets / mBucketFixedYExtend : (mNumBuckets / mBucketFixedYExtend) + 1, mBucketFixedYExtend);
    mpGlobalPosBucket = Texture::create2D(bucketExtend.x, bucketExtend.y, ResourceFormat::RGBA32Float,1,1,nullptr,ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
    mpGlobalPosBucket->setName("PhotonMapperStochasticHash::GlobalPosBucket");
    mpGlobalDirBucket = Texture::create2D(bucketExtend.x, bucketExtend.y, ResourceFormat::RGBA16Float, 1, 1, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
    mpGlobalDirBucket->setName("PhotonMapperStochasticHash::GlobalDirBucket");
    mpGlobalFluxBucket = Texture::create2D(bucketExtend.x, bucketExtend.y, ResourceFormat::RGBA16Float, 1, 1, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
    mpGlobalFluxBucket->setName("PhotonMapperStochasticHash::GlobalFluxBucket");
    mpCausticPosBucket = Texture::create2D(bucketExtend.x, bucketExtend.y, ResourceFormat::RGBA32Float, 1, 1, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
    mpCausticPosBucket->setName("PhotonMapperStochasticHash::CausticPosBucket");
    mpCausticDirBucket = Texture::create2D(bucketExtend.x, bucketExtend.y, ResourceFormat::RGBA16Float, 1, 1, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
    mpCausticDirBucket->setName("PhotonMapperStochasticHash::CausticDirBucket");
    mpCausticFluxBucket = Texture::create2D(bucketExtend.x, bucketExtend.y, ResourceFormat::RGBA16Float, 1, 1, nullptr, ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource);
    mpCausticFluxBucket->setName("PhotonMapperStochasticHash::CausticFluxBucket");

    mpGlobalHashPhotonCounter = Buffer::createStructured(sizeof(uint32_t), mNumBuckets);
    mpGlobalHashPhotonCounter->setName("PhotonMapperStochasticHash::CounterHashGlobal");
    mpCausticHashPhotonCounter = Buffer::createStructured(sizeof(uint32_t), mNumBuckets);
    mpCausticHashPhotonCounter->setName("PhotonMapperStochasticHash::CounterHashCaustic");
    
    return true;
}

void PhotonMapperStochasticHash::prepareRandomSeedBuffer(const uint2 screenDimensions)
{
    FALCOR_ASSERT(screenDimensions.x > 0 && screenDimensions.y > 0);

    //fill a std vector with random seed from the seed_seq
    std::seed_seq seq{ time(0) };
    std::vector<uint32_t> cpuSeeds(screenDimensions.x * screenDimensions.y);
    seq.generate(cpuSeeds.begin(), cpuSeeds.end());

    //create the gpu buffer
    mRandNumSeedBuffer = Texture::create2D(screenDimensions.x, screenDimensions.y, ResourceFormat::R32Uint, 1, 1, cpuSeeds.data());
    mRandNumSeedBuffer->setName("PhotonMapperStochasticHash::RandomSeedBuffer");

    FALCOR_ASSERT(mRandNumSeedBuffer);
}

void PhotonMapperStochasticHash::checkTimer()
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

void PhotonMapperStochasticHash::outputTimes()
{
    if (mTimesOutputFilePath.empty() || mTimesList.empty()) return;

    std::ofstream file = std::ofstream(mTimesOutputFilePath, std::ios::trunc);

    if (!file) {
        reportError(fmt::format("Failed to open file '{}'.", mTimesOutputFilePath));
        mTimesOutputFilePath.clear();
        return;
    }

    //Write into file
    file << "StochHash_Times" << std::endl;
    file << std::fixed << std::setprecision(16);
    for (size_t i = 0; i < mTimesList.size(); i++) {
        file << mTimesList[i];
        file << std::endl;
    }
    file.close();
}

