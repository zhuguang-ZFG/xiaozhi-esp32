"""身份字节契约与 NVS 故障注入；真实密码学验签另由 portal 回归验证。"""
import base64
import hashlib
import os
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

from test_hutuji_recovery_core import find_compiler

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/lichuang-dev"


def _run(source, headers=None):
    compiler = find_compiler()
    if compiler is None:
        raise RuntimeError("身份验证必须有可用的 host C++ 编译器")
    with tempfile.TemporaryDirectory(prefix="hutuji-identity-") as directory:
        temp = Path(directory)
        for name, content in (headers or {}).items():
            path = temp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        path = temp / "identity.cpp"
        path.write_text(source, encoding="utf-8")
        exe = temp / ("identity.exe" if os.name == "nt" else "identity")
        build = subprocess.run([
            str(compiler), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(temp),
            "-I", str(BOARD), str(path), "-o", str(exe),
        ], capture_output=True, text=True, timeout=120)
        if build.returncode:
            raise AssertionError(build.stderr or build.stdout)
        env = dict(os.environ, PATH=str(compiler.parent) + os.pathsep + os.environ.get("PATH", ""))
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30, env=env)
        if result.returncode:
            raise AssertionError(result.stderr or result.stdout or str(result.returncode))
        return result.stdout.splitlines()


class HutujiBindIdentityTest(unittest.TestCase):
    def test_byte_contract_and_stale_worker_lifecycle(self):
        lines = _run(textwrap.dedent(r'''
            #include "hutuji_bind_identity_core.h"
            #include <array>
            #include <cassert>
            #include <iostream>
            int main() {
                using namespace hutuji;
                std::array<uint8_t, 65> bytes{};
                for (size_t i=0; i<bytes.size(); ++i) bytes[i]=static_cast<uint8_t>(i*7);
                for (size_t n=0; n<=bytes.size(); ++n) {
                    const auto encoded=BindBase64Url(bytes.data(),n);
                    std::cout << encoded << "\n";
                    if (n) assert(BindCanonicalBase64Url(encoded,n));
                }
                assert(!BindCanonicalBase64Url("AAAAAAAAAAAAAAAAAAAAAB",16));
                assert(!BindCanonicalBase64Url("AAAAAAAAAAAAAAAAAAAAAA==",16));
                BindIdentitySession first{"dev", "pub", "nonce1", "ABCDEF"};
                BindIdentitySession second{"dev", "pub", "nonce2", "UVWXYZ"};
                BindIdentityRun run;
                run.Request(first, BindRunMode::Headless);
                const auto old=run.generation;
                run.Stop();
                run.Request(second, BindRunMode::Headless);
                assert(!run.Complete(old));
                assert(run.Current(run.generation));
                assert(run.session.nonce=="nonce2");
                assert(run.Complete(run.generation));
                assert(!run.Current(run.generation));
                run.Request(first, BindRunMode::Manual);
                assert(run.mode==BindRunMode::Manual);
                std::cout << BindIdentityMessage(first,"challenge","aa:bb:cc:dd:ee:ff","paper","digest") << "\n";
            }
        '''))
        raw = bytes(i * 7 % 256 for i in range(65))
        self.assertEqual(lines[:66], [base64.urlsafe_b64encode(raw[:n]).rstrip(b"=").decode() for n in range(66)])
        self.assertEqual(lines[66], "hutuji-bind-v2|dev|nonce1|challenge|ABCDEF|aa:bb:cc:dd:ee:ff|paper|digest")

    def test_nvs_failure_reboot_and_signature_message(self):
        # 公钥为测试私钥 1 的 P-256 点；签名桩只记录 PSA 调用字节，不冒充真 PSA 运算。
        public = bytes.fromhex(
            "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
            "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"
        )
        pub_hash = hashlib.sha256(public).digest()
        token = "identity-host-test-token"
        token_hash = hashlib.sha256(token.encode()).digest()
        initializer = lambda raw: ",".join(str(x) for x in raw)
        common = r'''
            #pragma once
            #include <algorithm>
            #include <cassert>
            #include <cstdint>
            #include <cstring>
            #include <map>
            #include <string>
            #include <vector>
            inline std::map<std::string,std::vector<uint8_t>> disk, working;
            inline bool fail_open=false, fail_write=false, fail_commit=false, fail_read=false;
            inline int generated=0, destroyed=0;
            inline std::string signed_bytes;
            inline unsigned random_counter=1;
            using esp_err_t=int;
            using nvs_handle_t=unsigned;
            constexpr int ESP_OK=0, ESP_ERR_NVS_NOT_FOUND=1, NVS_READWRITE=2;
            inline int nvs_open(const char*,int,nvs_handle_t* handle) {
                if(fail_open) return -1;
                working=disk; *handle=1; return ESP_OK;
            }
            inline void nvs_close(nvs_handle_t) { working=disk; }
            inline int nvs_get_blob(nvs_handle_t,const char* key,void* out,size_t* size) {
                if(fail_read) return -1;
                if(!working.count(key)) return ESP_ERR_NVS_NOT_FOUND;
                const auto& value=working[key];
                if(*size<value.size()) return -1;
                if(out) std::memcpy(out,value.data(),value.size());
                *size=value.size(); return ESP_OK;
            }
            inline int nvs_set_blob(nvs_handle_t,const char* key,const void* data,size_t size) {
                if(fail_write) return -1;
                const auto* bytes=static_cast<const uint8_t*>(data);
                working[key]={bytes,bytes+size}; return ESP_OK;
            }
            inline int nvs_commit(nvs_handle_t) { if(fail_commit) return -1; disk=working; return ESP_OK; }
            inline int nvs_erase_key(nvs_handle_t,const char* key) { working.erase(key); return ESP_OK; }
            using mbedtls_svc_key_id_t=unsigned;
            struct psa_key_attributes_t { unsigned usage=0; };
            #define PSA_KEY_ATTRIBUTES_INIT {}
            constexpr int PSA_SUCCESS=0, PSA_ALG_SHA_256=1, PSA_ECC_FAMILY_SECP_R1=2;
            constexpr unsigned PSA_KEY_USAGE_SIGN_MESSAGE=1, PSA_KEY_USAGE_EXPORT=2;
            inline int PSA_KEY_TYPE_ECC_KEY_PAIR(int family) { assert(family==2); return 3; }
            inline int PSA_ALG_ECDSA(int hash) { assert(hash==1); return 4; }
            inline void psa_set_key_type(psa_key_attributes_t*,int type) { assert(type==3); }
            inline void psa_set_key_bits(psa_key_attributes_t*,unsigned bits) { assert(bits==256); }
            inline void psa_set_key_usage_flags(psa_key_attributes_t* a,unsigned u) { a->usage=u; }
            inline void psa_set_key_algorithm(psa_key_attributes_t*,int alg) { assert(alg==4); }
            inline void psa_reset_key_attributes(psa_key_attributes_t*) {}
            inline int psa_crypto_init() { return PSA_SUCCESS; }
            inline int psa_generate_key(const psa_key_attributes_t* a,mbedtls_svc_key_id_t* key) {
                assert(a->usage==(PSA_KEY_USAGE_SIGN_MESSAGE|PSA_KEY_USAGE_EXPORT));
                ++generated; *key=1; return PSA_SUCCESS;
            }
            inline int psa_export_key(mbedtls_svc_key_id_t,uint8_t* out,size_t size,size_t* actual) {
                assert(size==32); std::memset(out,0,size); out[31]=1; *actual=32; return PSA_SUCCESS;
            }
            inline int psa_import_key(const psa_key_attributes_t*,const uint8_t* data,size_t size,mbedtls_svc_key_id_t* key) {
                if(size!=32 || data[31]!=1) return -1;
                *key=1; return PSA_SUCCESS;
            }
            inline int psa_destroy_key(mbedtls_svc_key_id_t) { ++destroyed; return PSA_SUCCESS; }
            inline int psa_generate_random(uint8_t* out,size_t size) {
                for(size_t i=0;i<size;++i) out[i]=static_cast<uint8_t>(random_counter++);
                return PSA_SUCCESS;
            }
            inline int psa_export_public_key(mbedtls_svc_key_id_t,uint8_t* out,size_t size,size_t* actual) {
                const uint8_t pub[]={PUBLIC_BYTES}; assert(size==sizeof(pub));
                std::memcpy(out,pub,size); *actual=size; return PSA_SUCCESS;
            }
            inline int psa_hash_compute(int alg,const uint8_t* data,size_t len,uint8_t* out,size_t size,size_t* actual) {
                assert(alg==PSA_ALG_SHA_256 && size==32);
                const uint8_t pub_hash[]={PUBLIC_HASH};
                const uint8_t token_hash[]={TOKEN_HASH};
                if(len!=65) assert(std::string(reinterpret_cast<const char*>(data),len)=="identity-host-test-token");
                std::memcpy(out,len==65?pub_hash:token_hash,32); *actual=32; return PSA_SUCCESS;
            }
            inline int psa_sign_message(mbedtls_svc_key_id_t,int alg,const uint8_t* data,size_t len,uint8_t* out,size_t size,size_t* actual) {
                assert(alg==PSA_ALG_ECDSA(PSA_ALG_SHA_256) && size==64);
                signed_bytes.assign(reinterpret_cast<const char*>(data),len);
                for(size_t i=0;i<size;++i) out[i]=static_cast<uint8_t>(i);
                *actual=64; return PSA_SUCCESS;
            }
            inline void mbedtls_platform_zeroize(void* data,size_t size) { std::memset(data,0,size); }
        '''.replace("PUBLIC_BYTES", initializer(public)).replace("PUBLIC_HASH", initializer(pub_hash)).replace("TOKEN_HASH", initializer(token_hash))
        headers = {
            "host_identity.h": common,
            "nvs.h": '#include "host_identity.h"\n',
            "psa/crypto.h": '#include "host_identity.h"\n',
            "mbedtls/platform_util.h": '#include "host_identity.h"\n',
            "esp_log.h": '#define ESP_LOGW(...) ((void)0)\n',
        }
        source = r'''
            #include "hutuji_bind_identity.cc"
            #include <iostream>
            void Reboot() { hutuji::g_key=0; hutuji::g_device_id.clear(); hutuji::g_public_key.clear(); }
            int main() {
                using namespace hutuji;
                BindIdentitySession first, second, loaded;
                fail_open=true; assert(!BeginBindIdentitySession(first)); fail_open=false;
                fail_read=true; assert(!BeginBindIdentitySession(first)); fail_read=false;
                assert(generated==0);
                fail_write=true; assert(!BeginBindIdentitySession(first)); fail_write=false;
                assert(g_key==0 && disk.empty() && destroyed==1);
                fail_commit=true; assert(!BeginBindIdentitySession(first)); fail_commit=false;
                assert(g_key==0 && disk.empty() && destroyed==2);
                assert(BeginBindIdentitySession(first));
                const int calls=generated;
                Reboot(); assert(LoadBindIdentitySession(loaded));
                assert(loaded.device_id==first.device_id && loaded.nonce==first.nonce && generated==calls);
                const std::string challenge(43,'A');
                std::string signature;
                assert(SignBindIdentity(first,challenge,"aa:bb:cc:dd:ee:ff","paper","identity-host-test-token",signature));
                std::cout << first.device_id << "\n" << first.public_key << "\n" << first.nonce << "\n";
                std::cout << first.bind_code << "\n" << signature << "\n" << signed_bytes << "\n";
                assert(!SignBindIdentity(first,std::string(42,'A')+"B","aa:bb:cc:dd:ee:ff","paper","identity-host-test-token",signature));
                assert(signature.empty());
                BindIdentitySession tampered=first; tampered.device_id="bad";
                assert(!SignBindIdentity(tampered,challenge,"aa:bb:cc:dd:ee:ff","paper","identity-host-test-token",signature));
                assert(BeginBindIdentitySession(second));
                assert(second.device_id==first.device_id && second.nonce!=first.nonce);
                assert(!FinishBindIdentitySession(first.nonce));
                assert(!SignBindIdentity(first,challenge,"aa:bb:cc:dd:ee:ff","paper","identity-host-test-token",signature));
                fail_commit=true; assert(!FinishBindIdentitySession(second.nonce)); fail_commit=false;
                assert(LoadBindIdentitySession(loaded) && loaded.nonce==second.nonce);
                assert(FinishBindIdentitySession(second.nonce));
                assert(!LoadBindIdentitySession(loaded) && disk.count("private_v2"));
                Reboot(); disk["private_v2"]=std::vector<uint8_t>(32,0);
                assert(!BeginBindIdentitySession(loaded) && generated==calls);
            }
        '''
        lines = _run(textwrap.dedent(source), headers)
        self.assertEqual(lines[0], pub_hash.hex()[:32])
        self.assertEqual(lines[1], base64.urlsafe_b64encode(public).rstrip(b"=").decode())
        self.assertEqual(lines[4], base64.urlsafe_b64encode(bytes(range(64))).rstrip(b"=").decode())
        self.assertEqual(lines[5], f"hutuji-bind-v2|{lines[0]}|{lines[2]}|{'A' * 43}|{lines[3]}|aa:bb:cc:dd:ee:ff|paper|{token_hash.hex()}")


if __name__ == "__main__":
    unittest.main()
