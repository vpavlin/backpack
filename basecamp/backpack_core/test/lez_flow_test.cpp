// lez_flow_test <wallet dir> <sequencer> <program hex> <publisher> <member1> <member2>
// The whole membership flow through bp_lez.hpp, against a live chain, on a fresh config id:
// config create/update, register x2, publisher read (lezRead), renew, add/remove device, refusals.
// (BACKPACK_LEZ_LIB=/path/to/libbackpack_access_ffi.so; the wallet must hold all three accounts)
#include "bp_lez.hpp"
#include <openssl/rand.h>
#include <chrono>
#include <iostream>
using namespace bp;
static int fails = 0;
static void check(bool ok, const std::string& what) { std::cout << (ok ? "ok   " : "FAIL ") << what << "\n"; if (!ok) fails++; }
static std::string rnd32b64() { Bytes b(32); RAND_bytes(b.data(), 32); return b64url(b); }
static std::string rnd32hex() { Bytes b(32); RAND_bytes(b.data(), 32); std::string o; static const char* H = "0123456789abcdef"; for (auto c : b) { o += H[c >> 4]; o += H[c & 15]; } return o; }
static const LezMember* find(const LezSnapshot& s, const std::string& owner) { for (auto& m : s.members) if (accountToHex(m.owner) == accountToHex(owner)) return &m; return nullptr; }
static bool has(const std::vector<std::string>& v, const std::string& x) { for (auto& e : v) if (e == x) return true; return false; }
int main(int argc, char** argv) {
    if (argc < 7) { std::cerr << "usage: lez_flow_test wallet sequencer program publisher member1 member2\n"; return 2; }
    std::string pub = argv[4], m1 = argv[5], m2 = argv[6];
    json cfg{{"wallet", argv[1]}, {"sequencer", argv[2]}, {"program", argv[3]}, {"config", rnd32hex()}};
    auto log = [](const std::string& m) { std::cout << "     " << m << "\n"; };
    std::string d1 = rnd32b64(), d2 = rnd32b64(), d3 = rnd32b64();
    const int64_t period = 60000;
    try {
        std::cout << "config " << cfg["config"].get<std::string>() << "\n";
        check(lezConfigOf(cfg).is_null(), "a fresh config id has no config");
        lezEnsureConfig(cfg, pub, "1", period, log);
        json c = lezConfigOf(cfg);
        check(!c.is_null(), "config created");
        check(c.value("admin_hex", std::string()) == accountToHex(pub), "admin is the publisher");
        check(lezPrice(c) == "1", "price is 1 (got " + lezPrice(c) + ")");
        check(c.value("epoch_len_ms", (int64_t)0) == period, "period is one minute");
        check(lezEnsureConfig(cfg, pub, "1", period, log) == c, "saving the same terms again sends nothing");

        lezMemberOp(cfg, "register", json{{"account", m1}, {"devicePk", d1}, {"epochs", 2}});
        lezMemberOp(cfg, "register", json{{"account", m2}, {"devicePk", d2}, {"epochs", 1}});
        LezSnapshot s = lezRead(cfg);
        check(s.members.size() == 2, "publisher reads 2 members (got " + std::to_string(s.members.size()) + ")");
        const LezMember* a = find(s, m1); const LezMember* b = find(s, m2);
        check(a && has(a->devices, d1), "member1 has its device");
        check(b && has(b->devices, d2), "member2 has its device");
        int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        check(a && a->paidThroughMs > now, "member1 is paid up");
        check(a && b && a->paidThroughMs > b->paidThroughMs, "2 periods last longer than 1");
        int64_t before = a ? a->paidThroughMs : 0;

        lezMemberOp(cfg, "renew", json{{"account", m1}, {"epochs", 1}});
        s = lezRead(cfg); a = find(s, m1);
        check(a && a->paidThroughMs >= before + period, "renew extends member1 by a period");

        lezMemberOp(cfg, "addDevice", json{{"account", m1}, {"devicePk", d3}});
        a = find(s = lezRead(cfg), m1);
        check(a && has(a->devices, d1) && has(a->devices, d3), "member1 has both devices after add");
        lezMemberOp(cfg, "removeDevice", json{{"account", m1}, {"devicePk", d1}});
        a = find(s = lezRead(cfg), m1);
        check(a && !has(a->devices, d1) && has(a->devices, d3), "only the removed device is gone");
        json g = lezMemberOp(cfg, "get", json{{"account", m1}});
        check(g.contains("devices") && g["devices"].size() == 1 && g["devices"][0] == d3, "member get agrees (" + g.dump().substr(0, 120) + ")");

        bool refused = false;
        try { lezMemberOp(cfg, "register", json{{"account", m1}, {"devicePk", rnd32b64()}, {"epochs", 1}}); } catch (std::exception& e) { refused = true; log(std::string("refused: ") + e.what()); }
        check(refused, "a second registration of the same member is refused");
        refused = false;
        try { lezEnsureConfig(cfg, m2, "5", period, log); } catch (std::exception& e) { refused = true; log(std::string("refused: ") + e.what()); }
        check(refused, "another account can't change the terms");

        lezEnsureConfig(cfg, pub, "2", 2 * period, log);
        c = lezConfigOf(cfg);
        check(lezPrice(c) == "2" && c.value("epoch_len_ms", (int64_t)0) == 2 * period, "publisher updates price and period");
        check(lezRead(cfg).members.size() == 2, "members survive the update");
    } catch (std::exception& e) { std::cout << "FAIL exception: " << e.what() << "\n"; fails++; }
    std::cout << (fails ? std::to_string(fails) + " FAILED" : "all passed") << "\n";
    return fails ? 1 : 0;
}
