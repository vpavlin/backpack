// g++ -std=c++17 -I../src -I<nlohmann> vectors_test.cpp -lcrypto && ./a.out <backpack repo root>
#include "bp_crypto.hpp"
#include <fstream>
#include <iostream>
using namespace bp;
static int fail = 0;
#define CHECK(c, m) do { if (!(c)) { fail++; std::cout << "FAIL " << m << "\n"; } } while (0)
int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : "../../..";
    auto rd = [](const std::string& p) { std::ifstream f(p); return json::parse(f); };
    json v = rd(root + "/cli/test/vectors.json");
    for (auto& t : v["vectors"]) {
        bool ok = true; int seq = -1;
        try { seq = open(t["env"], v["key"], t["kind"]).value("seq", -1); } catch (...) { ok = false; }
        CHECK(ok == t["ok"].get<bool>(), t["name"].get<std::string>());
        if (ok && t["ok"].get<bool>()) CHECK(seq == t["seq"].get<int>(), "seq " + t["name"].get<std::string>());
    }
    json c = rd(root + "/cli/test/crypt-vectors.json");
    CHECK(topicId(c["readKey"]) == c["topicId"].get<std::string>(), "topic id");
    CHECK(open(c["env"], c["key"], "index", c["readKey"]).value("seq", -1) == 7, "decrypt index");
    bool threw = false; try { open(c["env"], c["key"], "index", c["otherKey"]); } catch (...) { threw = true; } CHECK(threw, "wrong read key refused");
    // file vector: decrypt the CLI's bpe1 and compare sha256
    std::string b64 = c["file"]["b64"]; std::string raw;
    { BIO* b = BIO_new_mem_buf(b64.data(), (int)b64.size()); BIO* d = BIO_new(BIO_f_base64()); BIO_set_flags(d, BIO_FLAGS_BASE64_NO_NL); b = BIO_push(d, b); char buf[65536]; int n; while ((n = BIO_read(b, buf, sizeof buf)) > 0) raw.append(buf, n); BIO_free_all(b); }
    Bytes plain = decryptBuffer(Bytes(raw.begin(), raw.end()), c["file"]["key"]);
    auto d = sha256(plain.data(), plain.size());
    CHECK(hex(d.data(), 32) == c["file"]["sha256"].get<std::string>(), "file decrypt sha256");
    // our own round trips: sign/seal/open with and without a read key, files
    Key k = Key::generate(); std::string K = k.pub(), R = newKey();
    json body = {{"key", K}, {"seq", 3}, {"apps", json::array()}};
    CHECK(open(json::parse(seal("index", body, k)), K, "index")["seq"] == 3, "plain round trip");
    CHECK(open(json::parse(seal("pointer", body, k, R)), K, "pointer", R)["seq"] == 3, "private round trip");
    Key k2 = Key::fromPem(k.pem()); CHECK(k2.pub() == K, "pem round trip");
    { std::ofstream f("/tmp/bp-vt.bin", std::ios::binary); for (int i = 0; i < (1 << 20) + 77; i++) f.put((char)(i * 31)); }
    EncResult e = encryptFile("/tmp/bp-vt.bin", "/tmp/bp-vt.bpe");
    std::ifstream f("/tmp/bp-vt.bpe", std::ios::binary); Bytes ct((std::istreambuf_iterator<char>(f)), {});
    CHECK(ct.size() == e.size, "enc size"); CHECK(decryptBuffer(ct, e.key).size() == (1u << 20) + 77, "enc round trip");
    std::cout << (fail ? "FAILED " : "vectors: all ok ") << fail << "\n";
    return fail ? 1 : 0;
}
