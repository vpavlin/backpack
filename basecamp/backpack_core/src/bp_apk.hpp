#pragma once
// bp_apk.hpp - read what Backpack needs from an APK without the Android SDK (the CLI uses aapt2 +
// apksigner): package, versionCode/Name, label, minSdk, ABIs, a raster icon, and the signing
// certificate. The signature is VERIFIED, like `apksigner verify`: APK Signature Scheme v3 or v2 (the
// signed data's signature with the certificate's key, and the content digest over the whole file).
// An APK with only a v1 (JAR) signature is refused: every Logos APK is v2+.
#include "bp_crypto.hpp"
#include <openssl/x509.h>
#include <zlib.h>
#include <algorithm>
#include <map>
#include <set>

namespace bp {

struct Zip {
    struct Entry { std::string name; uint16_t method = 0; uint32_t csize = 0, usize = 0, local = 0; };
    std::string path; Bytes data; std::vector<Entry> entries; uint64_t cdOffset = 0, cdSize = 0, eocdOffset = 0;
    static uint16_t u16(const Bytes& b, size_t o) { if (o + 2 > b.size()) throw Err("zip: out of range"); return b[o] | b[o + 1] << 8; }
    static uint32_t u32(const Bytes& b, size_t o) { if (o + 4 > b.size()) throw Err("zip: out of range"); return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; }
    static uint64_t u64(const Bytes& b, size_t o) { return u32(b, o) | (uint64_t)u32(b, o + 4) << 32; }
    explicit Zip(const std::string& p) : path(p) {
        std::ifstream f(p, std::ios::binary); if (!f) throw Err("can't read " + p);
        data.assign(std::istreambuf_iterator<char>(f), {});
        if (data.size() < 22) throw Err("not a zip file");
        size_t lo = data.size() > 65557 ? data.size() - 65557 : 0;
        for (size_t i = data.size() - 22 + 1; i-- > lo;) if (u32(data, i) == 0x06054b50) { eocdOffset = i; break; }
        if (!eocdOffset && u32(data, 0) != 0x06054b50) throw Err("not a zip file (no end record)");
        cdSize = u32(data, eocdOffset + 12); cdOffset = u32(data, eocdOffset + 16);
        uint16_t n = u16(data, eocdOffset + 10);
        size_t o = cdOffset;
        for (int i = 0; i < n; i++) {
            if (u32(data, o) != 0x02014b50) throw Err("zip: bad central directory");
            Entry e; e.method = u16(data, o + 10); e.csize = u32(data, o + 20); e.usize = u32(data, o + 24);
            uint16_t nl = u16(data, o + 28), xl = u16(data, o + 30), cl = u16(data, o + 32); e.local = u32(data, o + 42);
            e.name.assign((const char*)&data[o + 46], nl); entries.push_back(e); o += 46 + nl + xl + cl;
        }
    }
    const Entry* find(const std::string& n) const { for (auto& e : entries) if (e.name == n) return &e; return nullptr; }
    Bytes read(const Entry& e, size_t max = 64u << 20) const {
        if (e.usize > max) throw Err(e.name + " is too large");
        size_t lo = e.local; if (u32(data, lo) != 0x04034b50) throw Err("zip: bad local header");
        size_t start = lo + 30 + u16(data, lo + 26) + u16(data, lo + 28);
        if (start + e.csize > data.size()) throw Err("zip: truncated entry");
        if (e.method == 0) return Bytes(data.begin() + start, data.begin() + start + e.csize);
        if (e.method != 8) throw Err("zip: unsupported compression");
        Bytes out(e.usize); z_stream z{}; z.next_in = (Bytef*)&data[start]; z.avail_in = e.csize; z.next_out = out.data(); z.avail_out = e.usize;
        if (inflateInit2(&z, -15) != Z_OK) throw Err("zip: inflate init");
        int r = inflate(&z, Z_FINISH); inflateEnd(&z);
        if (r != Z_STREAM_END || z.total_out != e.usize) throw Err("zip: can't inflate " + e.name);
        return out;
    }
};

// ---- resources.arsc (just enough: string values and file paths per resource id) -------------------
struct StringPool {
    std::vector<std::string> s;
    void parse(const Bytes& b, size_t o) {
        uint32_t count = Zip::u32(b, o + 8), flags = Zip::u32(b, o + 16), strStart = Zip::u32(b, o + 20);
        bool utf8 = flags & 0x100; size_t hs = Zip::u16(b, o + 2);
        s.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            size_t p = o + Zip::u32(b, o + hs + i * 4) + strStart;
            if (utf8) {
                size_t q = p; if (b.at(q) & 0x80) q += 2; else q += 1;              // utf16 length
                size_t len = b.at(q); if (len & 0x80) { len = (len & 0x7f) << 8 | b.at(q + 1); q += 2; } else q += 1;
                if (q + len > b.size()) throw Err("arsc: bad string");
                s[i].assign((const char*)&b[q], len);
            } else {
                size_t len = Zip::u16(b, p), q = p + 2; if (len & 0x8000) { len = (len & 0x7fff) << 16 | Zip::u16(b, p + 2); q += 2; }
                std::string out; for (size_t k = 0; k < len; k++) { uint16_t c = Zip::u16(b, q + 2 * k); if (c < 0x80) out += (char)c; else if (c < 0x800) { out += (char)(0xc0 | c >> 6); out += (char)(0x80 | (c & 63)); } else { out += (char)(0xe0 | c >> 12); out += (char)(0x80 | (c >> 6 & 63)); out += (char)(0x80 | (c & 63)); } }
                s[i] = out;
            }
        }
    }
};
struct Arsc {
    StringPool values;
    struct Val { uint8_t type = 0; uint32_t data = 0; uint16_t density = 0; };
    std::map<uint32_t, std::vector<Val>> res;   // resource id -> one value per configuration
    void parse(const Bytes& b) {
        if (Zip::u16(b, 0) != 0x0002) throw Err("arsc: not a resource table");
        size_t o = Zip::u16(b, 2), end = std::min<size_t>(Zip::u32(b, 4), b.size());
        while (o + 8 <= end) {
            uint16_t t = Zip::u16(b, o); uint32_t sz = Zip::u32(b, o + 4); if (!sz) break;
            if (t == 0x0001) values.parse(b, o);
            else if (t == 0x0200) package(b, o);
            o += sz;
        }
    }
    void package(const Bytes& b, size_t po) {
        uint32_t pkgId = Zip::u32(b, po + 8); size_t o = po + Zip::u16(b, po + 2), end = po + Zip::u32(b, po + 4);
        while (o + 8 <= end) {
            uint16_t t = Zip::u16(b, o); uint32_t sz = Zip::u32(b, o + 4); if (!sz) break;
            if (t == 0x0201) {   // type chunk
                uint8_t typeId = b.at(o + 8); uint32_t count = Zip::u32(b, o + 12), entriesStart = Zip::u32(b, o + 16);
                size_t cfg = o + 20; uint16_t density = Zip::u16(b, cfg + 14);
                // newer aapt2 output: sparse type chunks (0x01), 16-bit offsets (0x02), compact entries
                uint8_t flags = b.at(o + 9); bool sparse = flags & 1, off16 = flags & 2;
                size_t hs = Zip::u16(b, o + 2);
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t idx = i, off;
                    if (sparse) { idx = Zip::u16(b, o + hs + i * 4); off = Zip::u16(b, o + hs + i * 4 + 2) * 4u; }
                    else if (off16) { off = Zip::u16(b, o + hs + i * 2); if (off == 0xffff) continue; off *= 4; }
                    else { off = Zip::u32(b, o + hs + i * 4); if (off == 0xffffffff) continue; }
                    size_t e = o + entriesStart + off; uint16_t eflags = Zip::u16(b, e + 2);
                    Val val;
                    if (eflags & 0x08) val = Val{(uint8_t)(eflags >> 8), Zip::u32(b, e + 4), density};   // compact: key16, flags(type in the high byte), data32
                    else {
                        if (eflags & 1) continue;   // complex (bags): not needed
                        size_t v = e + Zip::u16(b, e); val = Val{b.at(v + 3), Zip::u32(b, v + 4), density};
                    }
                    res[pkgId << 24 | (uint32_t)typeId << 16 | idx].push_back(val);
                }
            }
            o += sz;
        }
    }
    std::string str(uint32_t id, int depth = 0) const {   // first string value (default config first in practice)
        auto it = res.find(id); if (it == res.end() || depth > 4) return "";
        for (auto& v : it->second) { if (v.type == 0x03 && v.data < values.s.size()) return values.s[v.data]; if (v.type == 0x01) return str(v.data, depth + 1); }
        return "";
    }
    std::string rasterFile(uint32_t id) const {   // the densest png/webp of a drawable/mipmap
        auto it = res.find(id); if (it == res.end()) return "";
        std::string best; int bestD = -1;
        for (auto& v : it->second) {
            if (v.type != 0x03 || v.data >= values.s.size()) continue;
            const std::string& f = values.s[v.data];
            if (f.size() < 5 || !(f.compare(f.size() - 4, 4, ".png") == 0 || f.compare(f.size() - 5, 5, ".webp") == 0)) continue;
            int d = v.density == 0xffff || v.density == 0xfffe ? 0 : v.density;
            if (d > bestD) { bestD = d; best = f; }
        }
        return best;
    }
};

// ---- binary AndroidManifest.xml ----------------------------------------------------------------------
struct Manifest { std::string id, versionName, label; int64_t versionCode = 0; int minSdk = 0; uint32_t labelRef = 0, iconRef = 0; std::string iconPath; };
inline Manifest parseManifest(const Bytes& b) {
    if (Zip::u16(b, 0) != 0x0003) throw Err("AndroidManifest.xml is not binary XML");
    StringPool sp; std::vector<uint32_t> resMap; Manifest m;
    size_t o = Zip::u16(b, 2);
    while (o + 8 <= b.size()) {
        uint16_t t = Zip::u16(b, o); uint32_t sz = Zip::u32(b, o + 4); if (!sz) break;
        if (t == 0x0001) sp.parse(b, o);
        else if (t == 0x0180) for (size_t i = 8; i < sz; i += 4) resMap.push_back(Zip::u32(b, o + i));
        else if (t == 0x0102) {
            uint32_t nameIdx = Zip::u32(b, o + 20); std::string el = nameIdx < sp.s.size() ? sp.s[nameIdx] : "";
            uint16_t attrStart = Zip::u16(b, o + 24), attrSize = Zip::u16(b, o + 26), attrCount = Zip::u16(b, o + 28);
            for (int i = 0; i < attrCount; i++) {
                size_t a = o + 16 + attrStart + i * attrSize;
                uint32_t an = Zip::u32(b, a + 4), raw = Zip::u32(b, a + 8); uint8_t vt = b.at(a + 15); uint32_t vd = Zip::u32(b, a + 16);
                uint32_t rid = an < resMap.size() ? resMap[an] : 0; std::string aname = an < sp.s.size() ? sp.s[an] : "";
                auto sval = [&]() -> std::string { if (vt == 0x03 && vd < sp.s.size()) return sp.s[vd]; if (raw != 0xffffffff && raw < sp.s.size()) return sp.s[raw]; return ""; };
                if (el == "manifest" && aname == "package") m.id = sval();
                else if (el == "manifest" && rid == 0x0101021b) m.versionCode = vd;
                else if (el == "manifest" && rid == 0x0101021c) { if (vt == 0x01) m.labelRef = m.labelRef; m.versionName = sval(); }
                else if (el == "uses-sdk" && rid == 0x0101020c) m.minSdk = vt == 0x10 ? (int)vd : std::atoi(sval().c_str());
                else if (el == "application" && rid == 0x01010001) { if (vt == 0x01) m.labelRef = vd; else m.label = sval(); }
                else if (el == "application" && rid == 0x01010002) { if (vt == 0x01) m.iconRef = vd; else m.iconPath = sval(); }
            }
        }
        o += sz;
    }
    return m;
}

// ---- APK Signature Scheme v2/v3 -------------------------------------------------------------------------
struct Reader {   // length-prefixed (u32 LE) sequences inside the signing block
    const Bytes& b; size_t p, end;
    Reader(const Bytes& b_, size_t p_, size_t e_) : b(b_), p(p_), end(e_) {}
    bool more() const { return p < end; }
    uint32_t u32() { if (p + 4 > end) throw Err("signing block: truncated"); uint32_t v = Zip::u32(b, p); p += 4; return v; }
    Reader sub() { uint32_t n = u32(); if (p + n > end) throw Err("signing block: bad length"); Reader r(b, p, p + n); p += n; return r; }
    Bytes bytes() { Reader r = sub(); return Bytes(b.begin() + r.p, b.begin() + r.end); }
};
inline const EVP_MD* sigMd(uint32_t alg) {
    switch (alg) { case 0x0101: case 0x0103: case 0x0201: case 0x0301: return EVP_sha256(); case 0x0102: case 0x0104: case 0x0202: return EVP_sha512(); default: return nullptr; }
}
// The v2 content digest: chunked (1 MiB) over entries | central directory | EOCD (with cd offset = sig block start).
inline Bytes contentDigest(const Zip& z, size_t sigBlockStart, const EVP_MD* md) {
    Bytes eocd(z.data.begin() + z.eocdOffset, z.data.end());
    uint32_t sb = (uint32_t)sigBlockStart; eocd[16] = sb & 255; eocd[17] = sb >> 8 & 255; eocd[18] = sb >> 16 & 255; eocd[19] = sb >> 24;
    struct Part { const unsigned char* p; size_t n; };
    std::vector<Part> parts = {{z.data.data(), sigBlockStart}, {z.data.data() + z.cdOffset, (size_t)z.cdSize}, {eocd.data(), eocd.size()}};
    size_t dl = EVP_MD_size(md); Bytes chunks; uint32_t count = 0;
    for (auto& part : parts) for (size_t off = 0; off < part.n; off += 1u << 20) {
        size_t n = std::min<size_t>(1u << 20, part.n - off);
        unsigned char pre[5] = {0xa5, (unsigned char)(n & 255), (unsigned char)(n >> 8 & 255), (unsigned char)(n >> 16 & 255), (unsigned char)(n >> 24)};
        EVP_MD_CTX* c = EVP_MD_CTX_new(); unsigned char d[64]; unsigned dn = 0;
        EVP_DigestInit_ex(c, md, nullptr); EVP_DigestUpdate(c, pre, 5); EVP_DigestUpdate(c, part.p + off, n); EVP_DigestFinal_ex(c, d, &dn); EVP_MD_CTX_free(c);
        chunks.insert(chunks.end(), d, d + dl); count++;
    }
    unsigned char pre[5] = {0x5a, (unsigned char)(count & 255), (unsigned char)(count >> 8 & 255), (unsigned char)(count >> 16 & 255), (unsigned char)(count >> 24)};
    EVP_MD_CTX* c = EVP_MD_CTX_new(); unsigned char d[64]; unsigned dn = 0;
    EVP_DigestInit_ex(c, md, nullptr); EVP_DigestUpdate(c, pre, 5); EVP_DigestUpdate(c, chunks.data(), chunks.size()); EVP_DigestFinal_ex(c, d, &dn); EVP_MD_CTX_free(c);
    return Bytes(d, d + dl);
}
// Verify the APK's v3 (preferred) or v2 signature; returns the SHA-256 of the first signer's certificate.
inline std::string verifiedSigner(const Zip& z) {
    size_t cd = z.cdOffset;
    if (cd < 32 || std::string((const char*)&z.data[cd - 16], 16) != "APK Sig Block 42") throw Err("the APK has no v2/v3 signature (v1-only or unsigned APKs are refused)");
    uint64_t sz = Zip::u64(z.data, cd - 24); size_t start = cd - sz - 8;
    if (sz > cd || Zip::u64(z.data, start) != sz) throw Err("signing block: bad size");
    std::map<uint32_t, std::pair<size_t, size_t>> blocks;
    for (size_t p = start + 8; p < cd - 24;) {
        uint64_t len = Zip::u64(z.data, p); uint32_t id = Zip::u32(z.data, p + 8);
        if (len < 4 || p + 8 + len > cd - 24) throw Err("signing block: bad pair");
        blocks[id] = {p + 12, p + 8 + len}; p += 8 + len;
    }
    uint32_t which = blocks.count(0xf05368c0) ? 0xf05368c0 : blocks.count(0x7109871a) ? 0x7109871a : 0;
    if (!which) throw Err("the APK has no v2/v3 signature");
    Reader signers(z.data, blocks[which].first, blocks[which].second);
    Reader list = signers.sub(); Reader signer = list.sub();
    Bytes signedData = signer.bytes();
    Reader sd(signedData, 0, signedData.size());
    Reader digests = sd.sub(); Reader certs = sd.sub();
    Bytes cert = certs.bytes();
    // v3: the signer also carries minSDK and maxSDK after the signed data (v2 has nothing there)
    if (which == 0xf05368c0) { signer.u32(); signer.u32(); }
    Reader sigs = signer.sub(); Bytes pubkey = signer.bytes();
    const unsigned char* cp = cert.data(); X509* x = d2i_X509(nullptr, &cp, (long)cert.size()); if (!x) throw Err("bad signing certificate");
    EVP_PKEY* certKey = X509_get_pubkey(x); X509_free(x);
    const unsigned char* pk = pubkey.data(); EVP_PKEY* sigKey = d2i_PUBKEY(nullptr, &pk, (long)pubkey.size());
    bool sameKey = certKey && sigKey && EVP_PKEY_eq(certKey, sigKey) == 1;
    if (sigKey) EVP_PKEY_free(sigKey);
    if (!sameKey) { if (certKey) EVP_PKEY_free(certKey); throw Err("the signer's key doesn't match its certificate"); }
    // a signature we understand over the signed data
    uint32_t alg = 0; bool sigOk = false;
    while (sigs.more()) {
        Reader s = sigs.sub(); uint32_t a = s.u32(); Bytes sig = s.bytes(); const EVP_MD* md = sigMd(a); if (!md) continue;
        EVP_MD_CTX* c = EVP_MD_CTX_new(); EVP_PKEY_CTX* pc = nullptr;
        bool ok = EVP_DigestVerifyInit(c, &pc, md, nullptr, certKey) == 1;
        if (ok && (a == 0x0101 || a == 0x0102)) { EVP_PKEY_CTX_set_rsa_padding(pc, RSA_PKCS1_PSS_PADDING); EVP_PKEY_CTX_set_rsa_pss_saltlen(pc, EVP_MD_size(md)); EVP_PKEY_CTX_set_rsa_mgf1_md(pc, md); }
        ok = ok && EVP_DigestVerify(c, sig.data(), sig.size(), signedData.data(), signedData.size()) == 1;
        EVP_MD_CTX_free(c); if (ok) { sigOk = true; alg = a; break; }
    }
    EVP_PKEY_free(certKey);
    if (!sigOk) throw Err("the APK signature doesn't verify");
    // the signed digest for that algorithm must match the file
    Bytes want; while (digests.more()) { Reader d = digests.sub(); uint32_t a = d.u32(); Bytes v = d.bytes(); if (a == alg) want = v; }
    if (want.empty() || contentDigest(z, start, sigMd(alg)) != want) throw Err("the APK was modified after signing (content digest mismatch)");
    auto h = sha256(cert.data(), cert.size()); return hex(h.data(), 32);
}

struct ApkInfo { std::string id, versionName, label, signer, iconEntry; int64_t versionCode = 0; int minSdk = 0; std::vector<std::string> abis; };
inline ApkInfo readApk(const std::string& path) {
    Zip z(path); ApkInfo a;
    a.signer = verifiedSigner(z);
    const Zip::Entry* me = z.find("AndroidManifest.xml"); if (!me) throw Err("no AndroidManifest.xml");
    Manifest m = parseManifest(z.read(*me, 8u << 20));
    a.id = m.id; a.versionCode = m.versionCode; a.versionName = m.versionName; a.minSdk = m.minSdk; a.label = m.label;
    Arsc arsc; bool haveArsc = false;
    if (const Zip::Entry* re = z.find("resources.arsc")) { try { arsc.parse(z.read(*re, 64u << 20)); haveArsc = true; } catch (...) {} }
    if (a.label.empty() && m.labelRef && haveArsc) a.label = arsc.str(m.labelRef);
    if (a.label.empty()) a.label = a.id;
    if (!m.iconPath.empty() && (m.iconPath.size() > 4) && (m.iconPath.find(".png") != std::string::npos || m.iconPath.find(".webp") != std::string::npos)) a.iconEntry = m.iconPath;
    else if (m.iconRef && haveArsc) a.iconEntry = arsc.rasterFile(m.iconRef);
    std::set<std::string> abis;
    for (auto& e : z.entries) if (e.name.rfind("lib/", 0) == 0) { size_t s = e.name.find('/', 4); if (s != std::string::npos) abis.insert(e.name.substr(4, s - 4)); }
    a.abis.assign(abis.begin(), abis.end());
    if (a.id.empty() || a.versionCode <= 0) throw Err("can't read the package name or versionCode");
    return a;
}
}  // namespace bp
