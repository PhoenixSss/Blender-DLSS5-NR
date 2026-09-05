// SPDX-License-Identifier: MIT
#include "report.h"

#include <windows.h>

#include <cstdio>
#include <ctime>

#include "diagnostics/file_identity.h"
#include "diagnostics/json_writer.h"

namespace blender_dlss5::probe {

namespace {

using diagnostics::JsonWriter;

std::string UtcTimestamp() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string VendorHex(uint32_t vendor_id) {
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "0x%04X", vendor_id);
    return buf;
}

}  // namespace

std::string FrameSha256(const canonical::CanonicalColor& frame) {
    return diagnostics::Sha256Buffer(frame.rgba_f32.data(),
                                     frame.rgba_f32.size() * sizeof(float));
}

bool WriteReportJson(const std::string& path, const ProbeReport& r,
                     std::string* error) {
    using namespace backends;

    JsonWriter w;
    w.begin_object();
    w.key("schema_version"); w.string_value(r.schema_version);
    w.key("probe"); w.begin_object();
    w.key("name"); w.string_value(r.probe_name);
    w.key("version"); w.string_value(r.probe_version);
    w.end_object();
    w.key("timestamp_utc"); w.string_value(r.timestamp_utc.empty() ? UtcTimestamp() : r.timestamp_utc);
    w.key("success"); w.bool_value(r.success);

    w.key("gpu"); w.begin_object();
    w.key("name"); w.string_value(r.gpu_name);
    w.key("vendor_id"); w.string_value(r.gpu_vendor);
    w.key("adapter_index"); w.int_value(r.adapter_index);
    w.key("nvidia_index"); w.int_value(r.nvidia_index);
    w.key("architecture"); w.string_value(r.architecture);
    w.key("nvml_arch_raw"); w.int_value(r.nvml_arch_raw);
    w.key("rtx50_compatible"); w.bool_value(r.rtx50_compatible);
    w.key("detection_method"); w.string_value(r.detection_method);
    w.end_object();

    w.key("driver"); w.begin_object();
    w.key("version"); w.string_value(r.driver_version);
    w.end_object();

    w.key("runtime"); w.begin_object();
    w.key("path"); w.string_value(r.runtime_path);
    w.key("size"); w.uint_value(r.runtime_size);
    w.key("file_version"); w.string_value(r.runtime_file_version);
    w.key("sha256"); w.string_value(r.runtime_sha256);
    w.key("authenticode"); w.string_value(r.runtime_authenticode);
    w.key("signer"); w.string_value(r.runtime_signer);
    w.key("classification"); w.string_value(r.runtime_classification);
    w.key("policy"); w.string_value(r.runtime_policy);
    w.end_object();

    w.key("ngx_core"); w.begin_object();
    w.key("path"); w.string_value(r.ngx_core_path);
    w.key("discovery"); w.string_value(r.ngx_core_discovery);
    w.end_object();

    w.key("shim"); w.begin_object();
    w.key("path"); w.string_value(r.shim_path);
    w.end_object();

    w.key("backend"); w.begin_object();
    w.key("name"); w.string_value(r.backend_name);
    w.end_object();

    w.key("settings"); w.begin_object();
    w.key("style"); w.int_value(r.style);
    w.key("preset"); w.int_value(r.preset);
    w.key("intensity"); w.double_value(r.intensity);
    w.key("tone"); w.double_value(r.tone);
    w.key("structure"); w.double_value(r.structure);
    w.key("skin"); w.double_value(r.skin);
    w.key("auto_mask"); w.bool_value(r.auto_mask);
    w.key("reset"); w.bool_value(r.reset);
    w.key("encoding"); w.string_value(r.encoding);
    w.end_object();

    w.key("resolution"); w.begin_object();
    w.key("width"); w.int_value(r.width);
    w.key("height"); w.int_value(r.height);
    w.end_object();

    w.key("hashes"); w.begin_object();
    w.key("input_sha256"); w.string_value(r.input_sha256);
    w.key("output_sha256"); w.string_value(r.output_sha256);
    w.end_object();

    w.key("results"); w.begin_object();
    w.key("create_feature_result"); w.int_value(r.create_feature_result);
    w.key("evaluate_feature_result"); w.int_value(r.evaluate_feature_result);
    w.key("processing_ms"); w.double_value(r.processing_ms);
    w.end_object();

    w.key("outputs"); w.begin_object();
    w.key("png"); w.string_value(r.output_png);
    w.key("raw_rgba_f32"); w.string_value(r.output_raw_f32);
    w.end_object();

    w.key("error");
    if (r.has_error) {
        w.begin_object();
        w.key("category"); w.string_value(to_string(r.error.category));
        w.key("stage"); w.string_value(r.error.stage);
        w.key("raw_code"); w.string_value([&r]() {
            char buf[16] = {};
            std::snprintf(buf, sizeof(buf), "0x%08X", r.error.raw_code);
            return std::string(buf);
        }());
        w.key("message"); w.string_value(r.error.message);
        w.key("gpu"); w.string_value(r.error.gpu_name);
        w.key("driver"); w.string_value(r.error.driver_version);
        w.key("runtime_sha256"); w.string_value(r.error.runtime_sha256);
        w.key("runtime_version"); w.string_value(r.error.runtime_version);
        w.end_object();
    } else {
        w.null_value();
    }
    w.end_object();

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) {
        if (error) *error = "Cannot open report file: " + path;
        return false;
    }
    const std::string json = w.str();
    const size_t written = fwrite(json.data(), 1, json.size(), f);
    fclose(f);
    if (written != json.size()) {
        if (error) *error = "Failed to write report file: " + path;
        return false;
    }
    return true;
}

}  // namespace blender_dlss5::probe
