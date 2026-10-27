// members_repo_test <dataDir> <slug> <apk> <member link>... : a member repo end to end (ADR 0013)
#include "bp_repo.hpp"
#include <iostream>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    bp::fs::path d = bp::fs::path(argv[1]) / argv[2];
    try {
        bp::Repo r = bp::fs::exists(d / "repo.json") ? bp::Repo::load(d) : bp::Repo::create(d, std::string("members test ") + argv[2], false, "http://[fdb0:9afc:a5ef:f167:7ad7:68b7:aca0:b9fa]:8480", true);
        r.log = [](const std::string& m) { std::cout << m << "\n"; };
        if (std::string(argv[3]) != "-") r.addApk(argv[3]);
        for (int i = 4; i < argc; i++) { try { r.addMember(argv[i]); } catch (std::exception& e) { std::cout << "member: " << e.what() << "\n"; } }
        std::cout << r.publish(false).dump() << "\n";
        auto v = r.verify(true); std::cout << "verify ok=" << v["ok"] << "\n";
        std::cout << "link " << r.s.value("lastLink", "").substr(0, 60) << "... m=1? " << (r.s.value("lastLink", "").find("&m=1") != std::string::npos) << " x=? " << (r.s.value("lastLink", "").find("&x=") != std::string::npos) << "\n";
        return v["ok"] ? 0 : 1;
    } catch (std::exception& e) { std::cout << "ERROR " << e.what() << "\n"; return 1; }
}
