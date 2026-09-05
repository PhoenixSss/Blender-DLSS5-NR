// SPDX-License-Identifier: MIT
#include "file_identity.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wintrust.h>

#include <bcrypt.h>
#include <softpub.h>   // WINTRUST_ACTION_GENERIC_VERIFY_V2
#include <wincrypt.h>

#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace blender_dlss5::diagnostics {

const char* to_string(SignatureStatus s) {
    switch (s) {
        case SignatureStatus::NotApplicable: return "NotApplicable";
        case SignatureStatus::ValidTrusted: return "ValidTrusted";
        case SignatureStatus::Invalid: return "Invalid";
        case SignatureStatus::Unknown: return "Unknown";
    }
    return "Unknown";
}

const char* to_string(RuntimeClassification c) {
    switch (c) {
        case RuntimeClassification::NvidiaOriginal: return "nvidia_original";
        case RuntimeClassification::KnownModified: return "known_modified";
        case RuntimeClassification::Unknown: return "unknown";
    }
    return "unknown";
}

namespace {

// Shared BCrypt SHA-256 core; hex-encodes the digest into `hex`.
bool Sha256Hex(const BYTE* data, size_t size, std::string* hex) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD hash_len = 0, got = 0;
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) goto done;
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len),
                          sizeof(hash_len), &got, 0) != 0) goto done;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) goto done;
    if (size > 0 && BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) != 0) goto done;

    {
        std::vector<BYTE> digest(hash_len);
        if (BCryptFinishHash(hash, digest.data(), hash_len, 0) != 0) goto done;
        char tmp[3];
        hex->clear();
        hex->reserve(hash_len * 2);
        for (DWORD i = 0; i < hash_len; ++i) {
            std::snprintf(tmp, sizeof(tmp), "%02x", digest[i]);
            *hex += tmp;
        }
    }
    ok = true;

done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string Sha256File(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "[sha256] CreateFileW failed: Win32 %lu\n", GetLastError());
        return {};
    }

    std::string hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD hash_len = 0, got = 0;
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        std::fprintf(stderr, "[sha256] BCryptOpenAlgorithmProvider failed\n");
        goto done;
    }
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len),
                          sizeof(hash_len), &got, 0) != 0) {
        std::fprintf(stderr, "[sha256] BCryptGetProperty failed\n");
        goto done;
    }
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        std::fprintf(stderr, "[sha256] BCryptCreateHash failed\n");
        goto done;
    }

    {
        std::vector<BYTE> buf(1u << 20);  // 1 MiB chunks; ~1 s for a 165 MB DLL
        unsigned long long total = 0;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)) {
                // EOF: some filesystems return FALSE with ERROR_HANDLE_EOF,
                // others with ERROR_SUCCESS — judge by bytes read instead.
                if (read == 0) break;
                std::fprintf(stderr, "[sha256] ReadFile failed at byte %llu: Win32 %lu\n",
                             total, GetLastError());
                goto done;
            }
            if (read == 0) break;  // defensive
            total += read;
            if (BCryptHashData(hash, buf.data(), read, 0) != 0) {
                std::fprintf(stderr, "[sha256] BCryptHashData failed at byte %llu\n", total);
                goto done;
            }
        }
    }

    {
        std::vector<BYTE> digest(hash_len);
        if (BCryptFinishHash(hash, digest.data(), hash_len, 0) != 0) {
            std::fprintf(stderr, "[sha256] BCryptFinishHash failed\n");
            goto done;
        }
        char tmp[3];
        hex.reserve(hash_len * 2);
        for (DWORD i = 0; i < hash_len; ++i) {
            std::snprintf(tmp, sizeof(tmp), "%02x", digest[i]);
            hex += tmp;
        }
    }
    ok = true;

done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return ok ? hex : std::string{};
}

std::string FileVersionOf(const std::wstring& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), handle, size, data.data())) return {};
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi) return {};
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                  HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return buf;
}

struct SignatureResult {
    SignatureStatus status = SignatureStatus::Unknown;
    std::string signer;
};

SignatureResult CheckAuthenticode(const std::wstring& path) {
    SignatureResult r;

    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = path.c_str();
    fi.hFile = INVALID_HANDLE_VALUE;

    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_SAFER_FLAG;

    // WINTRUST_ACTION_GENERIC_VERIFY_V2 is a GUID brace-initializer macro;
    // taking its address directly is not valid C++. Use a local GUID.
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG st = WinVerifyTrust(nullptr, &action, &wd);
    switch (st) {
        case ERROR_SUCCESS:
            r.status = SignatureStatus::ValidTrusted;
            break;
        case TRUST_E_NOSIGNATURE:
            r.status = SignatureStatus::NotApplicable;
            break;
        default:
            r.status = SignatureStatus::Invalid;
            break;
    }

    if (r.status == SignatureStatus::ValidTrusted && wd.hWVTStateData) {
        if (CRYPT_PROVIDER_DATA* prov = WTHelperProvDataFromStateData(wd.hWVTStateData)) {
            if (CRYPT_PROVIDER_SGNR* sgnr = WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0)) {
                // SDK 26100 layout: pasCertChain is a flat array of
                // CRYPT_PROVIDER_CERT; the leaf signer cert is [0].pCert.
                if (sgnr->csCertChain > 0 && sgnr->pasCertChain &&
                    sgnr->pasCertChain[0].pCert) {
                    PCCERT_CONTEXT cert = sgnr->pasCertChain[0].pCert;
                    DWORD chars = CertNameToStrW(
                        cert->dwCertEncodingType, &cert->pCertInfo->Subject,
                        CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG,
                        nullptr, 0);
                    if (chars > 1) {
                        std::vector<wchar_t> wbuf(chars);
                        CertNameToStrW(
                            cert->dwCertEncodingType, &cert->pCertInfo->Subject,
                            CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG,
                            wbuf.data(), chars);
                        char utf8[1024] = {};
                        WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), -1, utf8,
                                            static_cast<int>(sizeof(utf8)), nullptr, nullptr);
                        r.signer = utf8;
                    }
                }
            }
        }
    }

    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    return r;
}

// §17: known modified-runtime hashes. A1 ships with one empty placeholder
// entry (zero-size constexpr arrays are illegal); adding a real 64-char
// SHA-256 here makes ClassifyRuntime() return KnownModified, which the
// probe rejects.
constexpr std::string_view kKnownModifiedHashes[] = {
    "",  // placeholder; e.g. "0123456789abcdef...64 hex chars"
};

}  // namespace

std::string Sha256Buffer(const void* data, size_t size) {
    if (!data || size == 0) return {};
    std::string hex;
    if (!Sha256Hex(static_cast<const BYTE*>(data), size, &hex)) return {};
    return hex;
}

bool ComputeFileIdentity(const std::wstring& path, FileIdentity* out,
                         std::string* error) {
    if (!out) return false;
    out->path = path;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        if (error) *error = "File does not exist: " + std::string(path.begin(), path.end());
        return false;
    }
    out->exists = true;
    out->size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    out->file_version = FileVersionOf(path);
    out->sha256 = Sha256File(path);
    if (out->sha256.empty()) {
        if (error) *error = "SHA-256 computation failed";
        return false;
    }
    const SignatureResult sig = CheckAuthenticode(path);
    out->signature = sig.status;
    out->signer_subject = sig.signer;
    out->classification = ClassifyRuntime(*out);
    return true;
}

RuntimeClassification ClassifyRuntime(const FileIdentity& id) {
    for (const auto& h : kKnownModifiedHashes) {
        if (!h.empty() && id.sha256 == h) return RuntimeClassification::KnownModified;
    }
    if (id.signature == SignatureStatus::ValidTrusted &&
        id.signer_subject.find("NVIDIA Corporation") != std::string::npos) {
        return RuntimeClassification::NvidiaOriginal;
    }
    return RuntimeClassification::Unknown;
}

}  // namespace blender_dlss5::diagnostics
