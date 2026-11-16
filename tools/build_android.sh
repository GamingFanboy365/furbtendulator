#!/bin/bash
# build_android.sh -- build Furbtendulator for Android: bin-android/Furbtendulator.apk
# for phones, tablets, TVs and Chromebooks (64- and 32-bit ARM and x86,
# Android 4.4 or newer).
#
#     tools/build_android.sh [--accept-sdk-license] [WORKDIR]   (default WORKDIR: ./build-android)
#
# Needs the Android SDK with two NDKs, in its ndk/ folder or given as
# ANDROID_NDK_HOME and FURB_NDK32: r27 builds the 64-bit libraries (64-bit
# devices all have Android 5.0 or newer), and r25c the 32-bit ones, for
# Android 4.4 (API 19), which newer NDKs no longer build for.  ANDROID_HOME
# (or ANDROID_SDK_ROOT) names the SDK.
# Without an SDK, --accept-sdk-license downloads Google's command-line tools
# into WORKDIR and installs the platform, build tools and both NDKs there, which
# means accepting the Android SDK license (https://developer.android.com/studio/terms)
# -- only pass it if you do.  Also needs git, cmake, ninja, python3, a JDK 17
# or newer, and network access to dl.google.com, maven.google.com and github.com.
#
# Steps: SDL 2 (fetched with git) is built for each ABI with the NDK;
# build.py --android builds the emulator, the front end (android/native) and
# the mapper packs; Gradle packages them with the Java activity
# (android/app) and SDL's Java half into a signed APK.  FURB_ABIS limits the
# ABIs (e.g. FURB_ABIS=arm64-v8a for a quicker test build).
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
ACCEPT=0
if [ "${1:-}" = --accept-sdk-license ]; then ACCEPT=1; shift; fi
WORK=$(realpath -m "${1:-$HERE/build-android}")
ABIS=${FURB_ABIS:-arm64-v8a armeabi-v7a x86 x86_64}
SDL_TAG=release-2.32.10
NDK_VERSION=27.2.12479018	# 64-bit ABIs, API 21
NDK32_VERSION=25.2.9519653	# 32-bit ABIs, API 19 (r25c: the last NDK for API 19)
PLATFORM=35
BUILD_TOOLS=35.0.0
JOBS=$(nproc 2>/dev/null || echo 4)
mkdir -p "$WORK"

# ---- the SDK and NDK
SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}
if [ -z "$SDK" ]; then
	SDK=$WORK/sdk
	if [ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then
		if [ $ACCEPT != 1 ]; then
			echo "build_android.sh: no Android SDK (set ANDROID_HOME), or pass --accept-sdk-license to download one" >&2
			exit 1
		fi
		echo "fetching the Android command-line tools ..."
		curl -fL -o "$WORK/cmdline-tools.zip" https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
		rm -rf "$SDK/cmdline-tools" && mkdir -p "$SDK/cmdline-tools"
		(cd "$SDK/cmdline-tools" && unzip -q "$WORK/cmdline-tools.zip" && mv cmdline-tools latest)
	fi
fi
SDKMANAGER=$SDK/cmdline-tools/latest/bin/sdkmanager
if [ -x "$SDKMANAGER" ] && [ $ACCEPT = 1 ]; then
	yes | "$SDKMANAGER" --sdk_root="$SDK" --licenses > /dev/null || true
	"$SDKMANAGER" --sdk_root="$SDK" "platforms;android-$PLATFORM" "build-tools;$BUILD_TOOLS" "ndk;$NDK_VERSION" \
		"ndk;$NDK32_VERSION" "platform-tools"
fi
NDK64=${ANDROID_NDK_HOME:-$SDK/ndk/$NDK_VERSION}
NDK32=${FURB_NDK32:-$SDK/ndk/$NDK32_VERSION}
for n in "$NDK64:$NDK_VERSION:ANDROID_NDK_HOME" "$NDK32:$NDK32_VERSION:FURB_NDK32"; do
	IFS=: read -r dir version var <<< "$n"
	if [ ! -d "$dir/toolchains/llvm" ]; then
		echo "build_android.sh: no NDK at $dir (set $var, or install it with sdkmanager \"ndk;$version\")" >&2
		exit 1
	fi
done
echo "SDK $SDK"
echo "NDK $NDK64 (64-bit), $NDK32 (32-bit)"

# ---- SDL 2
if [ ! -d "$WORK/SDL2/.git" ]; then
	git clone --depth 1 -b "$SDL_TAG" https://github.com/libsdl-org/SDL "$WORK/SDL2"
fi

OUT=$HERE/android/native-out
rm -rf "$OUT" && mkdir -p "$OUT/jniLibs" "$OUT/assets/furb"
for abi in $ABIS; do
	echo "==== $abi"
	case $abi in
	arm64-v8a|x86_64) NDK=$NDK64; API=21 ;;
	*) NDK=$NDK32; API=19 ;;
	esac
	cmake -S "$WORK/SDL2" -B "$WORK/sdl-$abi" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$abi" \
		-DANDROID_PLATFORM=android-$API -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
		-DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
		-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF > "$WORK/sdl-$abi.log"
	cmake --build "$WORK/sdl-$abi" -j "$JOBS" >> "$WORK/sdl-$abi.log"
	python3 "$HERE/build.py" --android "$abi" --ndk "$NDK" --api $API --sdl "$WORK/SDL2" --sdl-lib "$WORK/sdl-$abi" \
		--build "$WORK/build-$abi" -j "$JOBS"
	mkdir -p "$OUT/jniLibs/$abi"
	cp "$WORK/build-$abi"/android/*.so "$OUT/jniLibs/$abi/"
	# (without the debug information the NDK's CMake toolchain builds SDL with)
	"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-debug \
		-o "$OUT/jniLibs/$abi/libSDL2.so" "$WORK/sdl-$abi/libSDL2.so"
	DATA=$WORK/build-$abi
done
# Furbtendulator's data files (the same for every ABI)
cp "$DATA"/*.cfg "$OUT/assets/furb/"
cp -r "$DATA/BIOS" "$DATA/samples" "$OUT/assets/furb/"
# SDL's Java half, which must match the libSDL2.so built above
cp -r "$WORK/SDL2/android-project/app/src/main/java" "$OUT/sdl-java"

# ---- the signing key: FURB_KEYSTORE, or one made here once (keep it: updates need the same key)
if [ -z "${FURB_KEYSTORE:-}" ] && [ ! -f "$HERE/android/furb-release.keystore" ]; then
	keytool -genkeypair -keystore "$HERE/android/furb-release.keystore" -alias furb -keyalg RSA -keysize 2048 \
		-validity 10000 -storepass furbtendulator -keypass furbtendulator -dname "CN=Furbtendulator" -noprompt
	echo "made a signing key: android/furb-release.keystore (password furbtendulator) -- keep it"
fi

# ---- the APK
echo "sdk.dir=$SDK" > "$HERE/android/local.properties"
# (one worker, and a few tries: Maven Central answers bursts of parallel
# downloads with 429 Too Many Requests; what was fetched stays cached)
for try in 1 2 3 4; do
	(cd "$HERE/android" && ANDROID_HOME="$SDK" ./gradlew --no-daemon --max-workers=1 assembleRelease) && break
	[ $try = 4 ] && exit 1
	echo "Gradle failed; trying again in 20 s"; sleep 20
done
mkdir -p "$HERE/bin-android"
cp "$HERE/android/app/build/outputs/apk/release/app-release.apk" "$HERE/bin-android/Furbtendulator.apk"
echo "built $HERE/bin-android/Furbtendulator.apk"
