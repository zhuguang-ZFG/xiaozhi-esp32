#include "hutuji_bind_identity.h"

#include <esp_log.h>
#include <mbedtls/platform_util.h>
#include <nvs.h>
#include <psa/crypto.h>

#include <array>
#include <cstring>
#include <mutex>

namespace hutuji {
namespace {

constexpr const char* kTag = "HutujiIdentity";
constexpr const char* kNamespace = "hutuji_identity";
constexpr size_t kSecretSize = 32;
constexpr size_t kSessionSize =
    23;  // 版本 1 字节 + nonce 16 字节 + 短码 6 字节，一次 NVS blob 写。
std::mutex g_mutex;
mbedtls_svc_key_id_t g_key = 0;
std::string g_device_id;
std::string g_public_key;

class NvsHandle {
public:
    nvs_handle_t value = 0;
    ~NvsHandle() {
        if (value != 0) {
            nvs_close(value);
        }
    }
};

bool Hash(const std::string& text, uint8_t* digest) {
    size_t length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(text.data()),
                            text.size(), digest, 32, &length) == PSA_SUCCESS &&
           length == 32;
}

bool LoadKey(nvs_handle_t nvs) {
    if (g_key != 0) {
        return true;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attrs, 256);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attrs, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    std::array<uint8_t, kSecretSize> secret{};
    size_t size = secret.size();
    const esp_err_t read = nvs_get_blob(nvs, "private_v2", secret.data(), &size);
    mbedtls_svc_key_id_t key = 0;
    bool ok = false;
    if (read == ESP_ERR_NVS_NOT_FOUND) {
        ok = psa_generate_key(&attrs, &key) == PSA_SUCCESS &&
             psa_export_key(key, secret.data(), secret.size(), &size) == PSA_SUCCESS &&
             size == secret.size() &&
             nvs_set_blob(nvs, "private_v2", secret.data(), secret.size()) == ESP_OK &&
             nvs_commit(nvs) == ESP_OK;
    } else if (read == ESP_OK && size == secret.size()) {
        ok = psa_import_key(&attrs, secret.data(), secret.size(), &key) == PSA_SUCCESS;
    }
    // 坏记录绝不触发自动换钥；失败后的私钥和临时 PSA 槽也必须回收。
    mbedtls_platform_zeroize(secret.data(), secret.size());
    psa_reset_key_attributes(&attrs);
    std::array<uint8_t, 65> public_key{};
    size_t public_size = 0;
    std::array<uint8_t, 32> digest{};
    ok = ok &&
         psa_export_public_key(key, public_key.data(), public_key.size(), &public_size) ==
             PSA_SUCCESS &&
         public_size == public_key.size() && public_key[0] == 4;
    if (ok) {
        ok = Hash(std::string(reinterpret_cast<const char*>(public_key.data()), public_size),
                  digest.data());
    }
    if (!ok) {
        if (key != 0) {
            psa_destroy_key(key);
        }
        ESP_LOGW(kTag, "identity key unavailable");
        return false;
    }
    g_key = key;
    g_device_id = BindHex(digest.data(), 16);
    g_public_key = BindBase64Url(public_key.data(), public_key.size());
    return true;
}

bool ReadSession(nvs_handle_t nvs, BindIdentitySession& session) {
    std::array<uint8_t, kSessionSize> record{};
    size_t size = record.size();
    if (nvs_get_blob(nvs, "session_v2", record.data(), &size) != ESP_OK || size != record.size() ||
        record[0] != kBindIdentityVersion) {
        return false;
    }
    constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    for (size_t i = 17; i < record.size(); ++i) {
        if (record[i] == 0 || std::strchr(alphabet, record[i]) == nullptr) {
            return false;
        }
    }
    session = {g_device_id, g_public_key, BindBase64Url(record.data() + 1, 16),
               std::string(reinterpret_cast<const char*>(record.data() + 17), 6)};
    return true;
}

}  // namespace

bool BeginBindIdentitySession(BindIdentitySession& session) {
    std::lock_guard<std::mutex> lock(g_mutex);
    NvsHandle nvs;
    if (nvs_open(kNamespace, NVS_READWRITE, &nvs.value) != ESP_OK || !LoadKey(nvs.value)) {
        return false;
    }
    std::array<uint8_t, kSessionSize> record{};
    record[0] = kBindIdentityVersion;
    if (psa_generate_random(record.data() + 1, record.size() - 1) != PSA_SUCCESS) {
        return false;
    }
    constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    for (size_t i = 17; i < record.size(); ++i) {
        record[i] = alphabet[record[i] % (sizeof(alphabet) - 1)];
    }
    if (nvs_set_blob(nvs.value, "session_v2", record.data(), record.size()) != ESP_OK ||
        nvs_commit(nvs.value) != ESP_OK) {
        return false;
    }
    return ReadSession(nvs.value, session);
}

bool LoadBindIdentitySession(BindIdentitySession& session) {
    std::lock_guard<std::mutex> lock(g_mutex);
    NvsHandle nvs;
    return nvs_open(kNamespace, NVS_READWRITE, &nvs.value) == ESP_OK && LoadKey(nvs.value) &&
           ReadSession(nvs.value, session);
}

bool FinishBindIdentitySession(const std::string& nonce) {
    std::lock_guard<std::mutex> lock(g_mutex);
    NvsHandle nvs;
    BindIdentitySession current;
    return nvs_open(kNamespace, NVS_READWRITE, &nvs.value) == ESP_OK &&
           ReadSession(nvs.value, current) && current.nonce == nonce &&
           nvs_erase_key(nvs.value, "session_v2") == ESP_OK && nvs_commit(nvs.value) == ESP_OK;
}

bool SignBindIdentity(const BindIdentitySession& session, const std::string& challenge,
                      const std::string& mac, const std::string& sku, const std::string& token,
                      std::string& signature) {
    std::lock_guard<std::mutex> lock(g_mutex);
    NvsHandle nvs;
    BindIdentitySession current;
    signature.clear();
    if (!BindCanonicalBase64Url(challenge, 32) ||
        nvs_open(kNamespace, NVS_READWRITE, &nvs.value) != ESP_OK || !LoadKey(nvs.value) ||
        !ReadSession(nvs.value, current) || current.nonce != session.nonce ||
        current.bind_code != session.bind_code || current.device_id != session.device_id ||
        current.public_key != session.public_key) {
        return false;
    }
    std::array<uint8_t, 32> digest{};
    if (!Hash(token, digest.data())) {
        return false;
    }
    const std::string message =
        BindIdentityMessage(session, challenge, mac, sku, BindHex(digest.data(), digest.size()));
    std::array<uint8_t, 64> bytes{};
    size_t size = 0;
    if (psa_sign_message(g_key, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                         reinterpret_cast<const uint8_t*>(message.data()), message.size(),
                         bytes.data(), bytes.size(), &size) != PSA_SUCCESS ||
        size != bytes.size()) {
        return false;
    }
    signature = BindBase64Url(bytes.data(), bytes.size());
    return true;
}

}  // namespace hutuji
