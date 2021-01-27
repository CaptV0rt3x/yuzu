// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/core_timing_util.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/input.h"
#include "core/hardware_properties.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/readable_event.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/kernel/writable_event.h"
#include "core/hle/service/hid/errors.h"
#include "core/hle/service/hid/hid.h"
#include "core/hle/service/hid/irs.h"
#include "core/hle/service/hid/xcd.h"
#include "core/hle/service/service.h"
#include "core/settings.h"

#include "core/hle/service/hid/controllers/controller_base.h"
#include "core/hle/service/hid/controllers/debug_pad.h"
#include "core/hle/service/hid/controllers/gesture.h"
#include "core/hle/service/hid/controllers/keyboard.h"
#include "core/hle/service/hid/controllers/mouse.h"
#include "core/hle/service/hid/controllers/npad.h"
#include "core/hle/service/hid/controllers/stubbed.h"
#include "core/hle/service/hid/controllers/touchscreen.h"
#include "core/hle/service/hid/controllers/xpad.h"

namespace Service::HID {

// Updating period for each HID device.
// HID is polled every 15ms, this value was derived from
// https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering#joy-con-status-data-packet
constexpr auto pad_update_ns = std::chrono::nanoseconds{1000 * 1000};         // (1ms, 1000Hz)
constexpr auto motion_update_ns = std::chrono::nanoseconds{15 * 1000 * 1000}; // (15ms, 66.666Hz)
constexpr std::size_t SHARED_MEMORY_SIZE = 0x40000;

IAppletResource::IAppletResource(Core::System& system_)
    : ServiceFramework{system_, "IAppletResource"} {
    static const FunctionInfo functions[] = {
        {0, &IAppletResource::GetSharedMemoryHandle, "GetSharedMemoryHandle"},
    };
    RegisterHandlers(functions);

    auto& kernel = system.Kernel();
    shared_mem = SharedFrom(&kernel.GetHidSharedMem());

    MakeController<Controller_DebugPad>(HidController::DebugPad);
    MakeController<Controller_Touchscreen>(HidController::Touchscreen);
    MakeController<Controller_Mouse>(HidController::Mouse);
    MakeController<Controller_Keyboard>(HidController::Keyboard);
    MakeController<Controller_XPad>(HidController::XPad);
    MakeController<Controller_Stubbed>(HidController::Unknown1);
    MakeController<Controller_Stubbed>(HidController::Unknown2);
    MakeController<Controller_Stubbed>(HidController::Unknown3);
    MakeController<Controller_Stubbed>(HidController::SixAxisSensor);
    MakeController<Controller_NPad>(HidController::NPad);
    MakeController<Controller_Gesture>(HidController::Gesture);

    // Homebrew doesn't try to activate some controllers, so we activate them by default
    GetController<Controller_NPad>(HidController::NPad).ActivateController();
    GetController<Controller_Touchscreen>(HidController::Touchscreen).ActivateController();

    GetController<Controller_Stubbed>(HidController::Unknown1).SetCommonHeaderOffset(0x4c00);
    GetController<Controller_Stubbed>(HidController::Unknown2).SetCommonHeaderOffset(0x4e00);
    GetController<Controller_Stubbed>(HidController::Unknown3).SetCommonHeaderOffset(0x5000);

    // Register update callbacks
    pad_update_event = Core::Timing::CreateEvent(
        "HID::UpdatePadCallback",
        [this](std::uintptr_t user_data, std::chrono::nanoseconds ns_late) {
            const auto guard = LockService();
            UpdateControllers(user_data, ns_late);
        });
    motion_update_event = Core::Timing::CreateEvent(
        "HID::MotionPadCallback",
        [this](std::uintptr_t user_data, std::chrono::nanoseconds ns_late) {
            const auto guard = LockService();
            UpdateMotion(user_data, ns_late);
        });

    system.CoreTiming().ScheduleEvent(pad_update_ns, pad_update_event);
    system.CoreTiming().ScheduleEvent(motion_update_ns, motion_update_event);

    ReloadInputDevices();
}

void IAppletResource::ActivateController(HidController controller) {
    controllers[static_cast<size_t>(controller)]->ActivateController();
}

void IAppletResource::DeactivateController(HidController controller) {
    controllers[static_cast<size_t>(controller)]->DeactivateController();
}

IAppletResource ::~IAppletResource() {
    system.CoreTiming().UnscheduleEvent(pad_update_event, 0);
}

void IAppletResource::GetSharedMemoryHandle(Kernel::HLERequestContext& ctx) {
    LOG_DEBUG(Service_HID, "called");

    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(RESULT_SUCCESS);
    rb.PushCopyObjects(shared_mem);
}

void IAppletResource::UpdateControllers(std::uintptr_t user_data,
                                        std::chrono::nanoseconds ns_late) {
    auto& core_timing = system.CoreTiming();

    const bool should_reload = Settings::values.is_device_reload_pending.exchange(false);
    for (const auto& controller : controllers) {
        if (should_reload) {
            controller->OnLoadInputDevices();
        }
        controller->OnUpdate(core_timing, shared_mem->GetPointer(), SHARED_MEMORY_SIZE);
    }

    core_timing.ScheduleEvent(pad_update_ns - ns_late, pad_update_event);
}

void IAppletResource::UpdateMotion(std::uintptr_t user_data, std::chrono::nanoseconds ns_late) {
    auto& core_timing = system.CoreTiming();

    for (const auto& controller : controllers) {
        controller->OnMotionUpdate(core_timing, shared_mem->GetPointer(), SHARED_MEMORY_SIZE);
    }

    core_timing.ScheduleEvent(motion_update_ns - ns_late, motion_update_event);
}

class IActiveVibrationDeviceList final : public ServiceFramework<IActiveVibrationDeviceList> {
public:
    explicit IActiveVibrationDeviceList(Core::System& system_,
                                        std::shared_ptr<IAppletResource> applet_resource_)
        : ServiceFramework{system_, "IActiveVibrationDeviceList"},
          applet_resource(applet_resource_) {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IActiveVibrationDeviceList::InitializeVibrationDevice, "InitializeVibrationDevice"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }

private:
    void InitializeVibrationDevice(Kernel::HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto vibration_device_handle{rp.PopRaw<Controller_NPad::DeviceHandle>()};

        if (applet_resource != nullptr) {
            applet_resource->GetController<Controller_NPad>(HidController::NPad)
                .InitializeVibrationDevice(vibration_device_handle);
        }

        LOG_DEBUG(Service_HID, "called, npad_type={}, npad_id={}, device_index={}",
                  vibration_device_handle.npad_type, vibration_device_handle.npad_id,
                  vibration_device_handle.device_index);

        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(RESULT_SUCCESS);
    }

    std::shared_ptr<IAppletResource> applet_resource;
};

std::shared_ptr<IAppletResource> Hid::GetAppletResource() {
    if (applet_resource == nullptr) {
        applet_resource = std::make_shared<IAppletResource>(system);
    }

    return applet_resource;
}

Hid::Hid(Core::System& system_) : ServiceFramework{system_, "hid"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &Hid::CreateAppletResource, "CreateAppletResource"},
        {1, &Hid::ActivateDebugPad, "ActivateDebugPad"},
        {11, &Hid::ActivateTouchScreen, "ActivateTouchScreen"},
        {21, &Hid::ActivateMouse, "ActivateMouse"},
        {31, &Hid::ActivateKeyboard, "ActivateKeyboard"},
        {32, &Hid::SendKeyboardLockKeyEvent, "SendKeyboardLockKeyEvent"},
        {40, nullptr, "AcquireXpadIdEventHandle"},
        {41, nullptr, "ReleaseXpadIdEventHandle"},
        {51, &Hid::ActivateXpad, "ActivateXpad"},
        {55, &Hid::GetXpadIDs, "GetXpadIds"},
        {56, nullptr, "ActivateJoyXpad"},
        {58, nullptr, "GetJoyXpadLifoHandle"},
        {59, nullptr, "GetJoyXpadIds"},
        {60, &Hid::ActivateSixAxisSensor, "ActivateSixAxisSensor"},
        {61, &Hid::DeactivateSixAxisSensor, "DeactivateSixAxisSensor"},
        {62, nullptr, "GetSixAxisSensorLifoHandle"},
        {63, nullptr, "ActivateJoySixAxisSensor"},
        {64, nullptr, "DeactivateJoySixAxisSensor"},
        {65, nullptr, "GetJoySixAxisSensorLifoHandle"},
        {66, &Hid::StartSixAxisSensor, "StartSixAxisSensor"},
        {67, &Hid::StopSixAxisSensor, "StopSixAxisSensor"},
        {68, nullptr, "IsSixAxisSensorFusionEnabled"},
        {69, &Hid::EnableSixAxisSensorFusion, "EnableSixAxisSensorFusion"},
        {70, &Hid::SetSixAxisSensorFusionParameters, "SetSixAxisSensorFusionParameters"},
        {71, &Hid::GetSixAxisSensorFusionParameters, "GetSixAxisSensorFusionParameters"},
        {72, &Hid::ResetSixAxisSensorFusionParameters, "ResetSixAxisSensorFusionParameters"},
        {73, nullptr, "SetAccelerometerParameters"},
        {74, nullptr, "GetAccelerometerParameters"},
        {75, nullptr, "ResetAccelerometerParameters"},
        {76, nullptr, "SetAccelerometerPlayMode"},
        {77, nullptr, "GetAccelerometerPlayMode"},
        {78, nullptr, "ResetAccelerometerPlayMode"},
        {79, &Hid::SetGyroscopeZeroDriftMode, "SetGyroscopeZeroDriftMode"},
        {80, &Hid::GetGyroscopeZeroDriftMode, "GetGyroscopeZeroDriftMode"},
        {81, &Hid::ResetGyroscopeZeroDriftMode, "ResetGyroscopeZeroDriftMode"},
        {82, &Hid::IsSixAxisSensorAtRest, "IsSixAxisSensorAtRest"},
        {83, nullptr, "IsFirmwareUpdateAvailableForSixAxisSensor"},
        {91, &Hid::ActivateGesture, "ActivateGesture"},
        {100, &Hid::SetSupportedNpadStyleSet, "SetSupportedNpadStyleSet"},
        {101, &Hid::GetSupportedNpadStyleSet, "GetSupportedNpadStyleSet"},
        {102, &Hid::SetSupportedNpadIdType, "SetSupportedNpadIdType"},
        {103, &Hid::ActivateNpad, "ActivateNpad"},
        {104, &Hid::DeactivateNpad, "DeactivateNpad"},
        {106, &Hid::AcquireNpadStyleSetUpdateEventHandle, "AcquireNpadStyleSetUpdateEventHandle"},
        {107, &Hid::DisconnectNpad, "DisconnectNpad"},
        {108, &Hid::GetPlayerLedPattern, "GetPlayerLedPattern"},
        {109, &Hid::ActivateNpadWithRevision, "ActivateNpadWithRevision"},
        {120, &Hid::SetNpadJoyHoldType, "SetNpadJoyHoldType"},
        {121, &Hid::GetNpadJoyHoldType, "GetNpadJoyHoldType"},
        {122, &Hid::SetNpadJoyAssignmentModeSingleByDefault, "SetNpadJoyAssignmentModeSingleByDefault"},
        {123, &Hid::SetNpadJoyAssignmentModeSingle, "SetNpadJoyAssignmentModeSingle"},
        {124, &Hid::SetNpadJoyAssignmentModeDual, "SetNpadJoyAssignmentModeDual"},
        {125, &Hid::MergeSingleJoyAsDualJoy, "MergeSingleJoyAsDualJoy"},
        {126, &Hid::StartLrAssignmentMode, "StartLrAssignmentMode"},
        {127, &Hid::StopLrAssignmentMode, "StopLrAssignmentMode"},
        {128, &Hid::SetNpadHandheldActivationMode, "SetNpadHandheldActivationMode"},
        {129, &Hid::GetNpadHandheldActivationMode, "GetNpadHandheldActivationMode"},
        {130, &Hid::SwapNpadAssignment, "SwapNpadAssignment"},
        {131, &Hid::IsUnintendedHomeButtonInputProtectionEnabled, "IsUnintendedHomeButtonInputProtectionEnabled"},
        {132, &Hid::EnableUnintendedHomeButtonInputProtection, "EnableUnintendedHomeButtonInputProtection"},
        {133, nullptr, "SetNpadJoyAssignmentModeSingleWithDestination"},
        {134, nullptr, "SetNpadAnalogStickUseCenterClamp"},
        {135, nullptr, "SetNpadCaptureButtonAssignment"},
        {136, nullptr, "ClearNpadCaptureButtonAssignment"},
        {200, &Hid::GetVibrationDeviceInfo, "GetVibrationDeviceInfo"},
        {201, &Hid::SendVibrationValue, "SendVibrationValue"},
        {202, &Hid::GetActualVibrationValue, "GetActualVibrationValue"},
        {203, &Hid::CreateActiveVibrationDeviceList, "CreateActiveVibrationDeviceList"},
        {204, &Hid::PermitVibration, "PermitVibration"},
        {205, &Hid::IsVibrationPermitted, "IsVibrationPermitted"},
        {206, &Hid::SendVibrationValues, "SendVibrationValues"},
        {207, nullptr, "SendVibrationGcErmCommand"},
        {208, nullptr, "GetActualVibrationGcErmCommand"},
        {209, &Hid::BeginPermitVibrationSession, "BeginPermitVibrationSession"},
        {210, &Hid::EndPermitVibrationSession, "EndPermitVibrationSession"},
        {211, &Hid::IsVibrationDeviceMounted, "IsVibrationDeviceMounted"},
        {300, &Hid::ActivateConsoleSixAxisSensor, "ActivateConsoleSixAxisSensor"},
        {301, &Hid::StartConsoleSixAxisSensor, "StartConsoleSixAxisSensor"},
        {302, &Hid::StopConsoleSixAxisSensor, "StopConsoleSixAxisSensor"},
        {303, &Hid::ActivateSevenSixAxisSensor, "ActivateSevenSixAxisSensor"},
        {304, &Hid::StartSevenSixAxisSensor, "StartSevenSixAxisSensor"},
        {305, &Hid::StopSevenSixAxisSensor, "StopSevenSixAxisSensor"},
        {306, &Hid::InitializeSevenSixAxisSensor, "InitializeSevenSixAxisSensor"},
        {307, &Hid::FinalizeSevenSixAxisSensor, "FinalizeSevenSixAxisSensor"},
        {308, nullptr, "SetSevenSixAxisSensorFusionStrength"},
        {309, nullptr, "GetSevenSixAxisSensorFusionStrength"},
        {310, &Hid::ResetSevenSixAxisSensorTimestamp, "ResetSevenSixAxisSensorTimestamp"},
        {400, nullptr, "IsUsbFullKeyControllerEnabled"},
        {401, nullptr, "EnableUsbFullKeyController"},
        {402, nullptr, "IsUsbFullKeyControllerConnected"},
        {403, nullptr, "HasBattery"},
        {404, nullptr, "HasLeftRightBattery"},
        {405, nullptr, "GetNpadInterfaceType"},
        {406, nullptr, "GetNpadLeftRightInterfaceType"},
        {407, nullptr, "GetNpadOfHighestBatteryLevel"},
        {408, nullptr, "GetNpadOfHighestBatteryLevelForJoyRight"},
        {500, nullptr, "GetPalmaConnectionHandle"},
        {501, nullptr, "InitializePalma"},
        {502, nullptr, "AcquirePalmaOperationCompleteEvent"},
        {503, nullptr, "GetPalmaOperationInfo"},
        {504, nullptr, "PlayPalmaActivity"},
        {505, nullptr, "SetPalmaFrModeType"},
        {506, nullptr, "ReadPalmaStep"},
        {507, nullptr, "EnablePalmaStep"},
        {508, nullptr, "ResetPalmaStep"},
        {509, nullptr, "ReadPalmaApplicationSection"},
        {510, nullptr, "WritePalmaApplicationSection"},
        {511, nullptr, "ReadPalmaUniqueCode"},
        {512, nullptr, "SetPalmaUniqueCodeInvalid"},
        {513, nullptr, "WritePalmaActivityEntry"},
        {514, nullptr, "WritePalmaRgbLedPatternEntry"},
        {515, nullptr, "WritePalmaWaveEntry"},
        {516, nullptr, "SetPalmaDataBaseIdentificationVersion"},
        {517, nullptr, "GetPalmaDataBaseIdentificationVersion"},
        {518, nullptr, "SuspendPalmaFeature"},
        {519, nullptr, "GetPalmaOperationResult"},
        {520, nullptr, "ReadPalmaPlayLog"},
        {521, nullptr, "ResetPalmaPlayLog"},
        {522, &Hid::SetIsPalmaAllConnectable, "SetIsPalmaAllConnectable"},
        {523, nullptr, "SetIsPalmaPairedConnectable"},
        {524, nullptr, "PairPalma"},
        {525, &Hid::SetPalmaBoostMode, "SetPalmaBoostMode"},
        {526, nullptr, "CancelWritePalmaWaveEntry"},
        {527, nullptr, "EnablePalmaBoostMode"},
        {528, nullptr, "GetPalmaBluetoothAddress"},
        {529, nullptr, "SetDisallowedPalmaConnection"},
        {1000, &Hid::SetNpadCommunicationMode, "SetNpadCommunicationMode"},
        {1001, &Hid::GetNpadCommunicationMode, "GetNpadCommunicationMode"},
        {1002, nullptr, "SetTouchScreenConfiguration"},
        {1003, nullptr, "IsFirmwareUpdateNeededForNotification"},
        {2000, nullptr, "ActivateDigitizer"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

Hid::~Hid() = default;

void Hid::CreateAppletResource(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    if (applet_resource == nullptr) {
        applet_resource = std::make_shared<IAppletResource>(system);
    }

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(RESULT_SUCCESS);
    rb.PushIpcInterface<IAppletResource>(applet_resource);
}

void Hid::ActivateDebugPad(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    applet_resource->ActivateController(HidController::DebugPad);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::ActivateTouchScreen(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    applet_resource->ActivateController(HidController::Touchscreen);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::ActivateMouse(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    applet_resource->ActivateController(HidController::Mouse);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::ActivateKeyboard(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    applet_resource->ActivateController(HidController::Keyboard);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::SendKeyboardLockKeyEvent(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto flags{rp.Pop<u32>()};

    LOG_WARNING(Service_HID, "(STUBBED) called. flags={}", flags);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::ActivateXpad(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 basic_xpad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->ActivateController(HidController::XPad);

    LOG_DEBUG(Service_HID, "called, basic_xpad_id={}, applet_resource_user_id={}",
              parameters.basic_xpad_id, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::GetXpadIDs(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_DEBUG(Service_HID, "(STUBBED) called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(RESULT_SUCCESS);
    rb.Push(0);
}

void Hid::ActivateSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad).SetSixAxisEnabled(true);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::DeactivateSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad).SetSixAxisEnabled(false);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::StartSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad).SetSixAxisEnabled(true);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::StopSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad).SetSixAxisEnabled(false);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::EnableSixAxisSensorFusion(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        bool enable_sixaxis_sensor_fusion;
        INSERT_PADDING_BYTES_NOINIT(3);
        Controller_NPad::DeviceHandle sixaxis_handle;
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_WARNING(Service_HID,
                "(STUBBED) called, enable_sixaxis_sensor_fusion={}, npad_type={}, npad_id={}, "
                "device_index={}, applet_resource_user_id={}",
                parameters.enable_sixaxis_sensor_fusion, parameters.sixaxis_handle.npad_type,
                parameters.sixaxis_handle.npad_id, parameters.sixaxis_handle.device_index,
                parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::SetSixAxisSensorFusionParameters(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        f32 parameter1;
        f32 parameter2;
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetSixAxisFusionParameters(parameters.parameter1, parameters.parameter2);

    LOG_WARNING(Service_HID,
                "(STUBBED) called, float1={}, float2={}, npad_type={}, npad_id={}, "
                "device_index={}, applet_resource_user_id={}",
                parameters.parameter1, parameters.parameter2, parameters.sixaxis_handle.npad_type,
                parameters.sixaxis_handle.npad_id, parameters.sixaxis_handle.device_index,
                parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::GetSixAxisSensorFusionParameters(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        u64 applet_resource_user_id;
    };

    f32 parameter1 = 0;
    f32 parameter2 = 0;
    const auto parameters{rp.PopRaw<Parameters>()};

    std::tie(parameter1, parameter2) =
        applet_resource->GetController<Controller_NPad>(HidController::NPad)
            .GetSixAxisFusionParameters();

    LOG_WARNING(Service_HID,
                "(STUBBED) called, npad_type={}, npad_id={}, "
                "device_index={}, applet_resource_user_id={}",
                parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
                parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(RESULT_SUCCESS);
    rb.Push(parameter1);
    rb.Push(parameter2);
}

void Hid::ResetSixAxisSensorFusionParameters(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .ResetSixAxisFusionParameters();

    LOG_WARNING(Service_HID,
                "(STUBBED) called, npad_type={}, npad_id={}, "
                "device_index={}, applet_resource_user_id={}",
                parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
                parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::SetGyroscopeZeroDriftMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto sixaxis_handle{rp.PopRaw<Controller_NPad::DeviceHandle>()};
    const auto drift_mode{rp.PopEnum<Controller_NPad::GyroscopeZeroDriftMode>()};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetGyroscopeZeroDriftMode(drift_mode);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, drift_mode={}, "
              "applet_resource_user_id={}",
              sixaxis_handle.npad_type, sixaxis_handle.npad_id, sixaxis_handle.device_index,
              drift_mode, applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::GetGyroscopeZeroDriftMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(RESULT_SUCCESS);
    rb.PushEnum(applet_resource->GetController<Controller_NPad>(HidController::NPad)
                    .GetGyroscopeZeroDriftMode());
}

void Hid::ResetGyroscopeZeroDriftMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetGyroscopeZeroDriftMode(Controller_NPad::GyroscopeZeroDriftMode::Standard);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::IsSixAxisSensorAtRest(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
              parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(RESULT_SUCCESS);
    rb.Push(applet_resource->GetController<Controller_NPad>(HidController::NPad)
                .IsSixAxisSensorAtRest());
}

void Hid::ActivateGesture(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 unknown;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->ActivateController(HidController::Gesture);

    LOG_DEBUG(Service_HID, "called, unknown={}, applet_resource_user_id={}", parameters.unknown,
              parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::SetSupportedNpadStyleSet(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto supported_styleset{rp.Pop<u32>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetSupportedStyleSet({supported_styleset});

    LOG_DEBUG(Service_HID, "called, supported_styleset={}", supported_styleset);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::GetSupportedNpadStyleSet(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(RESULT_SUCCESS);
    rb.Push(applet_resource->GetController<Controller_NPad>(HidController::NPad)
                .GetSupportedStyleSet()
                .raw);
}

void Hid::SetSupportedNpadIdType(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetSupportedNpadIdTypes(ctx.ReadBuffer().data(), ctx.GetReadBufferSize());

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::ActivateNpad(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    applet_resource->ActivateController(HidController::NPad);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::DeactivateNpad(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    applet_resource->DeactivateController(HidController::NPad);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::AcquireNpadStyleSetUpdateEventHandle(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 npad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
        u64 unknown;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_DEBUG(Service_HID, "called, npad_id={}, applet_resource_user_id={}, unknown={}",
              parameters.npad_id, parameters.applet_resource_user_id, parameters.unknown);

    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(RESULT_SUCCESS);
    rb.PushCopyObjects(applet_resource->GetController<Controller_NPad>(HidController::NPad)
                           .GetStyleSetChangedEvent(parameters.npad_id));
}

void Hid::DisconnectNpad(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 npad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .DisconnectNpad(parameters.npad_id);

    LOG_DEBUG(Service_HID, "called, npad_id={}, applet_resource_user_id={}", parameters.npad_id,
              parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::GetPlayerLedPattern(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto npad_id{rp.Pop<u32>()};

    LOG_DEBUG(Service_HID, "called, npad_id={}", npad_id);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(RESULT_SUCCESS);
    rb.Push(applet_resource->GetController<Controller_NPad>(HidController::NPad)
                .GetLedPattern(npad_id)
                .raw);
}

void Hid::ActivateNpadWithRevision(Kernel::HLERequestContext& ctx) {
    // Should have no effect with how our npad sets up the data
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 unknown;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->ActivateController(HidController::NPad);

    LOG_DEBUG(Service_HID, "called, unknown={}, applet_resource_user_id={}", parameters.unknown,
              parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::SetNpadJoyHoldType(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};
    const auto hold_type{rp.PopEnum<Controller_NPad::NpadHoldType>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad).SetHoldType(hold_type);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}, hold_type={}",
              applet_resource_user_id, hold_type);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::GetNpadJoyHoldType(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(RESULT_SUCCESS);
    rb.PushEnum(applet_resource->GetController<Controller_NPad>(HidController::NPad).GetHoldType());
}

void Hid::SetNpadJoyAssignmentModeSingleByDefault(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 npad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetNpadMode(parameters.npad_id, Controller_NPad::NpadAssignments::Single);

    LOG_WARNING(Service_HID, "(STUBBED) called, npad_id={}, applet_resource_user_id={}",
                parameters.npad_id, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::SetNpadJoyAssignmentModeSingle(Kernel::HLERequestContext& ctx) {
    // TODO: Check the differences between this and SetNpadJoyAssignmentModeSingleByDefault
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 npad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
        u64 npad_joy_device_type;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetNpadMode(parameters.npad_id, Controller_NPad::NpadAssignments::Single);

    LOG_WARNING(Service_HID,
                "(STUBBED) called, npad_id={}, applet_resource_user_id={}, npad_joy_device_type={}",
                parameters.npad_id, parameters.applet_resource_user_id,
                parameters.npad_joy_device_type);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::SetNpadJoyAssignmentModeDual(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 npad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetNpadMode(parameters.npad_id, Controller_NPad::NpadAssignments::Dual);

    LOG_WARNING(Service_HID, "(STUBBED) called, npad_id={}, applet_resource_user_id={}",
                parameters.npad_id, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::MergeSingleJoyAsDualJoy(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto npad_id_1{rp.Pop<u32>()};
    const auto npad_id_2{rp.Pop<u32>()};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .MergeSingleJoyAsDualJoy(npad_id_1, npad_id_2);

    LOG_DEBUG(Service_HID, "called, npad_id_1={}, npad_id_2={}, applet_resource_user_id={}",
              npad_id_1, npad_id_2, applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::StartLrAssignmentMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad).StartLRAssignmentMode();

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::StopLrAssignmentMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad).StopLRAssignmentMode();

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::SetNpadHandheldActivationMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};
    const auto activation_mode{rp.PopEnum<Controller_NPad::NpadHandheldActivationMode>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetNpadHandheldActivationMode(activation_mode);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}, activation_mode={}",
              applet_resource_user_id, activation_mode);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::GetNpadHandheldActivationMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(RESULT_SUCCESS);
    rb.PushEnum(applet_resource->GetController<Controller_NPad>(HidController::NPad)
                    .GetNpadHandheldActivationMode());
}

void Hid::SwapNpadAssignment(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto npad_id_1{rp.Pop<u32>()};
    const auto npad_id_2{rp.Pop<u32>()};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    const bool res = applet_resource->GetController<Controller_NPad>(HidController::NPad)
                         .SwapNpadAssignment(npad_id_1, npad_id_2);

    LOG_DEBUG(Service_HID, "called, npad_id_1={}, npad_id_2={}, applet_resource_user_id={}",
              npad_id_1, npad_id_2, applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    if (res) {
        rb.Push(RESULT_SUCCESS);
    } else {
        LOG_ERROR(Service_HID, "Npads are not connected!");
        rb.Push(ERR_NPAD_NOT_CONNECTED);
    }
}

void Hid::IsUnintendedHomeButtonInputProtectionEnabled(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        u32 npad_id;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, npad_id={}, applet_resource_user_id={}",
                parameters.npad_id, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(RESULT_SUCCESS);
    rb.Push(applet_resource->GetController<Controller_NPad>(HidController::NPad)
                .IsUnintendedHomeButtonInputProtectionEnabled(parameters.npad_id));
}

void Hid::EnableUnintendedHomeButtonInputProtection(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        bool unintended_home_button_input_protection;
        INSERT_PADDING_BYTES_NOINIT(3);
        u32 npad_id;
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetUnintendedHomeButtonInputProtectionEnabled(
            parameters.unintended_home_button_input_protection, parameters.npad_id);

    LOG_WARNING(Service_HID,
                "(STUBBED) called, unintended_home_button_input_protection={}, npad_id={},"
                "applet_resource_user_id={}",
                parameters.unintended_home_button_input_protection, parameters.npad_id,
                parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::GetVibrationDeviceInfo(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto vibration_device_handle{rp.PopRaw<Controller_NPad::DeviceHandle>()};

    VibrationDeviceInfo vibration_device_info;

    vibration_device_info.type = VibrationDeviceType::LinearResonantActuator;

    switch (vibration_device_handle.device_index) {
    case Controller_NPad::DeviceIndex::Left:
        vibration_device_info.position = VibrationDevicePosition::Left;
        break;
    case Controller_NPad::DeviceIndex::Right:
        vibration_device_info.position = VibrationDevicePosition::Right;
        break;
    case Controller_NPad::DeviceIndex::None:
    default:
        UNREACHABLE_MSG("DeviceIndex should never be None!");
        vibration_device_info.position = VibrationDevicePosition::None;
        break;
    }

    LOG_DEBUG(Service_HID, "called, vibration_device_type={}, vibration_device_position={}",
              vibration_device_info.type, vibration_device_info.position);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(RESULT_SUCCESS);
    rb.PushRaw(vibration_device_info);
}

void Hid::SendVibrationValue(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle vibration_device_handle;
        Controller_NPad::VibrationValue vibration_value;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .VibrateController(parameters.vibration_device_handle, parameters.vibration_value);

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.vibration_device_handle.npad_type,
              parameters.vibration_device_handle.npad_id,
              parameters.vibration_device_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::GetActualVibrationValue(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle vibration_device_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.vibration_device_handle.npad_type,
              parameters.vibration_device_handle.npad_id,
              parameters.vibration_device_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(RESULT_SUCCESS);
    rb.PushRaw(applet_resource->GetController<Controller_NPad>(HidController::NPad)
                   .GetLastVibration(parameters.vibration_device_handle));
}

void Hid::CreateActiveVibrationDeviceList(Kernel::HLERequestContext& ctx) {
    LOG_DEBUG(Service_HID, "called");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(RESULT_SUCCESS);
    rb.PushIpcInterface<IActiveVibrationDeviceList>(system, applet_resource);
}

void Hid::PermitVibration(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto can_vibrate{rp.Pop<bool>()};

    Settings::values.vibration_enabled.SetValue(can_vibrate);

    LOG_DEBUG(Service_HID, "called, can_vibrate={}", can_vibrate);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::IsVibrationPermitted(Kernel::HLERequestContext& ctx) {
    LOG_DEBUG(Service_HID, "called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(RESULT_SUCCESS);
    rb.Push(Settings::values.vibration_enabled.GetValue());
}

void Hid::SendVibrationValues(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    const auto handles = ctx.ReadBuffer(0);
    const auto vibrations = ctx.ReadBuffer(1);

    std::vector<Controller_NPad::DeviceHandle> vibration_device_handles(
        handles.size() / sizeof(Controller_NPad::DeviceHandle));
    std::vector<Controller_NPad::VibrationValue> vibration_values(
        vibrations.size() / sizeof(Controller_NPad::VibrationValue));

    std::memcpy(vibration_device_handles.data(), handles.data(), handles.size());
    std::memcpy(vibration_values.data(), vibrations.data(), vibrations.size());

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .VibrateControllers(vibration_device_handles, vibration_values);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::BeginPermitVibrationSession(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetPermitVibrationSession(true);

    LOG_DEBUG(Service_HID, "called, applet_resource_user_id={}", applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::EndPermitVibrationSession(Kernel::HLERequestContext& ctx) {
    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetPermitVibrationSession(false);

    LOG_DEBUG(Service_HID, "called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::IsVibrationDeviceMounted(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle vibration_device_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_DEBUG(Service_HID,
              "called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
              parameters.vibration_device_handle.npad_type,
              parameters.vibration_device_handle.npad_id,
              parameters.vibration_device_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(RESULT_SUCCESS);
    rb.Push(applet_resource->GetController<Controller_NPad>(HidController::NPad)
                .IsVibrationDeviceMounted(parameters.vibration_device_handle));
}

void Hid::ActivateConsoleSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}",
                applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::StartConsoleSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_WARNING(
        Service_HID,
        "(STUBBED) called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
        parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
        parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::StopConsoleSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    struct Parameters {
        Controller_NPad::DeviceHandle sixaxis_handle;
        INSERT_PADDING_WORDS_NOINIT(1);
        u64 applet_resource_user_id;
    };

    const auto parameters{rp.PopRaw<Parameters>()};

    LOG_WARNING(
        Service_HID,
        "(STUBBED) called, npad_type={}, npad_id={}, device_index={}, applet_resource_user_id={}",
        parameters.sixaxis_handle.npad_type, parameters.sixaxis_handle.npad_id,
        parameters.sixaxis_handle.device_index, parameters.applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::ActivateSevenSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}",
                applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::StartSevenSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}",
                applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::StopSevenSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}",
                applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::InitializeSevenSixAxisSensor(Kernel::HLERequestContext& ctx) {
    LOG_WARNING(Service_HID, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::FinalizeSevenSixAxisSensor(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}",
                applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::ResetSevenSixAxisSensorTimestamp(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}",
                applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::SetIsPalmaAllConnectable(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};
    const auto is_palma_all_connectable{rp.Pop<bool>()};

    LOG_WARNING(Service_HID,
                "(STUBBED) called, applet_resource_user_id={}, is_palma_all_connectable={}",
                applet_resource_user_id, is_palma_all_connectable);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::SetPalmaBoostMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto palma_boost_mode{rp.Pop<bool>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, palma_boost_mode={}", palma_boost_mode);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::SetNpadCommunicationMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};
    const auto communication_mode{rp.PopEnum<Controller_NPad::NpadCommunicationMode>()};

    applet_resource->GetController<Controller_NPad>(HidController::NPad)
        .SetNpadCommunicationMode(communication_mode);

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}, communication_mode={}",
                applet_resource_user_id, communication_mode);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(RESULT_SUCCESS);
}

void Hid::GetNpadCommunicationMode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const auto applet_resource_user_id{rp.Pop<u64>()};

    LOG_WARNING(Service_HID, "(STUBBED) called, applet_resource_user_id={}",
                applet_resource_user_id);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(RESULT_SUCCESS);
    rb.PushEnum(applet_resource->GetController<Controller_NPad>(HidController::NPad)
                    .GetNpadCommunicationMode());
}

class HidDbg final : public ServiceFramework<HidDbg> {
public:
    explicit HidDbg(Core::System& system_) : ServiceFramework{system_, "hid:dbg"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "DeactivateDebugPad"},
            {1, nullptr, "SetDebugPadAutoPilotState"},
            {2, nullptr, "UnsetDebugPadAutoPilotState"},
            {10, nullptr, "DeactivateTouchScreen"},
            {11, nullptr, "SetTouchScreenAutoPilotState"},
            {12, nullptr, "UnsetTouchScreenAutoPilotState"},
            {13, nullptr, "GetTouchScreenConfiguration"},
            {20, nullptr, "DeactivateMouse"},
            {21, nullptr, "SetMouseAutoPilotState"},
            {22, nullptr, "UnsetMouseAutoPilotState"},
            {30, nullptr, "DeactivateKeyboard"},
            {31, nullptr, "SetKeyboardAutoPilotState"},
            {32, nullptr, "UnsetKeyboardAutoPilotState"},
            {50, nullptr, "DeactivateXpad"},
            {51, nullptr, "SetXpadAutoPilotState"},
            {52, nullptr, "UnsetXpadAutoPilotState"},
            {60, nullptr, "ClearNpadSystemCommonPolicy"},
            {61, nullptr, "DeactivateNpad"},
            {62, nullptr, "ForceDisconnectNpad"},
            {91, nullptr, "DeactivateGesture"},
            {110, nullptr, "DeactivateHomeButton"},
            {111, nullptr, "SetHomeButtonAutoPilotState"},
            {112, nullptr, "UnsetHomeButtonAutoPilotState"},
            {120, nullptr, "DeactivateSleepButton"},
            {121, nullptr, "SetSleepButtonAutoPilotState"},
            {122, nullptr, "UnsetSleepButtonAutoPilotState"},
            {123, nullptr, "DeactivateInputDetector"},
            {130, nullptr, "DeactivateCaptureButton"},
            {131, nullptr, "SetCaptureButtonAutoPilotState"},
            {132, nullptr, "UnsetCaptureButtonAutoPilotState"},
            {133, nullptr, "SetShiftAccelerometerCalibrationValue"},
            {134, nullptr, "GetShiftAccelerometerCalibrationValue"},
            {135, nullptr, "SetShiftGyroscopeCalibrationValue"},
            {136, nullptr, "GetShiftGyroscopeCalibrationValue"},
            {140, nullptr, "DeactivateConsoleSixAxisSensor"},
            {141, nullptr, "GetConsoleSixAxisSensorSamplingFrequency"},
            {142, nullptr, "DeactivateSevenSixAxisSensor"},
            {143, nullptr, "GetConsoleSixAxisSensorCountStates"},
            {144, nullptr, "GetAccelerometerFsr"},
            {145, nullptr, "SetAccelerometerFsr"},
            {146, nullptr, "GetAccelerometerOdr"},
            {147, nullptr, "SetAccelerometerOdr"},
            {148, nullptr, "GetGyroscopeFsr"},
            {149, nullptr, "SetGyroscopeFsr"},
            {150, nullptr, "GetGyroscopeOdr"},
            {151, nullptr, "SetGyroscopeOdr"},
            {152, nullptr, "GetWhoAmI"},
            {201, nullptr, "ActivateFirmwareUpdate"},
            {202, nullptr, "DeactivateFirmwareUpdate"},
            {203, nullptr, "StartFirmwareUpdate"},
            {204, nullptr, "GetFirmwareUpdateStage"},
            {205, nullptr, "GetFirmwareVersion"},
            {206, nullptr, "GetDestinationFirmwareVersion"},
            {207, nullptr, "DiscardFirmwareInfoCacheForRevert"},
            {208, nullptr, "StartFirmwareUpdateForRevert"},
            {209, nullptr, "GetAvailableFirmwareVersionForRevert"},
            {210, nullptr, "IsFirmwareUpdatingDevice"},
            {211, nullptr, "StartFirmwareUpdateIndividual"},
            {215, nullptr, "SetUsbFirmwareForceUpdateEnabled"},
            {216, nullptr, "SetAllKuinaDevicesToFirmwareUpdateMode"},
            {221, nullptr, "UpdateControllerColor"},
            {222, nullptr, "ConnectUsbPadsAsync"},
            {223, nullptr, "DisconnectUsbPadsAsync"},
            {224, nullptr, "UpdateDesignInfo"},
            {225, nullptr, "GetUniquePadDriverState"},
            {226, nullptr, "GetSixAxisSensorDriverStates"},
            {227, nullptr, "GetRxPacketHistory"},
            {228, nullptr, "AcquireOperationEventHandle"},
            {229, nullptr, "ReadSerialFlash"},
            {230, nullptr, "WriteSerialFlash"},
            {231, nullptr, "GetOperationResult"},
            {232, nullptr, "EnableShipmentMode"},
            {233, nullptr, "ClearPairingInfo"},
            {234, nullptr, "GetUniquePadDeviceTypeSetInternal"},
            {235, nullptr, "EnableAnalogStickPower"},
            {236, nullptr, "RequestKuinaUartClockCal"},
            {237, nullptr, "GetKuinaUartClockCal"},
            {238, nullptr, "SetKuinaUartClockTrim"},
            {239, nullptr, "KuinaLoopbackTest"},
            {240, nullptr, "RequestBatteryVoltage"},
            {241, nullptr, "GetBatteryVoltage"},
            {242, nullptr, "GetUniquePadPowerInfo"},
            {243, nullptr, "RebootUniquePad"},
            {244, nullptr, "RequestKuinaFirmwareVersion"},
            {245, nullptr, "GetKuinaFirmwareVersion"},
            {246, nullptr, "GetVidPid"},
            {301, nullptr, "GetAbstractedPadHandles"},
            {302, nullptr, "GetAbstractedPadState"},
            {303, nullptr, "GetAbstractedPadsState"},
            {321, nullptr, "SetAutoPilotVirtualPadState"},
            {322, nullptr, "UnsetAutoPilotVirtualPadState"},
            {323, nullptr, "UnsetAllAutoPilotVirtualPadState"},
            {324, nullptr, "AttachHdlsWorkBuffer"},
            {325, nullptr, "ReleaseHdlsWorkBuffer"},
            {326, nullptr, "DumpHdlsNpadAssignmentState"},
            {327, nullptr, "DumpHdlsStates"},
            {328, nullptr, "ApplyHdlsNpadAssignmentState"},
            {329, nullptr, "ApplyHdlsStateList"},
            {330, nullptr, "AttachHdlsVirtualDevice"},
            {331, nullptr, "DetachHdlsVirtualDevice"},
            {332, nullptr, "SetHdlsState"},
            {350, nullptr, "AddRegisteredDevice"},
            {400, nullptr, "DisableExternalMcuOnNxDevice"},
            {401, nullptr, "DisableRailDeviceFiltering"},
            {402, nullptr, "EnableWiredPairing"},
            {403, nullptr, "EnableShipmentModeAutoClear"},
            {500, nullptr, "SetFactoryInt"},
            {501, nullptr, "IsFactoryBootEnabled"},
            {550, nullptr, "SetAnalogStickModelDataTemporarily"},
            {551, nullptr, "GetAnalogStickModelData"},
            {552, nullptr, "ResetAnalogStickModelData"},
            {600, nullptr, "ConvertPadState"},
            {2000, nullptr, "DeactivateDigitizer"},
            {2001, nullptr, "SetDigitizerAutoPilotState"},
            {2002, nullptr, "UnsetDigitizerAutoPilotState"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class HidSys final : public ServiceFramework<HidSys> {
public:
    explicit HidSys(Core::System& system_) : ServiceFramework{system_, "hid:sys"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {31, nullptr, "SendKeyboardLockKeyEvent"},
            {101, nullptr, "AcquireHomeButtonEventHandle"},
            {111, nullptr, "ActivateHomeButton"},
            {121, nullptr, "AcquireSleepButtonEventHandle"},
            {131, nullptr, "ActivateSleepButton"},
            {141, nullptr, "AcquireCaptureButtonEventHandle"},
            {151, nullptr, "ActivateCaptureButton"},
            {161, nullptr, "GetPlatformConfig"},
            {210, nullptr, "AcquireNfcDeviceUpdateEventHandle"},
            {211, nullptr, "GetNpadsWithNfc"},
            {212, nullptr, "AcquireNfcActivateEventHandle"},
            {213, nullptr, "ActivateNfc"},
            {214, nullptr, "GetXcdHandleForNpadWithNfc"},
            {215, nullptr, "IsNfcActivated"},
            {230, nullptr, "AcquireIrSensorEventHandle"},
            {231, nullptr, "ActivateIrSensor"},
            {301, nullptr, "ActivateNpadSystem"},
            {303, nullptr, "ApplyNpadSystemCommonPolicy"},
            {304, nullptr, "EnableAssigningSingleOnSlSrPress"},
            {305, nullptr, "DisableAssigningSingleOnSlSrPress"},
            {306, nullptr, "GetLastActiveNpad"},
            {307, nullptr, "GetNpadSystemExtStyle"},
            {308, nullptr, "ApplyNpadSystemCommonPolicyFull"},
            {309, nullptr, "GetNpadFullKeyGripColor"},
            {310, nullptr, "GetMaskedSupportedNpadStyleSet"},
            {311, nullptr, "SetNpadPlayerLedBlinkingDevice"},
            {312, nullptr, "SetSupportedNpadStyleSetAll"},
            {313, nullptr, "GetNpadCaptureButtonAssignment"},
            {314, nullptr, "GetAppletFooterUiType"},
            {315, nullptr, "GetAppletDetailedUiType"},
            {321, nullptr, "GetUniquePadsFromNpad"},
            {322, nullptr, "GetIrSensorState"},
            {323, nullptr, "GetXcdHandleForNpadWithIrSensor"},
            {500, nullptr, "SetAppletResourceUserId"},
            {501, nullptr, "RegisterAppletResourceUserId"},
            {502, nullptr, "UnregisterAppletResourceUserId"},
            {503, nullptr, "EnableAppletToGetInput"},
            {504, nullptr, "SetAruidValidForVibration"},
            {505, nullptr, "EnableAppletToGetSixAxisSensor"},
            {510, nullptr, "SetVibrationMasterVolume"},
            {511, nullptr, "GetVibrationMasterVolume"},
            {512, nullptr, "BeginPermitVibrationSession"},
            {513, nullptr, "EndPermitVibrationSession"},
            {520, nullptr, "EnableHandheldHids"},
            {521, nullptr, "DisableHandheldHids"},
            {522, nullptr, "SetJoyConRailEnabled"},
            {523, nullptr, "IsJoyConRailEnabled"},
            {540, nullptr, "AcquirePlayReportControllerUsageUpdateEvent"},
            {541, nullptr, "GetPlayReportControllerUsages"},
            {542, nullptr, "AcquirePlayReportRegisteredDeviceUpdateEvent"},
            {543, nullptr, "GetRegisteredDevicesOld"},
            {544, nullptr, "AcquireConnectionTriggerTimeoutEvent"},
            {545, nullptr, "SendConnectionTrigger"},
            {546, nullptr, "AcquireDeviceRegisteredEventForControllerSupport"},
            {547, nullptr, "GetAllowedBluetoothLinksCount"},
            {548, nullptr, "GetRegisteredDevices"},
            {549, nullptr, "GetConnectableRegisteredDevices"},
            {700, nullptr, "ActivateUniquePad"},
            {702, nullptr, "AcquireUniquePadConnectionEventHandle"},
            {703, nullptr, "GetUniquePadIds"},
            {751, nullptr, "AcquireJoyDetachOnBluetoothOffEventHandle"},
            {800, nullptr, "ListSixAxisSensorHandles"},
            {801, nullptr, "IsSixAxisSensorUserCalibrationSupported"},
            {802, nullptr, "ResetSixAxisSensorCalibrationValues"},
            {803, nullptr, "StartSixAxisSensorUserCalibration"},
            {804, nullptr, "CancelSixAxisSensorUserCalibration"},
            {805, nullptr, "GetUniquePadBluetoothAddress"},
            {806, nullptr, "DisconnectUniquePad"},
            {807, nullptr, "GetUniquePadType"},
            {808, nullptr, "GetUniquePadInterface"},
            {809, nullptr, "GetUniquePadSerialNumber"},
            {810, nullptr, "GetUniquePadControllerNumber"},
            {811, nullptr, "GetSixAxisSensorUserCalibrationStage"},
            {812, nullptr, "GetConsoleUniqueSixAxisSensorHandle"},
            {821, nullptr, "StartAnalogStickManualCalibration"},
            {822, nullptr, "RetryCurrentAnalogStickManualCalibrationStage"},
            {823, nullptr, "CancelAnalogStickManualCalibration"},
            {824, nullptr, "ResetAnalogStickManualCalibration"},
            {825, nullptr, "GetAnalogStickState"},
            {826, nullptr, "GetAnalogStickManualCalibrationStage"},
            {827, nullptr, "IsAnalogStickButtonPressed"},
            {828, nullptr, "IsAnalogStickInReleasePosition"},
            {829, nullptr, "IsAnalogStickInCircumference"},
            {830, nullptr, "SetNotificationLedPattern"},
            {831, nullptr, "SetNotificationLedPatternWithTimeout"},
            {832, nullptr, "PrepareHidsForNotificationWake"},
            {850, nullptr, "IsUsbFullKeyControllerEnabled"},
            {851, nullptr, "EnableUsbFullKeyController"},
            {852, nullptr, "IsUsbConnected"},
            {870, nullptr, "IsHandheldButtonPressedOnConsoleMode"},
            {900, nullptr, "ActivateInputDetector"},
            {901, nullptr, "NotifyInputDetector"},
            {1000, nullptr, "InitializeFirmwareUpdate"},
            {1001, nullptr, "GetFirmwareVersion"},
            {1002, nullptr, "GetAvailableFirmwareVersion"},
            {1003, nullptr, "IsFirmwareUpdateAvailable"},
            {1004, nullptr, "CheckFirmwareUpdateRequired"},
            {1005, nullptr, "StartFirmwareUpdate"},
            {1006, nullptr, "AbortFirmwareUpdate"},
            {1007, nullptr, "GetFirmwareUpdateState"},
            {1008, nullptr, "ActivateAudioControl"},
            {1009, nullptr, "AcquireAudioControlEventHandle"},
            {1010, nullptr, "GetAudioControlStates"},
            {1011, nullptr, "DeactivateAudioControl"},
            {1050, nullptr, "IsSixAxisSensorAccurateUserCalibrationSupported"},
            {1051, nullptr, "StartSixAxisSensorAccurateUserCalibration"},
            {1052, nullptr, "CancelSixAxisSensorAccurateUserCalibration"},
            {1053, nullptr, "GetSixAxisSensorAccurateUserCalibrationState"},
            {1100, nullptr, "GetHidbusSystemServiceObject"},
            {1120, nullptr, "SetFirmwareHotfixUpdateSkipEnabled"},
            {1130, nullptr, "InitializeUsbFirmwareUpdate"},
            {1131, nullptr, "FinalizeUsbFirmwareUpdate"},
            {1132, nullptr, "CheckUsbFirmwareUpdateRequired"},
            {1133, nullptr, "StartUsbFirmwareUpdate"},
            {1134, nullptr, "GetUsbFirmwareUpdateState"},
            {1150, nullptr, "SetTouchScreenMagnification"},
            {1151, nullptr, "GetTouchScreenFirmwareVersion"},
            {1152, nullptr, "SetTouchScreenDefaultConfiguration"},
            {1153, nullptr, "GetTouchScreenDefaultConfiguration"},
            {1154, nullptr, "IsFirmwareAvailableForNotification"},
            {1155, nullptr, "SetForceHandheldStyleVibration"},
            {1156, nullptr, "SendConnectionTriggerWithoutTimeoutEvent"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class HidTmp final : public ServiceFramework<HidTmp> {
public:
    explicit HidTmp(Core::System& system_) : ServiceFramework{system_, "hid:tmp"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, nullptr, "GetConsoleSixAxisSensorCalibrationValues"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

class HidBus final : public ServiceFramework<HidBus> {
public:
    explicit HidBus(Core::System& system_) : ServiceFramework{system_, "hidbus"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {1, nullptr, "GetBusHandle"},
            {2, nullptr, "IsExternalDeviceConnected"},
            {3, nullptr, "Initialize"},
            {4, nullptr, "Finalize"},
            {5, nullptr, "EnableExternalDevice"},
            {6, nullptr, "GetExternalDeviceId"},
            {7, nullptr, "SendCommandAsync"},
            {8, nullptr, "GetSendCommandAsynceResult"},
            {9, nullptr, "SetEventForSendCommandAsycResult"},
            {10, nullptr, "GetSharedMemoryHandle"},
            {11, nullptr, "EnableJoyPollingReceiveMode"},
            {12, nullptr, "DisableJoyPollingReceiveMode"},
            {13, nullptr, "GetPollingData"},
            {14, nullptr, "SetStatusManagerType"},
        };
        // clang-format on

        RegisterHandlers(functions);
    }
};

void ReloadInputDevices() {
    Settings::values.is_device_reload_pending.store(true);
}

void InstallInterfaces(SM::ServiceManager& service_manager, Core::System& system) {
    std::make_shared<Hid>(system)->InstallAsService(service_manager);
    std::make_shared<HidBus>(system)->InstallAsService(service_manager);
    std::make_shared<HidDbg>(system)->InstallAsService(service_manager);
    std::make_shared<HidSys>(system)->InstallAsService(service_manager);
    std::make_shared<HidTmp>(system)->InstallAsService(service_manager);

    std::make_shared<IRS>(system)->InstallAsService(service_manager);
    std::make_shared<IRS_SYS>(system)->InstallAsService(service_manager);

    std::make_shared<XCD_SYS>(system)->InstallAsService(service_manager);
}

} // namespace Service::HID
