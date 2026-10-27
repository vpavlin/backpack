// lez_test <dataDir> <slug> <wallet dir> <sequencer url> <program id hex> : read LEZ memberships into a member repo
// (needs libbackpack_access_ffi.so: BACKPACK_LEZ_LIB=/path/to/it)
#include "bp_repo.hpp"
#include <iostream>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 6) { std::cerr << "usage: lez_test dataDir slug wallet sequencer program\n"; return 2; }
    try {
        bp::Repo r = bp::Repo::load(bp::fs::path(argv[1]) / argv[2]);
        r.log = [](const std::string& m) { std::cout << m << "\n"; };
        r.setLez(bp::json{{"wallet", argv[3]}, {"sequencer", argv[4]}, {"program", argv[5]}});
        bp::json res = r.syncLez();
        std::cout << res.dump(1) << "\n";
        for (auto& lm : r.s["members"]["lezMembers"]) std::cout << "member " << lm.value("owner", "") << " paidThrough " << lm.value("paidThroughMs", (int64_t)0) << " devices " << lm["devices"].dump() << "\n";
        std::cout << "active member keys: " << r.memberPks().size() << "\n";
        return 0;
    } catch (std::exception& e) { std::cout << "ERROR " << e.what() << "\n"; return 1; }
}
