#include <cstddef>
#include <type_traits>
#include "../src/OpenXinput.h"

static_assert(sizeof(OPENXINPUT_STATE_EXTENDED_V1) == 32, "V1 is exactly 32 bytes");
static_assert(offsetof(OPENXINPUT_STATE_EXTENDED_V1, cbSize) == 0, "cbSize offset");
static_assert(offsetof(OPENXINPUT_STATE_EXTENDED_V1, state) == 4, "state offset");
static_assert(offsetof(OPENXINPUT_STATE_EXTENDED_V1, extraByteCount) == 20, "extraByteCount offset");
static_assert(offsetof(OPENXINPUT_STATE_EXTENDED_V1, extraBytes) == 24, "extraBytes offset");
static_assert(sizeof(((OPENXINPUT_STATE_EXTENDED_V1*)nullptr)->extraBytes) == 6, "Six trailing bytes");
static_assert(std::is_standard_layout<OPENXINPUT_STATE_EXTENDED_V1>::value, "C-compatible layout");
static_assert(std::is_same<decltype(&OpenXInputGetStateExtendedV1),
    OpenXInputGetStateExtendedV1_t*>::value, "Header calling convention");
using ExpectedStateQuery = DWORD(WINAPI*)(DWORD, OPENXINPUT_STATE_EXTENDED_V1*);
static_assert(std::is_same<decltype(&OpenXInputGetStateExtendedV1),
    ExpectedStateQuery>::value, "V1 parameter types and WINAPI contract");

#ifndef OPENXINPUT_STATE_EXTENDED_ABI_ONLY
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// Compile the real query and dispatchers in this test translation unit. The
// only device I/O is the gamepad state IOCTL, answered below.
#include "../src/OpenXinput.cpp"

namespace state_extended_test {
static std::atomic<unsigned> ioCalls{0}, otherIoCalls{0}, enumerationCalls{0};
static unsigned checks = 0, failures = 0, cases = 0;

#define CHECK(expression) do { ++checks; if (!(expression)) { ++failures; \
    std::printf("FAIL line=%d expression=%s\n", __LINE__, #expression); } } while (false)

static GamepadState0101 reply0101;
static GamepadState0100 reply0100;
static std::array<BYTE, 16> lastRequest;
static DWORD lastRequestSize;

// Answers the gamepad state IOCTL with the reply its output size asks for
static BOOL WINAPI FakeIo(HANDLE, DWORD code, LPVOID in, DWORD inSize, LPVOID out, DWORD outSize,
    LPDWORD bytes, LPOVERLAPPED)
{
    if (bytes) *bytes = 0;
    if (code != Protocol::IOCTL_XINPUT_GET_GAMEPAD_STATE || !out) {
        ++otherIoCalls;
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    ++ioCalls;
    lastRequestSize = inSize;
    lastRequest.fill(0xEE);
    if (in && inSize <= lastRequest.size()) std::memcpy(lastRequest.data(), in, inSize);
    if (outSize == sizeof(reply0101)) {
        std::memcpy(out, &reply0101, sizeof(reply0101));
        if (bytes) *bytes = sizeof(reply0101);
        return TRUE;
    }
    if (outSize == sizeof(reply0100)) {
        std::memcpy(out, &reply0100, sizeof(reply0100));
        if (bytes) *bytes = sizeof(reply0100);
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}
static HANDLE WINAPI RejectCreate(HANDLE, LPCWSTR, PVOID, PVOID, PVOID)
{ ++enumerationCalls; SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
static BOOL WINAPI RejectClass(HANDLE, const GUID*, LPCWSTR, DWORD, LPCWSTR, SIZE_T)
{ ++enumerationCalls; return FALSE; }
static BOOL WINAPI RejectInfo(HANDLE, DWORD, PDO_DEVINFO_DATA)
{ ++enumerationCalls; return FALSE; }
static BOOL WINAPI RejectInterfaces(HANDLE, PDO_DEVINFO_DATA, const GUID*, DWORD, PDO_DEVICE_INTERFACE_DATA)
{ ++enumerationCalls; return FALSE; }
static BOOL WINAPI RejectDetail(HANDLE, PDO_DEVICE_INTERFACE_DATA, PDO_DEVICE_INTERFACE_DETAIL_DATA,
    DWORD, PDWORD, PDO_DEVINFO_DATA)
{ ++enumerationCalls; return FALSE; }
static BOOL WINAPI RejectProperty(HANDLE, PDO_DEVINFO_DATA, const DEVPROPKEY*, DEVPROPTYPE*,
    LPVOID, DWORD, PDWORD, DWORD)
{ ++enumerationCalls; return FALSE; }
static BOOL WINAPI RejectDestroy(HANDLE)
{ ++enumerationCalls; return FALSE; }

static XINPUT_STATE forwardedState;
static DWORD WINAPI Forwarder(DWORD, XINPUT_STATE* state)
{
    *state = forwardedState;
    return ERROR_SUCCESS;
}

static void SetReplies()
{
    std::memset(&reply0101, 0, sizeof(reply0101));
    reply0101.XUSBVersion = XUSB_VERSION_1_1;
    reply0101.status = 1;
    reply0101.inputId = 0;
    reply0101.dwPacketNumber = 0x1234;
    reply0101.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_GUIDE | XINPUT_GAMEPAD_DPAD_UP;
    reply0101.bLeftTrigger = 0x80;
    reply0101.bRightTrigger = 0x03;
    reply0101.sThumbLX = -32768;
    reply0101.sThumbLY = 0x0080;
    reply0101.sThumbRX = 0x7F00;
    reply0101.sThumbRY = 0x1122;
    reply0101.unk6 = 0x11;
    reply0101.unk7 = 0x22;
    reply0101.unk8 = 0x33;
    reply0101.unk9 = 0x44;
    reply0101.unk10 = 0x55;
    reply0101.bExtraButtons = 0x01;

    std::memset(&reply0100, 0, sizeof(reply0100));
    reply0100.status = 1;
    reply0100.dwPacketNumber = 0x99;
    reply0100.wButtons = XINPUT_GAMEPAD_B;
    reply0100.bLeftTrigger = 0x42;
}

struct Output {
    OPENXINPUT_STATE_EXTENDED_V1 value;
    Output()
    {
        std::memset(&value, 0xa5, sizeof(value));
        value.cbSize = sizeof(value);
    }
    DWORD Query(DWORD slot = 0) { return OpenXInputGetStateExtendedV1(slot, &value); }
};

struct Fixture {
    std::array<DeviceInfo_t*, 32> slots{};
    std::array<DeviceInfo_t*, 16> buses{};
    std::array<DeviceInfo_t, 16> records{};
    Fixture()
    {
        InitializeCriticalSection(&g_csGlobalLock);
        g_pDeviceList = slots.data();
        g_pBusDeviceList = buses.data();
        g_dwDeviceListSize = static_cast<DWORD>(slots.size());
        g_IsInitialized = TRUE;
        // No rescan, so a missing device never reaches enumeration
        g_IsCommunicationEnabled = FALSE;
        XInputCore::g_pfnXInputGetState_Override = nullptr;
        XInputInternal::DeviceInfo::OnEnableSettingChanged(TRUE);
        g_pfnDeviceIoControl = FakeIo;
        g_pfnCreateDeviceInfoList = RejectCreate;
        g_pfnGetClassDevs = RejectClass;
        g_pfnEnumDeviceInfo = RejectInfo;
        g_pfnEnumDeviceInterfaces = RejectInterfaces;
        g_pfnGetDeviceInterfaceDetail = RejectDetail;
        g_pfnGetDeviceProperty = RejectProperty;
        g_pfnDestroyDeviceInfoList = RejectDestroy;
        ioCalls = otherIoCalls = enumerationCalls = 0;
        SetReplies();
    }
    ~Fixture()
    {
        CHECK(otherIoCalls == 0);
        CHECK(enumerationCalls == 0);
        // Every exit path leaves the lock free for another thread
        bool unlocked = false;
        std::thread verifier([&] {
            unlocked = TryEnterCriticalSection(&g_csGlobalLock) != FALSE;
            if (unlocked) LeaveCriticalSection(&g_csGlobalLock);
        });
        verifier.join();
        CHECK(unlocked);
        for (auto* bus : buses) {
            if (bus) XInputInternal::DeviceInfo::Destroy(bus);
        }
        g_IsInitialized = FALSE;
        g_pDeviceList = nullptr;
        g_pBusDeviceList = nullptr;
        g_dwDeviceListSize = 0;
        XInputCore::g_pfnXInputGetState_Override = nullptr;
        DeleteCriticalSection(&g_csGlobalLock);
    }
    DeviceInfo_t& Add(DWORD slot, BYTE channel, WORD version)
    {
        auto& record = records[slot];
        record = {};
        record.status = DEVICE_STATUS_ACTIVE;
        // This sentinel is never passed to a Windows API: FakeIo takes the I/O
        record.hDevice = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1));
        record.hGuideWait = INVALID_HANDLE_VALUE;
        record.dwUserIndex = channel;
        record.XUSBVersion = version;
        record.interfaceCount = 1;
        EnterCriticalSection(&g_csGlobalLock);
        CHECK(DeviceList::SetDeviceOnPort(slot, &record) == S_OK);
        LeaveCriticalSection(&g_csGlobalLock);
        return record;
    }
};

static void CheckReply0101State(const XINPUT_STATE& state)
{
    CHECK(state.dwPacketNumber == 0x1234);
    // As XInputGetStateEx: the guide button stays
    CHECK(state.Gamepad.wButtons == (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_GUIDE | XINPUT_GAMEPAD_DPAD_UP));
    CHECK(state.Gamepad.bLeftTrigger == 0x80);
    CHECK(state.Gamepad.bRightTrigger == 0x03);
    CHECK(state.Gamepad.sThumbLX == -32768);
    CHECK(state.Gamepad.sThumbLY == 0x0080);
    CHECK(state.Gamepad.sThumbRX == 0x7F00);
    CHECK(state.Gamepad.sThumbRY == 0x1122);
}

static void TrailingBytes()
{
    Fixture fixture;
    fixture.Add(3, 2, XUSB_VERSION_1_1);
    Output output;
    CHECK(output.Query(3) == ERROR_SUCCESS);
    CHECK(ioCalls == 1);
    // One XUSB 1.1 request for the record's channel
    CHECK(lastRequestSize == sizeof(InBaseRequest_t));
    CHECK(lastRequest[0] == 0x01 && lastRequest[1] == 0x01 && lastRequest[2] == 2);
    CHECK(output.value.cbSize == sizeof(output.value));
    CheckReply0101State(output.value.state);
    CHECK(output.value.extraByteCount == 6);
    const BYTE expected[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x01 };
    CHECK(std::memcmp(output.value.extraBytes, expected, 6) == 0);

    // Unchanged: XInputGetStateEx returns the same state, and the Share
    // channel still reads bit 0 of the last trailing byte
    XINPUT_STATE state;
    std::memset(&state, 0xa5, sizeof(state));
    CHECK(OpenXInputGetStateEx(3, &state) == ERROR_SUCCESS);
    CheckReply0101State(state);
    XINPUT_SYSTEM_BUTTONS buttons;
    std::memset(&buttons, 0, sizeof(buttons));
    CHECK(OpenXInputGetSystemButtons(3, &buttons, nullptr) == ERROR_SUCCESS);
    CHECK(buttons.ExtraSystemButtons == XINPUT_GAMEPAD_EXTRAS_SHARE);

    // Every byte value arrives in order
    for (int value = 0; value < 256; value += 17) {
        reply0101.unk6 = static_cast<BYTE>(value);
        reply0101.unk7 = static_cast<BYTE>(value ^ 0xFF);
        reply0101.unk8 = static_cast<BYTE>(value + 1);
        reply0101.unk9 = static_cast<BYTE>(value + 2);
        reply0101.unk10 = static_cast<BYTE>(value + 3);
        reply0101.bExtraButtons = static_cast<BYTE>(value + 4);
        Output each;
        CHECK(each.Query(3) == ERROR_SUCCESS);
        CHECK(each.value.extraByteCount == 6);
        CHECK(each.value.extraBytes[0] == static_cast<BYTE>(value));
        CHECK(each.value.extraBytes[1] == static_cast<BYTE>(value ^ 0xFF));
        CHECK(each.value.extraBytes[2] == static_cast<BYTE>(value + 1));
        CHECK(each.value.extraBytes[3] == static_cast<BYTE>(value + 2));
        CHECK(each.value.extraBytes[4] == static_cast<BYTE>(value + 3));
        CHECK(each.value.extraBytes[5] == static_cast<BYTE>(value + 4));
    }
}

static void Xusb10HasNoTrailingBytes()
{
    Fixture fixture;
    auto& record = fixture.Add(0, 0, XUSB_VERSION_1_0);
    Output output;
    CHECK(output.Query(0) == ERROR_SUCCESS);
    CHECK(lastRequestSize == sizeof(InGamepadState0100));
    CHECK(output.value.state.dwPacketNumber == 0x99);
    CHECK(output.value.state.Gamepad.wButtons == XINPUT_GAMEPAD_B);
    CHECK(output.value.state.Gamepad.bLeftTrigger == 0x42);
    CHECK(output.value.extraByteCount == 0);
    for (BYTE b : output.value.extraBytes) CHECK(b == 0);

    // A record that answered with 1.1 before and 1.0 now keeps no stale bytes
    record.XUSBVersion = XUSB_VERSION_1_1;
    Output before;
    CHECK(before.Query(0) == ERROR_SUCCESS && before.value.extraByteCount == 6);
    record.XUSBVersion = XUSB_VERSION_1_0;
    Output after;
    CHECK(after.Query(0) == ERROR_SUCCESS);
    CHECK(after.value.extraByteCount == 0);
    for (BYTE b : after.value.extraBytes) CHECK(b == 0);
}

static void DisabledInput()
{
    Fixture fixture;
    fixture.Add(1, 0, XUSB_VERSION_1_1);
    Output live;
    CHECK(live.Query(1) == ERROR_SUCCESS);
    const unsigned io = ioCalls;
    XInputInternal::DeviceInfo::OnEnableSettingChanged(FALSE);
    Output disabled;
    CHECK(disabled.Query(1) == ERROR_SUCCESS);
    CHECK(ioCalls == io);
    CHECK(disabled.value.cbSize == sizeof(disabled.value));
    CHECK(disabled.value.state.dwPacketNumber == 0x1234 + 1);
    CHECK(disabled.value.state.Gamepad.wButtons == 0);
    CHECK(disabled.value.state.Gamepad.sThumbLX == 0);
    CHECK(disabled.value.extraByteCount == 0);
    for (BYTE b : disabled.value.extraBytes) CHECK(b == 0);
    XInputInternal::DeviceInfo::OnEnableSettingChanged(TRUE);
}

static void Arguments()
{
    Fixture fixture;
    fixture.Add(0, 0, XUSB_VERSION_1_1);
    CHECK(OpenXInputGetStateExtendedV1(0, nullptr) == ERROR_INVALID_PARAMETER);
    for (DWORD size : {0u, 16u, 31u, 33u, 64u}) {
        Output invalid;
        invalid.value.cbSize = size;
        const auto before = invalid.value;
        CHECK(invalid.Query(0) == ERROR_INVALID_PARAMETER);
        CHECK(std::memcmp(&invalid.value, &before, sizeof(before)) == 0);
    }
    for (DWORD slot : {16u, 255u, MAXDWORD}) {
        Output invalid;
        const auto before = invalid.value;
        CHECK(invalid.Query(slot) == ERROR_BAD_ARGUMENTS);
        CHECK(std::memcmp(&invalid.value, &before, sizeof(before)) == 0);
    }
    CHECK(ioCalls == 0);
    // An empty port
    Output empty;
    CHECK(empty.Query(5) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(ioCalls == 0);
}

static void Forwarded()
{
    Fixture fixture;
    fixture.Add(0, 0, XUSB_VERSION_1_1);
    std::memset(&forwardedState, 0, sizeof(forwardedState));
    forwardedState.dwPacketNumber = 77;
    forwardedState.Gamepad.wButtons = XINPUT_GAMEPAD_Y;
    XInputCore::g_pfnXInputGetState_Override = Forwarder;
    Output output;
    CHECK(output.Query(0) == ERROR_SUCCESS);
    CHECK(output.value.state.dwPacketNumber == 77);
    CHECK(output.value.state.Gamepad.wButtons == XINPUT_GAMEPAD_Y);
    CHECK(output.value.extraByteCount == 0);
    for (BYTE b : output.value.extraBytes) CHECK(b == 0);
    CHECK(ioCalls == 0);
}

static void Run(const char* name, void (*test)())
{
    const unsigned before = failures;
    ++cases;
    std::printf("RUN %s\n", name);
    test();
    std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
}
} // namespace state_extended_test

int main()
{
    using namespace state_extended_test;
    setvbuf(stdout, nullptr, _IONBF, 0);
    Output uninitialized;
    CHECK(uninitialized.Query() != ERROR_SUCCESS);
    Run("trailing bytes after sThumbRY, in order", TrailingBytes);
    Run("XUSB 1.0 has none, and none stay behind", Xusb10HasNoTrailingBytes);
    Run("disabled input", DisabledInput);
    Run("arguments and an empty port", Arguments);
    Run("another XInput serves the call", Forwarded);
    std::printf("SUMMARY cases=%u checks=%u failures=%u\n", cases, checks, failures);
    return failures ? 1 : 0;
}
#endif
