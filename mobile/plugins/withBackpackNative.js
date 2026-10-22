/**
 * withBackpackNative — re-applies Backpack's native code on every `expo prebuild` (CNG wipes android/):
 *   native/logosstorage  Logos Storage fetch client (libstorage + JNI glue + Kotlin module), as in Scala
 *   native/installer     BackpackInstaller (PackageInstaller session, sha256, installed versions)
 * registers both RN packages, limits the build to arm64-v8a (libstorage ships for arm64 only),
 * and signs release builds with the Backpack key from ~/.gradle/gradle.properties (BACKPACK_*).
 */
const { withDangerousMod, withMainApplication, withGradleProperties, withAppBuildGradle } = require("@expo/config-plugins");
const fs = require("fs");
const path = require("path");

// BACKPACK_EMU=1 adds x86_64 (an x86_64 libstorage built for the emulator) for test builds only.
const ABIS = process.env.BACKPACK_EMU === "1" ? ["arm64-v8a", "x86_64"] : ["arm64-v8a"];
const PACKAGES = ["xyz.vpavlin.scalastorage.LogosStoragePackage", "xyz.vpavlin.backpack.installer.BackpackInstallerPackage"];

function copyDir(src, dst) {
  if (!fs.existsSync(src)) return;
  fs.mkdirSync(dst, { recursive: true });
  for (const e of fs.readdirSync(src, { withFileTypes: true })) {
    const s = path.join(src, e.name), d = path.join(dst, e.name);
    if (e.isDirectory()) copyDir(s, d); else fs.copyFileSync(s, d);
  }
}

const withFiles = (config) => withDangerousMod(config, ["android", async (cfg) => {
  const root = cfg.modRequest.projectRoot, android = cfg.modRequest.platformProjectRoot;
  for (const mod of ["logosstorage", "installer"]) copyDir(path.join(root, "native", mod, "android", "java"), path.join(android, "app/src/main/java"));
  for (const abi of ABIS) copyDir(path.join(root, "native", "logosstorage", abi), path.join(android, "app/src/main/jniLibs", abi));
  return cfg;
}]);

const withPackages = (config) => withMainApplication(config, (cfg) => {
  let src = cfg.modResults.contents;
  for (const p of PACKAGES) if (!src.includes(p)) src = src.replace(/PackageList\(this\)\.packages\.apply\s*\{/, (m) => `${m}\n          add(${p}())`);
  cfg.modResults.contents = src;
  return cfg;
});

// arm64 only, and legacy jniLibs packaging: libstorage.so is loaded with System.loadLibrary and must
// be a real file on disk, not mapped from inside the APK (Scala's withLogosDelivery does the same).
const withArm64 = (config) => withGradleProperties(config, (cfg) => {
  const set = (key, value) => { cfg.modResults = cfg.modResults.filter((p) => p.key !== key); cfg.modResults.push({ type: "property", key, value }); };
  set("reactNativeArchitectures", ABIS.join(","));
  set("expo.useLegacyPackaging", "true");
  return cfg;
});

const SIGNING = `
        release {
            if (project.hasProperty('BACKPACK_STORE_FILE')) {
                storeFile file(project.property('BACKPACK_STORE_FILE'))
                storePassword project.property('BACKPACK_STORE_PASSWORD')
                keyAlias project.property('BACKPACK_KEY_ALIAS')
                keyPassword project.property('BACKPACK_KEY_PASSWORD')
            }
            enableV1Signing true
            enableV2Signing true
        }`;
const withSigning = (config) => withAppBuildGradle(config, (cfg) => {
  let s = cfg.modResults.contents;
  if (!s.includes("BACKPACK_STORE_FILE")) {
    s = s.replace(/signingConfigs\s*\{/, (m) => m + SIGNING);
    s = s.replace(/(buildTypes\s*\{[\s\S]*?release\s*\{[\s\S]*?)signingConfig signingConfigs\.debug/, "$1signingConfig signingConfigs.release");
  }
  cfg.modResults.contents = s;
  return cfg;
});

module.exports = (config) => withSigning(withArm64(withPackages(withFiles(config))));
