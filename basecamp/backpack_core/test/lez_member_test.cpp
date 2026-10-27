// lez_member_test <wallet dir> <sequencer> <program hex> <op> <account> [devicePk b64url] [epochs]
// op: register | renew | addDevice | removeDevice | get   (BACKPACK_LEZ_LIB=/path/to/libbackpack_access_ffi.so)
#include "bp_lez.hpp"
#include <iostream>
int main(int argc, char** argv) {
    if (argc < 6) { std::cerr << "usage: lez_member_test wallet sequencer program op account [devicePk] [epochs]\n"; return 2; }
    try {
        bp::json cfg{{"wallet", argv[1]}, {"sequencer", argv[2]}, {"program", argv[3]}}, a{{"account", argv[5]}};
        if (argc > 6) a["devicePk"] = argv[6];
        if (argc > 7) a["epochs"] = std::stoull(argv[7]);
        std::cout << bp::lezMemberOp(cfg, argv[4], a).dump(1) << "\n"; return 0;
    } catch (std::exception& e) { std::cout << "ERROR " << e.what() << "\n"; return 1; }
}
