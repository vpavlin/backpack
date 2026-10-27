#pragma once
// bp_sign.hpp - sign an APK the way `apksigner sign` does, without the Android SDK: APK Signature Scheme
// v2 (Android 7+), plus a v1 JAR signature when the app supports older Android (minSdk < 24). The key is
// the APP's identity for good: every later version must be signed with it (keys.hpp keeps it).
// An APK that already has a v2/v3 block is refused (sign once, after zipalign, never re-sign by accident).
#include "bp_repo.hpp"
#include <openssl/pkcs7.h>
#include <openssl/x509.h>

namespace bp {

inline void put16(Bytes& b, uint16_t v) { b.push_back(v & 255); b.push_back(v >> 8); }
inline void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(v >> (8 * i) & 255); }
inline void put64(Bytes& b, uint64_t v) { for (int i = 0; i < 8; i++) b.push_back(v >> (8 * i) & 255); }
inline Bytes lp(const Bytes& v) { Bytes o; put32(o, (uint32_t)v.size()); o.insert(o.end(), v.begin(), v.end()); return o; }
inline void set32(Bytes& b, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) b[o + i] = v >> (8 * i) & 255; }

struct SigningKey {
    EVP_PKEY* key = nullptr; X509* cert = nullptr;
    SigningKey() = default; SigningKey(const SigningKey&) = delete;
    SigningKey(SigningKey&& o) noexcept : key(o.key), cert(o.cert) { o.key = nullptr; o.cert = nullptr; }
    ~SigningKey() { if (key) EVP_PKEY_free(key); if (cert) X509_free(cert); }
    Bytes certDer() const { unsigned char* d = nullptr; int n = i2d_X509(cert, &d); Bytes o(d, d + n); OPENSSL_free(d); return o; }
    std::string certSha256() const { Bytes d = certDer(); auto h = sha256(d.data(), d.size()); return hex(h.data(), 32); }
    Bytes pubDer() const { unsigned char* d = nullptr; int n = i2d_PUBKEY(key, &d); Bytes o(d, d + n); OPENSSL_free(d); return o; }
    bool isRsa() const { return EVP_PKEY_base_id(key) == EVP_PKEY_RSA; }
    // RSA PKCS#1 v1.5 SHA-256 (0x0103) or ECDSA SHA-256 (0x0201)
    uint32_t algId() const { return isRsa() ? 0x0103 : 0x0201; }
    Bytes sign(const Bytes& data) const {
        EVP_MD_CTX* c = EVP_MD_CTX_new(); size_t n = 0; Bytes s;
        bool ok = EVP_DigestSignInit(c, nullptr, EVP_sha256(), nullptr, key) == 1 && EVP_DigestSign(c, nullptr, &n, data.data(), data.size()) == 1;
        if (ok) { s.resize(n); ok = EVP_DigestSign(c, s.data(), &n, data.data(), data.size()) == 1; s.resize(n); }
        EVP_MD_CTX_free(c); if (!ok) throw Err("signing failed"); return s;
    }
};

// ---- v1 (JAR) -------------------------------------------------------------------------------------------
// "Name: x" lines are wrapped at 72 bytes with continuation lines starting with a space.
inline std::string mfLine(const std::string& l) {
    std::string out; size_t i = 0, first = 70;   // 72 incl. \r\n
    out += l.substr(0, first) + "\r\n"; i = first;
    while (i < l.size()) { out += " " + l.substr(i, 69) + "\r\n"; i += 69; }
    return l.size() <= first ? l + "\r\n" : out;
}
inline Bytes deflateRaw(const Bytes& in) {
    z_stream z{}; deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY);
    Bytes out(deflateBound(&z, in.size()) + 16); z.next_in = (Bytef*)in.data(); z.avail_in = in.size(); z.next_out = out.data(); z.avail_out = out.size();
    deflate(&z, Z_FINISH); out.resize(z.total_out); deflateEnd(&z); return out;
}
// Returns a new zip with META-INF/MANIFEST.MF, BACKPACK.SF, BACKPACK.RSA|EC added (entries before the CD,
// existing entries byte-for-byte unchanged).
inline Bytes addJarSignature(const Zip& z, const SigningKey& k) {
    for (auto& e : z.entries) if (e.name.rfind("META-INF/", 0) == 0) {
        std::string n = e.name; for (auto& c : n) c = (char)toupper((unsigned char)c);
        auto ends = [&](const char* x) { std::string s(x); return n.size() >= s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0; };
        if (n == "META-INF/MANIFEST.MF" || ends(".SF") || ends(".RSA") || ends(".EC") || ends(".DSA")) throw Err("the APK already has a JAR signature (" + e.name + ")");
    }
    auto b64 = [](const Bytes& d) { return b64std(d); };
    std::string mf = "Manifest-Version: 1.0\r\nCreated-By: 1.0 (Backpack)\r\n\r\n";
    std::string sfSections;
    for (auto& e : z.entries) {
        if (!e.name.empty() && e.name.back() == '/') continue;
        Bytes data = z.read(e, 1u << 31);
        std::string sec = mfLine("Name: " + e.name) + "SHA-256-Digest: " + b64(sha256(data.data(), data.size())) + "\r\n\r\n";
        mf += sec;
        sfSections += mfLine("Name: " + e.name) + "SHA-256-Digest: " + b64(sha256((const unsigned char*)sec.data(), sec.size())) + "\r\n\r\n";
    }
    std::string mainHead = "Manifest-Version: 1.0\r\nCreated-By: 1.0 (Backpack)\r\n\r\n";
    std::string sf = "Signature-Version: 1.0\r\nCreated-By: 1.0 (Backpack)\r\nSHA-256-Digest-Manifest: " + b64(sha256((const unsigned char*)mf.data(), mf.size())) +
                     "\r\nSHA-256-Digest-Manifest-Main-Attributes: " + b64(sha256((const unsigned char*)mainHead.data(), mainHead.size())) +
                     "\r\nX-Android-APK-Signed: 2\r\n\r\n" + sfSections;
    BIO* in = BIO_new_mem_buf(sf.data(), (int)sf.size());
    PKCS7* p7 = PKCS7_sign(k.cert, k.key, nullptr, in, PKCS7_DETACHED | PKCS7_BINARY | PKCS7_NOATTR | PKCS7_NOSMIMECAP);
    BIO_free(in); if (!p7) throw Err("JAR signing failed");
    unsigned char* der = nullptr; int dn = i2d_PKCS7(p7, &der); Bytes blk(der, der + dn); OPENSSL_free(der); PKCS7_free(p7);

    // rebuild: original local entries | 3 new entries | CD (original + 3) | EOCD
    Bytes out(z.data.begin(), z.data.begin() + z.cdOffset);
    Bytes cd(z.data.begin() + z.cdOffset, z.data.begin() + z.cdOffset + z.cdSize);
    std::vector<std::pair<std::string, Bytes>> add = {{"META-INF/MANIFEST.MF", Bytes(mf.begin(), mf.end())}, {"META-INF/BACKPACK.SF", Bytes(sf.begin(), sf.end())}, {std::string("META-INF/BACKPACK.") + (k.isRsa() ? "RSA" : "EC"), blk}};
    for (auto& [name, data] : add) {
        Bytes comp = deflateRaw(data); uint32_t crc = crc32(0L, data.data(), (uInt)data.size()); uint32_t off = (uint32_t)out.size();
        Bytes lh; put32(lh, 0x04034b50); put16(lh, 20); put16(lh, 0x0800); put16(lh, 8); put16(lh, 0); put16(lh, 0x21); put32(lh, crc); put32(lh, (uint32_t)comp.size()); put32(lh, (uint32_t)data.size()); put16(lh, (uint16_t)name.size()); put16(lh, 0);
        lh.insert(lh.end(), name.begin(), name.end()); out.insert(out.end(), lh.begin(), lh.end()); out.insert(out.end(), comp.begin(), comp.end());
        Bytes ch; put32(ch, 0x02014b50); put16(ch, 20); put16(ch, 20); put16(ch, 0x0800); put16(ch, 8); put16(ch, 0); put16(ch, 0x21); put32(ch, crc); put32(ch, (uint32_t)comp.size()); put32(ch, (uint32_t)data.size());
        put16(ch, (uint16_t)name.size()); put16(ch, 0); put16(ch, 0); put16(ch, 0); put16(ch, 0); put32(ch, 0); put32(ch, off);
        ch.insert(ch.end(), name.begin(), name.end()); cd.insert(cd.end(), ch.begin(), ch.end());
    }
    uint32_t cdOff = (uint32_t)out.size(); out.insert(out.end(), cd.begin(), cd.end());
    Bytes eocd(z.data.begin() + z.eocdOffset, z.data.end());
    uint16_t n = (uint16_t)(z.entries.size() + add.size());
    eocd[8] = n & 255; eocd[9] = n >> 8; eocd[10] = n & 255; eocd[11] = n >> 8;
    set32(eocd, 12, (uint32_t)cd.size()); set32(eocd, 16, cdOff);
    out.insert(out.end(), eocd.begin(), eocd.end());
    return out;
}

// ---- v2 -------------------------------------------------------------------------------------------------
inline Bytes addV2Signature(const Zip& z, const SigningKey& k) {
    if (z.cdOffset >= 16 && std::string((const char*)&z.data[z.cdOffset - 16], 16) == "APK Sig Block 42") throw Err("the APK is already signed (v2/v3): sign the unsigned build");
    Bytes digest = contentDigest(z, z.cdOffset, EVP_sha256());
    Bytes dig; put32(dig, k.algId()); Bytes dv = lp(digest); dig.insert(dig.end(), dv.begin(), dv.end());
    Bytes digests = lp(lp(dig)), certs = lp(lp(k.certDer())), attrs = lp(Bytes{});
    Bytes signedData = digests; signedData.insert(signedData.end(), certs.begin(), certs.end()); signedData.insert(signedData.end(), attrs.begin(), attrs.end());
    Bytes sig; put32(sig, k.algId()); Bytes sv = lp(k.sign(signedData)); sig.insert(sig.end(), sv.begin(), sv.end());
    Bytes signer = lp(signedData); Bytes sigs = lp(lp(sig)); Bytes pk = lp(k.pubDer());
    signer.insert(signer.end(), sigs.begin(), sigs.end()); signer.insert(signer.end(), pk.begin(), pk.end());
    Bytes value = lp(lp(signer));
    // block: size | pair(len, id, value) | size | magic
    Bytes pairs; put64(pairs, value.size() + 4); put32(pairs, 0x7109871a); pairs.insert(pairs.end(), value.begin(), value.end());
    uint64_t blockSize = pairs.size() + 8 + 16;   // excludes the leading size field
    Bytes block; put64(block, blockSize); block.insert(block.end(), pairs.begin(), pairs.end()); put64(block, blockSize);
    const char* magic = "APK Sig Block 42"; block.insert(block.end(), magic, magic + 16);
    Bytes out(z.data.begin(), z.data.begin() + z.cdOffset);
    out.insert(out.end(), block.begin(), block.end());
    uint32_t cdOff = (uint32_t)out.size();
    out.insert(out.end(), z.data.begin() + z.cdOffset, z.data.begin() + z.cdOffset + z.cdSize);
    Bytes eocd(z.data.begin() + z.eocdOffset, z.data.end()); set32(eocd, 16, cdOff);
    out.insert(out.end(), eocd.begin(), eocd.end());
    return out;
}

// Sign src into dst: v1 too when minSdk < 24. Returns the certificate's SHA-256 (what Backpack pins).
inline std::string signApk(const std::string& src, const std::string& dst, const SigningKey& k) {
    Zip z(src);
    if (z.cdOffset >= 16 && std::string((const char*)&z.data[z.cdOffset - 16], 16) == "APK Sig Block 42") throw Err("the APK is already signed: sign the unsigned build");
    const Zip::Entry* me = z.find("AndroidManifest.xml"); if (!me) throw Err("no AndroidManifest.xml");
    Manifest m = parseManifest(z.read(*me, 8u << 20));
    TempDir t; std::string mid = (t.p / "v1.apk").string();
    if (m.minSdk > 0 && m.minSdk < 24) { Bytes v1 = addJarSignature(z, k); std::ofstream(mid, std::ios::binary).write((const char*)v1.data(), v1.size()); }
    else fs::copy_file(src, mid, fs::copy_options::overwrite_existing);
    Zip z1(mid); Bytes v2 = addV2Signature(z1, k);
    { std::ofstream f(dst, std::ios::binary | std::ios::trunc); f.write((const char*)v2.data(), v2.size()); if (!f) throw Err("can't write " + dst); }
    std::string got = verifiedSigner(Zip(dst));   // our own verifier must accept it
    if (got != k.certSha256()) throw Err("signed APK doesn't verify with the key");
    return got;
}
}  // namespace bp
