#include "stdafx.h"
#include "MainMenuRenderPipeline.h"

#include <Vorb/graphics/TextureCache.h>
#include <Vorb/io/IOManager.h>
#include <Vorb/io/FileOps.h>
#include <Vorb/utils.h>
#include <Vorb/ui/InputDispatcher.h>

#include "Errors.h"
#include "GameManager.h"
#include "HdrRenderStage.h"

#include "SoaOptions.h"
#include "SoaState.h"
#include "soaUtils.h"
#include "MainMenuScriptedUI.h"

MainMenuRenderPipeline::MainMenuRenderPipeline() {
    // Empty
}

MainMenuRenderPipeline::~MainMenuRenderPipeline() {
    destroy(true);
}

void MainMenuRenderPipeline::init(const SoaState* soaState, const ui32v4& viewport,
                                  MainMenuScriptedUI* mainMenuUI,
                                  Camera* camera,
                                  const MainMenuSystemViewer* systemViewer) {
    // Set the viewport
    m_viewport = viewport;
    m_mainMenuUI = mainMenuUI;

    vui::InputDispatcher::window.onResize += makeDelegate(*this, &MainMenuRenderPipeline::onWindowResize);

    // Check to make sure we don't double init
    if (m_isInitialized) {
        pError("Reinitializing MainMenuRenderPipeline without first calling destroy()!");
        return;
    } else {
        m_isInitialized = true;
    }

    initFramebuffer();

    m_quad.init();

    // Register render stages
    registerStage(&stages.colorFilter);
    registerStage(&stages.skybox);
    registerStage(&stages.hdr);
    registerStage(&stages.spaceSystem);
    registerStage(&stages.exposureCalc);

    // Init render stages
    stages.colorFilter.init(&m_quad);
    stages.skybox.init(camera, &soaState->texturePathResolver);
    stages.hdr.init(&m_quad, camera);
    stages.spaceSystem.init(soaState, ui32v2(m_viewport.z, m_viewport.w), systemViewer, camera, nullptr);
    stages.exposureCalc.init(&m_quad, m_hdrFrameBuffer, &m_viewport, 1024);
}

void MainMenuRenderPipeline::render() {
    
    // Check for window resize
    if (m_shouldResize) resize();

    // Bind the FBO
    m_hdrFrameBuffer->use();
    // Clear depth buffer. Don't have to clear color since skybox will overwrite it
    glClear(GL_DEPTH_BUFFER_BIT);

    // Main render passes
    stages.skybox.render();

    // Check fore wireframe mode
    if (m_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    stages.spaceSystem.setShowAR(m_showAR);
    stages.spaceSystem.render();

    // Restore fill
    if (m_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    f32v3 colorFilter(1.0);
    // Color filter rendering
    if (m_colorFilter != 0) {
        switch (m_colorFilter) {
            case 1:
                colorFilter = f32v3(0.66f);
                stages.colorFilter.setColor(f32v4(0.0, 0.0, 0.0, 0.33f)); break;
            case 2:
                colorFilter = f32v3(0.3f);
                stages.colorFilter.setColor(f32v4(0.0, 0.0, 0.0, 0.66f)); break;
            case 3:
                colorFilter = f32v3(0.0f);
                stages.colorFilter.setColor(f32v4(0.0, 0.0, 0.0, 0.9f)); break;
        }
        stages.colorFilter.render();
    }

    // Render last
    glBlendFunc(GL_ONE, GL_ONE);
    stages.spaceSystem.renderStarGlows(colorFilter);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Post processing
    m_swapChain->reset(0, m_hdrFrameBuffer->getID(), m_hdrFrameBuffer->getTextureID(), soaOptions.get(OPT_MSAA).value.i > 0, false);

    // TODO: More Effects?

    // Draw to backbuffer for the last effect
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDrawBuffer(GL_BACK);
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

    stages.exposureCalc.render();
    // Move exposure towards target
    static const f32 EXPOSURE_STEP = 0.005f;
    stepTowards(soaOptions.get(OPT_HDR_EXPOSURE).value.f, stages.exposureCalc.getExposure(), EXPOSURE_STEP);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(m_hdrFrameBuffer->getTextureTarget(), m_hdrFrameBuffer->getTextureID());
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(m_hdrFrameBuffer->getTextureTarget(), m_hdrFrameBuffer->getTextureDepthID());
    stages.hdr.render();

    if (m_showUI) m_mainMenuUI->draw();

    if (m_shouldScreenshot) dumpScreenshot();

    // Check for errors, just in case
    checkGlError("MainMenuRenderPipeline::render()");
}

void MainMenuRenderPipeline::destroy(bool shouldDisposeStages) {
    if (!m_isInitialized) return;
    RenderPipeline::destroy(shouldDisposeStages);

    if (m_swapChain) {
        m_swapChain->dispose();
        delete m_swapChain;
        m_swapChain = nullptr;
    }

    vui::InputDispatcher::window.onResize -= makeDelegate(*this, &MainMenuRenderPipeline::onWindowResize);

    m_quad.dispose();

    m_isInitialized = false;
}

void MainMenuRenderPipeline::onWindowResize(Sender s, const vui::WindowResizeEvent& e) {
    m_newDims = ui32v2(e.w, e.h);
    m_shouldResize = true;
}

void MainMenuRenderPipeline::initFramebuffer() {
    // Construct framebuffer
    m_hdrFrameBuffer = new vg::GLRenderTarget(m_viewport.z, m_viewport.w);
    m_hdrFrameBuffer->init(vg::TextureInternalFormat::RGBA16F, (ui32)soaOptions.get(OPT_MSAA).value.i).initDepth();
    if (soaOptions.get(OPT_MSAA).value.i > 0) {
        glEnable(GL_MULTISAMPLE);
    } else {
        glDisable(GL_MULTISAMPLE);
    }

    // Make swap chain
    m_swapChain = new vg::RTSwapChain<2>();
    m_swapChain->init(m_viewport.z, m_viewport.w, vg::TextureInternalFormat::RGBA8);
}

void MainMenuRenderPipeline::resize() {
    m_viewport.z = m_newDims.x;
    m_viewport.w = m_newDims.y;

    m_hdrFrameBuffer->dispose();
    delete m_hdrFrameBuffer;
    m_swapChain->dispose();
    delete m_swapChain;
    initFramebuffer();

    stages.spaceSystem.setViewport(m_newDims);
    stages.exposureCalc.setFrameBuffer(m_hdrFrameBuffer);

    m_mainMenuUI->setDimensions(f32v2(m_newDims));

    m_shouldResize = false;
}

void MainMenuRenderPipeline::dumpScreenshot() {
    // Make screenshots directory
    vio::buildDirectoryTree("Screenshots");
    // Take screenshot
    dumpFramebufferImage("Screenshots/", m_viewport);
    m_shouldScreenshot = false;
}
