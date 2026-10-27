#pragma once
// bp_fdroid.hpp - import an F-Droid v2 repo into a Backpack repo, only through a verified index (ADR 0004,
// the CLI's importFdroid without the JDK): entry.jar is a JAR signed by the certificate the user vouched
// for (fingerprint), with a SHA-256 (or stronger) digest; entry.json pins index-v2.json by SHA-256;
// index-v2 pins every APK and icon by SHA-256; and addApk checks package, versionCode and signer again.
#include "bp_repo.hpp"
#include <openssl/pkcs7.h>
#include <sstream>

namespace bp {

// JAR manifest sections: "Name: x" -> raw bytes of that section (for .SF per-entry digests) + attrs
struct JarManifest {
    std::map<std::string, std::string> main;                 // main attributes
    std::map<std::string, std::map<std::string, std::string>> attrs;
    std::map<std::string, std::string> rawSection;           // name -> exact bytes incl. the blank line
    static JarManifest parse(const std::string& text) {
        JarManifest m; std::vector<std::string> sections; std::string cur;
        size_t i = 0;
        while (i < text.size()) {   // split on blank lines, keeping the separators in the raw text
            size_t e = text.find('\n', i); std::string line = text.substr(i, e == std::string::npos ? std::string::npos : e - i + 1);
            cur += line; i = e == std::string::npos ? text.size() : e + 1;
            std::string t = line; while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
            if (t.empty()) { sections.push_back(cur); cur.clear(); }
        }
        if (!cur.empty()) sections.push_back(cur);
        for (size_t k = 0; k < sections.size(); k++) {
            std::map<std::string, std::string> a; std::string last; std::istringstream in(sections[k]); std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (line[0] == ' ' && !last.empty()) { a[last] += line.substr(1); continue; }
                size_t c = line.find(": "); if (c == std::string::npos) continue;
                last = line.substr(0, c); a[last] = line.substr(c + 2);
            }
            if (k == 0) m.main = a;
            else if (a.count("Name")) { m.attrs[a["Name"]] = a; m.rawSection[a["Name"]] = sections[k]; }
        }
        return m;
    }
};
inline std::string b64std(const Bytes& b) { std::string s = b64url(b); for (auto& c : s) { if (c == '-') c = '+'; else if (c == '_') c = '/'; } while (s.size() % 4) s += '='; return s; }

// Verify entry.jar; returns entry.json's text. fingerprint: lowercase hex SHA-256 of the signer cert.
inline std::string verifiedEntryJson(const fs::path& jarPath, const std::string& fingerprint, bool allowSha1) {
    Zip z(jarPath.string());
    const Zip::Entry *mf = z.find("META-INF/MANIFEST.MF"), *sf = nullptr, *blk = nullptr, *entry = z.find("entry.json");
    if (!mf || !entry) throw Err("entry.jar has no manifest or no entry.json");
    for (auto& e : z.entries) {
        if (e.name.rfind("META-INF/", 0) != 0) continue;
        auto ends = [&](const char* x) { std::string s(x); return e.name.size() > s.size() && e.name.compare(e.name.size() - s.size(), s.size(), s) == 0; };
        if (ends(".SF")) { if (sf) throw Err("entry.jar has more than one signature file"); sf = &e; }
        if (ends(".RSA") || ends(".EC") || ends(".DSA")) { if (blk) throw Err("entry.jar has more than one signature block"); blk = &e; }
    }
    if (!sf || !blk) throw Err("entry.jar is not signed");
    Bytes mfB = z.read(*mf, 1u << 20), sfB = z.read(*sf, 1u << 20), blkB = z.read(*blk, 1u << 20), entB = z.read(*entry, 1u << 20);
    // 1. the PKCS#7 block signs the .SF, by the vouched-for certificate, with SHA-256+
    const unsigned char* p = blkB.data(); PKCS7* p7 = d2i_PKCS7(nullptr, &p, (long)blkB.size());
    if (!p7) throw Err("entry.jar: unreadable signature block");
    BIO* data = BIO_new_mem_buf(sfB.data(), (int)sfB.size());
    int ok = PKCS7_verify(p7, nullptr, nullptr, data, nullptr, PKCS7_NOVERIFY | PKCS7_BINARY | PKCS7_NOCHAIN);
    BIO_free(data);
    STACK_OF(X509)* signers = ok == 1 ? PKCS7_get0_signers(p7, nullptr, 0) : nullptr;
    std::set<std::string> fps;
    for (int i = 0; signers && i < sk_X509_num(signers); i++) { unsigned char* der = nullptr; int n = i2d_X509(sk_X509_value(signers, i), &der); auto h = sha256(der, n); OPENSSL_free(der); fps.insert(hex(h.data(), 32)); }
    bool strong = false;
    STACK_OF(PKCS7_SIGNER_INFO)* sis = ok == 1 ? PKCS7_get_signer_info(p7) : nullptr;
    for (int i = 0; sis && i < sk_PKCS7_SIGNER_INFO_num(sis); i++) {
        X509_ALGOR* dig = nullptr; PKCS7_SIGNER_INFO_get0_algs(sk_PKCS7_SIGNER_INFO_value(sis, i), nullptr, &dig, nullptr);
        int nid = OBJ_obj2nid(dig->algorithm); if (nid == NID_sha256 || nid == NID_sha384 || nid == NID_sha512) strong = true;
    }
    if (signers) sk_X509_free(signers);
    PKCS7_free(p7);
    if (ok != 1) throw Err("entry.jar's signature doesn't verify");
    if (fps.size() != 1 || *fps.begin() != fingerprint) throw Err("entry.jar is signed by " + (fps.empty() ? std::string("nobody") : *fps.begin()) + ", not " + fingerprint);
    if (!strong && !allowSha1) throw Err("entry.jar is signed with SHA-1 only: refusing (allow SHA-1 only for a source you trust)");
    // 2. the .SF pins the manifest (whole, or entry.json's section), 3. the manifest pins entry.json
    std::string sfText(sfB.begin(), sfB.end()), mfText(mfB.begin(), mfB.end());
    JarManifest sfm = JarManifest::parse(sfText), mfm = JarManifest::parse(mfText);
    auto s256 = [](const std::string& s) { return b64std(sha256((const unsigned char*)s.data(), s.size())); };
    bool sfOk = sfm.main.count("SHA-256-Digest-Manifest") && sfm.main["SHA-256-Digest-Manifest"] == s256(mfText);
    if (!sfOk && sfm.attrs.count("entry.json") && sfm.attrs["entry.json"].count("SHA-256-Digest") && mfm.rawSection.count("entry.json"))
        sfOk = sfm.attrs["entry.json"]["SHA-256-Digest"] == s256(mfm.rawSection["entry.json"]);
    if (!sfOk) throw Err("entry.jar: the signature file doesn't match the manifest (SHA-256)");
    std::string ent(entB.begin(), entB.end());
    if (!mfm.attrs.count("entry.json") || mfm.attrs["entry.json"]["SHA-256-Digest"] != s256(ent)) throw Err("entry.jar: entry.json doesn't match its manifest digest");
    return ent;
}

inline bool safeRemote(const std::string& s) {   // no "..", no leading dots, no backslashes (ADR 0004 / BP-1)
    if (s.empty() || s.size() > 300) return false;
    size_t i = s[0] == '/' ? 1 : 0; bool segStart = true;
    for (; i < s.size(); i++) {
        char c = s[i];
        if (c == '/') { if (segStart) return false; segStart = true; continue; }
        if (segStart && !isalnum((unsigned char)c)) return false;
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
        segStart = false;
    }
    return !segStart;
}
inline Bytes fetchChecked(const std::string& url, const std::string& sha, uint64_t max) {
    auto r = Http::get(url, max, 1800000);
    if (!r.ok()) throw Err(url + ": " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
    Bytes b(r.body.begin(), r.body.end());
    if (!sha.empty()) { auto h = sha256(b.data(), b.size()); if (hex(h.data(), 32) != sha) throw Err(url + ": SHA-256 doesn't match the signed index"); }
    return b;
}
inline bool isHex64(const json& v) { if (!v.is_string()) return false; std::string s = v; if (s.size() != 64) return false; for (char c : s) if (!isxdigit((unsigned char)c) || isupper((unsigned char)c)) return false; return true; }
inline json loc(const json& o) {
    if (!o.is_object()) return o;
    if (o.contains("en-US")) return o["en-US"]; if (o.contains("en")) return o["en"];
    return o.empty() ? json() : o.begin().value();
}

struct ImportOpts { std::string url, fingerprint; int versions = 1; std::set<std::string> only; bool allowSha1 = false, allowStale = false; };
inline json importFdroid(Repo& r, ImportOpts o) {
    std::string base = o.url; while (!base.empty() && base.back() == '/') base.pop_back();
    std::string fp; for (char c : o.fingerprint) if (c != ':' && c != ' ') fp += (char)tolower((unsigned char)c);
    TempDir t; fs::path jar = t.p / "entry.jar";
    { Bytes b = fetchChecked(base + "/entry.jar", "", 1u << 20); std::ofstream(jar, std::ios::binary).write((const char*)b.data(), b.size()); }
    json entry = json::parse(verifiedEntryJson(jar, fp, o.allowSha1));
    // freshness: never older than one already seen from this source, nor older than its own maxAge
    if (!entry.contains("timestamp") || !entry["timestamp"].is_number()) throw Err("entry.json has no timestamp");
    int64_t ts = entry["timestamp"];
    if (!r.s.contains("fdroidSeen")) r.s["fdroidSeen"] = ojson::object();
    if (r.s["fdroidSeen"].contains(fp) && ts < r.s["fdroidSeen"][fp].get<int64_t>()) throw Err("entry.json is older than one already seen from this repo: refusing a rollback");
    int64_t maxAge = entry.value("maxAge", 0); int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (maxAge > 0 && nowMs - ts > maxAge * 86400000LL && !o.allowStale) throw Err("entry.json is older than its maxAge: refusing");
    json ix = entry.value("index", json::object());
    if (!safeRemote(ix.value("name", "")) || !isHex64(ix["sha256"])) throw Err("entry.json: bad index name or hash");
    uint64_t ixSize = ix.value("size", 0ull); if (!ixSize || ixSize > (256ull << 20)) throw Err("index-v2.json too large");
    std::string ixName = ix["name"]; if (ixName[0] == '/') ixName = ixName.substr(1);
    Bytes ib = fetchChecked(base + "/" + ixName, ix["sha256"], ixSize);
    json index = json::parse(std::string(ib.begin(), ib.end()));
    json pkgs = index.value("packages", json::object());
    size_t wanted = 0; for (auto& [k, p] : pkgs.items()) if ((o.only.empty() || o.only.count(k)) && !r.s["apps"].contains(k)) wanted++;
    if (wanted + r.s["apps"].size() > MAX_APPS) throw Err("importing would take this repo over " + std::to_string(MAX_APPS) + " apps");
    r.log("verified F-Droid v2 index (" + fp.substr(0, 16) + "...), " + std::to_string(pkgs.size()) + " apps");
    int added = 0; json skipped = json::array();
    for (auto& [pkg, p] : pkgs.items()) {
        if (!o.only.empty() && !o.only.count(pkg)) continue;
        if (!validPackage(pkg)) { skipped.push_back(pkg.substr(0, 60) + ": not a package name"); continue; }
        if (!p.is_object()) continue;
        json md = p.value("metadata", json::object());
        auto str = [](const json& v, size_t max) { return v.is_string() ? v.get<std::string>().substr(0, max) : std::string(); };
        ApkMeta meta; meta.name = str(loc(md.value("name", json())), 100); meta.summary = str(loc(md.value("summary", json())), 300);
        meta.description = str(loc(md.value("description", json())), 8000); meta.license = str(md.value("license", json()), 100);
        for (auto k : {"sourceCode", "webSite"}) { std::string u = str(md.value(k, json()), 500); if (meta.source.empty() && (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0)) meta.source = u; }
        json icon = loc(md.value("icon", json()));
        if (icon.is_object() && safeRemote(icon.value("name", "")) && isHex64(icon["sha256"])) {
            try {
                std::string in = icon["name"]; if (in[0] == '/') in = in.substr(1);
                Bytes b = fetchChecked(base + "/" + in, icon["sha256"], 1u << 20);
                fs::path f = t.p / (std::string("icon-") + pkg + (in.size() > 5 && in.substr(in.size() - 5) == ".webp" ? ".webp" : ".png"));
                std::ofstream(f, std::ios::binary).write((const char*)b.data(), b.size()); meta.iconFile = f.string();
            } catch (std::exception& e) { r.log(pkg + ": icon skipped (" + e.what() + ")"); }
        }
        json versions = p.value("versions", json::object());   // a named copy: items() of a temporary dangles
        std::vector<json> vs; for (auto& [h, v] : versions.items()) if (v.is_object() && v.contains("manifest") && v["manifest"].value("versionCode", -1) > 0) vs.push_back(v);
        std::sort(vs.begin(), vs.end(), [](const json& a, const json& b) { return a["manifest"]["versionCode"].get<int64_t>() > b["manifest"]["versionCode"].get<int64_t>(); });
        if ((int)vs.size() > o.versions) vs.resize(o.versions);
        std::reverse(vs.begin(), vs.end());
        for (auto& v : vs) {
            int64_t vc = v["manifest"]["versionCode"]; json f = v.value("file", json::object());
            json signer = v["manifest"].value("signer", json::object()).value("sha256", json::array());
            std::string sg = signer.is_array() && !signer.empty() && isHex64(signer[0]) ? signer[0].get<std::string>() : "";
            if (!safeRemote(f.value("name", "")) || !isHex64(f["sha256"]) || sg.empty()) { skipped.push_back(pkg + " " + std::to_string(vc) + ": bad file name, hash or signer"); continue; }
            bool have = false;
            if (r.s["apps"].contains(pkg)) for (auto& x : r.s["apps"][pkg]["versions"]) if (x["versionCode"] == vc && x.value("sha256", "") == f["sha256"].get<std::string>() && x.contains("signer")) have = true;
            if (have) continue;
            try {
                std::string fn = f["name"]; if (fn[0] == '/') fn = fn.substr(1);
                Bytes b = fetchChecked(base + "/" + fn, f["sha256"], 512ull << 20);
                fs::path apk = t.p / ("apk-" + std::to_string(vc) + ".apk"); std::ofstream(apk, std::ios::binary).write((const char*)b.data(), b.size());
                ApkMeta m = meta; m.expectSha256 = f["sha256"]; m.expectSigner = sg; m.expectId = pkg; m.expectVersionCode = vc;
                if (r.addApk(apk.string(), m)) added++;
                fs::remove(apk);
            } catch (std::exception& e) { skipped.push_back(pkg + " " + std::to_string(vc) + ": " + e.what()); r.log(pkg + " " + std::to_string(vc) + ": NOT imported: " + e.what()); }
        }
    }
    r.s["fdroidSeen"][fp] = ts; r.save();
    return json{{"ok", true}, {"added", added}, {"skipped", skipped}};
}
}  // namespace bp
