// fdroid_test <dataDir> <slug> <repoUrl> <fingerprint> [only]  : import into a NEW test repo, then publish + verify
#include "bp_fdroid.hpp"
#include <iostream>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    bp::fs::path d = bp::fs::path(argv[1]) / argv[2];
    try {
        bp::Repo r = bp::fs::exists(d / "repo.json") ? bp::Repo::load(d) : bp::Repo::create(d, std::string("import test ") + argv[2], false, "");
        r.log = [](const std::string& m) { std::cout << m << "\n"; };
        bp::ImportOpts o; o.url = argv[3]; o.fingerprint = argv[4]; if (argc > 5) o.only.insert(argv[5]);
        std::cout << bp::importFdroid(r, o).dump() << "\n";
        std::cout << r.publish(false).dump() << "\n";
        auto v = r.verify(true); std::cout << "verify ok=" << v["ok"] << "\n"; return v["ok"] ? 0 : 1;
    } catch (std::exception& e) { std::cout << "ERROR " << e.what() << "\n"; return 1; }
}
