# Hello World under Multimedia — confirmed on vehicle

The user requested an actual selectable iDrive entry. The plugin files are now
installed and byte-verified under /var/opt/hmi/data/rw/Plugins/HelloNbt.
After a normal restart, the user confirmed on 2026-11-05 that Hello NBT appears
under Multimedia and selecting it opens "Hello World from QNX x86!".
No forced HmiMain3D restart was performed by the agent.

Read-only inspection on 2026-11-05 confirmed original NBT x86, HMI version
NBT_3D1407_Release 3D1407-490-1407N Build 103. Native plugin examples include
TerminalUIApp and SecurityMenu. Private snapshots remain in .local/hmi-analysis,
outside Git. They must not be redistributed.

## ABI findings from this vehicle

- libSecurityMenu.so exports a 20-byte pluginAppInfo record, not the newer
  24-byte structure containing an RHMI version field.
- Its factory takes dispatcher, name, parameters and config references.
- libAppFramework.A.so exports PluginApp(dispatcher, BasicString<char>,
  parameters, config), without the extra version arguments of the Evo sample.
- The base onInit implementation returns true; onFinalize is empty.
- Plugin text archives contain UTF-8 language.txt members with id=text lines.
- A localhost listener exists on port 4004; its protocol/authentication has not
  been tested. It is not a verified working alternative to plugin loading.

## Local prototype

src/idrive contains original code for a base PluginApp plus a static RHMI page.
The verified entry is Hello NBT under Multimedia and opens a text-only page.
It does not start external processes or access vehicle services.

The ABI declarations and oversized opaque allocation are a prototype, not a
complete SDK. Isolated linkage checks and the actual menu opening passed.
Destruction/unloading and repeated lifecycle stress have not been tested;
this result does not establish a complete framework ABI for future callbacks.

## Build and isolated loading checks completed

- VM networking/SSH restored; compiled using qcc 4.4.2 gcc_ntox86_cpp-ne,
  -shared -fPIC -fno-exceptions -fno-rtti and -Wl,-z,defs.
- ELF32/i386; NEEDED: libAppFramework.A.so, libCoreFramework.A.so,
  libcpp-ne.so.4, libc.so.3. The no-exceptions runtime matches the NBT libraries.
- Base PluginApp vtable has no pure-virtual callbacks. Original pluginInfo
  factory/destroy offsets are 12/16, matching our 20-byte record.
- check_plugin.c successfully used dlopen(RTLD_NOW), validated metadata and
  dlclose in the VM and in a SEPARATE process on the real NBT. It deliberately
  did not call the factory or interact with HmiMain3D.
- Both temporary binaries were roundtrip SHA256-verified before execution and
  their RAM test directory was removed afterwards.
- All five installed files were independently downloaded and SHA256-verified.
- Existing HmiMain3D PID 139314 remained running after installation.
- Existing system files, boot configuration and mount flags were not changed.

Artifacts: build/idrive-hello-prototype/package/HelloNbt and package-sha256.json.
Private install/rollback helpers: .local/install_hello_nbt.py and
.local/remove_hello_nbt.py. Rollback removes only the five named plugin files;
inspect the directory before removing the empty directory. A running loaded
plugin may require a subsequent normal HMI restart to unload.

The installed plugin has autoStart=yes so the HMI can register the entry on
its next plugin discovery. It contains no vehicle-service operations or
external command execution, only a static text page.

## Vehicle validation

The user's visual confirmation establishes resource loading, factory
initialization, menu registration and navigation to the static page on this
firmware. Loading from the writable Plugins directory is now demonstrated.
This completes the requested Hello World menu test. Android Auto transport,
video, audio and controller integration are separate, still unverified work;
see ANDROID-AUTO.md.

Research references (examples, not proof of original-NBT compatibility):
- https://bimmergestalt.github.io/BMWConnectedAnalysis/rhmi-resources/
- https://github.com/BimmerGestalt/IDriveConnectKit
- https://github.com/Sq00ky/Supra-iDrive6-Android-Auto/tree/main/milestone-7/rhmiapp
