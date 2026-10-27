// repo_test <dataDir> <slug> <private 0|1> <apk>...  : create a repo, add APKs, publish, verify --deep.
// The CLI must then accept the same repo: node cli/backpack.mjs --repo <slug> verify --deep
#include "bp_repo.hpp"
#include <iostream>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 4) { std::cerr << "usage\n"; return 2; }
    bp::fs::path d = bp::fs::path(argv[1]) / argv[2];
    try {
        if (std::string(argv[3]) == "v") {   // verify only (a repo the CLI made)
            bp::Repo r = bp::Repo::load(d); r.log = [](const std::string& m) { std::cout << m << "\n"; };
            auto v = r.verify(true); std::cout << "verify ok=" << v["ok"] << "\n"; return v["ok"] ? 0 : 1;
        }
        bp::Repo r = bp::fs::exists(d / "repo.json") ? bp::Repo::load(d) : bp::Repo::create(d, std::string("app test ") + argv[2], std::string(argv[3]) == "1", "http://[fdb0:9afc:a5ef:f167:7ad7:68b7:aca0:b9fa]:8480");
        r.log = [](const std::string& m) { std::cout << m << "\n"; };
        for (int i = 4; i < argc; i++) r.addApk(argv[i]);
        std::cout << r.publish(false).dump() << "\n";
        auto v = r.verify(true); std::cout << "verify ok=" << v["ok"] << "\n";
        std::cout << r.summary().dump().substr(0, 300) << "\n";
        return v["ok"] ? 0 : 1;
    } catch (std::exception& e) { std::cout << "ERROR " << e.what() << "\n"; return 1; }
}
