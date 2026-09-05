// SPDX-License-Identifier: MIT
#include "png_io.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
// tinyexr reuses stb's zlib decoder (stbi_zlib_decode_buffer) — no
// separate miniz/zlib dependency needed. MINIZ must be explicitly
// disabled: its include branch wins over STB_ZLIB by default.
#define TINYEXR_USE_MINIZ (0)
#define TINYEXR_USE_STB_ZLIB (1)
#define TINYEXR_IMPLEMENTATION
#include "third_party/tinyexr.h"

namespace blender_dlss5::probe {

namespace {

// stb read callback over an in-memory buffer.
struct MemBuf {
    const unsigned char* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

int ReadCb(void* user, char* data, int size) {
    auto* m = static_cast<MemBuf*>(user);
    if (!m || m->pos >= m->size) return 0;
    const size_t avail = m->size - m->pos;
    const size_t n = std::min<size_t>(static_cast<size_t>(size), avail);
    std::copy(m->data + m->pos, m->data + m->pos + n, data);
    m->pos += n;
    return static_cast<int>(n);
}

void SkipCb(void* user, int n) {
    auto* m = static_cast<MemBuf*>(user);
    if (!m || n <= 0) return;
    m->pos = std::min(m->size, m->pos + static_cast<size_t>(n));
}

int EofCb(void* user) {
    auto* m = static_cast<MemBuf*>(user);
    return !m || m->pos >= m->size;
}

// stb write callback into a std::vector.
struct VecBuf {
    std::vector<unsigned char>* out = nullptr;
};

void WriteCb(void* user, void* data, int size) {
    auto* v = static_cast<VecBuf*>(user);
    if (!v || !v->out || size <= 0) return;
    const auto* p = static_cast<const unsigned char*>(data);
    v->out->insert(v->out->end(), p, p + size);
}

}  // namespace

bool LoadPng(const std::string& path, canonical::CanonicalColor* out,
             std::string* error) {
    if (!out) return false;

    // Read the whole file (test PNGs are small).
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
        if (error) *error = "Cannot open input PNG: " + path;
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        if (error) *error = "Input PNG is empty: " + path;
        return false;
    }
    std::vector<unsigned char> buf(static_cast<size_t>(size));
    if (fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        fclose(f);
        if (error) *error = "Failed to read input PNG: " + path;
        return false;
    }
    fclose(f);

    MemBuf mb{buf.data(), buf.size(), 0};
    stbi_io_callbacks cb{ReadCb, SkipCb, EofCb};
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load_from_callbacks(&cb, &mb, &w, &h, &comp, 4);
    if (!pixels) {
        if (error) *error = "stb_image failed: " + std::string(stbi_failure_reason());
        return false;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(pixels);
        if (error) *error = "Input PNG has invalid dimensions";
        return false;
    }

    out->width = static_cast<uint32_t>(w);
    out->height = static_cast<uint32_t>(h);
    out->rgba_f32.resize(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < out->rgba_f32.size(); ++i) {
        out->rgba_f32[i] = static_cast<float>(pixels[i]) / 255.0f;
    }
    stbi_image_free(pixels);
    return true;
}

bool LoadExr(const std::string& path, canonical::CanonicalColor* out,
             std::string* error) {
    if (!out) return false;
    float* pixels = nullptr;
    int w = 0, h = 0;
    const char* exr_err = nullptr;
    const int ret = LoadEXR(&pixels, &w, &h, path.c_str(), &exr_err);
    if (ret != TINYEXR_SUCCESS) {
        if (error) *error = exr_err ? std::string("tinyexr: ") + exr_err
                                    : "tinyexr: unknown EXR error";
        FreeEXRErrorMessage(exr_err);
        return false;
    }
    if (w <= 0 || h <= 0 || !pixels) {
        if (pixels) free(pixels);
        if (error) *error = "EXR has invalid dimensions";
        return false;
    }
    out->width = static_cast<uint32_t>(w);
    out->height = static_cast<uint32_t>(h);
    out->encoding = canonical::ColorEncoding::SceneLinear;
    const size_t n = static_cast<size_t>(w) * h * 4;
    out->rgba_f32.assign(pixels, pixels + n);
    free(pixels);
    return true;
}

bool SavePng(const std::string& path, const canonical::CanonicalColor& frame,
             std::string* error) {
    if (!frame.valid()) {
        if (error) *error = "Cannot save invalid frame";
        return false;
    }
    const size_t n = static_cast<size_t>(frame.width) * frame.height;
    std::vector<unsigned char> bytes(n * 4);
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 4; ++c) {
            const float v = std::clamp(frame.rgba_f32[i * 4 + c], 0.0f, 1.0f);
            bytes[i * 4 + c] = static_cast<unsigned char>(v * 255.0f + 0.5f);
        }
    }

    VecBuf vb;
    std::vector<unsigned char> outbuf;
    vb.out = &outbuf;
    if (stbi_write_png_to_func(WriteCb, &vb, static_cast<int>(frame.width),
                               static_cast<int>(frame.height), 4, bytes.data(),
                               static_cast<int>(frame.width) * 4) == 0) {
        if (error) *error = "stb_image_write failed";
        return false;
    }

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) {
        if (error) *error = "Cannot open output PNG: " + path;
        return false;
    }
    const size_t written = fwrite(outbuf.data(), 1, outbuf.size(), f);
    fclose(f);
    if (written != outbuf.size()) {
        if (error) *error = "Failed to write output PNG: " + path;
        return false;
    }
    return true;
}

}  // namespace blender_dlss5::probe
