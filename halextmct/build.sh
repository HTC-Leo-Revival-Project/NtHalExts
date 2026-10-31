#!/bin/sh
# Build halextmct.dll on Linux with clang + lld-link (no Visual Studio).
#
# Needs the Windows SDK + MSVC CRT headers, e.g. from xwin:
#   xwin --accept-license --arch x86_64 splat --output ~/s2/winsdk/splat
# (headers only; the SDK no longer ships ARM32 libraries, and none are needed:
# the Win8 DDK headers and ntoskrnl.lib are in this directory).
set -e
cd "$(dirname "$0")"

WINSDK=${WINSDK:-$HOME/s2/winsdk/splat}
OUT=${OUT:-build}
mkdir -p "$OUT"

CFLAGS="--target=thumbv7-pc-windows-msvc -fms-extensions -fms-compatibility
  -O1 -fno-stack-protector -I.
  -isystem $WINSDK/crt/include -isystem $WINSDK/sdk/include/shared
  -isystem $WINSDK/sdk/include/ucrt -isystem $WINSDK/sdk/include/um
  -D_ARM_=1 -D_KERNEL_MODE -D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1
  -DNTDDI_VERSION=0x06030000 -D_WIN32_WINNT=0x0603
  -Wno-microsoft -Wno-pragma-pack -Wno-ignored-attributes -Wno-unknown-pragmas -Wno-visibility -Wno-ignored-pragma-intrinsic"

clang $CFLAGS -c halextmct.c -o "$OUT/halextmct.obj"
clang $CFLAGS -c halextapi.c -o "$OUT/halextapi.obj"

lld-link /DRIVER /DLL /RELEASE /SUBSYSTEM:NATIVE /MACHINE:ARM /ENTRY:HalExtensionInit \
  /NODEFAULTLIB /OUT:"$OUT/halextmct.dll" \
  "$OUT/halextmct.obj" "$OUT/halextapi.obj" ntoskrnl.lib

# Test-sign (needs bcdedit /set testsigning on).  The certificate needs the
# code signing and HAL extension (1.3.6.1.4.1.311.61.5.1) EKUs.
SIGN=${SIGN:-$HOME/s2/winsdk/testsign/testsign}
if [ -f "$SIGN.key" ]; then
  osslsigncode sign -certs "$SIGN.crt" -key "$SIGN.key" -h sha256 \
    -in "$OUT/halextmct.dll" -out "$OUT/halextmct.signed.dll" > /dev/null
  mv "$OUT/halextmct.signed.dll" "$OUT/halextmct.dll"
else
  echo "warning: $SIGN.key not found, halextmct.dll is unsigned"
fi

cp halextmct.inf "$OUT/"
echo "$OUT/halextmct.dll $OUT/halextmct.inf"
