#pragma once
// bp_crypto.hpp - Backpack's formats in C++ (ADR 0002, 0009), byte-compatible with cli/envelope.mjs +
// cli/crypt.mjs and the phone. test/vectors_test.cpp checks it against cli/test/*vectors.json.
//   envelope: {"backpack":1,["kind":"pointer",]["enc":"a256gcm",]["key":<repo key>,]"body":..,"sig":..}
//             sig = ed25519 over the exact body string; enc body = b64url(nonce12 | AES-256-GCM | tag16)
//   bpe1 file: "BPE1" | chunk u32 BE (1 MiB) | prefix8 | chunks (nonce = prefix | u32 BE index, aad = [last])
//   topic id:  b64url(SHA-256("backpack/topic/1" | readKey))
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace bp {
using json = nlohmann::json;
using Bytes = std::vector<unsigned char>;
struct Err : std::runtime_error { using std::runtime_error::runtime_error; };

inline std::string b64url(const unsigned char* p, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o; o.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) { uint32_t v = p[i] << 16 | p[i + 1] << 8 | p[i + 2]; o += T[v >> 18]; o += T[v >> 12 & 63]; o += T[v >> 6 & 63]; o += T[v & 63]; }
    if (n - i == 1) { uint32_t v = p[i] << 16; o += T[v >> 18]; o += T[v >> 12 & 63]; }
    else if (n - i == 2) { uint32_t v = p[i] << 16 | p[i + 1] << 8; o += T[v >> 18]; o += T[v >> 12 & 63]; o += T[v >> 6 & 63]; }
    return o;
}
inline std::string b64url(const Bytes& b) { return b64url(b.data(), b.size()); }
inline Bytes unb64url(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '-' || c == '+') return 62; if (c == '_' || c == '/') return 63; return -1; };
    Bytes o; uint32_t acc = 0; int bits = 0;
    for (char c : s) { if (c == '=') break; int v = val(c); if (v < 0) throw Err("not base64url"); acc = acc << 6 | v; bits += 6; if (bits >= 8) { bits -= 8; o.push_back((acc >> bits) & 255); } }
    return o;
}
inline std::string b64std(const Bytes& b) { std::string s = b64url(b); for (auto& c : s) { if (c == '-') c = '+'; else if (c == '_') c = '/'; } while (s.size() % 4) s += '='; return s; }
inline std::string hex(const unsigned char* p, size_t n) { static const char* H = "0123456789abcdef"; std::string o; for (size_t i = 0; i < n; i++) { o += H[p[i] >> 4]; o += H[p[i] & 15]; } return o; }
inline Bytes sha256(const unsigned char* p, size_t n) { Bytes d(32); SHA256(p, n, d.data()); return d; }
inline std::string sha256hex(const std::string& s) { auto d = sha256((const unsigned char*)s.data(), s.size()); return hex(d.data(), 32); }
inline Bytes randomBytes(size_t n) { Bytes b(n); if (RAND_bytes(b.data(), (int)n) != 1) throw Err("no randomness"); return b; }

// ---- keys -----------------------------------------------------------------------------------------
struct Key {   // an ed25519 private key (the CLI's key.pem: PKCS#8 PEM)
    EVP_PKEY* k = nullptr;
    Key() = default; Key(const Key&) = delete; Key& operator=(const Key&) = delete;
    Key(Key&& o) noexcept : k(o.k) { o.k = nullptr; }
    ~Key() { if (k) EVP_PKEY_free(k); }
    static Key generate() { Key r; EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr); EVP_PKEY_keygen_init(c); EVP_PKEY_keygen(c, &r.k); EVP_PKEY_CTX_free(c); if (!r.k) throw Err("keygen failed"); return r; }
    static Key fromPem(const std::string& pem) { Key r; BIO* b = BIO_new_mem_buf(pem.data(), (int)pem.size()); r.k = PEM_read_bio_PrivateKey(b, nullptr, nullptr, nullptr); BIO_free(b); if (!r.k) throw Err("can't read the signing key"); return r; }
    std::string pem() const { BIO* b = BIO_new(BIO_s_mem()); PEM_write_bio_PKCS8PrivateKey(b, k, nullptr, nullptr, 0, nullptr, nullptr); char* p; long n = BIO_get_mem_data(b, &p); std::string s(p, n); BIO_free(b); return s; }
    std::string pub() const { unsigned char raw[32]; size_t n = 32; if (EVP_PKEY_get_raw_public_key(k, raw, &n) != 1) throw Err("no public key"); return b64url(raw, n); }
    std::string sign(const std::string& msg) const {
        EVP_MD_CTX* c = EVP_MD_CTX_new(); unsigned char sig[64]; size_t n = 64;
        bool ok = EVP_DigestSignInit(c, nullptr, nullptr, nullptr, k) == 1 && EVP_DigestSign(c, sig, &n, (const unsigned char*)msg.data(), msg.size()) == 1;
        EVP_MD_CTX_free(c); if (!ok) throw Err("signing failed"); return b64url(sig, n);
    }
};
inline bool verify(const std::string& pubB64, const std::string& msg, const std::string& sigB64) {
    Bytes pub, sig; try { pub = unb64url(pubB64); sig = unb64url(sigB64); } catch (...) { return false; }
    if (pub.size() != 32 || sig.size() != 64) return false;
    EVP_PKEY* p = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), 32); if (!p) return false;
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    bool ok = EVP_DigestVerifyInit(c, nullptr, nullptr, nullptr, p) == 1 && EVP_DigestVerify(c, sig.data(), 64, (const unsigned char*)msg.data(), msg.size()) == 1;
    EVP_MD_CTX_free(c); EVP_PKEY_free(p); return ok;
}

// ---- AES-256-GCM ----------------------------------------------------------------------------------
inline Bytes gcmEnc(const Bytes& key, const Bytes& nonce, const unsigned char* p, size_t n, const Bytes& aad) {
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new(); Bytes out(n + 16); int len = 0, tot = 0;
    EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, (int)nonce.size(), nullptr);
    EVP_EncryptInit_ex(c, nullptr, nullptr, key.data(), nonce.data());
    if (!aad.empty()) EVP_EncryptUpdate(c, nullptr, &len, aad.data(), (int)aad.size());
    if (n) { EVP_EncryptUpdate(c, out.data(), &len, p, (int)n); tot = len; }
    EVP_EncryptFinal_ex(c, out.data() + tot, &len); tot += len;
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, out.data() + tot);
    EVP_CIPHER_CTX_free(c); out.resize(tot + 16); return out;
}
inline bool gcmDec(const Bytes& key, const Bytes& nonce, const unsigned char* ct, size_t n, const Bytes& aad, Bytes& out) {
    if (n < 16) return false;
    EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new(); out.assign(n - 16, 0); int len = 0, tot = 0;
    EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, (int)nonce.size(), nullptr);
    EVP_DecryptInit_ex(c, nullptr, nullptr, key.data(), nonce.data());
    if (!aad.empty()) EVP_DecryptUpdate(c, nullptr, &len, aad.data(), (int)aad.size());
    if (n > 16) { EVP_DecryptUpdate(c, out.data(), &len, ct, (int)(n - 16)); tot = len; }
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, (void*)(ct + n - 16));
    bool ok = EVP_DecryptFinal_ex(c, out.data() + tot, &len) == 1;
    EVP_CIPHER_CTX_free(c); return ok;
}
inline Bytes readKeyBytes(const std::string& k) { Bytes b = unb64url(k); if (b.size() != 32) throw Err("malformed key"); return b; }
inline std::string newKey() { return b64url(randomBytes(32)); }
inline std::string topicId(const std::string& readKey) {
    std::string m = "backpack/topic/1"; Bytes k = readKeyBytes(readKey); m.append((const char*)k.data(), k.size());
    return b64url(sha256((const unsigned char*)m.data(), m.size()));
}
inline std::string encBody(const std::string& readKey, const std::string& text) {
    Bytes nonce = randomBytes(12);
    Bytes ct = gcmEnc(readKeyBytes(readKey), nonce, (const unsigned char*)text.data(), text.size(), {});
    Bytes all = nonce; all.insert(all.end(), ct.begin(), ct.end()); return b64url(all);
}
inline std::string decBody(const std::string& readKey, const std::string& body) {
    Bytes raw = unb64url(body); if (raw.size() < 28) throw Err("encrypted body too short");
    Bytes nonce(raw.begin(), raw.begin() + 12), out;
    if (!gcmDec(readKeyBytes(readKey), nonce, raw.data() + 12, raw.size() - 12, {}, out)) throw Err("can't decrypt: wrong read key or damaged data");
    return std::string(out.begin(), out.end());
}

// ---- envelopes --------------------------------------------------------------------------------------
// kind: "index" (no kind field) or "pointer". Returns the envelope as compact JSON text.
template <class J>
inline std::string seal(const std::string& kind, const J& body, const Key& key, const std::string& readKey = "") {
    std::string text = body.dump();
    std::string b = readKey.empty() ? text : encBody(readKey, text);
    nlohmann::ordered_json env = nlohmann::ordered_json::object(); env["backpack"] = 1;
    if (kind != "index") env["kind"] = kind;
    if (!readKey.empty()) { env["enc"] = "a256gcm"; if (kind == "pointer") env["key"] = body.value("key", std::string()); }
    env["body"] = b; env["sig"] = key.sign(b);
    return env.dump();
}
inline json open(const json& env, const std::string& key, const std::string& kind, const std::string& readKey = "") {
    if (!env.is_object() || env.value("backpack", 0) != 1 || !env.contains("body") || !env["body"].is_string() || !env.contains("sig") || !env["sig"].is_string()) throw Err("not a Backpack " + kind);
    bool hasKind = env.contains("kind");
    if (kind == "pointer" ? (!hasKind || env["kind"] != "pointer") : hasKind) throw Err("expected a " + kind);
    const std::string body = env["body"];
    if (!verify(key, body, env["sig"])) throw Err(kind + " signature does not match the repo key");
    if (env.contains("key") && env["key"] != key) throw Err(kind + " was signed for a different repo");
    bool enc = env.contains("enc");
    if (enc && env["enc"] != "a256gcm") throw Err(kind + ": unknown encryption");
    if (enc && readKey.empty()) throw Err(kind + " is encrypted: this is a private repo and there is no read key");
    json b = json::parse(enc ? decBody(readKey, body) : body);
    if (!b.is_object() || b.value("key", "") != key) throw Err(kind + " was signed for a different repo");
    return b;
}

// ---- bpe1 files -------------------------------------------------------------------------------------
constexpr uint32_t CHUNK = 1u << 20;
inline Bytes chunkNonce(const Bytes& prefix, uint32_t i) { Bytes n = prefix; n.push_back(i >> 24); n.push_back(i >> 16 & 255); n.push_back(i >> 8 & 255); n.push_back(i & 255); return n; }
struct EncResult { std::string key; uint64_t size = 0; };
// Encrypt the file at src into dst with a fresh file key.
inline EncResult encryptFile(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary); if (!in) throw Err("can't read " + src);
    std::ofstream out(dst, std::ios::binary | std::ios::trunc); if (!out) throw Err("can't write " + dst);
    in.seekg(0, std::ios::end); uint64_t total = in.tellg(); in.seekg(0);
    Bytes key = randomBytes(32), prefix = randomBytes(8);
    unsigned char head[16] = {'B', 'P', 'E', '1', (unsigned char)(CHUNK >> 24), (unsigned char)(CHUNK >> 16 & 255), (unsigned char)(CHUNK >> 8 & 255), (unsigned char)(CHUNK & 255)};
    for (int i = 0; i < 8; i++) head[8 + i] = prefix[i];
    out.write((const char*)head, 16);
    uint64_t n = total ? (total + CHUNK - 1) / CHUNK : 1, size = 16;
    Bytes buf(CHUNK);
    for (uint64_t i = 0; i < n; i++) {
        in.read((char*)buf.data(), CHUNK); size_t got = (size_t)in.gcount();
        Bytes ct = gcmEnc(key, chunkNonce(prefix, (uint32_t)i), buf.data(), got, Bytes{(unsigned char)(i == n - 1 ? 1 : 0)});
        out.write((const char*)ct.data(), ct.size()); size += ct.size();
    }
    if (!out) throw Err("write failed");
    return {b64url(key), size};
}
// Decrypt a whole bpe1 buffer (verify, small files). Throws on any tampering.
inline Bytes decryptBuffer(const Bytes& data, const std::string& fileKey) {
    Bytes key = readKeyBytes(fileKey);
    if (data.size() < 16 || std::string(data.begin(), data.begin() + 4) != "BPE1") throw Err("not a Backpack encrypted file");
    uint32_t chunk = (uint32_t)data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];
    if (chunk != CHUNK) throw Err("unexpected chunk size");
    Bytes prefix(data.begin() + 8, data.begin() + 16), out, part;
    size_t off = 16; uint32_t i = 0; bool last = false;
    while (off < data.size()) {
        size_t len = std::min<size_t>(chunk + 16, data.size() - off);
        if (len < 16) throw Err("truncated chunk");
        last = off + len == data.size();
        if (!gcmDec(key, chunkNonce(prefix, i), data.data() + off, len, Bytes{(unsigned char)(last ? 1 : 0)}, part)) throw Err("chunk can't be decrypted (wrong key, damaged or truncated)");
        out.insert(out.end(), part.begin(), part.end()); off += len; i++;
    }
    if (!last) throw Err("truncated: no last chunk");
    return out;
}
}  // namespace bp
