// SPDX-License-Identifier: MIT
// diagnostics/file_identity.h — runtime DLL identity: size, version,
// SHA-256, Authenticode and project-policy classification (§16/§17).

#pragma once

#include <cstdint>
#include <string>

namespace blender_dlss5::diagnostics {

enum class SignatureStatus {
    NotApplicable,   // no signature present (unsigned)
    ValidTrusted,    // valid Authenticode signature
    Invalid,         // signature present but invalid / untrusted
    Unknown,         // check could not be completed
};

enum class RuntimeClassification {
    NvidiaOriginal,  // signed by NVIDIA Corporation
    KnownModified,   // SHA-256 in the project blocklist — reject (§17)
    Unknown,         // anything else — warn, do not silently accept
};

const char* to_string(SignatureStatus s);
const char* to_string(RuntimeClassification c);

struct FileIdentity {
    std::wstring path;
    bool exists = false;
    uint64_t size = 0;
    std::string file_version;   // VS_FIXEDFILEINFO "a.b.c.d"; empty when absent
    std::string sha256;         // lowercase hex, 64 chars
    SignatureStatus signature = SignatureStatus::Unknown;
    std::string signer_subject; // certificate subject of the signer
    RuntimeClassification classification = RuntimeClassification::Unknown;
};

// SHA-256 over an in-memory buffer (lowercase hex). Empty on failure.
std::string Sha256Buffer(const void* data, size_t size);

// Computes all identity fields for the file at `path`. Returns false only
// when the file cannot be opened; a missing signature is a successful
// result with signature == NotApplicable.
bool ComputeFileIdentity(const std::wstring& path, FileIdentity* out,
                         std::string* error);

// Project policy classification (§17). The known-modified blocklist lives
// here and is intentionally empty in A1: adding a hash is a one-line change.
RuntimeClassification ClassifyRuntime(const FileIdentity& id);

}  // namespace blender_dlss5::diagnostics
