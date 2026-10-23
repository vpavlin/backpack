#!/usr/bin/env bash
# make.sh VERSIONCODE [SIZE_MB] — a big, installable test APK (xyz.vpavlin.backpack.bigtest) for testing
# Backpack downloads and updates: no code, one launcher-less manifest, SIZE_MB of random (incompressible)
# filler as an asset. Signed with a test key (~/keystores/bigtest-test.jks, created on first use).
# Output: ./bigtest-<vc>.apk
set -euo pipefail
VC=${1:?versionCode}; MB=${2:-60}
BT=$(ls -d ~/Android/Sdk/build-tools/*/ | sort -V | tail -1); JAR=$(ls -d ~/Android/Sdk/platforms/android-* | sort -V | tail -1)/android.jar
KS=~/keystores/bigtest-test.jks
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/assets"
cat > "$W/AndroidManifest.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="xyz.vpavlin.backpack.bigtest"
    android:versionCode="$VC" android:versionName="0.$VC">
  <uses-sdk android:minSdkVersion="24" android:targetSdkVersion="34" />
  <application android:label="Backpack Big Test $VC" android:hasCode="false" />
</manifest>
XML
head -c $((MB << 20)) /dev/urandom > "$W/assets/filler.bin"
"$BT/aapt2" link -o "$W/u.apk" -I "$JAR" --manifest "$W/AndroidManifest.xml" -A "$W/assets" -0 bin
"$BT/zipalign" -f 4 "$W/u.apk" "$W/a.apk"
if [ ! -f "$KS" ]; then
  keytool -genkeypair -keystore "$KS" -storetype pkcs12 -storepass bigtest -keypass bigtest -alias bigtest -keyalg RSA -keysize 2048 -dname "CN=Backpack big test" -validity 3650 >/dev/null 2>&1
  chmod 600 "$KS"
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:bigtest --key-pass pass:bigtest --min-sdk-version 24 --out "bigtest-$VC.apk" "$W/a.apk"
ls -la "bigtest-$VC.apk"
