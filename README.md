# NT HAL extensions
HAL extensions for ARMv7 NT on the Samsung Galaxy Note (GT-N7000, Exynos4210). Forked from [HTC-Leo-Revival-Project/NtHalExts](https://github.com/HTC-Leo-Revival-Project/NtHalExts); headers and libraries from the (Win8 M2-era) NT DDK are reproduced here for ease of building.

You need correct `CSRT` ACPI tables to load these (Exynos441xPkg provides them).

## halextmct
HAL extension for the Exynos4 Multi-Core Timer (MCT). The Cortex-A9 has no ARM generic timer, so the HAL gets the MCT instead:

- counter: the free-running 64-bit global counter (`G_CNT`, 24 MHz)
- interrupt timer: global comparator 0 (`G_COMP0`, SPI 57 = GSI 89), one-shot and pseudo-periodic

Only one processor is covered, like Linux's `mct-comp` clock event on CPU0.

Loaded for the CSRT resource group with VendorId `SAMS`, DeviceId `0x4210` (`OSExtensionDatabase\VEN_SAMS&DEV_4210`). Tested on Windows RT 8.1 (9600), booting `boot.wim` from a RAM disk.

### Building (Linux, no Visual Studio)
Needs clang, lld-link and osslsigncode, plus the Windows SDK and MSVC CRT headers, e.g. from [xwin](https://github.com/Jake-Shadle/xwin):

    xwin --accept-license --arch x86_64 splat --output ~/s2/winsdk/splat
    halextmct/build.sh          # WINSDK=<splat dir> to override

Only headers are used (the current SDK has no ARM32 libraries, and none are needed). `build.sh` test-signs the DLL with `~/s2/winsdk/testsign/testsign.{crt,key}` (`SIGN=` to override); the certificate needs the Code Signing and HAL Extension (`1.3.6.1.4.1.311.61.5.1`) EKUs.

`-DLOAD_TEST=1` makes the extension bugcheck with `0x4D435421` ("MCT!", parameters VendorId, DeviceId, handle) as soon as the HAL calls it, to check that it is found and loaded at all.

### Installing
Into a mounted image (e.g. `boot.wim`):

    dism /Image:<mount> /Add-Driver /Driver:halextmct.inf /ForceUnsigned

and `bcdedit /set {default} testsigning on` in the BCD that boots it. `halextmct.reg` does the same registry change by hand (e.g. `hivexregedit --merge`).
