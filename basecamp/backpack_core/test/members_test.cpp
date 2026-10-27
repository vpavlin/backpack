// members_test [--write <vectors.json>] : wrap/unwrap round trip, AAD binding, member links; optionally
// writes fixed vectors for the phone (mobile/test/member-vectors.test.ts).
#include "bp_members.hpp"
#include <iostream>
using namespace bp;
int main(int argc, char** argv) {
    int fail = 0; auto check = [&](bool c, const char* m) { if (!c) { fail++; std::cout << "FAIL " << m << "\n"; } };
    Bytes dsk = randomBytes(32); std::string dpk = b64url(x25519Public(dsk));
    std::string repo = b64url(randomBytes(32)), rk = newKey();
    std::string blob = wrapKey(rk, dpk, repo, 7);
    check(unb64url(blob).size() == 92, "blob size");
    check(unwrapKey(blob, dsk, repo, 7) == rk, "round trip");
    bool t = false; try { unwrapKey(blob, dsk, repo, 8); } catch (...) { t = true; } check(t, "other epoch refused");
    t = false; try { unwrapKey(blob, dsk, b64url(randomBytes(32)), 7); } catch (...) { t = true; } check(t, "other repo refused");
    t = false; try { unwrapKey(blob, randomBytes(32), repo, 7); } catch (...) { t = true; } check(t, "other device refused");
    MemberLink m = parseMemberLink("backpack://member?pk=" + dpk + "&n=My%20phone"); check(m.pk == dpk && m.name == "My phone", "member link");
    t = false; try { parseMemberLink("backpack://member?pk=abc"); } catch (...) { t = true; } check(t, "bad member link refused");
    if (argc > 2 && std::string(argv[1]) == "--write") {
        std::ofstream(argv[2]) << json{{"deviceSk", b64url(dsk)}, {"devicePk", dpk}, {"repoKey", repo}, {"epoch", 7}, {"blob", blob}, {"readKey", rk}}.dump(1);
        std::cout << "wrote " << argv[2] << "\n";
    }
    std::cout << (fail ? "FAILED" : "members: all ok") << "\n"; return fail ? 1 : 0;
}
