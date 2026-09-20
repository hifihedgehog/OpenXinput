# OpenXInput (PadForge fork)

Fork of [Nemirtingas/OpenXInput](https://github.com/Nemirtingas/OpenXInput) used by [PadForge](https://github.com/hifihedgehog/PadForge) to filter out HIDMaestro virtual controllers from PadForge's own in-process XInput view, without affecting other processes.

## What changed from upstream

One addition in [`src/OpenXinput.cpp`](src/OpenXinput.cpp):

- `IsHidMaestroInterface(DevicePath)` is a self-contained PnP walk (cfgmgr32 + devpkey, no external deps). Fast-path substring-matches the interface symlink for `HIDMAESTRO` / `HMCOMPANION`, then falls back to a depth-4 parent walk checking `DEVPKEY_Device_HardwareIds` for `HIDMAESTRO`.
- One call site in `EnumerateXInputDevices`: right after `GetDeviceInterfaceDetail` returns a `DevicePath`, before `OpenDevice`. HM interfaces are skipped before registration, so they never occupy a user-index. Surviving non-HM interfaces pack `0..N-1` naturally via the existing enumeration logic.

No other changes beyond upstream `OpenXinput1_4`.

## Why

HIDMaestro (PadForge's virtual-controller backend) spoofs real VID/PIDs by design, so VID/PID can't be used as a classifier. The PnP-ancestor walk for the literal string `HIDMaestro` is the structural signal that's independent of device identity spoofing.

PadForge ships this DLL embedded in its single-file exe; at startup it calls `SetDllDirectory` on the single-file extraction directory so SDL3's `LoadLibrary("xinput1_4.dll")` picks up this fork instead of `System32\xinput1_4.dll`. Only PadForge's process is affected; every other XInput consumer on the system keeps seeing the virtuals.

## Building

PadForge bundles this library for x64 and ARM64. Both builds use the Visual Studio generator and link the static C runtime, so the DLL depends on nothing beyond Windows itself.

```
cmake -S . -B build -A x64
cmake --build build --config Release --target Xinput1_4

cmake -S . -B build-arm64 -A ARM64
cmake --build build-arm64 --config Release --target Xinput1_4
```

Each build writes `bin/Release/Xinput1_4.dll` under its build directory. PadForge takes that file as `Resources/OpenXInput/<arch>/xinput1_4.dll`.

Ship only that file. The build also produces a `devobj.dll`, which is a link-time stub whose exports return a fill value. Placed beside an application, it replaces the system library for the whole process.

The ARM64 build cross-compiles on an x64 machine that has the MSVC ARM64 build tools. Its export table matches the x64 build, including the ordinal-only entries 100 to 104, 108, and 109 that SDL resolves. It has not run on ARM64 hardware.

## Licensing

Upstream ships only a Microsoft trademark disclaimer, not an OSS license. This fork inherits that state and adds no new grant. Use at your own risk. See upstream's own disclaimer below.

---

# OpenXInput
A re-implementation of the XInput userspace library for Windows that allows for use of more than 4 XInput devices, while maintaining compatibility with standard XInput.

## Purpose
Standard XInput can only handle 4 devices, but the underlying XUSB driver can handle more. By using OpenXInput, you can bypass this limitation and set your own controller limit at compile-time.

## Usage
See the [wiki](../../wiki).

## Disclaimer
This code is based on Xinput binaries and PDBs and might not be suitable for your business as its standing in the reverse engineering area and is very different in many countries.
For a free to use Windows Xinput like library, you can take a look at my [gamepad library](https://github.com/Nemirtingas/gamepad/blob/d4dd203c6bce54521ac73e8d8fd7653ed2056850/src/gamepad.cpp#L354-L714) or switch to the new [Windows Gaming Input (WGI) APIs](https://learn.microsoft.com/en-us/uwp/api/windows.gaming.input):
