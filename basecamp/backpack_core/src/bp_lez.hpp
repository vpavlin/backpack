#pragma once
// bp_lez.hpp - LEZ memberships (backpack-access, ADR 0010) as a source of member devices (ADR 0013).
// The backpack_access_ffi library is loaded at run time (dlopen), so backpack_core works without it;
// only a repo set to use LEZ needs it. Read-only here: list the registry, read each membership.
#include "bp_crypto.hpp"
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>

namespace bp {
struct LezFfi {
    void* h = nullptr;
    void* (*open)(const char*) = nullptr;
    void (*close)(void*) = nullptr;
    char* (*list)(const void*, const char*) = nullptr;
    char* (*getMany)(const void*, const char*) = nullptr;
    char* (*getConfig)(const void*) = nullptr;
    char* (*reg)(const void*, const char*) = nullptr;
    char* (*renew)(const void*, const char*) = nullptr;
    char* (*addDev)(const void*, const char*) = nullptr;
    char* (*rmDev)(const void*, const char*) = nullptr;
    char* (*getOne)(const void*, const char*) = nullptr;
    char* (*balance)(const void*) = nullptr;
    void (*freeStr)(char*) = nullptr;
    // lib: an explicit path, else $BACKPACK_LEZ_LIB, else the loader's search path
    static LezFfi& get(const std::string& lib) {
        static LezFfi f; static std::string loaded;
        std::string p = !lib.empty() ? lib : std::getenv("BACKPACK_LEZ_LIB") ? std::getenv("BACKPACK_LEZ_LIB") : "libbackpack_access_ffi.so";
        if (f.h && loaded == p) return f;
        void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { const char* e = dlerror(); throw Err(std::string("can't load the LEZ membership library (") + p + "): " + (e ? e : "?")); }
        LezFfi n; n.h = h;
        n.open = (void* (*)(const char*))dlsym(h, "backpack_access_config_open");
        n.close = (void (*)(void*))dlsym(h, "backpack_access_config_close");
        n.list = (char* (*)(const void*, const char*))dlsym(h, "backpack_access_member_list");
        n.getMany = (char* (*)(const void*, const char*))dlsym(h, "backpack_access_member_get_many");
        n.getConfig = (char* (*)(const void*))dlsym(h, "backpack_access_get_config");
        n.freeStr = (void (*)(char*))dlsym(h, "backpack_access_free_string");
        using W = char* (*)(const void*, const char*);
        n.reg = (W)dlsym(h, "backpack_access_member_register"); n.renew = (W)dlsym(h, "backpack_access_member_renew");
        n.addDev = (W)dlsym(h, "backpack_access_member_add_device"); n.rmDev = (W)dlsym(h, "backpack_access_member_remove_device");
        n.getOne = (W)dlsym(h, "backpack_access_member_get"); n.balance = (char* (*)(const void*))dlsym(h, "backpack_access_treasury_balance");
        if (!n.open || !n.close || !n.list || !n.getMany || !n.getConfig || !n.freeStr) { dlclose(h); throw Err("the LEZ membership library lacks the expected functions"); }
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

// The member side: pay for / extend a membership, add or remove a device. The wallet in cfg pays and signs.
// op: register {account, devicePk, epochs} | renew {account, epochs} | addDevice/removeDevice {account, devicePk} | get {account}
inline json lezMemberOp(const json& cfg, const std::string& op, const json& a) {
    std::string wallet = cfg.value("wallet", ""), seq = cfg.value("sequencer", ""), prog = cfg.value("program", "");
    if (wallet.empty() || seq.empty() || prog.size() != 64) throw Err("LEZ isn't set up (wallet, sequencer, program id)");
    LezFfi& f = LezFfi::get(cfg.value("lib", ""));
    if (!f.reg || !f.renew || !f.addDev || !f.rmDev || !f.getOne) throw Err("the LEZ library lacks the membership calls");
    json o = {{"wallet_path", wallet}, {"sequencer_url", seq}, {"program_id_hex", prog}};
    void* h = f.open(o.dump().c_str()); if (!h) throw Err("LEZ: couldn't open the config (check the wallet folder and program id)");
    struct Closer { LezFfi& f; void* h; ~Closer() { f.close(h); } } closer{f, h};
    std::string acct = accountToHex(a.value("account", ""));
    uint64_t epochs = a.value("epochs", (uint64_t)0);
    if ((op == "register" || op == "renew") && (epochs < 1 || epochs > 100000)) throw Err("epochs: 1 to 100000");
    if (op == "register") return f.take(f.reg(h, json{{"member_account_id_hex", acct}, {"x25519_pubkey_hex", b64ToHex32(a.value("devicePk", ""))}, {"epochs", epochs}}.dump().c_str()));
    if (op == "renew") return f.take(f.renew(h, json{{"holder_account_id_hex", acct}, {"epochs", epochs}}.dump().c_str()));
    if (op == "addDevice") return f.take(f.addDev(h, json{{"owner_account_id_hex", acct}, {"x25519_pubkey_hex", b64ToHex32(a.value("devicePk", ""))}}.dump().c_str()));
    if (op == "removeDevice") return f.take(f.rmDev(h, json{{"owner_account_id_hex", acct}, {"x25519_pubkey_hex", b64ToHex32(a.value("devicePk", ""))}}.dump().c_str()));
    if (op == "get") { json r = f.take(f.getOne(h, json{{"owner_account_id_hex", acct}}.dump().c_str()));
        if (r.contains("devices")) { json ds = json::array(); for (auto& d : r["devices"]) if (d.is_string()) ds.push_back(lezDeviceToB64(d)); r["devices"] = ds; }
        return r; }
    throw Err("unknown LEZ operation");
}

struct LezMember { std::string owner; std::vector<std::string> devices; int64_t paidThroughMs = 0; };
struct LezSnapshot { std::vector<LezMember> members; json config; };

// Every membership in the registry, with its devices and paid-through time.
inline LezSnapshot lezRead(const json& cfg) {
    std::string wallet = cfg.value("wallet", ""), seq = cfg.value("sequencer", ""), prog = cfg.value("program", "");
    if (wallet.empty() || seq.empty() || prog.size() != 64) throw Err("LEZ isn't set up for this repo (wallet, sequencer, program id)");
    LezFfi& f = LezFfi::get(cfg.value("lib", ""));
    json o = {{"wallet_path", wallet}, {"sequencer_url", seq}, {"program_id_hex", prog}};
    void* h = f.open(o.dump().c_str());
    if (!h) throw Err("LEZ: couldn't open the config (check the wallet folder and program id)");
    struct Closer { LezFfi& f; void* h; ~Closer() { f.close(h); } } closer{f, h};
    LezSnapshot snap; snap.config = f.take(f.getConfig(h));
    std::vector<std::string> pdas; uint64_t from = 0;
    for (int guard = 0; guard < 200; guard++) {
        json page = f.take(f.list(h, json{{"from_index", from}}.dump().c_str()));
        for (auto& e : page.value("entries", json::array())) if (e.contains("membership_pda_hex") && e["membership_pda_hex"].is_string()) pdas.push_back(e["membership_pda_hex"]);
        uint64_t next = page.value("next_cursor", from), upto = page.value("next_member_index", next);
        if (next == from || next >= upto) break;
        from = next;
    }
    if (pdas.size() > 5000) throw Err("LEZ: too many memberships");
    for (size_t i = 0; i < pdas.size(); i += 100) {
        std::vector<std::string> chunk(pdas.begin() + i, pdas.begin() + std::min(pdas.size(), i + 100));
        json r = f.take(f.getMany(h, json{{"membership_pdas_hex", chunk}}.dump().c_str()));
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
