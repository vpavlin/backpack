// sign_test <unsigned.apk> <out.apk> : make a key, sign, print the cert sha256 (then: apksigner verify)
#include "bp_keys.hpp"
#include <iostream>
int main(int argc, char** argv) {
    try {
        bp::Keys keys{bp::fs::temp_directory_path() / "bp-sign-test"}; bp::fs::remove_all(keys.dir);
        bp::ApkInfo a; { bp::Zip z(argv[1]); auto m = bp::parseManifest(z.read(*z.find("AndroidManifest.xml"))); a.id = m.id; a.minSdk = m.minSdk; }
        std::string cert = keys.create(a.id, "Backpack test");
        std::cout << "pkg " << a.id << " minSdk " << a.minSdk << " cert " << cert << "\n";
        std::cout << "signed " << bp::signApk(argv[1], argv[2], keys.load(a.id)) << "\n";
        auto info = bp::readApk(argv[2]); std::cout << "readApk signer " << info.signer << "\n";
        keys.exportP12(a.id, "/tmp/claude-1000/bp-test.p12", "testpassword1");
        bp::Keys k2{bp::fs::temp_directory_path() / "bp-sign-test2"}; bp::fs::remove_all(k2.dir);
        std::cout << "reimported " << k2.importP12(a.id, "/tmp/claude-1000/bp-test.p12", "testpassword1") << "\n";
        return 0;
    } catch (std::exception& e) { std::cout << "ERROR " << e.what() << "\n"; return 1; }
}
