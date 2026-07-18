#include "OpenXRContext.hpp"
#include "../core/VRMod.hpp"
#include "../config/Config.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace bl1gotyvr { namespace xr {

OpenXRContext& OpenXRContext::Instance() {
    static OpenXRContext ctx;
    return ctx;
}

bool OpenXRContext::GetProjectionCrop(int eye, float sourceAspect, float& scaleX, float& scaleY,
                                      float& offsetX, float& offsetY,
                                      float& horizontalFovDegrees) const {
    if (!m_viewsValid || eye < 0 || eye > 1 || sourceAspect <= 0.0f) return false;
    const XrFovf& fov = m_views[eye].fov;
    const float tanLeft = tanf(fov.angleLeft);
    const float tanRight = tanf(fov.angleRight);
    const float tanUp = tanf(fov.angleUp);
    const float tanDown = tanf(fov.angleDown);
    const float sourceHalfY = (std::max)(fabsf(tanUp), fabsf(tanDown));
    const float sourceHalfX = sourceHalfY * sourceAspect;
    if (sourceHalfX <= 0.0f || sourceHalfY <= 0.0f) return false;
    scaleX = (tanRight - tanLeft) / (2.0f * sourceHalfX);
    scaleY = (tanUp - tanDown) / (2.0f * sourceHalfY);
    offsetX = (tanLeft + sourceHalfX) / (2.0f * sourceHalfX);
    offsetY = (sourceHalfY - tanUp) / (2.0f * sourceHalfY);
    horizontalFovDegrees = 2.0f * atanf(sourceHalfX) * 57.29577951308232f;
    return scaleX > 0.0f && scaleX <= 1.0f && scaleY > 0.0f && scaleY <= 1.0f;
}

void OpenXRContext::MarkEyeRendered(int eye) {
    if (eye < 0 || eye > 1 || !m_viewsValid) return;
    m_renderedViews[eye] = m_views[eye];
    m_renderedViewValid[eye] = true;
}

XrResult OpenXRContext::CheckResult(XrResult result, const char* call) {
    if (result != XR_SUCCESS) {
        Log("[OpenXR] %s failed: %d", call, (int)result);
    }
    return result;
}

bool OpenXRContext::Initialize(ID3D11Device* device, DXGI_FORMAT backbufferFormat) {
    if (m_initialized) return true;

    Log("[OpenXR] Initializing...");

    m_device = device;
    m_device->AddRef();
    m_device->GetImmediateContext(&m_context);
    m_swapchainFormat = backbufferFormat;
    m_nearPlane = config::Get().near_plane;
    m_farPlane = config::Get().far_plane;

    if (!CreateInstance()) return false;
    if (!CreateSession()) return false;
    if (!CreateSpaces()) return false;
    if (!CreateSwapchains()) return false;

    m_initialized = true;
    Log("[OpenXR] Initialized successfully (IPD=%.1fmm)", m_ipd * 1000.0f);
    return true;
}

void OpenXRContext::Shutdown() {
    if (!m_initialized) return;
    Log("[OpenXR] Shutting down...");

    // Destroy swapchains
    if (m_leftEye.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_leftEye.swapchain);
        m_leftEye.swapchain = XR_NULL_HANDLE;
    }
    if (m_rightEye.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_rightEye.swapchain);
        m_rightEye.swapchain = XR_NULL_HANDLE;
    }
    m_leftEye.images.clear();
    m_rightEye.images.clear();

    // Destroy spaces
    if (m_stageSpace != XR_NULL_HANDLE) { xrDestroySpace(m_stageSpace); m_stageSpace = XR_NULL_HANDLE; }
    if (m_viewSpace != XR_NULL_HANDLE) { xrDestroySpace(m_viewSpace); m_viewSpace = XR_NULL_HANDLE; }

    // End session if active
    if (m_session != XR_NULL_HANDLE) {
        if (m_deviceBound) xrEndSession(m_session);
        xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
    }

    // Destroy instance
    if (m_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(m_instance);
        m_instance = XR_NULL_HANDLE;
    }

    if (m_context) { m_context->Release(); m_context = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }

    m_initialized = false;
    m_deviceBound = false;
    Log("[OpenXR] Shutdown complete");
}

bool OpenXRContext::CreateInstance() {
    Log("[OpenXR] Creating instance...");

    // Enumerate extensions
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount);
    for (auto& e : exts) e.type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasD3D11 = false;
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) {
            hasD3D11 = true;
            break;
        }
    }
    if (!hasD3D11) {
        Log("[OpenXR] ERROR: XR_KHR_D3D11_ENABLE not supported");
        return false;
    }

    const char* enabledExts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };

    XrInstanceCreateInfo ci = {};
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = enabledExts;

    XrApplicationInfo& appInfo = ci.applicationInfo;
    strcpy(appInfo.applicationName, "BL1GOTYVR");
    strcpy(appInfo.engineName, "Unreal Engine 3");
    appInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    XrResult r = xrCreateInstance(&ci, &m_instance);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] ERROR: xrCreateInstance = %d", (int)r);
        return false;
    }
    Log("[OpenXR] Instance created");
    return true;
}

bool OpenXRContext::CreateSession() {
    Log("[OpenXR] Creating session...");

    // Get system
    XrSystemGetInfo sysInfo = {};
    sysInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrResult r = xrGetSystem(m_instance, &sysInfo, &m_systemId);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] ERROR: xrGetSystem = %d (no HMD?)", (int)r);
        return false;
    }

    // Get D3D11 graphics requirements
    PFN_xrGetD3D11GraphicsRequirementsKHR getReqs = nullptr;
    xrGetInstanceProcAddr(m_instance, "xrGetD3D11GraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*)&getReqs);
    if (getReqs) {
        XrGraphicsRequirementsD3D11KHR reqs = {};
        reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
        r = getReqs(m_instance, m_systemId, &reqs);
        if (r != XR_SUCCESS) {
            Log("[OpenXR] xrGetD3D11GraphicsRequirementsKHR = %d", (int)r);
        }
        Log("[OpenXR] D3D11 min feature level: %d", (int)reqs.minFeatureLevel);
    }

    // Create session with D3D11 binding
    XrGraphicsBindingD3D11KHR binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
    binding.device = m_device;

    XrSessionCreateInfo sessionCI = {};
    sessionCI.type = XR_TYPE_SESSION_CREATE_INFO;
    sessionCI.next = &binding;
    sessionCI.systemId = m_systemId;

    r = xrCreateSession(m_instance, &sessionCI, &m_session);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] ERROR: xrCreateSession = %d", (int)r);
        return false;
    }

    // The graphics device is attached, but the session is not running until
    // the runtime reports READY and xrBeginSession succeeds.
    m_deviceBound = false;
    Log("[OpenXR] Session created");
    return true;
}

bool OpenXRContext::CreateSpaces() {
    Log("[OpenXR] Creating reference spaces...");

    // Stage space (seated/standing origin)
    XrReferenceSpaceCreateInfo spaceCI = {};
    spaceCI.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    spaceCI.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    spaceCI.poseInReferenceSpace = {};
    spaceCI.poseInReferenceSpace.orientation.w = 1.0f;

    XrResult r = xrCreateReferenceSpace(m_session, &spaceCI, &m_stageSpace);
    if (r != XR_SUCCESS) {
        // Fallback to LOCAL
        Log("[OpenXR] STAGE space failed (%d), trying LOCAL", (int)r);
        spaceCI.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        r = xrCreateReferenceSpace(m_session, &spaceCI, &m_stageSpace);
        if (r != XR_SUCCESS) {
            Log("[OpenXR] ERROR: xrCreateReferenceSpace(LOCAL) = %d", (int)r);
            return false;
        }
    }

    // View space
    spaceCI.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    r = xrCreateReferenceSpace(m_session, &spaceCI, &m_viewSpace);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] WARNING: xrCreateReferenceSpace(VIEW) = %d", (int)r);
    }

    Log("[OpenXR] Reference spaces created");
    return true;
}

static uint32_t FindSupportedFormat(ID3D11Device* device, const int64_t* supported, uint32_t count, DXGI_FORMAT desired) {
    for (uint32_t i = 0; i < count; i++) {
        if (supported[i] == (int64_t)desired) return i;
    }
    return 0;  // fallback to first
}

bool OpenXRContext::CreateSwapchains() {
    Log("[OpenXR] Creating eye swapchains...");

    // Get recommended swapchain sizes
    uint32_t viewConfigCount = 0;
    xrEnumerateViewConfigurations(m_instance, m_systemId, 0, &viewConfigCount, nullptr);
    std::vector<XrViewConfigurationType> viewTypes(viewConfigCount);
    xrEnumerateViewConfigurations(m_instance, m_systemId, viewConfigCount, &viewConfigCount, viewTypes.data());

    // Get view config views for PRIMARY_STEREO
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> viewConfigs(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, viewConfigs.data());

    if (viewCount < 2) {
        Log("[OpenXR] ERROR: Expected 2 views, got %u", viewCount);
        return false;
    }

    // Enumerate supported swapchain formats
    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data());

    uint32_t formatIdx = FindSupportedFormat(m_device, formats.data(), formatCount, m_swapchainFormat);
    int64_t selectedFormat = formats[formatIdx];
    Log("[OpenXR] Swapchain format: %u (requested %u, found %u matches)", (uint32_t)selectedFormat, (uint32_t)m_swapchainFormat, formatCount);

    // Create per-eye swapchains
    EyeData* eyes[2] = { &m_leftEye, &m_rightEye };
    for (int i = 0; i < 2; i++) {
        XrSwapchainCreateInfo sci = {};
        sci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sci.format = selectedFormat;
        sci.sampleCount = 1;
        const float resolutionScale = config::Get().resolution_scale;
        sci.width = static_cast<uint32_t>(std::clamp(
            viewConfigs[i].recommendedImageRectWidth * resolutionScale,
            256.0f, static_cast<float>(viewConfigs[i].maxImageRectWidth)));
        sci.height = static_cast<uint32_t>(std::clamp(
            viewConfigs[i].recommendedImageRectHeight * resolutionScale,
            256.0f, static_cast<float>(viewConfigs[i].maxImageRectHeight)));
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;

        XrResult r = xrCreateSwapchain(m_session, &sci, &eyes[i]->swapchain);
        if (r != XR_SUCCESS) {
            Log("[OpenXR] ERROR: xrCreateSwapchain(eye %d) = %d", i, (int)r);
            return false;
        }

        eyes[i]->width = sci.width;
        eyes[i]->height = sci.height;
        eyes[i]->submittedWidth = sci.width;
        eyes[i]->submittedHeight = sci.height;

        // Get swapchain images
        uint32_t imgCount = 0;
        xrEnumerateSwapchainImages(eyes[i]->swapchain, 0, &imgCount, nullptr);
        eyes[i]->images.resize(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages(eyes[i]->swapchain, imgCount, &imgCount, (XrSwapchainImageBaseHeader*)eyes[i]->images.data());

        Log("[OpenXR] Eye %d: %ux%u, %u images", i, sci.width, sci.height, imgCount);
    }

    // Compute IPD from eye poses
    // (will be updated each frame from LocateViews)
    Log("[OpenXR] Swapchains created");
    return true;
}

void OpenXRContext::PollEvents() {
    XrEventDataBuffer event = {};
    event.type = XR_TYPE_EVENT_DATA_BUFFER;

    while (xrPollEvent(m_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto& stateChanged = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            m_sessionState = stateChanged.state;
            Log("[OpenXR] Session state changed: %d", (int)m_sessionState);

            switch (m_sessionState) {
            case XR_SESSION_STATE_READY:
                if (!m_deviceBound) {
                    XrSessionBeginInfo bi = {};
                    bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    const XrResult beginResult = xrBeginSession(m_session, &bi);
                    if (beginResult == XR_SUCCESS) {
                        m_deviceBound = true;
                        Log("[OpenXR] Session READY - began");
                    } else {
                        Log("[OpenXR] ERROR: xrBeginSession = %d", (int)beginResult);
                    }
                }
                break;
            case XR_SESSION_STATE_STOPPING:
                if (m_deviceBound) {
                    xrEndSession(m_session);
                    m_deviceBound = false;
                    Log("[OpenXR] Session STOPPING - ended");
                }
                break;
            case XR_SESSION_STATE_LOSS_PENDING:
            case XR_SESSION_STATE_EXITING:
                Log("[OpenXR] Session state %d - shutting down", (int)m_sessionState);
                break;
            default:
                break;
            }
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

bool OpenXRContext::WaitForFrame() {
    PollEvents();
    if (!m_deviceBound) return false;

    XrFrameWaitInfo waitInfo = {};
    waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;
    XrResult r = xrWaitFrame(m_session, &waitInfo, &m_frameState);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] xrWaitFrame = %d", (int)r);
        return false;
    }
    return true;
}

bool OpenXRContext::BeginFrame() {
    if (m_frameActive) return true;

    XrFrameBeginInfo beginInfo = {};
    beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;
    XrResult r = xrBeginFrame(m_session, &beginInfo);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] xrBeginFrame = %d", (int)r);
        return false;
    }

    m_frameActive = true;
    m_viewsValid = false;
    return true;
}

bool OpenXRContext::LocateViews() {
    if (!m_frameActive || !m_frameState.shouldRender) return false;

    XrViewState viewState = {};
    viewState.type = XR_TYPE_VIEW_STATE;

    XrViewLocateInfo locInfo = {};
    locInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
    locInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locInfo.space = m_stageSpace;
    locInfo.displayTime = m_frameState.predictedDisplayTime;

    XrView views[2] = {};
    views[0].type = XR_TYPE_VIEW;
    views[1].type = XR_TYPE_VIEW;
    uint32_t viewCount = 0;

    XrResult r = xrLocateViews(m_session, &locInfo, &viewState, 2, &viewCount, views);
    if (r != XR_SUCCESS || viewCount < 2) return false;

    m_poseValid = (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                  (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

    // Extract per-eye poses
    for (int i = 0; i < 2; i++) {
        m_eyePositions[i][0] = views[i].pose.position.x;
        m_eyePositions[i][1] = views[i].pose.position.y;
        m_eyePositions[i][2] = views[i].pose.position.z;
        m_eyeRotations[i][0] = views[i].pose.orientation.x;
        m_eyeRotations[i][1] = views[i].pose.orientation.y;
        m_eyeRotations[i][2] = views[i].pose.orientation.z;
        m_eyeRotations[i][3] = views[i].pose.orientation.w;

        m_views[i] = views[i];
    }

    // Head position = average of eyes
    for (int c = 0; c < 3; c++) {
        m_headPosition[c] = (m_eyePositions[0][c] + m_eyePositions[1][c]) * 0.5f;
    }
    memcpy(m_headRotation, m_eyeRotations[0], sizeof(float) * 4);

    // IPD
    float dx = m_eyePositions[1][0] - m_eyePositions[0][0];
    float dy = m_eyePositions[1][1] - m_eyePositions[0][1];
    float dz = m_eyePositions[1][2] - m_eyePositions[0][2];
    m_ipd = sqrtf(dx * dx + dy * dy + dz * dz);

    // Compute view matrices: conjugate(rot) * translate(-pos)
    static bool loggedFov = false;
    if (!loggedFov) {
        constexpr float kRadiansToDegrees = 57.29577951308232f;
        for (int eye = 0; eye < 2; ++eye) {
            const XrFovf& fov = views[eye].fov;
            Log("[OpenXR] Eye %d FOV: left=%.2f right=%.2f up=%.2f down=%.2f "
                "total=%.2fx%.2f degrees", eye, fov.angleLeft * kRadiansToDegrees,
                fov.angleRight * kRadiansToDegrees, fov.angleUp * kRadiansToDegrees,
                fov.angleDown * kRadiansToDegrees,
                (fov.angleRight - fov.angleLeft) * kRadiansToDegrees,
                (fov.angleUp - fov.angleDown) * kRadiansToDegrees);
        }
        loggedFov = true;
    }
    for (int eye = 0; eye < 2; eye++) {
        float qx = m_eyeRotations[eye][0];
        float qy = m_eyeRotations[eye][1];
        float qz = m_eyeRotations[eye][2];
        float qw = m_eyeRotations[eye][3];

        // Conjugate quaternion
        float cqx = -qx, cqy = -qy, cqz = -qz, cqw = qw;

        // Quaternion to rotation matrix (row-major)
        float r00 = 1 - 2*(cqy*cqy + cqz*cqz);
        float r01 = 2*(cqx*cqy - cqz*cqw);
        float r02 = 2*(cqx*cqz + cqy*cqw);
        float r10 = 2*(cqx*cqy + cqz*cqw);
        float r11 = 1 - 2*(cqx*cqx + cqz*cqz);
        float r12 = 2*(cqy*cqz - cqx*cqw);
        float r20 = 2*(cqx*cqz - cqy*cqw);
        float r21 = 2*(cqy*cqz + cqx*cqw);
        float r22 = 1 - 2*(cqx*cqx + cqy*cqy);

        float tx = -r00*m_eyePositions[eye][0] - r01*m_eyePositions[eye][1] - r02*m_eyePositions[eye][2];
        float ty = -r10*m_eyePositions[eye][0] - r11*m_eyePositions[eye][1] - r12*m_eyePositions[eye][2];
        float tz = -r20*m_eyePositions[eye][0] - r21*m_eyePositions[eye][1] - r22*m_eyePositions[eye][2];

        float (*v)[4] = m_viewMatrices[eye];
        v[0][0] = r00; v[0][1] = r01; v[0][2] = r02; v[0][3] = 0;
        v[1][0] = r10; v[1][1] = r11; v[1][2] = r12; v[1][3] = 0;
        v[2][0] = r20; v[2][1] = r21; v[2][2] = r22; v[2][3] = 0;
        v[3][0] = tx;  v[3][1] = ty;  v[3][2] = tz;  v[3][3] = 1;

        // Projection matrix from FOV angles (D3D row-major)
        const XrFovf& fov = views[eye].fov;
        float tanL = tanf(fov.angleLeft);
        float tanR = tanf(fov.angleRight);
        float tanU = tanf(fov.angleUp);
        float tanD = tanf(fov.angleDown);

        float width = tanR - tanL;
        float height = tanU - tanD;

        float (*p)[4] = m_projectionMatrices[eye];
        memset(p, 0, sizeof(float) * 16);
        p[0][0] = 2.0f / width;
        p[1][1] = 2.0f / height;
        p[2][0] = (tanR + tanL) / width;
        p[2][1] = (tanU + tanD) / height;
        p[2][2] = -(m_farPlane / (m_farPlane - m_nearPlane));
        p[2][3] = -1.0f;
        p[3][2] = -(m_farPlane * m_nearPlane / (m_farPlane - m_nearPlane));
    }

    m_viewsValid = true;
    return true;
}

bool OpenXRContext::EndFrame(bool submitProjectionLayer) {
    if (!m_frameActive) return false;

    XrFrameEndInfo endInfo = {};
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = m_frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    XrCompositionLayerProjectionView projViews[2] = {};
    XrCompositionLayerProjection projLayer = {};
    const XrCompositionLayerBaseHeader* layers[1] = {};

    if (submitProjectionLayer && m_viewsValid && ShouldRender()) {
        for (int i = 0; i < 2; i++) {
            EyeData& eye = (i == 0) ? m_leftEye : m_rightEye;
            const XrView& submittedView = m_useRenderedViewPoses && m_renderedViewValid[i]
                ? m_renderedViews[i] : m_views[i];

            projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projViews[i].pose = submittedView.pose;
            projViews[i].fov = submittedView.fov;
            projViews[i].subImage.swapchain = eye.swapchain;
            projViews[i].subImage.imageArrayIndex = 0;
            projViews[i].subImage.imageRect.offset = { 0, 0 };
            projViews[i].subImage.imageRect.extent = { (int32_t)eye.submittedWidth, (int32_t)eye.submittedHeight };
        }

        projLayer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        projLayer.space = m_stageSpace;
        projLayer.viewCount = 2;
        projLayer.views = projViews;

        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projLayer);
        endInfo.layerCount = 1;
        endInfo.layers = layers;
    }

    XrResult r = xrEndFrame(m_session, &endInfo);
    if (r != XR_SUCCESS) {
        Log("[OpenXR] xrEndFrame = %d", (int)r);
    }

    m_frameActive = false;
    return r == XR_SUCCESS;
}

}} // namespace bl1gotyvr::xr
