#pragma once
// bp_members.hpp - member repos (ADR 0013): wrap an epoch's read key for each member device (X25519 +
// HKDF-SHA256 + AES-256-GCM, AAD = repo key | device pk | epoch u64 LE), and the signed key bundle.
#include "bp_crypto.hpp"
#include <openssl/kdf.h>

namespace bp {
inline Bytes x25519Public(const Bytes& sk) {
    EVP_PKEY* k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, sk.data(), 32); if (!k) throw Err("bad x25519 key");
    Bytes pk(32); size_t n = 32; EVP_PKEY_get_raw_public_key(k, pk.data(), &n); EVP_PKEY_free(k); return pk;
}
inline Bytes x25519Shared(const Bytes& sk, const Bytes& peerPk) {
    EVP_PKEY* a = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, sk.data(), 32);
    EVP_PKEY* b = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peerPk.data(), 32);
    if (!a || !b) { if (a) EVP_PKEY_free(a); if (b) EVP_PKEY_free(b); throw Err("bad x25519 key"); }
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new(a, nullptr); Bytes s(32); size_t n = 32;
    bool ok = EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_derive_set_peer(c, b) == 1 && EVP_PKEY_derive(c, s.data(), &n) == 1;
    EVP_PKEY_CTX_free(c); EVP_PKEY_free(a); EVP_PKEY_free(b);
    if (!ok) throw Err("x25519 failed"); return s;
}
inline Bytes hkdf256(const Bytes& ikm, const std::string& info) {
    EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr); Bytes out(32); size_t n = 32;
    bool ok = EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_CTX_set_hkdf_md(c, EVP_sha256()) == 1 &&
              EVP_PKEY_CTX_set1_hkdf_salt(c, (const unsigned char*)"", 0) == 1 && EVP_PKEY_CTX_set1_hkdf_key(c, ikm.data(), (int)ikm.size()) == 1 &&
              EVP_PKEY_CTX_add1_hkdf_info(c, (const unsigned char*)info.data(), (int)info.size()) == 1 && EVP_PKEY_derive(c, out.data(), &n) == 1;
    EVP_PKEY_CTX_free(c); if (!ok) throw Err("hkdf failed"); return out;
}
inline Bytes wrapAad(const std::string& repoKey, const Bytes& devicePk, uint64_t epoch) {
    Bytes a = readKeyBytes(repoKey); a.insert(a.end(), devicePk.begin(), devicePk.end());
    for (int i = 0; i < 8; i++) a.push_back(epoch >> (8 * i) & 255);
    return a;
}
// blob = ephemeral pk (32) | nonce (12) | ct+tag (48)
inline std::string wrapKey(const std::string& readKey, const std::string& devicePkB64, const std::string& repoKey, uint64_t epoch) {
    Bytes dpk = readKeyBytes(devicePkB64), esk = randomBytes(32), epk = x25519Public(esk);
    Bytes k = hkdf256(x25519Shared(esk, dpk), "backpack/keywrap/1"), nonce = randomBytes(12), rk = readKeyBytes(readKey);
    Bytes ct = gcmEnc(k, nonce, rk.data(), rk.size(), wrapAad(repoKey, dpk, epoch));
    Bytes blob = epk; blob.insert(blob.end(), nonce.begin(), nonce.end()); blob.insert(blob.end(), ct.begin(), ct.end());
    return b64url(blob);
}
inline std::string unwrapKey(const std::string& blobB64, const Bytes& deviceSk, const std::string& repoKey, uint64_t epoch) {
    Bytes blob = unb64url(blobB64); if (blob.size() != 92) throw Err("bad key blob");
    Bytes dpk = x25519Public(deviceSk), epk(blob.begin(), blob.begin() + 32), nonce(blob.begin() + 32, blob.begin() + 44), out;
    Bytes k = hkdf256(x25519Shared(deviceSk, epk), "backpack/keywrap/1");
    if (!gcmDec(k, nonce, blob.data() + 44, 48, wrapAad(repoKey, dpk, epoch), out)) throw Err("key blob doesn't open");
    return b64url(out);
}
// A member link from a phone: backpack://member?pk=<b64url 32>&n=<name>
struct MemberLink { std::string pk, name; };
inline MemberLink parseMemberLink(const std::string& link) {
    std::string t = link; while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back(); while (!t.empty() && isspace((unsigned char)t.front())) t.erase(0, 1);
    if (t.rfind("backpack://member?", 0) != 0) throw Err("not a Backpack member code (backpack://member?...)");
    MemberLink m; std::string q = t.substr(18);
    auto dec = [](const std::string& v) { std::string o; for (size_t i = 0; i < v.size(); i++) { if (v[i] == '%' && i + 2 < v.size()) { o += (char)std::stoi(v.substr(i + 1, 2), nullptr, 16); i += 2; } else if (v[i] == '+') o += ' '; else o += v[i]; } return o; };
    size_t p = 0;
    while (p <= q.size()) {
        size_t e = q.find('&', p); std::string kv = q.substr(p, e == std::string::npos ? std::string::npos : e - p);
        size_t eq = kv.find('='); if (eq != std::string::npos) { std::string k = kv.substr(0, eq), v = dec(kv.substr(eq + 1)); if (k == "pk") m.pk = v; else if (k == "n") m.name = v.substr(0, 40); }
        if (e == std::string::npos) break; p = e + 1;
    }
    if (m.pk.size() != 43 || readKeyBytes(m.pk).size() != 32) throw Err("the member code's key is malformed");
    return m;
}
}  // namespace bp
