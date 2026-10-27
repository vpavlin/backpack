// ./apk_test <apk>...  prints what bp_apk reads, as JSON, for comparison with aapt2/apksigner
#include "bp_apk.hpp"
#include <iostream>
int main(int argc, char** argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        try { auto a = bp::readApk(argv[i]); std::cout << bp::json{{"id", a.id}, {"versionCode", a.versionCode}, {"versionName", a.versionName}, {"label", a.label}, {"minSdk", a.minSdk}, {"abis", a.abis}, {"icon", a.iconEntry}, {"signer", a.signer}}.dump() << "\n"; }
        catch (std::exception& e) { std::cout << "{\"error\":\"" << e.what() << "\"}\n"; rc = 1; }
    }
    return rc;
}
