#pragma once
// bp_lez.hpp - LEZ memberships (backpack-access, ADR 0010) as a source of member devices (ADR 0013).
// The backpack_access_ffi library is loaded at run time (dlopen), so backpack_core works without it;
// only a repo set to use LEZ needs it. One program deployment serves many repos: each repo has its own
// config (price, period, treasury, member registry) keyed by its config id = the repo's publisher key.
#include "bp_crypto.hpp"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace bp {
struct LezFfi {
    void* h = nullptr;
    void* (*open)(const char*) = nullptr;
    void (*close)(void*) = nullptr;
    char* (*list)(const void*, const char*) = nullptr;
    char* (*getMany)(const void*, const char*) = nullptr;
    char* (*getConfig)(const void*, const char*) = nullptr;
    char* (*cfgCreate)(const void*, const char*) = nullptr;
    char* (*cfgUpdate)(const void*, const char*) = nullptr;
    char* (*reg)(const void*, const char*) = nullptr;
    char* (*renew)(const void*, const char*) = nullptr;
    char* (*addDev)(const void*, const char*) = nullptr;
    char* (*rmDev)(const void*, const char*) = nullptr;
    char* (*getOne)(const void*, const char*) = nullptr;
    char* (*balance)(const void*, const char*) = nullptr;
    void (*freeStr)(char*) = nullptr;
    // The copy shipped in the module package, next to this plugin's .so ("" if there is none)
    static std::string bundled() {
        Dl_info di{};
        if (!dladdr((void*)&LezFfi::bundled, &di) || !di.dli_fname) return "";
        std::string d(di.dli_fname); size_t s = d.rfind('/'); if (s == std::string::npos) return "";
        std::string p = d.substr(0, s) + "/libbackpack_access_ffi.so";
        return access(p.c_str(), R_OK) == 0 ? p : "";
    }
    // lib: an explicit path, else $BACKPACK_LEZ_LIB, else the bundled copy, else the loader's search path
    static LezFfi& get(const std::string& lib) {
        static LezFfi f; static std::string loaded;
        std::string p = !lib.empty() ? lib : std::getenv("BACKPACK_LEZ_LIB") ? std::getenv("BACKPACK_LEZ_LIB") : bundled();
        if (p.empty()) p = "libbackpack_access_ffi.so";
        if (f.h && loaded == p) return f;
        void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { const char* e = dlerror(); throw Err(std::string("can't load the LEZ membership library (") + p + "): " + (e ? e : "?")); }
        LezFfi n; n.h = h;
        n.open = (void* (*)(const char*))dlsym(h, "backpack_access_config_open");
        n.close = (void (*)(void*))dlsym(h, "backpack_access_config_close");
        n.list = (char* (*)(const void*, const char*))dlsym(h, "backpack_access_member_list");
        n.getMany = (char* (*)(const void*, const char*))dlsym(h, "backpack_access_member_get_many");
        n.getConfig = (char* (*)(const void*, const char*))dlsym(h, "backpack_access_get_config");
        n.freeStr = (void (*)(char*))dlsym(h, "backpack_access_free_string");
        using W = char* (*)(const void*, const char*);
        n.reg = (W)dlsym(h, "backpack_access_member_register"); n.renew = (W)dlsym(h, "backpack_access_member_renew");
        n.addDev = (W)dlsym(h, "backpack_access_member_add_device"); n.rmDev = (W)dlsym(h, "backpack_access_member_remove_device");
        n.getOne = (W)dlsym(h, "backpack_access_member_get"); n.balance = (W)dlsym(h, "backpack_access_treasury_balance");
        n.cfgCreate = (W)dlsym(h, "backpack_access_config_create"); n.cfgUpdate = (W)dlsym(h, "backpack_access_config_update");
        if (!n.open || !n.close || !n.list || !n.getMany || !n.getConfig || !n.freeStr) { dlclose(h); throw Err("the LEZ membership library lacks the expected functions"); }
        // per-repo configs (multi-publisher program): an older library speaks a different API
        if (!n.cfgCreate || !n.cfgUpdate) { dlclose(h); throw Err("this LEZ membership library is too old (no per-repo configs)"); }
        if (f.h) dlclose(f.h);
        f = n; loaded = p; return f;
    }
    json take(char* s) const {
        if (!s) throw Err("the LEZ library returned nothing");
        std::string t(s); freeStr(s);
        json j = json::parse(t, nullptr, false);
        if (j.is_discarded() || !j.is_object()) throw Err("the LEZ library returned something unreadable");
        if (j.contains("ok") && j["ok"] == false) throw Err("LEZ: " + j.value("error", std::string("failed")));
        if (j.contains("success") && j["success"] == false) throw Err("LEZ: " + j.value("error", std::string("failed")));
        return j;
    }
};

// Bitcoin-alphabet base58 -> bytes (empty on a bad character)
inline Bytes base58Decode(const std::string& s) {
    static const char* A = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    Bytes out; size_t zeros = 0; while (zeros < s.size() && s[zeros] == '1') zeros++;
    std::vector<uint8_t> b256((s.size() - zeros) * 733 / 1000 + 1, 0);
    for (size_t i = zeros; i < s.size(); i++) {
        const char* p = strchr(A, s[i]); if (!p || !s[i]) return {};
        int carry = (int)(p - A);
        for (auto it = b256.rbegin(); it != b256.rend(); ++it) { carry += 58 * (*it); *it = carry % 256; carry /= 256; }
        if (carry) return {};
    }
    auto it = b256.begin(); while (it != b256.end() && *it == 0) ++it;
    out.assign(zeros, 0); out.insert(out.end(), it, b256.end()); return out;
}

// A device key as the chain returns it -> b64url 32 bytes, or "" if malformed. Reads give base58
// (AccountId form, maybe "Public/"-prefixed); 64 hex chars (the write form) is accepted too.
// No b64url branch: a 43-char base58 key is also valid b64url and would pass through undecoded.
inline std::string lezDeviceToB64(std::string d) {
    if (d.rfind("Public/", 0) == 0) d = d.substr(7);
    if (d.size() == 64 && d.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
        Bytes b; for (size_t i = 0; i < 64; i += 2) b.push_back((uint8_t)std::stoi(d.substr(i, 2), nullptr, 16));
        return b64url(b);
    }
    if (d.size() < 32 || d.size() > 44) return "";
    Bytes b = base58Decode(d); return b.size() == 32 ? b64url(b) : "";
}

// b64url 32 bytes (the phone's member key) -> 64 hex, as the chain's write calls take it
inline std::string b64ToHex32(const std::string& b64) {
    Bytes b = unb64url(b64); if (b.size() != 32) throw Err("the device key is malformed");
    static const char* H = "0123456789abcdef"; std::string o; for (auto c : b) { o += H[c >> 4]; o += H[c & 15]; } return o;
}
// base58 account id (as wallets show it) or 64 hex -> 64 hex
inline std::string accountToHex(std::string a) {
    if (a.rfind("Public/", 0) == 0) a = a.substr(7);
    if (a.size() == 64 && a.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) return a;
    Bytes b = base58Decode(a); if (b.size() != 32) throw Err("the account id is malformed");
    static const char* H = "0123456789abcdef"; std::string o; for (auto c : b) { o += H[c >> 4]; o += H[c & 15]; } return o;
}

// The repo's config id on the chain: its publisher key (Ed25519, 32 bytes), as hex. Anyone holding the
// repo link can derive it; the publisher creates the config under it.
inline std::string lezConfigId(const std::string& repoKeyB64) { return b64ToHex32(repoKeyB64); }

// An open handle on the program with one repo's config. cfg: {wallet, sequencer, program, config, lib}
struct LezSession {
    LezFfi& f; void* h = nullptr; std::string cid;
    explicit LezSession(const json& cfg) : f(LezFfi::get(cfg.value("lib", ""))) {
        std::string wallet = cfg.value("wallet", ""), seq = cfg.value("sequencer", ""), prog = cfg.value("program", "");
        cid = cfg.value("config", "");
        if (wallet.empty() || seq.empty() || prog.size() != 64) throw Err("LEZ isn't set up (wallet, sequencer, program id)");
        if (cid.size() != 64 || cid.find_first_not_of("0123456789abcdef") != std::string::npos) throw Err("LEZ: the repo's config id is malformed");
        h = f.open(json{{"wallet_path", wallet}, {"sequencer_url", seq}, {"program_id_hex", prog}}.dump().c_str());
        if (!h) throw Err("LEZ: couldn't open the wallet (check the wallet folder and program id)");
    }
    ~LezSession() { if (h) f.close(h); }
    LezSession(const LezSession&) = delete; LezSession& operator=(const LezSession&) = delete;
    json call(char* (*fn)(const void*, const char*), json args) { if (!fn) throw Err("the LEZ library lacks that call"); args["config_id_hex"] = cid; return f.take(fn(h, args.dump().c_str())); }
    // the repo's config, or a null json when it hasn't been created yet
    json config() {
        try { return call(f.getConfig, json::object()); }
        catch (Err& e) { std::string m = e.what(); if (m.find("no ConfigState") != std::string::npos || m.find("not found") != std::string::npos || m.find("not initialized") != std::string::npos || m.find("does not exist") != std::string::npos) return json(); throw; }
    }
};

inline std::string lezPrice(const json& c) {
    if (!c.contains("price_per_epoch")) return "";
    return c["price_per_epoch"].is_string() ? c["price_per_epoch"].get<std::string>() : c["price_per_epoch"].dump();
}

// The member side: pay for / extend a membership, add or remove a device. The wallet in cfg pays and signs.
// op: register {account, devicePk, epochs} | renew {account, epochs} | addDevice/removeDevice {account, devicePk} | get {account}
inline json lezMemberOp(const json& cfg, const std::string& op, const json& a) {
    LezSession z(cfg);
    std::string acct = accountToHex(a.value("account", ""));
    uint64_t epochs = a.value("epochs", (uint64_t)0);
    if ((op == "register" || op == "renew") && (epochs < 1 || epochs > 100000)) throw Err("epochs: 1 to 100000");
    if (op == "register") return z.call(z.f.reg, json{{"member_account_id_hex", acct}, {"x25519_pubkey_hex", b64ToHex32(a.value("devicePk", ""))}, {"epochs", epochs}});
    if (op == "renew") return z.call(z.f.renew, json{{"holder_account_id_hex", acct}, {"epochs", epochs}});
    if (op == "addDevice") return z.call(z.f.addDev, json{{"owner_account_id_hex", acct}, {"x25519_pubkey_hex", b64ToHex32(a.value("devicePk", ""))}});
    if (op == "removeDevice") return z.call(z.f.rmDev, json{{"owner_account_id_hex", acct}, {"x25519_pubkey_hex", b64ToHex32(a.value("devicePk", ""))}});
    if (op == "get") { json r = z.call(z.f.getOne, json{{"owner_account_id_hex", acct}});
        if (r.contains("devices")) { json ds = json::array(); for (auto& d : r["devices"]) if (d.is_string()) ds.push_back(lezDeviceToB64(d)); r["devices"] = ds; }
        return r; }
    throw Err("unknown LEZ operation");
}

// The repo's config as the chain has it now (price_per_epoch, epoch_len_ms, treasury_hex, admin_hex, ...); null if none yet
inline json lezConfigOf(const json& cfg) { LezSession z(cfg); return z.config(); }

// The publisher side: create the repo's config, or update its price and period (applies to later payments).
// admin: the publisher's LEZ account (base58 or hex), in the wallet; it signs, becomes the admin and receives the payments.
inline json lezEnsureConfig(const json& cfg, const std::string& admin, const std::string& price, int64_t periodMs, const std::function<void(const std::string&)>& log) {
    if (price.empty() || price.size() > 30 || price.find_first_not_of("0123456789") != std::string::npos) throw Err("the price is a whole number");
    if (periodMs < 60000) throw Err("the period is at least a minute");
    LezSession z(cfg);
    std::string adminHex = accountToHex(admin);
    json c = z.config();
    if (c.is_null()) {
        log("LEZ: creating this repo's membership config (" + price + " per period)");
        json r = z.call(z.f.cfgCreate, json{{"admin_account_id_hex", adminHex}, {"treasury_hex", adminHex}, {"price_per_epoch", price}, {"epoch_len_ms", periodMs}});
        log("LEZ: created, transaction " + r.value("tx_hash", std::string("?")).substr(0, 16) + "...");
        return r;
    }
    if (c.value("admin_hex", std::string()) != adminHex) throw Err("this repo's membership config belongs to another account (" + c.value("admin_hex", std::string()).substr(0, 12) + "...): set that account");
    if (lezPrice(c) == price && c.value("epoch_len_ms", (int64_t)0) == periodMs) return c;
    log("LEZ: updating the price/period to " + price + " per " + std::to_string(periodMs / 86400000) + " day(s) (later payments)");
    return z.call(z.f.cfgUpdate, json{{"admin_account_id_hex", adminHex}, {"price_per_epoch", price}, {"epoch_len_ms", periodMs}});
}

struct LezMember { std::string owner; std::vector<std::string> devices; int64_t paidThroughMs = 0; };
struct LezSnapshot { std::vector<LezMember> members; json config; };

// Every membership in the repo's registry, with its devices and paid-through time.
inline LezSnapshot lezRead(const json& cfg) {
    LezSession z(cfg);
    LezSnapshot snap; snap.config = z.config();
    if (snap.config.is_null()) throw Err("LEZ: this repo has no membership config yet (save the LEZ settings to create it)");
    std::vector<std::string> pdas; uint64_t from = 0;
    for (int guard = 0; guard < 200; guard++) {
        json page = z.call(z.f.list, json{{"from_index", from}});
        for (auto& e : page.value("entries", json::array())) if (e.contains("membership_pda_hex") && e["membership_pda_hex"].is_string()) pdas.push_back(e["membership_pda_hex"]);
        uint64_t next = page.value("next_cursor", from), upto = page.value("next_member_index", next);
        if (next == from || next >= upto) break;
        from = next;
    }
    if (pdas.size() > 5000) throw Err("LEZ: too many memberships");
    for (size_t i = 0; i < pdas.size(); i += 100) {
        std::vector<std::string> chunk(pdas.begin() + i, pdas.begin() + std::min(pdas.size(), i + 100));
        json r = z.call(z.f.getMany, json{{"membership_pdas_hex", chunk}});
        for (auto& m : r.value("members", json::array())) {
            if (!m.value("found", false)) continue;
            LezMember lm; lm.owner = m.value("owner", std::string());
            const json& pt = m.contains("paid_through_ms") ? m["paid_through_ms"] : json();
            lm.paidThroughMs = pt.is_number() ? pt.get<int64_t>() : pt.is_string() ? std::atoll(pt.get<std::string>().c_str()) : 0;
            for (auto& d : m.value("devices", json::array())) if (d.is_string()) { std::string b = lezDeviceToB64(d); if (!b.empty()) lm.devices.push_back(b); }
            snap.members.push_back(lm);
        }
    }
    return snap;
}
}  // namespace bp
