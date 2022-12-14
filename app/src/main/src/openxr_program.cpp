// Copyright (c) 2017-2020 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "pch.h"
#include "common.h"
#include "options.h"
#include "platformdata.h"
#include "platformplugin.h"
#include "graphicsplugin.h"
#include "openxr_program.h"
#include "pController.h"
#include <common/xr_linear.h>
#include <array>
#include <cmath>
#include "cloudXRClient.h"

namespace {

#if !defined(XR_USE_PLATFORM_WIN32)
#define strcpy_s(dest, source) strncpy((dest), (source), sizeof(dest))
#endif

namespace Side {
const int LEFT = 0;
const int RIGHT = 1;
const int COUNT = 2;
}  // namespace Side

int mGsIndex = 0;
XrFrameEndInfoEXT   xrFrameEndInfoEXT;
PFN_xrResetSensorPICO  pfnXrResetSensorPICO = nullptr;
PFN_xrGetConfigPICO    pfnXrGetConfigPICO = nullptr;
PFN_xrSetConfigPICO    pfnXrSetConfigPICO = nullptr;
inline std::string GetXrVersionString(XrVersion ver) {
    return Fmt("%d.%d.%d", XR_VERSION_MAJOR(ver), XR_VERSION_MINOR(ver), XR_VERSION_PATCH(ver));
}

inline XrFormFactor GetXrFormFactor(const std::string& formFactorStr) {
    if (EqualsIgnoreCase(formFactorStr, "Hmd")) {
        return XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    }
    if (EqualsIgnoreCase(formFactorStr, "Handheld")) {
        return XR_FORM_FACTOR_HANDHELD_DISPLAY;
    }
    throw std::invalid_argument(Fmt("Unknown form factor '%s'", formFactorStr.c_str()));
}

inline XrViewConfigurationType GetXrViewConfigurationType(const std::string& viewConfigurationStr) {
    if (EqualsIgnoreCase(viewConfigurationStr, "Mono")) {
        return XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO;
    }
    if (EqualsIgnoreCase(viewConfigurationStr, "Stereo")) {
        return XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    }
    throw std::invalid_argument(Fmt("Unknown view configuration '%s'", viewConfigurationStr.c_str()));
}

inline XrEnvironmentBlendMode GetXrEnvironmentBlendMode(const std::string& environmentBlendModeStr) {
    if (EqualsIgnoreCase(environmentBlendModeStr, "Opaque")) {
        return XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    }
    if (EqualsIgnoreCase(environmentBlendModeStr, "Additive")) {
        return XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    }
    if (EqualsIgnoreCase(environmentBlendModeStr, "AlphaBlend")) {
        return XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    }
    throw std::invalid_argument(Fmt("Unknown environment blend mode '%s'", environmentBlendModeStr.c_str()));
}

namespace Math {
namespace Pose {
XrPosef Identity() {
    XrPosef t{};
    t.orientation.w = 1;
    return t;
}

XrPosef Translation(const XrVector3f& translation) {
    XrPosef t = Identity();
    t.position = translation;
    return t;
}

XrPosef RotateCCWAboutYAxis(float radians, XrVector3f translation) {
    XrPosef t = Identity();
    t.orientation.x = 0.f;
    t.orientation.y = std::sin(radians * 0.5f);
    t.orientation.z = 0.f;
    t.orientation.w = std::cos(radians * 0.5f);
    t.position = translation;
    return t;
}
}  // namespace Pose
}  // namespace Math

inline XrReferenceSpaceCreateInfo GetXrReferenceSpaceCreateInfo(const std::string& referenceSpaceTypeStr) {
    XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::Identity();
    if (EqualsIgnoreCase(referenceSpaceTypeStr, "View")) {
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "ViewFront")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::Translation({0.f, 0.f, -2.f}),
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "Local")) {
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "Stage")) {
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "StageLeft")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::RotateCCWAboutYAxis(0.f, {-2.f, 0.f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "StageRight")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::RotateCCWAboutYAxis(0.f, {2.f, 0.f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "StageLeftRotated")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::RotateCCWAboutYAxis(3.14f / 3.f, {-2.f, 0.5f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "StageRightRotated")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::RotateCCWAboutYAxis(-3.14f / 3.f, {2.f, 0.5f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else {
        throw std::invalid_argument(Fmt("Unknown reference space type '%s'", referenceSpaceTypeStr.c_str()));
    }
    return referenceSpaceCreateInfo;
}

struct OpenXrProgram : IOpenXrProgram {
    OpenXrProgram(const std::shared_ptr<Options>& options, const std::shared_ptr<IPlatformPlugin>& platformPlugin,
                  const std::shared_ptr<IGraphicsPlugin>& graphicsPlugin)
        : m_options(options), m_platformPlugin(platformPlugin), m_graphicsPlugin(graphicsPlugin) {}

    ~OpenXrProgram() override {
        if (m_input.actionSet != XR_NULL_HANDLE) {
            for (auto hand : {Side::LEFT, Side::RIGHT}) {
                xrDestroySpace(m_input.handSpace[hand]);
            }
            xrDestroyActionSet(m_input.actionSet);
        }

        for (Swapchain swapchain : m_swapchains) {
            xrDestroySwapchain(swapchain.handle);
        }

        for (XrSpace visualizedSpace : m_visualizedSpaces) {
            xrDestroySpace(visualizedSpace);
        }

        if (m_appSpace != XR_NULL_HANDLE) {
            xrDestroySpace(m_appSpace);
        }

        if (m_session != XR_NULL_HANDLE) {
            xrDestroySession(m_session);
        }

        if (m_instance != XR_NULL_HANDLE) {
            xrDestroyInstance(m_instance);
        }
    }

    static void LogLayersAndExtensions() {
        // Write out extension properties for a given layer.
        const auto logExtensions = [](const char* layerName, int indent = 0) {
            uint32_t instanceExtensionCount;
            CHECK_XRCMD(xrEnumerateInstanceExtensionProperties(layerName, 0, &instanceExtensionCount, nullptr));

            std::vector<XrExtensionProperties> extensions(instanceExtensionCount);
            for (XrExtensionProperties& extension : extensions) {
                extension.type = XR_TYPE_EXTENSION_PROPERTIES;
            }

            CHECK_XRCMD(xrEnumerateInstanceExtensionProperties(layerName, (uint32_t)extensions.size(), &instanceExtensionCount, extensions.data()));

            const std::string indentStr(indent, ' ');
            Log::Write(Log::Level::Info, Fmt("%sAvailable Extensions: (%d)", indentStr.c_str(), instanceExtensionCount));
            for (const XrExtensionProperties& extension : extensions) {
                Log::Write(Log::Level::Info, Fmt("%s  Name=%s SpecVersion=%d", indentStr.c_str(), extension.extensionName,
                                                    extension.extensionVersion));
            }
        };

        // Log non-layer extensions (layerName==nullptr).
        logExtensions(nullptr);

        // Log layers and any of their extensions.
        {
            uint32_t layerCount;
            CHECK_XRCMD(xrEnumerateApiLayerProperties(0, &layerCount, nullptr));

            std::vector<XrApiLayerProperties> layers(layerCount);
            for (XrApiLayerProperties& layer : layers) {
                layer.type = XR_TYPE_API_LAYER_PROPERTIES;
            }

            CHECK_XRCMD(xrEnumerateApiLayerProperties((uint32_t)layers.size(), &layerCount, layers.data()));

            Log::Write(Log::Level::Info, Fmt("Available Layers: (%d)", layerCount));
            for (const XrApiLayerProperties& layer : layers) {
                Log::Write(Log::Level::Info,
                           Fmt("  Name=%s SpecVersion=%s LayerVersion=%d Description=%s", layer.layerName,
                               GetXrVersionString(layer.specVersion).c_str(), layer.layerVersion, layer.description));
                logExtensions(layer.layerName, 4);
            }
        }
    }

    void LogInstanceInfo() {
        CHECK(m_instance != XR_NULL_HANDLE);

        XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
        CHECK_XRCMD(xrGetInstanceProperties(m_instance, &instanceProperties));

        Log::Write(Log::Level::Info, Fmt("Instance RuntimeName=%s RuntimeVersion=%s", instanceProperties.runtimeName,
                                         GetXrVersionString(instanceProperties.runtimeVersion).c_str()));
    }

    void CreateInstanceInternal() {
        CHECK(m_instance == XR_NULL_HANDLE);

        // Create union of extensions required by platform and graphics plugins.
        std::vector<const char*> extensions;

        // Transform platform and graphics extension std::strings to C strings.
        const std::vector<std::string> platformExtensions = m_platformPlugin->GetInstanceExtensions();
        std::transform(platformExtensions.begin(), platformExtensions.end(), std::back_inserter(extensions),
                       [](const std::string& ext) { return ext.c_str(); });
        const std::vector<std::string> graphicsExtensions = m_graphicsPlugin->GetInstanceExtensions();
        std::transform(graphicsExtensions.begin(), graphicsExtensions.end(), std::back_inserter(extensions),
                       [](const std::string& ext) { return ext.c_str(); });

        extensions.push_back(XR_PICO_VIEW_STATE_EXT_ENABLE_EXTENSION_NAME);
        extensions.push_back(XR_PICO_FRAME_END_INFO_EXT_EXTENSION_NAME);
        // Enable Pico controller extension
        extensions.push_back(XR_PICO_ANDROID_CONTROLLER_FUNCTION_EXT_ENABLE_EXTENSION_NAME);
        // Enable reset haed sensor extension
        extensions.push_back(XR_PICO_CONFIGS_EXT_EXTENSION_NAME);
        extensions.push_back(XR_PICO_RESET_SENSOR_EXTENSION_NAME);
        // Enable Pico ipd extension
        extensions.push_back(XR_PICO_IPD_EXTENSION_NAME);
        XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        createInfo.next = m_platformPlugin->GetInstanceCreateExtension();
        createInfo.enabledExtensionCount = (uint32_t)extensions.size();
        createInfo.enabledExtensionNames = extensions.data();

        strcpy(createInfo.applicationInfo.applicationName, "HelloXR");
        createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

        CHECK_XRCMD(xrCreateInstance(&createInfo, &m_instance));
        pxr::InitializeGraphicDeivce(m_instance);
        xrGetInstanceProcAddr(m_instance, "xrGetConfigPICO", reinterpret_cast<PFN_xrVoidFunction *>(&pfnXrGetConfigPICO));
        xrGetInstanceProcAddr(m_instance, "xrSetConfigPICO", reinterpret_cast<PFN_xrVoidFunction *>(&pfnXrSetConfigPICO));
        xrGetInstanceProcAddr(m_instance, "xrResetSensorPICO", reinterpret_cast<PFN_xrVoidFunction *>(&pfnXrResetSensorPICO));
    }

    void CreateInstance() override {
        LogLayersAndExtensions();

        CreateInstanceInternal();

        LogInstanceInfo();
    }

    void LogViewConfigurations() {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId != XR_NULL_SYSTEM_ID);

        uint32_t viewConfigTypeCount;
        CHECK_XRCMD(xrEnumerateViewConfigurations(m_instance, m_systemId, 0, &viewConfigTypeCount, nullptr));
        std::vector<XrViewConfigurationType> viewConfigTypes(viewConfigTypeCount);
        CHECK_XRCMD(xrEnumerateViewConfigurations(m_instance, m_systemId, viewConfigTypeCount, &viewConfigTypeCount,
                                                  viewConfigTypes.data()));
        CHECK((uint32_t)viewConfigTypes.size() == viewConfigTypeCount);

        Log::Write(Log::Level::Info, Fmt("Available View Configuration Types: (%d)", viewConfigTypeCount));
        for (XrViewConfigurationType viewConfigType : viewConfigTypes) {
            Log::Write(Log::Level::Info, Fmt("  View Configuration Type: %s %s", to_string(viewConfigType),
                                                viewConfigType == m_viewConfigType ? "(Selected)" : ""));

            XrViewConfigurationProperties viewConfigProperties{XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
            CHECK_XRCMD(xrGetViewConfigurationProperties(m_instance, m_systemId, viewConfigType, &viewConfigProperties));

            Log::Write(Log::Level::Info,
                       Fmt("  View configuration FovMutable=%s", viewConfigProperties.fovMutable == XR_TRUE ? "True" : "False"));

            uint32_t viewCount;
            CHECK_XRCMD(xrEnumerateViewConfigurationViews(m_instance, m_systemId, viewConfigType, 0, &viewCount, nullptr));
            if (viewCount > 0) {
                std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
                CHECK_XRCMD(xrEnumerateViewConfigurationViews(m_instance, m_systemId, viewConfigType, viewCount, &viewCount, views.data()));

                for (uint32_t i = 0; i < views.size(); i++) {
                    const XrViewConfigurationView& view = views[i];

                    Log::Write(Log::Level::Info, Fmt("    View [%d]: Recommended Width=%d Height=%d SampleCount=%d", i,
                                                        view.recommendedImageRectWidth, view.recommendedImageRectHeight,
                                                        view.recommendedSwapchainSampleCount));
                    Log::Write(Log::Level::Info, Fmt("    View [%d]:     Maximum Width=%d Height=%d SampleCount=%d", i, view.maxImageRectWidth,
                                                        view.maxImageRectHeight, view.maxSwapchainSampleCount));
                }
            } else {
                Log::Write(Log::Level::Error, Fmt("Empty view configuration type"));
            }

            LogEnvironmentBlendMode(viewConfigType);
        }
    }

    void LogEnvironmentBlendMode(XrViewConfigurationType type) {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId != 0);

        uint32_t count;
        CHECK_XRCMD(xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, type, 0, &count, nullptr));
        CHECK(count > 0);

        Log::Write(Log::Level::Info, Fmt("Available Environment Blend Mode count : (%d)", count));

        std::vector<XrEnvironmentBlendMode> blendModes(count);
        CHECK_XRCMD(xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, type, count, &count, blendModes.data()));

        bool blendModeFound = false;
        for (XrEnvironmentBlendMode mode : blendModes) {
            const bool blendModeMatch = (mode == m_environmentBlendMode);
            Log::Write(Log::Level::Info,
                       Fmt("Environment Blend Mode (%s) : %s", to_string(mode), blendModeMatch ? "(Selected)" : ""));
            blendModeFound |= blendModeMatch;
        }
        CHECK(blendModeFound);
    }

    void InitializeSystem() override {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId == XR_NULL_SYSTEM_ID);

        m_formFactor = GetXrFormFactor(m_options->FormFactor);
        m_viewConfigType = GetXrViewConfigurationType(m_options->ViewConfiguration);
        m_environmentBlendMode = GetXrEnvironmentBlendMode(m_options->EnvironmentBlendMode);

        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = m_formFactor;
        CHECK_XRCMD(xrGetSystem(m_instance, &systemInfo, &m_systemId));

        Log::Write(Log::Level::Info, Fmt("Using system %d for form factor %s", m_systemId, to_string(m_formFactor)));
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId != XR_NULL_SYSTEM_ID);

        LogViewConfigurations();

        // The graphics API can initialize the graphics device now that the systemId and instance
        // handle are available.
        m_graphicsPlugin->InitializeDevice(m_instance, m_systemId);
    }

    void LogReferenceSpaces() {
        CHECK(m_session != XR_NULL_HANDLE);

        uint32_t spaceCount;
        CHECK_XRCMD(xrEnumerateReferenceSpaces(m_session, 0, &spaceCount, nullptr));
        std::vector<XrReferenceSpaceType> spaces(spaceCount);
        CHECK_XRCMD(xrEnumerateReferenceSpaces(m_session, spaceCount, &spaceCount, spaces.data()));

        Log::Write(Log::Level::Info, Fmt("Available reference spaces: %d", spaceCount));
        for (XrReferenceSpaceType space : spaces) {
            Log::Write(Log::Level::Info, Fmt("  Name: %s", to_string(space)));
        }
    }

    struct InputState {
        XrActionSet actionSet{XR_NULL_HANDLE};
        XrAction grabAction{XR_NULL_HANDLE};
        XrAction poseAction{XR_NULL_HANDLE};
        XrAction vibrateAction{XR_NULL_HANDLE};
        XrAction quitAction{XR_NULL_HANDLE};
        /*************************pico******************/
        XrAction touchpadAction{XR_NULL_HANDLE};
        XrAction AXAction{XR_NULL_HANDLE};
        XrAction homeAction{XR_NULL_HANDLE};
        XrAction BYAction{XR_NULL_HANDLE};
        XrAction backAction{XR_NULL_HANDLE};
        XrAction sideAction{XR_NULL_HANDLE};
        XrAction triggerAction{XR_NULL_HANDLE};
        XrAction joystickAction{XR_NULL_HANDLE};
        XrAction batteryAction{XR_NULL_HANDLE};
        //---add new----------
        XrAction AXTouchAction{XR_NULL_HANDLE};
        XrAction BYTouchAction{XR_NULL_HANDLE};
        XrAction RockerTouchAction{XR_NULL_HANDLE};
        XrAction TriggerTouchAction{XR_NULL_HANDLE};
        XrAction ThumbrestTouchAction{XR_NULL_HANDLE};
        XrAction GripAction{XR_NULL_HANDLE};
        //---add new----------zgt
        XrAction AAction{XR_NULL_HANDLE};
        XrAction BAction{XR_NULL_HANDLE};
        XrAction XAction{XR_NULL_HANDLE};
        XrAction YAction{XR_NULL_HANDLE};
        XrAction ATouchAction{XR_NULL_HANDLE};
        XrAction BTouchAction{XR_NULL_HANDLE};
        XrAction XTouchAction{XR_NULL_HANDLE};
        XrAction YTouchAction{XR_NULL_HANDLE};
        XrAction aimAction{XR_NULL_HANDLE};
        /*************************pico******************/
        std::array<XrSpace, Side::COUNT> aimSpace;
        std::array<XrPath, Side::COUNT> handSubactionPath;
        std::array<XrSpace, Side::COUNT> handSpace;
        std::array<float, Side::COUNT> handScale = {{1.0f, 1.0f}};
        std::array<XrVector2f, Side::COUNT> handXYPos = {0.0f};
        std::array<XrBool32, Side::COUNT> handActive;
    };

    void InitializeActions() {
        // Create an action set.
        {
            XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
            strcpy_s(actionSetInfo.actionSetName, "gameplay");
            strcpy_s(actionSetInfo.localizedActionSetName, "Gameplay");
            actionSetInfo.priority = 0;
            CHECK_XRCMD(xrCreateActionSet(m_instance, &actionSetInfo, &m_input.actionSet));
        }

        // Get the XrPath for the left and right hands - we will use them as subaction paths.
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left", &m_input.handSubactionPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right", &m_input.handSubactionPath[Side::RIGHT]));

        // Create actions.
        {
            // Create an input action for grabbing objects with the left and right hands.
            XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
            actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
            strcpy_s(actionInfo.actionName, "grab_object");
            strcpy_s(actionInfo.localizedActionName, "Grab Object");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.grabAction));

            // Create an input action getting the left and right hand poses.
            actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
            strcpy_s(actionInfo.actionName, "hand_pose");
            strcpy_s(actionInfo.localizedActionName, "Hand Pose");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.poseAction));

            actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
            strcpy_s(actionInfo.actionName, "aim_pose");
            strcpy_s(actionInfo.localizedActionName, "Aim Pose");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.aimAction));

            // Create output actions for vibrating the left and right controller.
            actionInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
            strcpy_s(actionInfo.actionName, "vibrate_hand");
            strcpy_s(actionInfo.localizedActionName, "Vibrate Hand");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.vibrateAction));

            // Create input actions for quitting the session using the left and right controller.
            // Since it doesn't matter which hand did this, we do not specify subaction paths for it.
            // We will just suggest bindings for both hands, where possible.
            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "quit_session");
            strcpy_s(actionInfo.localizedActionName, "Quit Session");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.quitAction));
            /**********************************pico***************************************/
            // Create input actions for toucpad key using the left and right controller.
            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "touchpad");
            strcpy_s(actionInfo.localizedActionName, "Touchpad");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.touchpadAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "axkey");
            strcpy_s(actionInfo.localizedActionName, "AXkey");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.AXAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "homekey");
            strcpy_s(actionInfo.localizedActionName, "Homekey");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.homeAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "bykey");
            strcpy_s(actionInfo.localizedActionName, "BYkey");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.BYAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "backkey");
            strcpy_s(actionInfo.localizedActionName, "Backkey");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.backAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "sidekey");
            strcpy_s(actionInfo.localizedActionName, "Sidekey");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.sideAction));

            actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
            strcpy_s(actionInfo.actionName, "trigger");
            strcpy_s(actionInfo.localizedActionName, "Trigger");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.triggerAction));

            actionInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
            strcpy_s(actionInfo.actionName, "joystick");
            strcpy_s(actionInfo.localizedActionName, "Joystick");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.joystickAction));

            actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
            strcpy_s(actionInfo.actionName, "battery");
            strcpy_s(actionInfo.localizedActionName, "battery");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.batteryAction));
             //------------------------add new---------------------------------
            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "axtouch");
            strcpy_s(actionInfo.localizedActionName, "AXtouch");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.AXTouchAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "bytouch");
            strcpy_s(actionInfo.localizedActionName, "BYtouch");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.BYTouchAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "rockertouch");
            strcpy_s(actionInfo.localizedActionName, "Rockertouch");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.RockerTouchAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "triggertouch");
            strcpy_s(actionInfo.localizedActionName, "Triggertouch");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.TriggerTouchAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "thumbresttouch");
            strcpy_s(actionInfo.localizedActionName, "Thumbresttouch");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.ThumbrestTouchAction));

            actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
            strcpy_s(actionInfo.actionName, "gripvalue");
            strcpy_s(actionInfo.localizedActionName, "GripValue");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.GripAction));

            //--------------add new----------zgt
            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "akey");
            strcpy_s(actionInfo.localizedActionName, "Akey");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.AAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "bkey");
            strcpy_s(actionInfo.localizedActionName, "Bkey");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.BAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "xkey");
            strcpy_s(actionInfo.localizedActionName, "Xkey");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.XAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "ykey");
            strcpy_s(actionInfo.localizedActionName, "Ykey");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.YAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "atouch");
            strcpy_s(actionInfo.localizedActionName, "Atouch");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.ATouchAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "btouch");
            strcpy_s(actionInfo.localizedActionName, "Btouch");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.BTouchAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "xtouch");
            strcpy_s(actionInfo.localizedActionName, "Xtouch");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.XTouchAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "ytouch");
            strcpy_s(actionInfo.localizedActionName, "Ytouch");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.YTouchAction));
            /**********************************pico***************************************/
        }

        std::array<XrPath, Side::COUNT> selectPath;
        std::array<XrPath, Side::COUNT> squeezeValuePath;
        std::array<XrPath, Side::COUNT> squeezeClickPath;
        std::array<XrPath, Side::COUNT> posePath;
        std::array<XrPath, Side::COUNT> hapticPath;
        std::array<XrPath, Side::COUNT> menuClickPath;
        std::array<XrPath, Side::COUNT> systemPath;
        std::array<XrPath, Side::COUNT> thumbrestPath;
        std::array<XrPath, Side::COUNT> triggerTouchPath;
        std::array<XrPath, Side::COUNT> triggerValuePath;
        std::array<XrPath, Side::COUNT> thumbstickClickPath;
        std::array<XrPath, Side::COUNT> thumbstickTouchPath;
        std::array<XrPath, Side::COUNT> thumbstickPosPath;
        std::array<XrPath, Side::COUNT> aimPath;

        /**************************pico************************************/
        std::array<XrPath, Side::COUNT> touchpadPath;
        std::array<XrPath, Side::COUNT> AXValuePath;
        std::array<XrPath, Side::COUNT> homeClickPath;
        std::array<XrPath, Side::COUNT> BYValuePath;
        std::array<XrPath, Side::COUNT> backPath;
        std::array<XrPath, Side::COUNT> sideClickPath;
        std::array<XrPath, Side::COUNT> triggerPath;
        std::array<XrPath, Side::COUNT> joystickPath;
        std::array<XrPath, Side::COUNT> batteryPath;
        //--------------add new----------
        std::array<XrPath, Side::COUNT> GripPath;
        std::array<XrPath, Side::COUNT> AXTouchPath;
        std::array<XrPath, Side::COUNT> BYTouchPath;
        std::array<XrPath, Side::COUNT> RockerTouchPath;
        std::array<XrPath, Side::COUNT> TriggerTouchPath;
        std::array<XrPath, Side::COUNT> ThumbresetTouchPath;
        /**************************pico************************************/
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/select/click", &selectPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/select/click", &selectPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/menu/click", &menuClickPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/menu/click", &menuClickPath[Side::RIGHT]));

        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/squeeze/value", &squeezeValuePath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/squeeze/value", &squeezeValuePath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/squeeze/click", &squeezeClickPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/squeeze/click", &squeezeClickPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/grip/pose", &posePath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/grip/pose", &posePath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/aim/pose", &aimPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/aim/pose", &aimPath[Side::RIGHT]));

        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/output/haptic", &hapticPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/output/haptic", &hapticPath[Side::RIGHT]));

        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/trigger/touch", &triggerTouchPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/trigger/touch", &triggerTouchPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/trigger/value", &triggerValuePath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/trigger/value", &triggerValuePath[Side::RIGHT]));

        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/thumbstick/click", &thumbstickClickPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/thumbstick/click", &thumbstickClickPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/thumbstick/touch", &thumbstickTouchPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/thumbstick/touch", &thumbstickTouchPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/thumbstick", &thumbstickPosPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/thumbstick", &thumbstickPosPath[Side::RIGHT]));

        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/system/click", &systemPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/system/click", &systemPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/thumbrest/touch", &thumbrestPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/thumbrest/touch", &thumbrestPath[Side::RIGHT]));

        /**************************pico************************************/
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/back/click", &backPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/back/click", &backPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/battery/value", &batteryPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/battery/value", &batteryPath[Side::RIGHT]));

        //--------------add new----------zgt
        std::array<XrPath, Side::COUNT> AValuePath;
        std::array<XrPath, Side::COUNT> BValuePath;
        std::array<XrPath, Side::COUNT> XValuePath;
        std::array<XrPath, Side::COUNT> YValuePath;
        std::array<XrPath, Side::COUNT> ATouchPath;
        std::array<XrPath, Side::COUNT> BTouchPath;
        std::array<XrPath, Side::COUNT> XTouchPath;
        std::array<XrPath, Side::COUNT> YTouchPath;
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/x/click", &XValuePath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/y/click", &YValuePath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/a/click", &AValuePath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/b/click", &BValuePath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/x/touch", &XTouchPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/y/touch", &YTouchPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/a/touch", &ATouchPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/b/touch", &BTouchPath[Side::RIGHT]));
        /**************************pico************************************/
        // Suggest bindings for KHR Simple.
       /* {
           XrPath khrSimpleInteractionProfilePath;
            CHECK_XRCMD(
                xrStringToPath(up.mInstance, "/interaction_profiles/khr/simple_controller", &khrSimpleInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{// Fall back to a click input for the grab action.
                                                            {m_input.grabAction, selectPath[Side::LEFT]},
                                                            {m_input.grabAction, selectPath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, menuClickPath[Side::LEFT]},
                                                            {m_input.quitAction, menuClickPath[Side::RIGHT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = khrSimpleInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }
        // Suggest bindings for the Oculus Touch.
        {
            XrPath oculusTouchInteractionProfilePath;
            CHECK_XRCMD(
                xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &oculusTouchInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{{m_input.grabAction, squeezeValuePath[Side::LEFT]},
                                                            {m_input.grabAction, squeezeValuePath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, menuClickPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = oculusTouchInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }
        // Suggest bindings for the Vive Controller.
        {
            XrPath viveControllerInteractionProfilePath;
            CHECK_XRCMD(
                xrStringToPath(m_instance, "/interaction_profiles/htc/vive_controller", &viveControllerInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{{m_input.grabAction, squeezeClickPath[Side::LEFT]},
                                                            {m_input.grabAction, squeezeClickPath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, menuClickPath[Side::LEFT]},
                                                            {m_input.quitAction, menuClickPath[Side::RIGHT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = viveControllerInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }

        // Suggest bindings for the Microsoft Mixed Reality Motion Controller.
        {
            XrPath microsoftMixedRealityInteractionProfilePath;
            CHECK_XRCMD(xrStringToPath(m_instance, "/interaction_profiles/microsoft/motion_controller",
                                       &microsoftMixedRealityInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{{m_input.grabAction, squeezeClickPath[Side::LEFT]},
                                                            {m_input.grabAction, squeezeClickPath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, menuClickPath[Side::LEFT]},
                                                            {m_input.quitAction, menuClickPath[Side::RIGHT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = microsoftMixedRealityInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }*/
        // Suggest bindings for the Microsoft Mixed Reality Motion Controller.
        {
            XrPath picoMixedRealityInteractionProfilePath;
            CHECK_XRCMD(xrStringToPath(m_instance, "/interaction_profiles/pico/neo3_controller",
                                       &picoMixedRealityInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{

                                                           {m_input.touchpadAction, thumbstickClickPath[Side::LEFT]},
                                                           {m_input.touchpadAction, thumbstickClickPath[Side::RIGHT]},
                                                           {m_input.joystickAction, thumbstickPosPath[Side::LEFT]},
                                                           {m_input.joystickAction, thumbstickPosPath[Side::RIGHT]},
                                                           {m_input.RockerTouchAction, thumbstickTouchPath[Side::LEFT]},
                                                           {m_input.RockerTouchAction, thumbstickTouchPath[Side::RIGHT]},

                                                           {m_input.triggerAction, triggerValuePath[Side::LEFT]},
                                                           {m_input.triggerAction, triggerValuePath[Side::RIGHT]},
                                                           {m_input.TriggerTouchAction, triggerTouchPath[Side::LEFT]},
                                                           {m_input.TriggerTouchAction, triggerTouchPath[Side::RIGHT]},

                                                           {m_input.sideAction, squeezeClickPath[Side::LEFT]},
                                                           {m_input.sideAction, squeezeClickPath[Side::RIGHT]},
                                                           {m_input.GripAction, squeezeValuePath[Side::LEFT]},
                                                           {m_input.GripAction, squeezeValuePath[Side::RIGHT]},
                                                           {m_input.poseAction, posePath[Side::LEFT]},
                                                           {m_input.poseAction, posePath[Side::RIGHT]},

                                                           {m_input.homeAction, systemPath[Side::LEFT]},
                                                           {m_input.homeAction, systemPath[Side::RIGHT]},
                                                           {m_input.backAction, backPath[Side::LEFT]},
                                                           {m_input.backAction, backPath[Side::RIGHT]},
                                                           {m_input.batteryAction, batteryPath[Side::LEFT]},
                                                           {m_input.batteryAction, batteryPath[Side::RIGHT]},

                                                           {m_input.ThumbrestTouchAction, thumbrestPath[Side::LEFT]},
                                                           {m_input.ThumbrestTouchAction, thumbrestPath[Side::RIGHT]},

                                                           /*{m_input.AXAction, AXValuePath[Side::LEFT]},
                                                           {m_input.AXAction, AXValuePath[Side::RIGHT]},
                                                           {m_input.BYAction, BYValuePath[Side::LEFT]},
                                                           {m_input.BYAction, BYValuePath[Side::RIGHT]},
                                                           {m_input.AXTouchAction, AXTouchPath[Side::LEFT]},
                                                           {m_input.AXTouchAction, AXTouchPath[Side::RIGHT]},
                                                           {m_input.BYTouchAction, BYTouchPath[Side::LEFT]},
                                                           {m_input.BYTouchAction, BYTouchPath[Side::RIGHT]},*/
                                                            //--------------add new------------------zgt
                                                            {m_input.XTouchAction, XTouchPath[Side::LEFT]},
                                                            {m_input.YTouchAction, YTouchPath[Side::LEFT]},
                                                            {m_input.ATouchAction, ATouchPath[Side::RIGHT]},
                                                            {m_input.BTouchAction, BTouchPath[Side::RIGHT]},
                                                            {m_input.XAction, XValuePath[Side::LEFT]},
                                                            {m_input.YAction, YValuePath[Side::LEFT]},
                                                            {m_input.AAction, AValuePath[Side::RIGHT]},
                                                            {m_input.BAction, BValuePath[Side::RIGHT]},
                                                            {m_input.aimAction, aimPath[Side::LEFT]},
                                                            {m_input.aimAction, aimPath[Side::RIGHT]}
                                                            }};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = picoMixedRealityInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }

        XrActionSpaceCreateInfo actionSpaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        actionSpaceInfo.action = m_input.poseAction;
        actionSpaceInfo.poseInActionSpace.orientation.w = 1.f;
        actionSpaceInfo.subactionPath = m_input.handSubactionPath[Side::LEFT];
        CHECK_XRCMD(xrCreateActionSpace(m_session, &actionSpaceInfo, &m_input.handSpace[Side::LEFT]));
        actionSpaceInfo.subactionPath = m_input.handSubactionPath[Side::RIGHT];
        CHECK_XRCMD(xrCreateActionSpace(m_session, &actionSpaceInfo, &m_input.handSpace[Side::RIGHT]));
        actionSpaceInfo.action = m_input.aimAction;
        actionSpaceInfo.poseInActionSpace.orientation.w = 1.f;
        actionSpaceInfo.subactionPath = m_input.handSubactionPath[Side::LEFT];
        CHECK_XRCMD(xrCreateActionSpace(m_session, &actionSpaceInfo, &m_input.aimSpace[Side::LEFT]));
        actionSpaceInfo.subactionPath = m_input.handSubactionPath[Side::RIGHT];
        CHECK_XRCMD(xrCreateActionSpace(m_session, &actionSpaceInfo, &m_input.aimSpace[Side::RIGHT]));

        XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attachInfo.countActionSets = 1;
        attachInfo.actionSets = &m_input.actionSet;
        CHECK_XRCMD(xrAttachSessionActionSets(m_session, &attachInfo));
    }

    void CreateVisualizedSpaces() {
        CHECK(m_session != XR_NULL_HANDLE);

        std::string visualizedSpaces[] = {"ViewFront", "Local", "Stage", "StageLeft", "StageRight", "StageLeftRotated", "StageRightRotated"};

        for (const auto& visualizedSpace : visualizedSpaces) {
            XrReferenceSpaceCreateInfo referenceSpaceCreateInfo = GetXrReferenceSpaceCreateInfo(visualizedSpace);
            XrSpace space;
            XrResult res = xrCreateReferenceSpace(m_session, &referenceSpaceCreateInfo, &space);
            if (XR_SUCCEEDED(res)) {
                m_visualizedSpaces.push_back(space);
            } else {
                Log::Write(Log::Level::Warning, Fmt("Failed to create reference space %s with error %d", visualizedSpace.c_str(), res));
            }
        }

        XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::Identity();
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        CHECK_XRCMD(xrCreateReferenceSpace(m_session, &referenceSpaceCreateInfo, &m_ViewSpace));

    }

    void InitializeSession() override {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_session == XR_NULL_HANDLE);

        {
            Log::Write(Log::Level::Info, Fmt("Creating session..."));

            XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
            createInfo.next = m_graphicsPlugin->GetGraphicsBinding();
            createInfo.systemId = m_systemId;
            CHECK_XRCMD(xrCreateSession(m_instance, &createInfo, &m_session));
        }
        //set eye level
        pfnXrSetConfigPICO(m_session, TRACKING_ORIGIN, (char*)"0");
        LogReferenceSpaces();
        InitializeActions();
        CreateVisualizedSpaces();

        {
            XrReferenceSpaceCreateInfo referenceSpaceCreateInfo = GetXrReferenceSpaceCreateInfo(m_options->AppSpace);
            CHECK_XRCMD(xrCreateReferenceSpace(m_session, &referenceSpaceCreateInfo, &m_appSpace));
        }
    }

    void CreateSwapchains() override {
        CHECK(m_session != XR_NULL_HANDLE);
        CHECK(m_swapchains.empty());
        CHECK(m_configViews.empty());

        // Read graphics properties for preferred swapchain length and logging.
        XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
        CHECK_XRCMD(xrGetSystemProperties(m_instance, m_systemId, &systemProperties));

        // Log system properties.
        Log::Write(Log::Level::Info, Fmt("System Properties: Name=%s VendorId=%d", systemProperties.systemName, systemProperties.vendorId));
        Log::Write(Log::Level::Info, Fmt("System Graphics Properties: MaxWidth=%d MaxHeight=%d MaxLayers=%d",
                                         systemProperties.graphicsProperties.maxSwapchainImageWidth,
                                         systemProperties.graphicsProperties.maxSwapchainImageHeight,
                                         systemProperties.graphicsProperties.maxLayerCount));
        Log::Write(Log::Level::Info, Fmt("System Tracking Properties: OrientationTracking=%s PositionTracking=%s",
                                         systemProperties.trackingProperties.orientationTracking == XR_TRUE ? "True" : "False",
                                         systemProperties.trackingProperties.positionTracking == XR_TRUE ? "True" : "False"));

        // Note: No other view configurations exist at the time this code was written. If this
        // condition is not met, the project will need to be audited to see how support should be
        // added.
        CHECK_MSG(m_viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, "Unsupported view configuration type");

        // Query and cache view configuration views.
        uint32_t viewCount;
        CHECK_XRCMD(xrEnumerateViewConfigurationViews(m_instance, m_systemId, m_viewConfigType, 0, &viewCount, nullptr));
        m_configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        CHECK_XRCMD(xrEnumerateViewConfigurationViews(m_instance, m_systemId, m_viewConfigType, viewCount, &viewCount, m_configViews.data()));

        // Create and cache view buffer for xrLocateViews later.
        m_views.resize(viewCount, {XR_TYPE_VIEW});

        // Create the swapchain and get the images.
        if (viewCount > 0) {
            // Select a swapchain format.
            uint32_t swapchainFormatCount;
            CHECK_XRCMD(xrEnumerateSwapchainFormats(m_session, 0, &swapchainFormatCount, nullptr));
            std::vector<int64_t> swapchainFormats(swapchainFormatCount);
            CHECK_XRCMD(xrEnumerateSwapchainFormats(m_session, (uint32_t)swapchainFormats.size(), &swapchainFormatCount, swapchainFormats.data()));
            CHECK(swapchainFormatCount == swapchainFormats.size());
            m_colorSwapchainFormat = m_graphicsPlugin->SelectColorSwapchainFormat(swapchainFormats);

            // Print swapchain formats and the selected one.
            {
                std::string swapchainFormatsString;
                for (int64_t format : swapchainFormats) {
                    const bool selected = format == m_colorSwapchainFormat;
                    swapchainFormatsString += " ";
                    if (selected) {
                        swapchainFormatsString += "[";
                    }
                    swapchainFormatsString += std::to_string(format);
                    if (selected) {
                        swapchainFormatsString += "]";
                    }
                }
                Log::Write(Log::Level::Info, Fmt("Swapchain Formats: %s", swapchainFormatsString.c_str()));
            }

            // Create a swapchain for each view.
            for (uint32_t i = 0; i < viewCount; i++) {
                const XrViewConfigurationView& vp = m_configViews[i];
                Log::Write(Log::Level::Info,
                           Fmt("Creating swapchain for view %d with dimensions Width=%d Height=%d SampleCount=%d", i,
                               vp.recommendedImageRectWidth, vp.recommendedImageRectHeight, vp.recommendedSwapchainSampleCount));

                // Create the swapchain.
                XrSwapchainCreateInfo swapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                swapchainCreateInfo.arraySize = 1;
                swapchainCreateInfo.format = m_colorSwapchainFormat;
                swapchainCreateInfo.width = vp.recommendedImageRectWidth;
                swapchainCreateInfo.height = vp.recommendedImageRectHeight;
                swapchainCreateInfo.mipCount = 1;
                swapchainCreateInfo.faceCount = 1;
                swapchainCreateInfo.sampleCount = m_graphicsPlugin->GetSupportedSwapchainSampleCount(vp);
                swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                Swapchain swapchain;
                swapchain.width = swapchainCreateInfo.width;
                swapchain.height = swapchainCreateInfo.height;
                CHECK_XRCMD(xrCreateSwapchain(m_session, &swapchainCreateInfo, &swapchain.handle));

                m_swapchains.push_back(swapchain);

                uint32_t imageCount;
                CHECK_XRCMD(xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr));
                // XXX This should really just return XrSwapchainImageBaseHeader*
                std::vector<XrSwapchainImageBaseHeader*> swapchainImages =
                    m_graphicsPlugin->AllocateSwapchainImageStructs(imageCount, swapchainCreateInfo);
                CHECK_XRCMD(xrEnumerateSwapchainImages(swapchain.handle, imageCount, &imageCount, swapchainImages[0]));

                m_swapchainImages.insert(std::make_pair(swapchain.handle, std::move(swapchainImages)));
            }
        }
    }

    // Return event if one is available, otherwise return null.
    const XrEventDataBaseHeader* TryReadNextEvent() {
        // It is sufficient to clear the just the XrEventDataBuffer header to
        // XR_TYPE_EVENT_DATA_BUFFER
        XrEventDataBaseHeader* baseHeader = reinterpret_cast<XrEventDataBaseHeader*>(&m_eventDataBuffer);
        *baseHeader = {XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult xr = xrPollEvent(m_instance, &m_eventDataBuffer);
        if (xr == XR_SUCCESS) {
            if (baseHeader->type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
                const XrEventDataEventsLost* const eventsLost = reinterpret_cast<const XrEventDataEventsLost*>(baseHeader);
                Log::Write(Log::Level::Warning, Fmt("%d events lost", eventsLost));
            }

            return baseHeader;
        }
        if (xr == XR_EVENT_UNAVAILABLE) {
            return nullptr;
        }
        THROW_XR(xr, "xrPollEvent");
    }

    void PollEvents(bool* exitRenderLoop, bool* requestRestart) override {
        *exitRenderLoop = *requestRestart = false;

        // Process all pending messages.
        while (const XrEventDataBaseHeader* event = TryReadNextEvent()) {
            switch (event->type) {
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
                    const auto& instanceLossPending = *reinterpret_cast<const XrEventDataInstanceLossPending*>(event);
                    Log::Write(Log::Level::Warning, Fmt("XrEventDataInstanceLossPending by %lld", instanceLossPending.lossTime));
                    *exitRenderLoop = true;
                    *requestRestart = true;
                    return;
                }
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    auto sessionStateChangedEvent = *reinterpret_cast<const XrEventDataSessionStateChanged*>(event);
                    HandleSessionStateChangedEvent(sessionStateChangedEvent, exitRenderLoop, requestRestart);
                    break;
                }
                case XR_TYPE_EVENT_CONTROLLER_STATE_CHANGED: {
                    auto eventDataPerfSettingsEXT = *reinterpret_cast<const XrControllerEventChanged *>(event);
                    Log::Write(Log::Level::Info, Fmt("controller event callback controller %d, status %d  eventtype %d",
                            eventDataPerfSettingsEXT.controller,eventDataPerfSettingsEXT.status,eventDataPerfSettingsEXT.eventtype));
                    break;
                }
                case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                    LogActionSourceName(m_input.grabAction, "Grab");
                    LogActionSourceName(m_input.quitAction, "Quit");
                    LogActionSourceName(m_input.poseAction, "Pose");
                    LogActionSourceName(m_input.vibrateAction, "Vibrate");
                    break;
                case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                default: {
                    Log::Write(Log::Level::Info, Fmt("Ignoring event type %d", event->type));
                    break;
                }
            }
        }
    }

    void HandleSessionStateChangedEvent(const XrEventDataSessionStateChanged& stateChangedEvent, bool* exitRenderLoop,
                                        bool* requestRestart) {
        const XrSessionState oldState = m_sessionState;
        m_sessionState = stateChangedEvent.state;

        Log::Write(Log::Level::Info, Fmt("XrEventDataSessionStateChanged: state %s->%s session=%lld time=%lld", to_string(oldState),
                                         to_string(m_sessionState), stateChangedEvent.session, stateChangedEvent.time));

        if ((stateChangedEvent.session != XR_NULL_HANDLE) && (stateChangedEvent.session != m_session)) {
            Log::Write(Log::Level::Error, "XrEventDataSessionStateChanged for unknown session");
            return;
        }

        switch (m_sessionState) {
            case XR_SESSION_STATE_READY: {
                CHECK(m_session != XR_NULL_HANDLE);
                XrSessionBeginInfo sessionBeginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                sessionBeginInfo.primaryViewConfigurationType = m_viewConfigType;
                CHECK_XRCMD(xrBeginSession(m_session, &sessionBeginInfo));
                m_sessionRunning = true;
                break;
            }
            case XR_SESSION_STATE_STOPPING: {
                CHECK(m_session != XR_NULL_HANDLE);
                m_sessionRunning = false;
                CHECK_XRCMD(xrEndSession(m_session))
                break;
            }
            case XR_SESSION_STATE_EXITING: {
                *exitRenderLoop = true;
                // Do not attempt to restart because user closed this session.
                *requestRestart = false;
                break;
            }
            case XR_SESSION_STATE_LOSS_PENDING: {
                *exitRenderLoop = true;
                // Poll for a new instance.
                *requestRestart = true;
                break;
            }
            default:
                break;
        }
    }

    void LogActionSourceName(XrAction action, const std::string& actionName) const {
        XrBoundSourcesForActionEnumerateInfo getInfo = {XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
        getInfo.action = action;
        uint32_t pathCount = 0;
        CHECK_XRCMD(xrEnumerateBoundSourcesForAction(m_session, &getInfo, 0, &pathCount, nullptr));
        std::vector<XrPath> paths(pathCount);
        CHECK_XRCMD(xrEnumerateBoundSourcesForAction(m_session, &getInfo, uint32_t(paths.size()), &pathCount, paths.data()));

        std::string sourceName;
        for (uint32_t i = 0; i < pathCount; ++i) {
            constexpr XrInputSourceLocalizedNameFlags all = XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT |
                                                            XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT |
                                                            XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;

            XrInputSourceLocalizedNameGetInfo nameInfo = {XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO};
            nameInfo.sourcePath = paths[i];
            nameInfo.whichComponents = all;

            uint32_t size = 0;
            CHECK_XRCMD(xrGetInputSourceLocalizedName(m_session, &nameInfo, 0, &size, nullptr));
            if (size < 1) {
                continue;
            }
            std::vector<char> grabSource(size);
            CHECK_XRCMD(xrGetInputSourceLocalizedName(m_session, &nameInfo, uint32_t(grabSource.size()), &size, grabSource.data()));
            if (!sourceName.empty()) {
                sourceName += " and ";
            }
            sourceName += "'";
            sourceName += std::string(grabSource.data(), size - 1);
            sourceName += "'";
        }

        Log::Write(Log::Level::Info,
                   Fmt("%s action is bound to %s", actionName.c_str(), ((!sourceName.empty()) ? sourceName.c_str() : "nothing")));
    }

    bool IsSessionRunning() const override { return m_sessionRunning; }

    bool IsSessionFocused() const override { return m_sessionState == XR_SESSION_STATE_FOCUSED; }

    bool PollActions() override {
        bool ret = false;
        m_input.handActive = {XR_FALSE, XR_FALSE};

        // Sync actions
        const XrActiveActionSet activeActionSet{m_input.actionSet, XR_NULL_PATH};
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeActionSet;
        CHECK_XRCMD(xrSyncActions(m_session, &syncInfo));

        cxrVRTrackingState trackingState{};

        // Get pose and grab action state and start haptic vibrate when hand is 90% squeezed.
        for (auto hand : {Side::LEFT, Side::RIGHT}) {
            XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
            getInfo.action = m_input.grabAction;
            getInfo.subactionPath = m_input.handSubactionPath[hand];

            XrActionStateFloat grabValue{XR_TYPE_ACTION_STATE_FLOAT};
            CHECK_XRCMD(xrGetActionStateFloat(m_session, &getInfo, &grabValue));
            if (grabValue.isActive == XR_TRUE) {
                // Scale the rendered hand by 1.0f (open) to 0.5f (fully squeezed).
                m_input.handScale[hand] = 1.0f - 0.5f * grabValue.currentState;
                if (grabValue.currentState > 0.9f) {
                    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
                    vibration.amplitude = 0.5;
                    vibration.duration = XR_MIN_HAPTIC_DURATION;
                    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

                    XrHapticActionInfo hapticActionInfo{XR_TYPE_HAPTIC_ACTION_INFO};
                    hapticActionInfo.action = m_input.vibrateAction;
                    hapticActionInfo.subactionPath = m_input.handSubactionPath[hand];
                    CHECK_XRCMD(xrApplyHapticFeedback(m_session, &hapticActionInfo, (XrHapticBaseHeader*)&vibration));
                }
            }

            getInfo.action = m_input.quitAction;
            XrActionStateBoolean quitValue{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &quitValue));
            if ((quitValue.isActive == XR_TRUE) && (quitValue.changedSinceLastSync == XR_TRUE) &&
                (quitValue.currentState == XR_TRUE)) {
                CHECK_XRCMD(xrRequestExitSession(m_session));
            }
            /**************************pico*******************************/
            getInfo.action = m_input.touchpadAction;
            XrActionStateBoolean touchpadValue{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &touchpadValue));

            //joystick key down/up
            if ((touchpadValue.isActive == XR_TRUE) && (touchpadValue.changedSinceLastSync == XR_TRUE)) {
                if(touchpadValue.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  touchpadValue pressed %d",hand));
                    m_input.handScale[hand] = 0.01f / 0.1f;
                } else {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  touchpadValue released %d",hand));
                    m_input.handScale[hand] = 1.0;
                }
            }

            //home button
            getInfo.action = m_input.homeAction;
            XrActionStateBoolean homeValue{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &homeValue));
            if (homeValue.isActive == XR_TRUE) {
                if(homeValue.changedSinceLastSync == XR_TRUE) {
                    if(homeValue.currentState == XR_TRUE) {
                        Log::Write(Log::Level::Error, Fmt("pico keyevent  homekey pressed %d xxx", hand));
                        trackingState.controller[hand].booleanComps |= 1UL << cxrButton_System;
                    }
                    else{
                        Log::Write(Log::Level::Error, Fmt("pico keyevent  homekey released %d xxx", hand));
                    }
                }
            }

            getInfo.action = m_input.backAction;
            XrActionStateBoolean backValue{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &backValue));
            if ((backValue.isActive == XR_TRUE) && (backValue.changedSinceLastSync == XR_TRUE)) {
                if(backValue.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  backkey pressed %d", hand));
                } else {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  backkey released %d", hand));
                    ret = true;
                }
            }

            getInfo.action = m_input.sideAction;
            XrActionStateBoolean sideValue{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &sideValue));
            if ((sideValue.isActive == XR_TRUE) && (sideValue.changedSinceLastSync == XR_TRUE)) {
                if(sideValue.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  sidekey pressed %d", hand));
                    m_input.handScale[hand] = 0.25f/0.1f;
                } else{
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  sidekey released %d", hand));
                    m_input.handScale[hand] = 1.0;
                }
            }

            //trigger value
            getInfo.action = m_input.triggerAction;
            XrActionStateFloat triggerValue{XR_TYPE_ACTION_STATE_FLOAT};
            CHECK_XRCMD(xrGetActionStateFloat(m_session, &getInfo, &triggerValue));
            if (triggerValue.isActive == XR_TRUE) {
                float trigger = triggerValue.currentState;
                trackingState.controller[hand].scalarComps[cxrAnalog_Trigger] = trigger;
            }

            //joystick x/y
            getInfo.action = m_input.joystickAction;
            XrActionStateVector2f joystickValue{XR_TYPE_ACTION_STATE_VECTOR2F};
            CHECK_XRCMD(xrGetActionStateVector2f(m_session, &getInfo, &joystickValue));
            if (joystickValue.isActive == XR_TRUE) {
                m_input.handXYPos[hand].x = joystickValue.currentState.x;
                m_input.handXYPos[hand].y = joystickValue.currentState.y;

                trackingState.controller[hand].scalarComps[cxrAnalog_JoystickX] = joystickValue.currentState.x;
                trackingState.controller[hand].scalarComps[cxrAnalog_JoystickY] = joystickValue.currentState.y;
                //Log::Write(Log::Level::Error,
                //           Fmt("pico keyevent  joystick value x = %f,y = %f",joystickValue.currentState.x, joystickValue.currentState.y));
            }

            getInfo.action = m_input.batteryAction;
            XrActionStateFloat batteryValue{XR_TYPE_ACTION_STATE_FLOAT};
            CHECK_XRCMD(xrGetActionStateFloat(m_session, &getInfo, &batteryValue));
            if (batteryValue.isActive == XR_TRUE) {
                //Log::Write(Log::Level::Error,
                //Fmt("controller %d pico keyevent  battery value %f",hand,batteryValue.currentState));
            }
            //------  add new ----------------

            getInfo.action = m_input.RockerTouchAction;
            XrActionStateBoolean RockerTouch{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &RockerTouch));
            if (RockerTouch.isActive == XR_TRUE) {
                if (RockerTouch.changedSinceLastSync == XR_TRUE && RockerTouch.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  RockerTouch click %d", hand));
                }
            }

            // trigger touch
            getInfo.action = m_input.TriggerTouchAction;
            XrActionStateBoolean TriggerTouch{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &TriggerTouch));
            if (TriggerTouch.isActive == XR_TRUE) {
                if (TriggerTouch.changedSinceLastSync == XR_TRUE && TriggerTouch.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  TriggerTouch click %d", hand));
                    trackingState.controller[hand].booleanComps |= 1UL << cxrButton_Trigger_Touch;
                }
            }

            getInfo.action = m_input.ThumbrestTouchAction;
            XrActionStateBoolean ThumbrestTouch{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &ThumbrestTouch));
            if (ThumbrestTouch.isActive == XR_TRUE) {
                if (ThumbrestTouch.changedSinceLastSync == XR_TRUE && ThumbrestTouch.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  ThumbrestTouch click %d", hand));
                }
            }

            getInfo.action = m_input.GripAction;
            XrActionStateFloat gripValue{XR_TYPE_ACTION_STATE_FLOAT};
            CHECK_XRCMD(xrGetActionStateFloat(m_session, &getInfo, &gripValue));
            if (gripValue.isActive == XR_TRUE) {
                //Log::Write(Log::Level::Error, Fmt("pico keyevent  grip value %f hand:%d", gripValue.currentState, hand));
                trackingState.controller[hand].scalarComps[cxrAnalog_Grip] = gripValue.currentState;
            }

            getInfo.action = m_input.BAction;
            XrActionStateBoolean BValue{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &BValue));
            if ((BValue.isActive == XR_TRUE) && (BValue.changedSinceLastSync == XR_TRUE)) {
                if(BValue.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  Bkey pressed %d",hand));
                    m_input.handScale[hand] = 0.15f/0.1f;
                    trackingState.controller[hand].booleanComps |= 1UL << cxrButton_B;
                } else{
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  Bkey released %d",hand));
                    m_input.handScale[hand] = 1.0;
                }
            }

            getInfo.action = m_input.YAction;
            XrActionStateBoolean YValue{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &YValue));
            if ((YValue.isActive == XR_TRUE) && (YValue.changedSinceLastSync == XR_TRUE)) {
                if(YValue.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  Ykey pressed %d",hand));
                    m_input.handScale[hand] = 0.15f/0.1f;
                    trackingState.controller[hand].booleanComps |= 1UL << cxrButton_Y;
                } else{
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  Ykey released %d",hand));
                    m_input.handScale[hand] = 1.0;
                }
            }
            getInfo.action = m_input.AAction;
            XrActionStateBoolean AValue{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &AValue));
            if ((AValue.isActive == XR_TRUE) && (AValue.changedSinceLastSync == XR_TRUE)) {
                if(AValue.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  Akey pressed %d",hand));
                    m_input.handScale[hand] = 0.05f/0.1f;
                    trackingState.controller[hand].booleanComps |= 1UL << cxrButton_A;
                } else{
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  Akey released %d",hand));
                    m_input.handScale[hand] = 1.0;
                }
            }

            // x button
            getInfo.action = m_input.XAction;
            XrActionStateBoolean XValue{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &XValue));
            if ((XValue.isActive == XR_TRUE) && (XValue.changedSinceLastSync == XR_TRUE)) {
                if(XValue.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  Xkey pressed %d",hand));
                    m_input.handScale[hand] = 0.05f/0.1f;
                    trackingState.controller[hand].booleanComps |= 1UL << cxrButton_X;
                } else{
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  Xkey released %d",hand));
                    m_input.handScale[hand] = 1.0;
                }
            }
            getInfo.action = m_input.ATouchAction;
            XrActionStateBoolean Atouch{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &Atouch));
            if (Atouch.isActive == XR_TRUE) {
                if (Atouch.changedSinceLastSync == XR_TRUE && Atouch.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  Atouch %d",hand));
                }
            }

            getInfo.action = m_input.XTouchAction;
            XrActionStateBoolean Xtouch{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &Xtouch));
            if (Xtouch.isActive == XR_TRUE) {
                if (Xtouch.changedSinceLastSync == XR_TRUE && Xtouch.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  Xtouch %d",hand));
                }
            }

            getInfo.action = m_input.BTouchAction;
            XrActionStateBoolean Btouch{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &Btouch));
            if (Btouch.isActive == XR_TRUE) {
                if (Btouch.changedSinceLastSync == XR_TRUE && Btouch.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  Btouch %d",hand));
                }
            }

            getInfo.action = m_input.YTouchAction;
            XrActionStateBoolean Ytouch{XR_TYPE_ACTION_STATE_BOOLEAN};
            CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &Ytouch));
            if (Ytouch.isActive == XR_TRUE) {
                if (Ytouch.changedSinceLastSync == XR_TRUE && Ytouch.currentState == XR_TRUE) {
                    Log::Write(Log::Level::Error, Fmt("pico keyevent  Ytouch %d",hand));
                }
            }
            /**************************pico*******************************/

            getInfo.action = m_input.poseAction;
            XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
            CHECK_XRCMD(xrGetActionStatePose(m_session, &getInfo, &poseState));
            m_input.handActive[hand] = poseState.isActive;

        }
        if (m_cloudxr.get()) {
            m_cloudxr->SetTrackingState(trackingState);
        }
        return ret;
    }

    void RenderFrame() override {
        CHECK(m_session != XR_NULL_HANDLE);

        XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        CHECK_XRCMD(xrWaitFrame(m_session, &frameWaitInfo, &frameState));

        XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        CHECK_XRCMD(xrBeginFrame(m_session, &frameBeginInfo));

        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::vector<XrCompositionLayerProjectionView> projectionLayerViews;
        if (frameState.shouldRender == XR_TRUE) {
            if (RenderLayer(frameState.predictedDisplayTime, projectionLayerViews, layer)) {
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));
            }
        }

        XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
        xrFrameEndInfoEXT.type = XR_TYPE_FRAME_END_INFO;
        xrFrameEndInfoEXT.useHeadposeExt = 1;
        xrFrameEndInfoEXT.gsIndex = mGsIndex;
        frameEndInfo.next = (void *)&xrFrameEndInfoEXT;
        frameEndInfo.displayTime = frameState.predictedDisplayTime;
        frameEndInfo.environmentBlendMode = m_environmentBlendMode;
        frameEndInfo.layerCount = (uint32_t)layers.size();
        frameEndInfo.layers = layers.data();
        CHECK_XRCMD(xrEndFrame(m_session, &frameEndInfo));
    }

    bool RenderLayer(XrTime predictedDisplayTime, std::vector<XrCompositionLayerProjectionView>& projectionLayerViews,
                     XrCompositionLayerProjection& layer) {
        XrResult res;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCapacityInput = (uint32_t)m_views.size();
        uint32_t viewCountOutput;

        XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        viewLocateInfo.viewConfigurationType = m_viewConfigType;
        viewLocateInfo.displayTime = predictedDisplayTime;
        viewLocateInfo.space = m_appSpace;
        XrViewStatePICOEXT  xrViewStatePICOEXT;
        viewState.next=(void *)&xrViewStatePICOEXT;
        res = xrLocateViews(m_session, &viewLocateInfo, &viewState, viewCapacityInput, &viewCountOutput, m_views.data());
        mGsIndex = xrViewStatePICOEXT.gsIndex;
        CHECK_XRRESULT(res, "xrLocateViews");
        if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
            //keep submitting layers to support boundary logic
            //return false;  // There is no valid tracking poses for the views.
        }

        CHECK(viewCountOutput == viewCapacityInput);
        CHECK(viewCountOutput == m_configViews.size());
        CHECK(viewCountOutput == m_swapchains.size());

        projectionLayerViews.resize(viewCountOutput);

        std::vector<XrPosef> handPose;
        for (auto hand : {Side::LEFT, Side::RIGHT}) {
            XrSpaceLocation spaceLocation{XR_TYPE_SPACE_LOCATION};
            res = xrLocateSpace(m_input.aimSpace[hand], m_appSpace, predictedDisplayTime, &spaceLocation);
            CHECK_XRRESULT(res, "xrLocateSpace");
            if (XR_UNQUALIFIED_SUCCESS(res)) {
                if ((spaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
                    (spaceLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0)
                {
                    handPose.push_back(spaceLocation.pose);
                }
            } else {
                // Tracking loss is expected when the hand is not active so only log a message
                // if the hand is active.
                if (m_input.handActive[hand] == XR_TRUE) {
                    const char* handName[] = {"left", "right"};
                    Log::Write(Log::Level::Info, Fmt("Unable to locate %s hand action space in app space: %d", handName[hand], res));
                }
            }
        }

        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation spaceLocation{XR_TYPE_SPACE_LOCATION, &velocity};
        res = xrLocateSpace(m_ViewSpace, m_appSpace, predictedDisplayTime, &spaceLocation);
        CHECK_XRRESULT(res, "xrLocateSpace");

        m_cloudxr->SetSenserPoseState(spaceLocation.pose, velocity.linearVelocity, velocity.angularVelocity, handPose);

        cxrFramesLatched framesLatched{};
        bool framevaild = m_cloudxr->LatchFrame(&framesLatched);

        XrPosef pose[Side::COUNT];
        for (uint32_t i = 0; i < viewCountOutput; i++) {
            pose[i] = m_views[i].pose;
        }
        if (framevaild) {
            XrQuaternionf orientation = m_cloudxr->cxrToQuaternion(framesLatched.poseMatrix);
            XrVector3f position =  m_cloudxr->cxrGetTranslation(framesLatched.poseMatrix);
            //Log::Write(Log::Level::Error, Fmt("-get poseId %llu, position: %f,%f,%f orientation: %f,%f,%f,%f", framesLatched.poseID,
            //    position.x, position.y, position.z, orientation.x, orientation.y, orientation.z, orientation.w));
           
            // todo get views pose
            for (uint32_t i = 0; i < viewCountOutput; i++) {
                pose[i].position = position;
                pose[i].orientation = orientation;
            }
        } else {
            Log::Write(Log::Level::Info, Fmt("not get framesLatched"));
        }

        // Render view to the appropriate part of the swapchain image.
        for (uint32_t i = 0; i < viewCountOutput; i++) {
            // Each view has a separate swapchain which is acquired, rendered to, and released.
            const Swapchain viewSwapchain = m_swapchains[i];

            XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

            uint32_t swapchainImageIndex;
            CHECK_XRCMD(xrAcquireSwapchainImage(viewSwapchain.handle, &acquireInfo, &swapchainImageIndex));

            XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = XR_INFINITE_DURATION;
            CHECK_XRCMD(xrWaitSwapchainImage(viewSwapchain.handle, &waitInfo));

            projectionLayerViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            projectionLayerViews[i].pose = pose[i];
            projectionLayerViews[i].fov = m_views[i].fov;
            projectionLayerViews[i].subImage.swapchain = viewSwapchain.handle;
            projectionLayerViews[i].subImage.imageRect.offset = {0, 0};
            projectionLayerViews[i].subImage.imageRect.extent = {viewSwapchain.width, viewSwapchain.height};

            const XrSwapchainImageBaseHeader* const swapchainImage = m_swapchainImages[viewSwapchain.handle][swapchainImageIndex];
            m_graphicsPlugin->RenderView(projectionLayerViews[i], swapchainImage, m_colorSwapchainFormat, framesLatched.frames[i].texture);

            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            CHECK_XRCMD(xrReleaseSwapchainImage(viewSwapchain.handle, &releaseInfo));
        }

        if (framevaild) {
            m_cloudxr->ReleaseFrame(&framesLatched);
        }

        layer.space = m_appSpace;
        layer.viewCount = (uint32_t)projectionLayerViews.size();
        layer.views = projectionLayerViews.data();
        return true;
    }

    bool CreateCloudxrClient() override {
        m_cloudxr = std::make_shared<CloudXRClient>();
        return true;
    }

    void SetCloudxrClientPaused(bool pause) override {
        if (m_cloudxr.get()) {
            m_cloudxr->SetPaused(pause);
        }
    }

    void StartCloudxrClient() override {
        if (m_cloudxr.get()) {
            m_cloudxr->Initialize(m_instance, m_systemId, m_session);
        }
    }

   private:
    const std::shared_ptr<Options> m_options;
    std::shared_ptr<IPlatformPlugin> m_platformPlugin;
    std::shared_ptr<IGraphicsPlugin> m_graphicsPlugin;
    XrInstance m_instance{XR_NULL_HANDLE};
    XrSession m_session{XR_NULL_HANDLE};
    XrSpace m_appSpace{XR_NULL_HANDLE};
    XrFormFactor m_formFactor{XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrViewConfigurationType m_viewConfigType{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    XrEnvironmentBlendMode m_environmentBlendMode{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    XrSystemId m_systemId{XR_NULL_SYSTEM_ID};

    std::vector<XrViewConfigurationView> m_configViews;
    std::vector<Swapchain> m_swapchains;
    std::map<XrSwapchain, std::vector<XrSwapchainImageBaseHeader*>> m_swapchainImages;
    std::vector<XrView> m_views;
    int64_t m_colorSwapchainFormat{-1};

    std::vector<XrSpace> m_visualizedSpaces;

    // Application's current lifecycle state according to the runtime
    XrSessionState m_sessionState{XR_SESSION_STATE_UNKNOWN};
    bool m_sessionRunning{false};

    XrEventDataBuffer m_eventDataBuffer;
    InputState m_input;

    std::shared_ptr<CloudXRClient> m_cloudxr;
    XrSpace m_ViewSpace{XR_NULL_HANDLE};

};
}  // namespace

std::shared_ptr<IOpenXrProgram> CreateOpenXrProgram(const std::shared_ptr<Options>& options,
                                                    const std::shared_ptr<IPlatformPlugin>& platformPlugin,
                                                    const std::shared_ptr<IGraphicsPlugin>& graphicsPlugin) {
    return std::make_shared<OpenXrProgram>(options, platformPlugin, graphicsPlugin);
}