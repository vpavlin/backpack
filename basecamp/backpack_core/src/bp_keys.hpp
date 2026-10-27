#pragma once
// bp_keys.hpp - APK signing keys, one per app (package), kept beside the repos in <data dir>/signing/<pkg>/
// (key.pem + cert.pem, mode 600). The key IS the app's identity: lose it and no update can ever be
// published for that app again, so export a backup (PKCS#12, password-protected: also what Gradle and
// apksigner read).
#include "bp_sign.hpp"
#include <openssl/pkcs12.h>
#include <openssl/rsa.h>

namespace bp {
struct Keys {
    fs::path dir;   // <data dir>/signing
    fs::path pkgDir(const std::string& pkg) const { if (!validPackage(pkg)) throw Err("not a package name"); return dir / pkg; }
    bool has(const std::string& pkg) const { return fs::exists(pkgDir(pkg) / "key.pem") && fs::exists(pkgDir(pkg) / "cert.pem"); }
    SigningKey load(const std::string& pkg) const {
        SigningKey k; std::string kp = readFile(pkgDir(pkg) / "key.pem"), cp = readFile(pkgDir(pkg) / "cert.pem");
        BIO* b = BIO_new_mem_buf(kp.data(), (int)kp.size()); k.key = PEM_read_bio_PrivateKey(b, nullptr, nullptr, nullptr); BIO_free(b);
        b = BIO_new_mem_buf(cp.data(), (int)cp.size()); k.cert = PEM_read_bio_X509(b, nullptr, nullptr, nullptr); BIO_free(b);
        if (!k.key || !k.cert) throw Err("can't read the signing key for " + pkg);
        return k;
    }
    void store(const std::string& pkg, const SigningKey& k) {
        if (has(pkg)) throw Err(pkg + " already has a signing key: refusing to replace it (that would change the app's identity)");
        fs::create_directories(pkgDir(pkg)); ::chmod(dir.c_str(), 0700); ::chmod(pkgDir(pkg).c_str(), 0700);
        BIO* b = BIO_new(BIO_s_mem()); PEM_write_bio_PKCS8PrivateKey(b, k.key, nullptr, nullptr, 0, nullptr, nullptr);
        char* p; long n = BIO_get_mem_data(b, &p); std::string kp(p, n); BIO_free(b);
        b = BIO_new(BIO_s_mem()); PEM_write_bio_X509(b, k.cert); n = BIO_get_mem_data(b, &p); std::string cp(p, n); BIO_free(b);
        writeFileAtomic(pkgDir(pkg) / "key.pem", kp, 0600); writeFileAtomic(pkgDir(pkg) / "cert.pem", cp, 0600);
    }
    // A new RSA-4096 key and a self-signed certificate valid for 30 years (Android ignores expiry for
    // installs, but tools warn), subject CN=<name>.
    std::string create(const std::string& pkg, const std::string& name) {
        SigningKey k; EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY_keygen_init(c); EVP_PKEY_CTX_set_rsa_keygen_bits(c, 4096); EVP_PKEY_keygen(c, &k.key); EVP_PKEY_CTX_free(c);
        if (!k.key) throw Err("key generation failed");
        k.cert = X509_new(); X509_set_version(k.cert, 2);
        Bytes serial = randomBytes(16); serial[0] &= 0x7f; BIGNUM* bn = BN_bin2bn(serial.data(), 16, nullptr); BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(k.cert)); BN_free(bn);
        X509_gmtime_adj(X509_getm_notBefore(k.cert), 0); X509_gmtime_adj(X509_getm_notAfter(k.cert), 60L * 60 * 24 * 365 * 30);
        X509_set_pubkey(k.cert, k.key);
        X509_NAME* nm = X509_get_subject_name(k.cert);
        std::string cn = name.empty() ? pkg : name.substr(0, 60);
        X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_UTF8, (const unsigned char*)cn.c_str(), -1, -1, 0);
        X509_NAME_add_entry_by_txt(nm, "O", MBSTRING_UTF8, (const unsigned char*)"Backpack", -1, -1, 0);
        X509_set_issuer_name(k.cert, nm);
        if (!X509_sign(k.cert, k.key, EVP_sha256())) throw Err("certificate signing failed");
        store(pkg, k); return k.certSha256();
    }
    // From a PKCS#12 keystore (keytool -storetype pkcs12, Android Studio's default since 2018).
    std::string importP12(const std::string& pkg, const std::string& path, const std::string& password) {
        std::string raw = readFile(path); const unsigned char* p = (const unsigned char*)raw.data();
        PKCS12* p12 = d2i_PKCS12(nullptr, &p, (long)raw.size()); if (!p12) throw Err("not a PKCS#12 keystore (JKS keystores: convert with keytool -importkeystore -deststoretype pkcs12)");
        SigningKey k; STACK_OF(X509)* ca = nullptr;
        int ok = PKCS12_parse(p12, password.c_str(), &k.key, &k.cert, &ca); PKCS12_free(p12); if (ca) sk_X509_pop_free(ca, X509_free);
        if (ok != 1 || !k.key || !k.cert) throw Err("wrong password, or no key and certificate inside");
        store(pkg, k); return k.certSha256();
    }
    void exportP12(const std::string& pkg, const std::string& path, const std::string& password) const {
        if (password.size() < 8) throw Err("choose a password of at least 8 characters");
        SigningKey k = load(pkg);
        PKCS12* p12 = PKCS12_create(password.c_str(), pkg.c_str(), k.key, k.cert, nullptr, 0, 0, 0, 0, 0);
        if (!p12) throw Err("export failed");
        unsigned char* der = nullptr; int n = i2d_PKCS12(p12, &der); PKCS12_free(p12);
        std::string out((const char*)der, n); OPENSSL_free(der); writeFileAtomic(path, out, 0600);
    }
    json list() const {
        json o = json::array(); std::error_code ec;
        if (!fs::is_directory(dir, ec)) return o;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            std::string pkg = e.path().filename().string(); if (!validPackage(pkg) || !has(pkg)) continue;
            try { SigningKey k = load(pkg); char cn[256] = {0}; X509_NAME_get_text_by_NID(X509_get_subject_name(k.cert), NID_commonName, cn, sizeof cn);
                  o.push_back(json{{"package", pkg}, {"cert", k.certSha256()}, {"name", std::string(cn)}, {"type", k.isRsa() ? "RSA" : "EC"}}); }
            catch (...) {}
        }
        return o;
    }
};
}  // namespace bp
