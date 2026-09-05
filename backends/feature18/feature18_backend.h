// SPDX-License-Identifier: MIT
// backends/feature18/feature18_backend.h — Feature-18 backend declaration.
//
// pimpl: this header leaks no NGX/D3D12/Feature-18 ABI types. The raw ABI
// lives in ngx_abi.h and feature18_backend.cpp only (§7).

#pragma once

#include <memory>

#include "backends/interface/neural_backend.h"

namespace blender_dlss5::backends::feature18 {

class Feature18Backend final : public INeuralRenderingBackend {
public:
    Feature18Backend();
    ~Feature18Backend() override;

    const char* name() const override;
    bool set_option(const char* key, const char* value) override;
    bool initialize(BackendError* err) override;
    bool process(const canonical::CanonicalColor& in,
                 canonical::CanonicalColor& out,
                 const NeuralSettings& settings,
                 BackendError* err) override;
    void shutdown() override;
    const BackendDiagnostics& diagnostics() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace blender_dlss5::backends::feature18
