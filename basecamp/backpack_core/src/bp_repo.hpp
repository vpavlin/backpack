#pragma once
// bp_repo.hpp - a Backpack repo on disk, the same layout and JSON the CLI keeps (~/.backpack/<repo>/
// repo.json + key.pem), so a repo moves between `cli/backpack.mjs` and the Basecamp app unchanged.
// Every operation here BLOCKS (Storage uploads, HTTP): the module runs them on a worker thread.
#include "bp_apk.hpp"
#include "bp_http.hpp"
#include "bp_members.hpp"
#include "bp_lez.hpp"
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <set>
#include <ctime>
#include <filesystem>
#include <functional>

extern char** environ;

namespace bp {
namespace fs = std::filesystem;
using ojson = nlohmann::ordered_json;   // keeps the CLI's key order in repo.json and the index

constexpr int KEEP_VERSIONS = 3;
constexpr size_t MAX_APPS = 500;
constexpr uint64_t BIG_BLOCK = 524288;
inline uint64_t blockSizeFor(uint64_t n) { return n > (1u << 17) ? BIG_BLOCK : 65536; }
// No built-in Storage node: every repo names the node that hosts its files (its REST API). Phones fetch
// from it, so it must be reachable from where they are (see nodeWarning).

using Log = std::function<void(const std::string&)>;
using Progress = std::function<void(const std::string& what, int64_t done, int64_t total)>;

inline std::string nowIso() {
    auto t = std::chrono::system_clock::now(); auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count() % 1000;
    std::time_t tt = std::chrono::system_clock::to_time_t(t); std::tm g{}; gmtime_r(&tt, &g);
    char b[40]; std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &g); char o[48]; std::snprintf(o, sizeof o, "%s.%03lldZ", b, (long long)ms); return o;
}
inline std::string readFile(const fs::path& p) { std::ifstream f(p, std::ios::binary); if (!f) throw Err("can't read " + p.string()); return std::string(std::istreambuf_iterator<char>(f), {}); }
inline void writeFileAtomic(const fs::path& p, const std::string& s, mode_t mode = 0600) {
    fs::path t = p; t += ".tmp";
    { std::ofstream f(t, std::ios::binary | std::ios::trunc); if (!f) throw Err("can't write " + t.string()); f << s; if (!f) throw Err("write failed: " + t.string()); }
    ::chmod(t.c_str(), mode); fs::rename(t, p);
}
struct TempDir {
    fs::path p;
    TempDir() { std::string tpl = (fs::temp_directory_path() / "backpack-XXXXXX").string(); std::vector<char> b(tpl.begin(), tpl.end()); b.push_back(0); if (!mkdtemp(b.data())) throw Err("no temp dir"); p = b.data(); }
    ~TempDir() { std::error_code ec; fs::remove_all(p, ec); }
};

// Run argv (no shell) with extra environment; returns the exit code and output (stdout+stderr).
inline int run(const std::vector<std::string>& argv, const std::vector<std::string>& extraEnv, std::string& out) {
    int pipefd[2]; if (pipe(pipefd)) throw Err("pipe failed");
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1); posix_spawn_file_actions_adddup2(&fa, pipefd[1], 2); posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    std::vector<char*> av; for (auto& a : argv) av.push_back((char*)a.c_str()); av.push_back(nullptr);
    std::vector<std::string> envs; for (char** e = environ; e && *e; e++) envs.push_back(*e);
    for (auto& e : extraEnv) envs.push_back(e);
    std::vector<char*> ev; for (auto& e : envs) ev.push_back((char*)e.c_str()); ev.push_back(nullptr);
    pid_t pid; int rc = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), ev.data());
    posix_spawn_file_actions_destroy(&fa); close(pipefd[1]);
    if (rc) { close(pipefd[0]); out = std::string("can't run ") + argv[0]; return 127; }
    char buf[4096]; ssize_t n; while ((n = read(pipefd[0], buf, sizeof buf)) > 0) out.append(buf, n);
    close(pipefd[0]); int st = 0; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

// ---- Storage node REST ------------------------------------------------------------------------------
struct Node {
    std::string url;
    std::string upload(const std::string& file, const std::string& name, const std::string& mime, Progress prog = {}) {
        uint64_t sz = fs::file_size(file);
        std::string q = blockSizeFor(sz) == BIG_BLOCK ? "?blockSize=" + std::to_string(BIG_BLOCK) : "";
        auto r = Http::postFile(url + "/api/storage/v1/data" + q, file, mime, name, [&](int64_t s, int64_t t) { if (prog) prog("uploading " + name, s, t); });
        if (!r.ok()) throw Err("upload " + name + ": " + (r.error.empty() ? "HTTP " + std::to_string(r.status) + " " + r.body.substr(0, 200) : r.error));
        std::string cid = r.body; while (!cid.empty() && isspace((unsigned char)cid.back())) cid.pop_back();
        return cid;
    }
    Bytes download(const std::string& cid, uint64_t max = 0) {
        for (auto p : {"/api/storage/v1/data/" + cid, "/api/storage/v1/data/" + cid + "/network/stream"}) {
            auto r = Http::get(url + p, max, 600000);
            if (r.ok()) return Bytes(r.body.begin(), r.body.end());
            if (!r.error.empty() && r.error.rfind("over ", 0) == 0) throw Err(cid + ": " + r.error);
        }
        throw Err("node can't serve " + cid);
    }
    json info() {
        auto r = Http::get(url + "/api/storage/v1/debug/info", 1 << 20, 30000);
        if (!r.ok()) throw Err("Storage node " + url + ": " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
        json j = json::parse(r.body); return json{{"peerId", j.value("id", "")}, {"addrs", j.value("addrs", json::array())}, {"spr", j.value("spr", "")}};
    }
};

// A Storage node address phones elsewhere probably can't reach: loopback, private or link-local ranges,
// IPv6 ULA (mesh overlays), .local/.lan names. Empty string = looks public.
inline std::string nodeWarning(const std::string& url) {
    auto p = url.find("://"); if (p == std::string::npos) return "not an http(s) address";
    std::string h = url.substr(p + 3); h = h.substr(0, h.find('/'));
    if (!h.empty() && h[0] == '[') h = h.substr(1, h.find(']') - 1); else h = h.substr(0, h.rfind(':') == std::string::npos ? h.size() : h.rfind(':'));
    for (auto& c : h) c = (char)tolower((unsigned char)c);
    auto starts = [&](const char* x) { return h.rfind(x, 0) == 0; };
    auto ends = [&](const char* x) { std::string e(x); return h.size() >= e.size() && h.compare(h.size() - e.size(), e.size(), e) == 0; };
    if (h == "localhost" || starts("127.") || h == "::1") return "this computer only";
    if (starts("10.") || starts("192.168.") || starts("169.254.") || ends(".local") || ends(".lan") || ends(".home.arpa")) return "a local network address";
    if (starts("172.")) { int b = std::atoi(h.c_str() + 4); if (b >= 16 && b <= 31) return "a local network address"; }
    if (starts("100.")) { int b = std::atoi(h.c_str() + 4); if (b >= 64 && b <= 127) return "a carrier/VPN (CGNAT) address"; }
    if (starts("fd") || starts("fc") || starts("fe80")) return "a private IPv6 (mesh/VPN) address";
    return "";
}

// ---- a repo ----------------------------------------------------------------------------------------
// Copy without some keys (works on every nlohmann version's ordered_map).
inline ojson without(const ojson& o, std::initializer_list<const char*> keys) {
    ojson n = ojson::object();
    for (auto& [k, v] : o.items()) { bool drop = false; for (auto x : keys) if (k == x) drop = true; if (!drop) n[k] = v; }
    return n;
}
// A Java package name: letters/digits/_ segments, each starting with a letter, at least two (BP-7).
inline bool validPackage(const std::string& p) {
    if (p.empty() || p.size() > 200) return false; bool start = true; int segs = 1;
    for (char c : p) {
        if (c == '.') { if (start) return false; start = true; segs++; continue; }
        if (start && !isalpha((unsigned char)c)) return false;
        if (!(isalnum((unsigned char)c) || c == '_')) return false;
        start = false;
    }
    return !start && segs >= 2;
}
// What a source index (F-Droid import) says about an APK, checked against the file.
struct ApkMeta { std::string name, summary, description, license, source, iconFile, expectSha256, expectSigner, expectId; int64_t expectVersionCode = -1; };
struct Repo {
    fs::path dir; ojson s; Log log = [](const std::string&) {}; Progress progress = {};
    fs::path stateFile() const { return dir / "repo.json"; }
    fs::path keyFile() const { return dir / "key.pem"; }
    static Repo load(const fs::path& d) { Repo r; r.dir = d; r.s = ojson::parse(readFile(r.stateFile())); return r; }
    void save() { writeFileAtomic(stateFile(), s.dump(2)); }
    Key key() const { return Key::fromPem(readFile(keyFile())); }
    std::string pub() const { return key().pub(); }
    // A member repo (ADR 0013) reads with the current epoch's key; a private repo with its fixed read key.
    bool isMembers() const { return s.contains("members") && s["members"].is_object(); }
    std::string readKey() const {
        if (isMembers()) return s["members"].value("epochKey", std::string());
        return s.contains("readKey") && s["readKey"].is_string() ? s["readKey"].get<std::string>() : "";
    }
    std::string node() const {
        std::string n = s.contains("node") && s["node"].is_string() ? s["node"].get<std::string>() : "";
        if (n.empty()) throw Err("This repo has no Storage node yet: set one in its settings (a node phones can reach)");
        return n;
    }
    std::string pointerId() const { return readKey().empty() || isMembers() ? pub() : topicId(readKey()); }

    static Repo create(const fs::path& d, const std::string& name, bool priv, const std::string& node, bool members = false) {
        if (fs::exists(d / "repo.json")) throw Err("a repo already exists in " + d.string());
        fs::create_directories(d); ::chmod(d.c_str(), 0700);
        Repo r; r.dir = d; Key k = Key::generate(); writeFileAtomic(r.keyFile(), k.pem());
        r.s = ojson{{"name", name}, {"node", node}, {"seq", 0}, {"pointers", {{"get", json::array()}, {"put", json::array()}}}, {"apps", ojson::object()}};
        // a first epoch key at once, so APKs added before the first publish are uploaded encrypted
        if (members) r.s["members"] = ojson{{"epochLenMs", 3600000}, {"devices", json::array()}, {"epochKey", newKey()}};
        else if (priv) r.s["readKey"] = newKey();
        r.save(); return r;
    }

    // Private (ADR 0009): a new read key; pointer addresses naming the old id move to the new one.
    void makePrivate(bool rotate) {
        if (isMembers()) throw Err("a member repo already gets a new key every epoch");
        if (rotate && readKey().empty()) throw Err("this repo isn't private");
        std::string oldId = pointerId(); s["readKey"] = newKey(); std::string newId = topicId(readKey());
        auto swap = [&](std::string u) {
            for (auto [a, b] : {std::pair<std::string, std::string>{oldId + ".json", newId + ".json"}, {"$BACKPACK_KEY", "$BACKPACK_ID"}}) {
                size_t p; while ((p = u.find(a)) != std::string::npos) u.replace(p, a.size(), b);
            }
            return u; };
        for (auto side : {"get", "put"}) for (auto& v : s["pointers"][side]) v = swap(v.get<std::string>());
        save();
    }

    struct Up { std::string cid, fk; uint64_t csize = 0; };
    Up uploadMaybeEnc(const std::string& file, const std::string& name, const std::string& mime) {
        Node n{node()};
        if (readKey().empty()) return {n.upload(file, name, mime, progress), "", 0};
        TempDir t; std::string enc = (t.p / "enc.bpe").string();
        EncResult e = encryptFile(file, enc);
        return {n.upload(enc, name + ".bpe", "application/octet-stream", progress), e.key, e.size};
    }

    // An APK into the repo. expect* come from a verified source index (F-Droid import).
    bool addApk(const std::string& apk) { return addApk(apk, ApkMeta{}); }
    bool addApk(const std::string& apk, const ApkMeta& meta) {
        ApkInfo m = readApk(apk);
        if (!validPackage(m.id)) throw Err(m.id + ": not a valid package name");
        std::string bytes = readFile(apk); std::string sha = sha256hex(bytes);
        if (!meta.expectSha256.empty() && meta.expectSha256 != sha) throw Err(m.id + ": SHA-256 differs from the source index");
        if (!meta.expectId.empty() && meta.expectId != m.id) throw Err("the APK is " + m.id + ", the source index says " + meta.expectId);
        if (meta.expectVersionCode >= 0 && meta.expectVersionCode != m.versionCode) throw Err(m.id + ": versionCode differs from the source index");
        if (!meta.expectSigner.empty() && meta.expectSigner != m.signer) throw Err(m.id + ": signer differs from the source index");
        auto& apps = s["apps"];
        if (!apps.contains(m.id)) apps[m.id] = ojson{{"id", m.id}, {"label", m.label}, {"icon", nullptr}, {"versions", json::array()}};
        auto& app = apps[m.id];
        for (auto& v : app["versions"]) if (v["versionCode"] == m.versionCode && v.value("sha256", "") == sha && v.contains("signer")) { log(m.id + " " + m.versionName + " already in the repo"); return false; }
        for (auto& v : app["versions"]) if (v.contains("signer") && v["versionCode"] != m.versionCode && v["signer"] != m.signer)
            throw Err(m.id + ": signed by " + m.signer.substr(0, 8) + "..., earlier versions by " + v["signer"].get<std::string>().substr(0, 8) + "...; refusing");
        if (apps.size() > MAX_APPS) throw Err("over " + std::to_string(MAX_APPS) + " apps: more than a phone accepts");
        log(m.id + " " + m.versionName + " (" + std::to_string(m.versionCode) + "): uploading...");
        Up up = uploadMaybeEnc(apk, m.id + "_" + std::to_string(m.versionCode) + ".apk", "application/vnd.android.package-archive");
        // icon: given (F-Droid), or the APK's own raster icon
        if (!meta.iconFile.empty() || (!m.iconEntry.empty() && app["icon"].is_null())) {
            TempDir t; std::string iconFile = meta.iconFile;
            if (iconFile.empty()) {
                Zip z(apk); const Zip::Entry* e = z.find(m.iconEntry);
                if (e) { Bytes ib = z.read(*e, 1u << 20); iconFile = (t.p / ("icon" + std::string(m.iconEntry.size() > 5 && m.iconEntry.substr(m.iconEntry.size() - 5) == ".webp" ? ".webp" : ".png"))).string(); std::ofstream(iconFile, std::ios::binary).write((const char*)ib.data(), ib.size()); }
            }
            if (!iconFile.empty()) {
                bool webp = iconFile.size() > 5 && iconFile.substr(iconFile.size() - 5) == ".webp";
                Up iu = uploadMaybeEnc(iconFile, m.id + (webp ? ".webp" : ".png"), webp ? "image/webp" : "image/png");
                app["icon"] = iu.cid;
                if (!iu.fk.empty()) { app["iconKey"] = iu.fk; app["iconCsize"] = iu.csize; } else app = without(app, {"iconKey", "iconCsize"});
            }
        }
        app["label"] = !meta.name.empty() ? meta.name : app.value("label", m.label);
        if (!meta.summary.empty()) app["summary"] = meta.summary;
        if (!meta.description.empty()) app["description"] = meta.description;
        if (!meta.license.empty()) app["license"] = meta.license;
        if (!meta.source.empty()) app["source"] = meta.source;
        ojson v{{"versionCode", m.versionCode}, {"versionName", m.versionName}, {"size", bytes.size()}, {"sha256", sha}, {"signer", m.signer}, {"cid", up.cid},
                {"blockSize", blockSizeFor(up.fk.empty() ? bytes.size() : up.csize)}, {"minSdk", m.minSdk}, {"abis", m.abis}, {"added", nowIso()}};
        if (!up.fk.empty()) { v["fk"] = up.fk; v["csize"] = up.csize; }
        std::vector<ojson> vs{v};
        for (auto& o : app["versions"]) if (o["versionCode"] != m.versionCode) vs.push_back(o);
        std::sort(vs.begin(), vs.end(), [](const ojson& a, const ojson& b) { return a["versionCode"].get<int64_t>() > b["versionCode"].get<int64_t>(); });
        if (vs.size() > (size_t)KEEP_VERSIONS) vs.resize(KEEP_VERSIONS);
        app["versions"] = ojson(vs); save();
        log(m.id + " " + m.versionName + " -> " + up.cid);
        return true;
    }

    // Private repo: anything still in the clear is downloaded, checked, encrypted and uploaded again.
    void encryptLeftovers() {
        Node n{node()};
        for (auto& [id, a] : s["apps"].items()) {
            for (auto& v : a["versions"]) {
                if (v.contains("fk")) continue;
                log(id + " " + v.value("versionName", "") + ": encrypting...");
                Bytes b = n.download(v["cid"]);
                auto h = sha256(b.data(), b.size()); if (hex(h.data(), 32) != v.value("sha256", "")) throw Err(id + ": the node's copy doesn't match the pinned SHA-256");
                TempDir t; fs::path f = t.p / "plain"; std::ofstream(f, std::ios::binary).write((const char*)b.data(), b.size());
                Up up = uploadMaybeEnc(f.string(), id + "_" + std::to_string(v["versionCode"].get<int64_t>()) + ".apk", "application/octet-stream");
                v["cid"] = up.cid; v["fk"] = up.fk; v["csize"] = up.csize; v["blockSize"] = blockSizeFor(up.csize); save();
            }
            if (!a["icon"].is_null() && !a.contains("iconKey")) {
                Bytes b = n.download(a["icon"], 1u << 20);
                TempDir t; fs::path f = t.p / "icon"; std::ofstream(f, std::ios::binary).write((const char*)b.data(), b.size());
                Up up = uploadMaybeEnc(f.string(), id + ".icon", "application/octet-stream");
                a["icon"] = up.cid; a["iconKey"] = up.fk; a["iconCsize"] = up.csize; save();
            }
        }
    }

    // ---- members (ADR 0013) ----
    void addMember(const std::string& link) {
        if (!isMembers()) throw Err("this isn't a member repo");
        MemberLink m = parseMemberLink(link);
        for (auto& d : s["members"]["devices"]) if (d["pk"] == m.pk) throw Err("that device is already a member");
        if (s["members"]["devices"].size() >= 2000) throw Err("too many member devices");
        s["members"]["devices"].push_back(ojson{{"pk", m.pk}, {"name", m.name.empty() ? std::string("device") : m.name}, {"added", nowIso()}});
        save();
    }
    void removeMember(const std::string& pk) {
        if (!isMembers()) throw Err("this isn't a member repo");
        std::vector<ojson> keep; for (auto& d : s["members"]["devices"]) if (d["pk"] != pk) keep.push_back(d);
        s["members"]["devices"] = ojson(keep); save();
    }
    // ---- LEZ memberships (ADR 0010): paid-up memberships' devices are members too ----
    static int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    void setLez(const json& cfg) {
        if (!isMembers()) throw Err("this isn't a member repo");
        if (cfg.is_null() || (cfg.is_object() && cfg.empty())) { s["members"].erase("lez"); s["members"].erase("lezMembers"); save(); return; }
        std::string w = cfg.value("wallet", ""), q = cfg.value("sequencer", ""), pr = cfg.value("program", ""), lib = cfg.value("lib", "");
        if (w.empty() || !fs::is_directory(w)) throw Err("the LEZ wallet folder doesn't exist");
        if (q.rfind("http://", 0) != 0 && q.rfind("https://", 0) != 0) throw Err("the sequencer is an http(s) address");
        if (pr.size() != 64 || pr.find_first_not_of("0123456789abcdef") != std::string::npos) throw Err("the program id is 64 lowercase hex characters");
        // the publisher's account: admin of this repo's config, and where the payments go
        std::string admin = cfg.value("admin", ""), price = cfg.contains("price") ? (cfg["price"].is_string() ? cfg["price"].get<std::string>() : cfg["price"].dump()) : "";
        int64_t period = cfg.value("periodMs", (int64_t)2592000000LL);   // a month (30 days) unless said otherwise
        accountToHex(admin);   // throws if malformed
        if (price.empty() || price.size() > 30 || price.find_first_not_of("0123456789") != std::string::npos) throw Err("the price is a whole number");
        if (period < 60000) throw Err("the period is at least a minute");
        s["members"]["lez"] = ojson{{"wallet", w}, {"sequencer", q}, {"program", pr}, {"lib", lib}, {"auto", cfg.value("auto", true)}, {"admin", admin}, {"price", price}, {"periodMs", period}}; save();
    }
    // the settings + this repo's config id, as bp_lez.hpp takes them
    json lezCfg() const { json c = json::parse(s["members"]["lez"].dump()); c["config"] = lezConfigId(pub()); return c; }
    // create the repo's config on the chain, or bring its price/period in line with the settings
    json lezEnsure() {
        if (!usesLez()) throw Err("this repo doesn't use LEZ memberships");
        const auto& z = s["members"]["lez"];
        return lezEnsureConfig(lezCfg(), z.value("admin", std::string()), z.value("price", std::string()), z.value("periodMs", (int64_t)0), log);
    }
    ojson lezAccess() const {
        const auto& z = s["members"]["lez"];
        ojson a{{"via", "lez"}, {"program", z["program"]}, {"sequencer", z["sequencer"]}, {"config", lezConfigId(pub())}};
        if (s["members"].contains("lezTerms")) { a["price"] = s["members"]["lezTerms"]["price"]; a["periodMs"] = s["members"]["lezTerms"]["periodMs"]; }
        return a;
    }
    // After a read: does the chain hold a change that needs a publish? Someone joined (a paid device not in
    // the last key bundle) or a device was taken off a membership: yes. A membership only running out: no,
    // its devices just miss the next key, whenever the next publish happens anyway.
    std::string lezChangeNeedsPublish() const {
        if (!usesLez()) return "";
        std::set<std::string> published, registered, manual;
        for (auto& p : s["members"].value("publishedPks", ojson::array())) published.insert(p.get<std::string>());
        for (auto& d : s["members"]["devices"]) manual.insert(d["pk"].get<std::string>());
        for (auto& lm : s["members"].value("lezMembers", ojson::array())) for (auto& d : lm["devices"]) registered.insert(d.get<std::string>());
        size_t added = 0, removed = 0;
        for (auto& p : memberPks()) if (!published.count(p)) added++;
        for (auto& p : published) if (!registered.count(p) && !manual.count(p)) removed++;
        if (!added && !removed) return "";
        return std::to_string(added) + " device(s) joined, " + std::to_string(removed) + " removed";
    }
    bool usesLez() const { return isMembers() && s["members"].contains("lez") && s["members"]["lez"].is_object(); }
    // publish by itself when someone joins or a device is taken off (default on)
    bool lezAuto() const { return usesLez() && s["members"]["lez"].value("auto", true); }
    // Read the memberships from the chain; keep the last good list if the chain can't be reached.
    json syncLez() {
        if (!usesLez()) throw Err("this repo doesn't use LEZ memberships");
        LezSnapshot snap = lezRead(lezCfg());
        ojson list = ojson::array(); int64_t now = nowMs(); size_t active = 0, devs = 0;
        for (auto& lm : snap.members) {
            ojson ds = ojson::array(); for (auto& d : lm.devices) ds.push_back(d);
            list.push_back(ojson{{"owner", lm.owner}, {"devices", ds}, {"paidThroughMs", lm.paidThroughMs}});
            if (lm.paidThroughMs > now) { active++; devs += lm.devices.size(); }
        }
        s["members"]["lezMembers"] = list; s["members"]["lezSyncedAt"] = nowIso();
        // what a membership costs, as the chain says: published in the pointer for people deciding to join
        const json& c = snap.config;
        std::string price = c.contains("price_per_epoch") ? (c["price_per_epoch"].is_string() ? c["price_per_epoch"].get<std::string>() : c["price_per_epoch"].dump()) : "";
        s["members"]["lezTerms"] = ojson{{"price", price}, {"periodMs", c.value("epoch_len_ms", (int64_t)0)}};
        save();
        log("LEZ: " + std::to_string(snap.members.size()) + " membership(s), " + std::to_string(active) + " paid up, " + std::to_string(devs) + " device(s)");
        return json{{"memberships", snap.members.size()}, {"active", active}, {"devices", devs}, {"config", snap.config}};
    }
    // Manual devices + devices of memberships paid through now (expiry is checked here, at publish time).
    std::vector<std::string> memberPks() const {
        std::vector<std::string> pks; for (auto& d : s["members"]["devices"]) pks.push_back(d["pk"]);
        int64_t now = nowMs();
        if (s["members"].contains("lezMembers")) for (auto& lm : s["members"]["lezMembers"]) if (lm.value("paidThroughMs", (int64_t)0) > now) for (auto& d : lm["devices"]) pks.push_back(d);
        std::sort(pks.begin(), pks.end()); pks.erase(std::unique(pks.begin(), pks.end()), pks.end());
        return pks;
    }

    // A new epoch key and key bundle when the epoch moved on or the members changed.
    void rotateMembersIfNeeded(const Key& k, Node& n) {
        if (usesLez()) { try { syncLez(); } catch (std::exception& e) { log(std::string("LEZ unreachable, using the last list: ") + e.what()); } }
        auto& m = s["members"];
        int64_t len =std::max<int64_t>(60000, m.value("epochLenMs", 3600000LL));
        uint64_t epoch = (uint64_t)(nowMs() / len);
        std::vector<std::string> pks = memberPks();
        std::string devHash; for (auto& p : pks) devHash += p; devHash = sha256hex(devHash);
        if (m.contains("epochKey") && m.value("epoch", (uint64_t)0) == epoch && m.value("devHash", "") == devHash) return;
        std::string ek = newKey(); std::string K = k.pub();
        ojson wrapped = ojson::object(); for (auto& p : pks) wrapped[p] = wrapKey(ek, p, K, epoch);
        std::string bundle = seal("keys", ojson{{"key", K}, {"epoch", epoch}, {"wrapped", wrapped}}, k);
        TempDir t; fs::path f = t.p / "backpack-keys.json"; std::ofstream(f, std::ios::binary) << bundle;
        log("members: new key for epoch " + std::to_string(epoch) + ", " + std::to_string(pks.size()) + " device(s)");
        std::string cid = n.upload(f.string(), "backpack-keys.json", "application/json");
        ojson pub = ojson::array(); for (auto& p : pks) pub.push_back(p);
        m["epoch"] = epoch; m["epochKey"] = ek; m["devHash"] = devHash; m["keysCid"] = cid; m["publishedPks"] = pub; save();
    }

    std::string linkFor(const std::string& indexCid, const json& nodeInfo) const {
        auto enc = [](const std::string& v) { std::string o; char h[4]; for (unsigned char c : v) { if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += c; else if (c == ' ') o += '+'; else { std::snprintf(h, sizeof h, "%%%02X", c); o += h; } } return o; };
        std::string q = "i=" + enc(indexCid) + "&k=" + enc(pub()) + "&n=" + enc(s.value("name", "")) + "&p=" + enc(nodeInfo.value("peerId", ""));
        for (auto& a : nodeInfo.value("addrs", json::array())) q += "&a=" + enc(a.get<std::string>());
        q += "&s=" + enc(nodeInfo.value("spr", ""));
        for (auto& u : s["pointers"]["get"]) q += "&u=" + enc(u.get<std::string>());
        if (isMembers()) q += "&m=1";   // the key comes from the key bundle, never the link
        else if (!readKey().empty()) q += "&x=" + enc(readKey());
        return "backpack://repo?" + q;
    }

    // Publish: seq+1, signed (and, private, encrypted) index to Storage, the pointer to every target.
    json publish(bool force) {
        Node n{node()}; json ni = n.info();
        if (isMembers()) rotateMembersIfNeeded(key(), n);
        if (!readKey().empty()) encryptLeftovers();
        if (std::string w = nodeWarning(node()); !w.empty())
            log("Note: the Storage node is " + w + ": phones outside that network can't fetch this repo's files");
        ojson nodes = ojson::array(); nodes.push_back(ojson{{"peerId", ni["peerId"]}, {"addrs", ni["addrs"]}, {"spr", ni["spr"]}});
        std::vector<ojson> apps; for (auto& [k, a] : s["apps"].items()) apps.push_back(a);
        std::sort(apps.begin(), apps.end(), [](const ojson& a, const ojson& b) { return a.value("label", "") < b.value("label", ""); });
        if (apps.size() > MAX_APPS) throw Err("more apps than a phone accepts");
        ojson sortedAddrs = ni["addrs"]; std::sort(sortedAddrs.begin(), sortedAddrs.end());
        ojson hashIn{{"name", s["name"]}, {"readKey", readKey().empty() ? ojson(nullptr) : ojson(readKey())}, {"nodes", ojson::array({ojson{{"peerId", ni["peerId"]}, {"addrs", sortedAddrs}}})}, {"pointers", s["pointers"]["get"]}, {"apps", apps}};
        if (usesLez()) hashIn["access"] = lezAccess();   // new terms are news for people deciding to join
        std::string contentHash = sha256hex(hashIn.dump());
        if (!force && s.value("lastContentHash", "") == contentHash) return json{{"ok", true}, {"unchanged", true}, {"seq", s.value("seq", 0)}};
        int64_t seq = s.value("seq", 0) + 1; std::string created = nowIso(); Key k = key(); std::string K = k.pub();
        ojson body{{"v", 2}, {"name", s["name"]}, {"key", K}, {"seq", seq}, {"created", created}, {"nodes", nodes}, {"pointers", s["pointers"]["get"]}, {"apps", apps}};
        std::string index = seal("index", body, k, readKey());
        if (index.size() > (1u << 20)) throw Err("the signed index is over 1 MB, more than a phone accepts");
        TempDir t; fs::path ixf = t.p / "backpack-index.json"; std::ofstream(ixf, std::ios::binary) << index;
        log("uploading the index...");
        std::string cid = n.upload(ixf.string(), "backpack-index.json", "application/json");
        ojson pbody{{"key", K}, {"seq", seq}, {"index", cid}, {"created", created}, {"nodes", nodes}};
        // a member repo's pointer is public and names the key bundle (ADR 0013)
        if (isMembers()) { pbody["keys"] = s["members"]["keysCid"]; pbody["epoch"] = s["members"]["epoch"]; }
        // a paid repo says where and for how much one joins (public: the membership is on a public chain anyway)
        if (usesLez()) pbody["access"] = lezAccess();
        std::string pointer = seal("pointer", pbody, k, isMembers() ? std::string() : readKey());
        fs::path pf = dir / "latest.json"; writeFileAtomic(pf, pointer, 0644);
        s["seq"] = seq; s["lastIndex"] = cid; s["lastContentHash"] = contentHash;
        json pushed = json::array();
        for (auto& tgt : s["pointers"]["put"]) {
            std::string target = tgt; std::string out; int rc = 0;
            try {
                if (target.rfind("rsync:", 0) == 0) rc = run({"rsync", "-q", "--chmod=F644", pf.string(), target.substr(6)}, {}, out);
                else if (target.rfind("exec:", 0) == 0) rc = run({"sh", "-c", target.substr(5)}, {"BACKPACK_KEY=" + K, "BACKPACK_ID=" + pointerId(), "BACKPACK_POINTER=" + pf.string()}, out);
                else if (target.rfind("put:", 0) == 0) { auto r = Http::put(target.substr(4), pointer); rc = r.ok() ? 0 : 1; out = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error; }
                else { rc = 2; out = "unknown target"; }
            } catch (std::exception& e) { rc = 1; out = e.what(); }
            pushed.push_back(json{{"target", target.substr(0, target.find(':'))}, {"ok", rc == 0}, {"detail", out.substr(0, 300)}});
            log(std::string("pointer -> ") + target.substr(0, target.find(':')) + (rc == 0 ? ": ok" : ": FAILED " + out.substr(0, 200)));
        }
        std::string link = linkFor(cid, ni); s["lastLink"] = link; save();
        return json{{"ok", true}, {"seq", seq}, {"index", cid}, {"pointers", pushed}};
    }

    // Check the repo as a phone would: every pointer source, the index, and (deep) every APK.
    json verify(bool deep) {
        json out{{"ok", true}, {"checks", json::array()}};
        auto add = [&](bool ok, const std::string& what) { out["checks"].push_back(json{{"ok", ok}, {"what", what}}); if (!ok) out["ok"] = false; log((ok ? "ok: " : "FAIL: ") + what); };
        std::string K = pub(), R = readKey(); Node n{node()};
        int64_t bestSeq = -1; std::string bestIndex = s.value("lastIndex", "");
        for (auto& u : s["pointers"]["get"]) {
            try {
                auto r = Http::get(u, 8192, 30000); if (!r.ok()) throw Err(r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error);
                json b = open(json::parse(r.body), K, "pointer", R);
                add(true, "pointer " + u.get<std::string>() + ": seq " + std::to_string(b.value("seq", 0)));
                if (b.value("seq", 0) > bestSeq) { bestSeq = b.value("seq", 0); bestIndex = b.value("index", ""); }
            } catch (std::exception& e) { add(false, "pointer " + u.get<std::string>() + ": " + e.what()); }
        }
        json body;
        try {
            Bytes raw = n.download(bestIndex, 1u << 20);
            body = open(json::parse(std::string(raw.begin(), raw.end())), K, "index", R);
            add(true, "index " + bestIndex + ": seq " + std::to_string(body.value("seq", 0)) + ", " + std::to_string(body.value("apps", json::array()).size()) + " apps");
            if (bestSeq >= 0 && body.value("seq", 0) != bestSeq) add(false, "index seq differs from the pointer's");
        } catch (std::exception& e) { add(false, "index " + bestIndex + ": " + e.what()); return out; }
        if (!deep) return out;
        for (auto& a : body["apps"]) for (auto& v : a["versions"]) {
            std::string what = a.value("id", "?") + " " + v.value("versionName", "");
            try {
                Bytes b = n.download(v["cid"]);
                if (!R.empty()) { if (!v.contains("fk")) throw Err("private repo, but no file key"); if (b.size() != v.value("csize", 0ull)) throw Err("encrypted size differs"); b = decryptBuffer(b, v["fk"]); }
                if (b.size() != v.value("size", 0ull)) throw Err("size differs");
                auto h = sha256(b.data(), b.size()); if (hex(h.data(), 32) != v.value("sha256", "")) throw Err("SHA-256 mismatch");
                TempDir t; fs::path f = t.p / "a.apk"; std::ofstream(f, std::ios::binary).write((const char*)b.data(), b.size());
                ApkInfo m = readApk(f.string());
                if (m.signer != v.value("signer", "")) throw Err("signer differs from the pinned one");
                if (m.id != a.value("id", "") || m.versionCode != v.value("versionCode", 0ll)) throw Err("package or versionCode differs from the index");
                add(true, what + (R.empty() ? "" : ": decrypted") + ", sha256, signer, package ok");
            } catch (std::exception& e) { add(false, what + ": " + e.what()); }
        }
        return out;
    }

    // What the UI shows for a repo (never the read key itself, see linkSecret()).
    json summary() const {
        json apps = json::array();
        for (auto& [id, a] : s["apps"].items()) {
            json vs = json::array(); for (auto& v : a["versions"]) vs.push_back(json{{"versionCode", v["versionCode"]}, {"versionName", v.value("versionName", "")}, {"size", v.value("size", 0)}, {"added", v.value("added", "")}, {"encrypted", v.contains("fk")}});
            apps.push_back(json{{"id", id}, {"label", a.value("label", id)}, {"summary", a.value("summary", "")}, {"versions", vs}, {"hasIcon", !a["icon"].is_null()}});
        }
        json gets = json::array(); for (auto& u : s["pointers"]["get"]) gets.push_back(u.get<std::string>());
        json puts = json::array(); for (auto& u : s["pointers"]["put"]) { std::string t = u; puts.push_back(t.substr(0, t.find(':'))); }
        return json{{"slug", dir.filename().string()}, {"name", s.value("name", "")}, {"key", pub()}, {"seq", s.value("seq", 0)}, {"private", !readKey().empty() || isMembers()}, {"members", isMembers()}, {"memberDevices", isMembers() ? json::parse(s["members"]["devices"].dump()) : json::array()}, {"epoch", isMembers() ? json(s["members"].value("epoch", (uint64_t)0)) : json()},
                    {"lez", usesLez() ? json{{"sequencer", s["members"]["lez"].value("sequencer", "")}, {"program", s["members"]["lez"].value("program", "")}, {"wallet", s["members"]["lez"].value("wallet", "")}, {"admin", s["members"]["lez"].value("admin", "")}, {"price", s["members"]["lez"].value("price", "")}, {"periodMs", s["members"]["lez"].value("periodMs", (int64_t)0)}, {"syncedAt", s["members"].value("lezSyncedAt", "")}, {"auto", lezAuto()}, {"terms", json::parse(s["members"].value("lezTerms", ojson::object()).dump())}, {"members", json::parse(s["members"].value("lezMembers", ojson::array()).dump())}} : json()},
                    {"pointerId", pointerId()}, {"node", s.value("node", std::string())}, {"nodeWarning", s.value("node", std::string()).empty() ? std::string("no Storage node set") : nodeWarning(s.value("node", std::string()))}, {"apps", apps}, {"pointersGet", gets}, {"pointersPut", puts}, {"hasLink", s.contains("lastLink")}, {"lastIndex", s.value("lastIndex", "")}};
    }
};
}  // namespace bp
