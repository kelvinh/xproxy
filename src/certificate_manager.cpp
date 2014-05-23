#include <boost/date_time.hpp>
#include <openssl/pem.h>
#include "xproxy/log/log.hpp"
#include "xproxy/ssl/certificate_manager.hpp"

namespace xproxy {
namespace ssl {

namespace {
    static util::Singleton<CertificateManager> manager_;
}

bool CertificateManager::init() {
    auto& cm = instance();
    if (!cm.loadRootCA()) {
        if (!cm.generateRootCA())
            return false;

        cm.saveRootCA(); // ignore the operation result
    }

    if (!cm.loadDHParameters()) {
        if (!cm.generateDHParameters())
            return false;

        cm.saveDHParameters(); // ignore the operation result
    }

    return true;
}

CertificateManager &CertificateManager::instance() {
    return manager_.get();
}

Certificate CertificateManager::getCertificate(const std::string& host) {
    auto common_name = parseCommonName(host);

    auto it = certificates_.find(common_name);
    if (it != certificates_.end())
        return it->second;

    XDEBUG << "Certificate for " << host << " not found in cache.";

    Certificate cert;

    auto filename = getCertificateFileName(common_name);
    if (loadCertificate(filename, cert)) {
        XDEBUG << "Certificate for host " << host << " loaded from file.";
        certificates_.insert(std::make_pair(common_name, cert));
        return cert;
    }

    XDEBUG << "Generating certificate for " << host << "...";

    if (!generateCertificate(common_name, cert)) {
        XERROR << "Certificate generation error, host: " << host;
        return cert;
    }

    XDEBUG << "Certificate for " << host << " generated.";

    certificates_.insert(std::make_pair(common_name, cert));
    if (!saveCertificate(filename, cert))
        XERROR << "Certificate saving error, host: " << host;
    return cert;
}

bool CertificateManager::loadRootCA(const std::string& file) {
    if (!loadCertificate(file, root_)) {
        XWARN << "Root CA loading error.";
        return false;
    }

    XINFO << "Root CA loaded.";
    return true;
}

bool CertificateManager::saveRootCA(const std::string& file) {
    if (!saveCertificate(file, root_)) {
        XWARN << "Root CA saving error.";
        return false;
    }

    XINFO << "Root CA saved.";
    return true;
}

bool CertificateManager::generateRootCA() {
    X509 *x509 = nullptr;
    x509 = X509_new();
    if(!x509) {
        XERROR << "X509 creation error.";
        return false;
    }

    // we should set the serial number to 0 for root CA, otherwise it will not
    // be accepted by browsers
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 0);

    // set the valid date, to 10 years
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 60 * 60 * 24 * 365 * 10);

    EVP_PKEY *key = nullptr;
    if(!generateKey(&key)) {
        XERROR << "Error generating root CA private key.";
        X509_free(x509);
        return false;
    }
    X509_set_pubkey(x509, key);

    X509_NAME *name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("xProxy Root CA"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "OU", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("xProxy CA"),      -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O",  MBSTRING_ASC, reinterpret_cast<const unsigned char *>("xProxy"),         -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "L",  MBSTRING_ASC, reinterpret_cast<const unsigned char *>("Lan"),            -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "ST", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("Internet"),       -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "C",  MBSTRING_ASC, reinterpret_cast<const unsigned char *>("CN"),             -1, -1, 0);

    X509_set_issuer_name(x509, name); //self signed

    if(!X509_sign(x509, key, EVP_sha1())) { // sign cert using sha1
        XERROR << "Error signing root certificate.";
        X509_free(x509);
        EVP_PKEY_free(key);
        return false;
    }

    root_.setKey(key);
    root_.setCertificate(x509);
    XINFO << "Root CA generated.";
    return true;
}

bool CertificateManager::loadCertificate(const std::string& file, Certificate& cert) {
    FILE *fp = nullptr;
    fp = std::fopen(file.c_str(), "rb");
    if(!fp) {
        XWARN << "Error opening file: " << file;
        return false;
    }

    X509 *x509 = PEM_read_X509(fp, nullptr, nullptr, nullptr);
    if (!x509) {
        XERROR << "Error reading certificate from file " << file;
        std::fclose(fp);
        return false;
    }
    cert.setCertificate(x509);

    EVP_PKEY *key = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    if(!key) {
        XERROR << "Error reading private key from file " << file;
        std::fclose(fp);
        return false;
    }
    cert.setKey(key);

    std::fclose(fp);
    XINFO << "Certificate and private key are loaded from " << file;
    return true;
}

bool CertificateManager::saveCertificate(const std::string& file, const Certificate& cert) {
    FILE *fp = nullptr;
    fp = std::fopen(file.c_str(), "wb");
    if(!fp) {
        XWARN << "Error opening file: " << file;
        return false;
    }

    if(!PEM_write_X509(fp, cert.certificate())) {
        XERROR << "Error writing certificate to " << file;
        std::fclose(fp);
        return false;
    }

    if(!PEM_write_PrivateKey(fp, cert.key(), nullptr, nullptr, 0, nullptr, nullptr)) {
        XERROR << "Error writing private key to " << file;
        std::fclose(fp);
        return false;
    }

    std::fclose(fp);
    XINFO << "Certificate and private key are saved to " << file;
    return true;
}

bool CertificateManager::generateCertificate(const std::string& common_name, Certificate& cert) {
    if(!root_.key() || !root_.certificate()) {
        XERROR << "Root CA does not exist.";
        return false;
    }

    X509_REQ *req = nullptr;
    EVP_PKEY *key = nullptr;

    if(!generateRequest(common_name, &req, &key)) {
        XERROR << "Error generating request for common name " << common_name;
        return false;
    }

    if(X509_REQ_verify(req, key) != 1) {
        XERROR << "Error verifying certificate request for common name " << common_name;
        EVP_PKEY_free(key);
        X509_REQ_free(req);
        return false;
    }

    X509 *x509 = nullptr;
    x509 = X509_new();
    if(!x509) {
        XERROR << "X509 creation error.";
        EVP_PKEY_free(key);
        X509_REQ_free(req);
        return false;
    }

    X509_NAME *name = X509_get_subject_name(root_.certificate());
    X509_set_issuer_name(x509, name);
    name = X509_REQ_get_subject_name(req);
    X509_set_subject_name(x509, name);
    X509_set_pubkey(x509, key);

    // use exact time as cert's serial number to make it different, otherwise
    // browsers will complain that too many certs use the same serial number
    boost::posix_time::ptime time = boost::posix_time::microsec_clock::universal_time();
    boost::posix_time::ptime epoch = boost::posix_time::time_from_string("1970-01-01 00:00:00.000");
    ASN1_INTEGER_set(X509_get_serialNumber(x509), (time - epoch).total_microseconds());

    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 60 * 60 * 24 * 365 * 10);

    if (!X509_sign(x509, root_.key(), EVP_sha1())) {
        XERROR << "Error signing certificate.";
        EVP_PKEY_free(key);
        X509_REQ_free(req);
        X509_free(x509);
        return false;
    }

    cert.setCertificate(x509);
    cert.setKey(key);
    X509_REQ_free(req);
    XINFO << "Certificate generated for common name " << common_name;
    return true;
}

bool CertificateManager::loadDHParameters(const std::string& file) {
    FILE *fp = nullptr;
    fp = std::fopen(file.c_str(), "rb");
    if (!fp) {
        XWARN << "Error opening DH parameters file: " << file;
        return false;
    }

    DH *dh = PEM_read_DHparams(fp, nullptr, nullptr, nullptr);
    if (!dh) {
        XERROR << "Error reading DH parameters from file " << file;
        std::fclose(fp);
        return false;
    }

    dh_.reset(dh);
    std::fclose(fp);
    XINFO << "DH parameters loaded.";
    return true;
}

bool CertificateManager::saveDHParameters(const std::string& file) {
    FILE *fp = nullptr;
    fp = std::fopen(file.c_str(), "wb");
    if (!fp) {
        XWARN << "Error opening DH parameters file: " << file;
        return false;
    }

    if (!PEM_write_DHparams(fp, dh_.get())) {
        XERROR << "Error writing DH parameters to " << file;
        std::fclose(fp);
        return false;
    }

    std::fclose(fp);
    XINFO << "DH parameters saved.";
    return true;
}

bool CertificateManager::generateDHParameters(){
    DH *dh = DH_generate_parameters(512, DH_GENERATOR_2, nullptr, nullptr); // 512 bits
    if(!dh) {
        XERROR << "Error generating DH parameters.";
        return false;
    }
    dh_.reset(dh);
    XINFO << "DH parameters generated.";
    return true;
}

bool CertificateManager::generateKey(EVP_PKEY **key) {
    EVP_PKEY *k = nullptr;
    k = EVP_PKEY_new();
    if(!k) {
        XERROR << "EVP_PKEY creation error.";
        return false;
    }

    RSA *rsa = nullptr;
    rsa = RSA_generate_key(2048, RSA_F4, nullptr, nullptr);
    if(!rsa) {
        XERROR << "Error generating 2048-bit RSA key.";
        EVP_PKEY_free(k);
        return false;
    }

    if(!EVP_PKEY_assign_RSA(k, rsa)) { // now rsa's memory is managed by k
        XERROR << "Error assigning rsa key to EVP_PKEY.";
        EVP_PKEY_free(k);
        RSA_free(rsa);
        return false;
    }

    *key = k; // we shouldn't free rsa here because it is managed by k
    return true;
}

bool CertificateManager::generateRequest(const std::string& common_name, X509_REQ **request, EVP_PKEY **key) {
    EVP_PKEY *k = nullptr;
    if (!generateKey(&k)) {
        XERROR << "Key generation error.";
        return false;
    }

    X509_REQ *req = nullptr;
    req = X509_REQ_new();
    if (!req) {
        XERROR << "X509_REQ creation error.";
        EVP_PKEY_free(k);
        return false;
    }
    X509_REQ_set_pubkey(req, k);

    X509_NAME *name = X509_REQ_get_subject_name(req);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>(common_name.c_str()), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "OU", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("xProxy Security"),   -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O",  MBSTRING_ASC, reinterpret_cast<const unsigned char *>(common_name.c_str()), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "L",  MBSTRING_ASC, reinterpret_cast<const unsigned char *>("Lan"),               -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "ST", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("Internet"),          -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "C",  MBSTRING_ASC, reinterpret_cast<const unsigned char *>("CN"),                -1, -1, 0);

    if (!X509_REQ_sign(req, k, EVP_sha1())) {
        XERROR << "Error signing request.";
        EVP_PKEY_free(k);
        X509_REQ_free(req);
        return false;
    }

    *request = req;
    *key = k;
    return true;
}

std::string CertificateManager::parseCommonName(const std::string& host) {
    std::size_t dot_count = std::count(host.begin(), host.end(), '.');
    if(dot_count < 2) // means something like "something.com", or even something like "localhost"
        return host;

    auto last = host.find_last_of('.');
    auto penult = host.find_last_of('.', last - 1);
    if(last - penult <= 4) // means something like "something.com.cn"
        return host;

    std::string common_name(host);
    auto first = host.find('.');
    common_name[first - 1] = '*';
    common_name.erase(0, first - 1);
    return common_name;
}

std::string CertificateManager::getCertificateFileName(const std::string& common_name) {
    // TODO enhance this function
    std::string filename(common_name);
    if(filename[0] == '*')
        filename[0] = '^';

    return cert_dir_ + filename + ".crt";
}

} // namespace ssl
} // namespace xproxy
