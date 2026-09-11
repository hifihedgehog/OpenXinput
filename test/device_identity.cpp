#include <cstddef>
#include <type_traits>
#include "../src/OpenXinput.h"

static_assert(sizeof(OPENXINPUT_DEVICE_IDENTITY_V1) == 24, "V1 is exactly 24 bytes");
static_assert(offsetof(OPENXINPUT_DEVICE_IDENTITY_V1, cbSize) == 0, "cbSize offset");
static_assert(offsetof(OPENXINPUT_DEVICE_IDENTITY_V1, requiredCch) == 4, "requiredCch offset");
static_assert(offsetof(OPENXINPUT_DEVICE_IDENTITY_V1, generation) == 8, "generation offset");
static_assert(offsetof(OPENXINPUT_DEVICE_IDENTITY_V1, interfaceIndex) == 16, "interfaceIndex offset");
static_assert(offsetof(OPENXINPUT_DEVICE_IDENTITY_V1, interfaceCount) == 20, "interfaceCount offset");
static_assert(std::is_standard_layout<OPENXINPUT_DEVICE_IDENTITY_V1>::value, "C-compatible layout");
static_assert(std::is_same<decltype(&OpenXInputGetDeviceIdentityV1),
    OpenXInputGetDeviceIdentityV1_t*>::value, "Header calling convention");
using ExpectedIdentityQuery = DWORD(WINAPI*)(DWORD, OPENXINPUT_DEVICE_IDENTITY_V1*, WCHAR*, DWORD);
static_assert(std::is_same<decltype(&OpenXInputGetDeviceIdentityV1),
    ExpectedIdentityQuery>::value, "V1 parameter types and WINAPI contract");

#ifndef OPENXINPUT_IDENTITY_ABI_ONLY
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// Compile the real query, records, and lifetime helpers in this test translation unit.
// No library initialization or native enumeration is used by the fixture.
#include "../src/OpenXinput.cpp"

namespace identity_test {
static_assert(XUSER_MAX_COUNT == 16, "Exercise the bundled 16-slot configuration");
static std::atomic<unsigned> ioCalls{0}, enumerationCalls{0}, forwardedCalls{0};
static unsigned checks = 0, failures = 0, cases = 0;
static unsigned totalIoCalls = 0, totalEnumerationCalls = 0, totalForwardedCalls = 0;

#define CHECK(expression) do { ++checks; if (!(expression)) { ++failures; \
    std::printf("FAIL line=%d expression=%s\n", __LINE__, #expression); } } while (false)

static BOOL WINAPI RejectIo(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD bytes, LPOVERLAPPED)
{
    ++ioCalls;
    if (bytes) *bytes = 0;
    SetLastError(ERROR_ACCESS_DENIED);
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
static DWORD WINAPI RejectForwarder(DWORD, XINPUT_STATE*)
{ ++forwardedCalls; return ERROR_ACCESS_DENIED; }

static void SetBarriers()
{
    g_pfnDeviceIoControl = RejectIo;
    g_pfnCreateDeviceInfoList = RejectCreate;
    g_pfnGetClassDevs = RejectClass;
    g_pfnEnumDeviceInfo = RejectInfo;
    g_pfnEnumDeviceInterfaces = RejectInterfaces;
    g_pfnGetDeviceInterfaceDetail = RejectDetail;
    g_pfnGetDeviceProperty = RejectProperty;
    g_pfnDestroyDeviceInfoList = RejectDestroy;
}

struct Output {
    OPENXINPUT_DEVICE_IDENTITY_V1 identity;
    std::array<WCHAR, 96> path;
    Output()
    {
        std::memset(&identity, 0xa5, sizeof(identity));
        identity.cbSize = sizeof(identity);
        path.fill(L'!');
    }
    DWORD Query(DWORD slot = 0, DWORD capacity = 96)
    { return OpenXInputGetDeviceIdentityV1(slot, &identity, path.data(), capacity); }
    void Cleared(DWORD required = 0, bool pathWritten = true) const
    {
        CHECK(identity.cbSize == 24);
        CHECK(identity.requiredCch == required);
        CHECK(identity.generation == 0);
        CHECK(identity.interfaceIndex == 0);
        CHECK(identity.interfaceCount == 0);
        CHECK(path[0] == (pathWritten ? L'\0' : L'!'));
        for (size_t i = 1; i < path.size(); ++i) CHECK(path[i] == L'!');
    }
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
        g_IsCommunicationEnabled = TRUE;
        g_identityGeneration = 0;
        XInputCore::g_pfnXInputGetState_Override = nullptr;
        SetBarriers();
        ioCalls = enumerationCalls = forwardedCalls = 0;
    }
    ~Fixture()
    {
        CHECK(ioCalls == 0);
        CHECK(enumerationCalls == 0);
        CHECK(forwardedCalls == 0);
        totalIoCalls += ioCalls.load();
        totalEnumerationCalls += enumerationCalls.load();
        totalForwardedCalls += forwardedCalls.load();
        // A different thread must acquire the lock after every query exit path.
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
        g_IsCommunicationEnabled = FALSE;
        g_pDeviceList = nullptr;
        g_pBusDeviceList = nullptr;
        g_dwDeviceListSize = 0;
        XInputCore::g_pfnXInputGetState_Override = nullptr;
        DeleteCriticalSection(&g_csGlobalLock);
    }
    DeviceInfo_t& Add(DWORD slot = 0, BYTE channel = 0, DWORD count = 1,
        const WCHAR* path = L"\\\\?\\HID#identity_\u03a9#MiXeD")
    {
        auto& record = records[slot];
        record = {};
        record.status = DEVICE_STATUS_ACTIVE;
        // This sentinel is never passed to a Windows API or to Destroy().
        record.hDevice = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1));
        record.hGuideWait = INVALID_HANDLE_VALUE;
        record.lpDevicePath = const_cast<WCHAR*>(path);
        record.identityPathCapacity = static_cast<DWORD>(wcslen(path) + 1);
        record.dwDevicePathSize = record.identityPathCapacity;
        record.dwUserIndex = channel;
        record.interfaceCount = count;
        EnterCriticalSection(&g_csGlobalLock);
        CHECK(DeviceList::SetDeviceOnPort(slot, &record) == S_OK);
        LeaveCriticalSection(&g_csGlobalLock);
        return record;
    }
};

struct Guarded {
    BYTE* allocation = nullptr;
    size_t page = 0;
    explicit Guarded()
    {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        page = info.dwPageSize;
        allocation = static_cast<BYTE*>(VirtualAlloc(nullptr, page * 2,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD previous = 0;
        if (!allocation || !VirtualProtect(allocation + page, page, PAGE_NOACCESS, &previous)) {
            std::puts("FAIL guard-page allocation");
            std::exit(70);
        }
    }
    ~Guarded() { VirtualFree(allocation, 0, MEM_RELEASE); }
    WCHAR* End(size_t cch) { return reinterpret_cast<WCHAR*>(allocation + page) - cch; }
};

static void InvalidArguments()
{
    Fixture fixture;
    fixture.Add();
    Output output;
    CHECK(OpenXInputGetDeviceIdentityV1(0, nullptr, output.path.data(), 96) == ERROR_INVALID_PARAMETER);
    for (DWORD size : {0u, 16u, 23u, 25u, 48u}) {
        Output invalid;
        invalid.identity.cbSize = size;
        const auto before = invalid.identity;
        CHECK(invalid.Query() == ERROR_INVALID_PARAMETER);
        CHECK(std::memcmp(&invalid.identity, &before, sizeof(before)) == 0);
        CHECK(invalid.path[0] == L'!');
    }
    for (DWORD slot : {16u, 255u, MAXDWORD}) {
        Output invalid;
        const auto before = invalid.identity;
        CHECK(invalid.Query(slot) == ERROR_INVALID_PARAMETER);
        CHECK(std::memcmp(&invalid.identity, &before, sizeof(before)) == 0);
        CHECK(invalid.path[0] == L'!');
    }
    for (DWORD capacity : {32769u, MAXDWORD}) {
        Output invalid;
        const auto before = invalid.identity;
        CHECK(invalid.Query(0, capacity) == ERROR_INVALID_PARAMETER);
        CHECK(std::memcmp(&invalid.identity, &before, sizeof(before)) == 0);
    }
    const auto before = output.identity;
    CHECK(OpenXInputGetDeviceIdentityV1(0, &output.identity, nullptr, 1) == ERROR_INVALID_PARAMETER);
    CHECK(std::memcmp(&output.identity, &before, sizeof(before)) == 0);
}

static void SizingAndCopy()
{
    Fixture fixture;
    const auto& record = fixture.Add();
    const DWORD required = record.identityPathCapacity;
    Output sizing;
    CHECK(OpenXInputGetDeviceIdentityV1(0, &sizing.identity, nullptr, 0) == ERROR_INSUFFICIENT_BUFFER);
    sizing.Cleared(required, false);
    Output zero;
    CHECK(zero.Query(0, 0) == ERROR_INSUFFICIENT_BUFFER);
    zero.Cleared(required, false);
    for (DWORD capacity : std::array<DWORD, 2>{1, required - 1}) {
        Output shortOutput;
        CHECK(shortOutput.Query(0, capacity) == ERROR_INSUFFICIENT_BUFFER);
        shortOutput.Cleared(required);
    }
    Output full;
    CHECK(full.Query(0, required) == ERROR_SUCCESS);
    CHECK(full.identity.requiredCch == required);
    CHECK(full.identity.generation == record.identityGeneration);
    CHECK(full.identity.interfaceIndex == 0 && full.identity.interfaceCount == 1);
    CHECK(std::memcmp(full.path.data(), record.lpDevicePath, required * sizeof(WCHAR)) == 0);
    CHECK(full.path[required] == L'!');
}

static void SlotAndMultiplexIdentity()
{
    Fixture fixture;
    auto& first = fixture.Add(15, 2, 4);
    const auto& second = fixture.Add(3, 0, 4, first.lpDevicePath);
    first.vendorId = 0x1234;
    first.productId = 0x5678;
    first.inputId = 99;
    first.DeviceState.dwPacketNumber = MAXDWORD;
    Output output;
    CHECK(output.Query(15) == ERROR_SUCCESS);
    CHECK(output.identity.interfaceIndex == 2 && output.identity.interfaceCount == 4);
    CHECK(output.identity.generation == first.identityGeneration);
    CHECK(output.Query(3) == ERROR_SUCCESS);
    CHECK(output.identity.interfaceIndex == 0 && output.identity.interfaceCount == 4);
    CHECK(output.identity.generation == second.identityGeneration);
    CHECK(first.identityGeneration != second.identityGeneration);
    CHECK(wcscmp(first.lpDevicePath, second.lpDevicePath) == 0);
}

static void AvailabilityAndStaleOutputs()
{
    Fixture fixture;
    auto& record = fixture.Add();
    Output live;
    CHECK(live.Query() == ERROR_SUCCESS);
    record.status = 0;
    CHECK(live.Query() == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(live.identity.requiredCch == 0 && live.identity.generation == 0);
    CHECK(live.identity.interfaceIndex == 0 && live.identity.interfaceCount == 0 && live.path[0] == 0);
    for (DWORD status : std::array<DWORD, 2>{0, DEVICE_STATUS_BUS_ACTIVE}) {
        record.status = status;
        Output missing;
        CHECK(missing.Query() == ERROR_DEVICE_NOT_CONNECTED);
        missing.Cleared();
    }
    record.status = DEVICE_STATUS_ACTIVE;
    for (HANDLE handle : {static_cast<HANDLE>(nullptr), INVALID_HANDLE_VALUE}) {
        record.hDevice = handle;
        Output missing;
        CHECK(missing.Query() == ERROR_DEVICE_NOT_CONNECTED);
        missing.Cleared();
    }
    record.hDevice = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1));
    Output missing;
    CHECK(missing.Query(14) == ERROR_DEVICE_NOT_CONNECTED);
    missing.Cleared();
    g_dwDeviceListSize = 0;
    Output empty;
    CHECK(empty.Query() == ERROR_DEVICE_NOT_CONNECTED);
    empty.Cleared();
    g_IsInitialized = FALSE;
    Output uninitialized;
    CHECK(uninitialized.Query() == ERROR_NOT_READY);
    uninitialized.Cleared();
    CHECK(ioCalls == 0 && enumerationCalls == 0 && forwardedCalls == 0);
}

static void RejectedMetadata()
{
    Fixture fixture;
    auto& record = fixture.Add();
    const auto good = record;
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        record = good;
        if (scenario == 0) record.lpDevicePath = nullptr;
        if (scenario == 1) record.identityPathCapacity = 0;
        if (scenario == 2) record.identityPathCapacity = 32769;
        if (scenario == 3) record.interfaceCount = 0;
        if (scenario == 4) record.dwUserIndex = 1;
        if (scenario == 5) record.lpDevicePath = const_cast<WCHAR*>(L"");
        Output output;
        CHECK(output.Query() == ERROR_INVALID_DATA);
        output.Cleared();
    }
    record = good;
    record.identityGeneration = 0;
    Output noGeneration;
    CHECK(noGeneration.Query() == ERROR_NOT_SUPPORTED);
    noGeneration.Cleared();
    record = good;
    XInputCore::g_pfnXInputGetState_Override = RejectForwarder;
    Output forwarded;
    CHECK(forwarded.Query() == ERROR_NOT_SUPPORTED);
    forwarded.Cleared();
    CHECK(forwardedCalls == 0);
}

static void ContentionAndRecursiveLock()
{
    Fixture fixture;
    fixture.Add();
    HANDLE held = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!held || !release) std::exit(70);
    DWORD workerWait = WAIT_FAILED;
    std::thread holder([&] {
        EnterCriticalSection(&g_csGlobalLock);
        SetEvent(held);
        workerWait = WaitForSingleObject(release, 5000);
        LeaveCriticalSection(&g_csGlobalLock);
    });
    CHECK(WaitForSingleObject(held, 5000) == WAIT_OBJECT_0);
    Output busy;
    CHECK(busy.Query() == ERROR_BUSY);
    busy.Cleared();
    SetEvent(release);
    holder.join();
    CHECK(workerWait == WAIT_OBJECT_0);
    CloseHandle(held);
    CloseHandle(release);
    EnterCriticalSection(&g_csGlobalLock);
    Output recursive;
    CHECK(recursive.Query() == ERROR_SUCCESS);
    LeaveCriticalSection(&g_csGlobalLock);
}

static void GuardPageBounds()
{
    Fixture fixture;
    auto& record = fixture.Add();
    Guarded input, output;
    constexpr DWORD count = 8;
    WCHAR* source = input.End(count);
    for (DWORD i = 0; i < count; ++i) source[i] = L'x';
    source[count - 1] = L'\0';
    record.lpDevicePath = source;
    record.identityPathCapacity = count;
    record.dwDevicePathSize = 0; // Recycle clears the legacy size.
    OPENXINPUT_DEVICE_IDENTITY_V1 identity{sizeof(identity), 0, 0, 0, 0};
    CHECK(OpenXInputGetDeviceIdentityV1(0, &identity, output.End(count), count) == ERROR_SUCCESS);
    CHECK(identity.requiredCch == count);
    CHECK(std::memcmp(source, output.End(count), count * sizeof(WCHAR)) == 0);
    source[count - 1] = L'x';
    Output unterminated;
    CHECK(unterminated.Query() == ERROR_INVALID_DATA);
    unterminated.Cleared();
    record.lpDevicePath = input.End(0); // An invalid capacity must reject before touching this page.
    record.identityPathCapacity = 0;
    Output invalid;
    CHECK(invalid.Query() == ERROR_INVALID_DATA);
    invalid.Cleared();
}

static void MaximumPath()
{
    Fixture fixture;
    std::vector<WCHAR> source(32768, L'x');
    source.back() = L'\0';
    fixture.Add(0, 0, 1, source.data());
    std::vector<WCHAR> destination(32770, L'!');
    OPENXINPUT_DEVICE_IDENTITY_V1 identity{sizeof(identity), 0, 0, 0, 0};
    CHECK(OpenXInputGetDeviceIdentityV1(0, &identity, destination.data() + 1, 32768) == ERROR_SUCCESS);
    CHECK(identity.requiredCch == 32768);
    CHECK(destination.front() == L'!' && destination.back() == L'!');
    CHECK(std::memcmp(source.data(), destination.data() + 1, 32768 * sizeof(WCHAR)) == 0);
}

static void RecycleAndGeneration()
{
    Fixture fixture;
    auto& record = fixture.Add(15, 2, 4);
    const auto old = record;
    EnterCriticalSection(&g_csGlobalLock);
    CHECK(DeviceList::SetDeviceOnPort(15, &record) == E_FAIL);
    CHECK(record.identityGeneration == old.identityGeneration);
    CHECK(DeviceList::SetDeviceOnPort(15, nullptr) == S_OK);
    XInputInternal::DeviceInfo::Recycle(&record);
    CHECK(record.lpDevicePath == old.lpDevicePath);
    CHECK(record.identityPathCapacity == old.identityPathCapacity);
    CHECK(record.identityGeneration == 0 && record.interfaceCount == 0);
    record.status = DEVICE_STATUS_ACTIVE;
    record.dwUserIndex = 1;
    record.interfaceCount = 4;
    CHECK(DeviceList::SetDeviceOnPort(15, &record) == S_OK);
    LeaveCriticalSection(&g_csGlobalLock);
    Output output;
    CHECK(output.Query(15) == ERROR_SUCCESS);
    CHECK(output.identity.requiredCch == old.identityPathCapacity);
    CHECK(output.identity.generation > old.identityGeneration);
    CHECK(output.identity.interfaceIndex == 1 && output.identity.interfaceCount == 4);
}

static void BusRemovalInvalidatesGeneration()
{
    Fixture fixture;
    auto& first = fixture.Add(3, 0, 2);
    auto& second = fixture.Add(15, 1, 2, first.lpDevicePath);
    first.dwBusIndex = second.dwBusIndex = 2;
    auto* bus = static_cast<DeviceInfo_t*>(Utilities::MemAlloc(sizeof(DeviceInfo_t)));
    if (!bus) std::exit(70);
    bus->hDevice = bus->hGuideWait = INVALID_HANDLE_VALUE;
    fixture.buses[2] = bus;
    EnterCriticalSection(&g_csGlobalLock);
    DeviceList::RemoveBusDevice(2);
    LeaveCriticalSection(&g_csGlobalLock);
    CHECK(fixture.buses[2] == nullptr);
    CHECK(first.identityGeneration == 0 && second.identityGeneration == 0);
    for (DWORD slot : {3u, 15u}) {
        Output output;
        CHECK(output.Query(slot) == ERROR_DEVICE_NOT_CONNECTED);
        output.Cleared();
    }
}

static void GenerationExhaustion()
{
    Fixture fixture;
    g_identityGeneration = MAXULONGLONG - 1;
    const auto& last = fixture.Add(0);
    CHECK(last.identityGeneration == MAXULONGLONG);
    Output valid;
    CHECK(valid.Query() == ERROR_SUCCESS);
    const auto& exhausted = fixture.Add(15);
    CHECK(exhausted.identityGeneration == 0 && g_identityGeneration == MAXULONGLONG);
    Output disabled;
    CHECK(disabled.Query(15) == ERROR_NOT_SUPPORTED);
    disabled.Cleared();
    CHECK(fixture.slots[15] == &exhausted);
    CHECK((exhausted.status & DEVICE_STATUS_ACTIVE) != 0);
    CHECK(valid.Query() == ERROR_SUCCESS); // Existing unique tokens remain valid.
}

static void Run(const char* name, void (*test)())
{
    const unsigned before = failures;
    ++cases;
    std::printf("RUN %s\n", name);
    test();
    std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
}
} // namespace identity_test

int main()
{
    using namespace identity_test;
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetBarriers();
    CHECK(!g_pfnDeviceIoControl(nullptr, 0, nullptr, 0, nullptr, 0, nullptr, nullptr));
    CHECK(g_pfnCreateDeviceInfoList(nullptr, nullptr, nullptr, nullptr, nullptr) == INVALID_HANDLE_VALUE);
    CHECK(ioCalls == 1 && enumerationCalls == 1);
    std::puts("BARRIER_CONTROLS io=1 enumeration=1");
    ioCalls = enumerationCalls = forwardedCalls = 0;
    Output uninitialized;
    CHECK(uninitialized.Query() == ERROR_NOT_READY);
    uninitialized.Cleared();
    Run("invalid arguments and exact ABI", InvalidArguments);
    Run("sizing, full copy, and no truncation", SizingAndCopy);
    Run("slot 15 and multiplexed local channels", SlotAndMultiplexIdentity);
    Run("availability and stale output clearing", AvailabilityAndStaleOutputs);
    Run("invalid metadata, zero generation, and forwarder", RejectedMetadata);
    Run("other-thread contention and recursive lock balance", ContentionAndRecursiveLock);
    Run("terminated and unterminated guard-page bounds", GuardPageBounds);
    Run("maximum path and output canaries", MaximumPath);
    Run("Recycle capacity and replacement generation", RecycleAndGeneration);
    Run("bus removal invalidates both attachments", BusRemovalInvalidatesGeneration);
    Run("generation exhaustion preserves ordinary slots", GenerationExhaustion);
    std::printf("SUMMARY cases=%u checks=%u failures=%u query_io=%u query_enumeration=%u forwarded=%u\n",
        cases, checks, failures, totalIoCalls, totalEnumerationCalls, totalForwardedCalls);
    return failures ? 1 : 0;
}
#endif
