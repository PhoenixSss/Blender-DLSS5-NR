// SPDX-License-Identifier: MIT
#include "backend_registry.h"

#include <mutex>
#include <utility>

namespace blender_dlss5::backends {

namespace {

struct Entry {
    std::string name;
    BackendFactory factory;
};

std::mutex& RegistryMutex() {
    static std::mutex m;
    return m;
}

std::vector<Entry>& Registry() {
    static std::vector<Entry> entries;
    return entries;
}

}  // namespace

bool RegisterBackend(const char* name, BackendFactory factory) {
    if (!name || !factory) return false;
    std::lock_guard<std::mutex> lock(RegistryMutex());
    Registry().push_back({name, factory});
    return true;
}

INeuralRenderingBackend* CreateBackendByName(const char* name) {
    if (!name) return nullptr;
    std::lock_guard<std::mutex> lock(RegistryMutex());
    for (const auto& e : Registry()) {
        if (e.name == name) return e.factory();
    }
    return nullptr;
}

INeuralRenderingBackend* CreateDefaultBackend() {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    if (Registry().empty()) return nullptr;
    return Registry().front().factory();
}

std::vector<std::string> ListBackends() {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    std::vector<std::string> names;
    names.reserve(Registry().size());
    for (const auto& e : Registry()) names.push_back(e.name);
    return names;
}

}  // namespace blender_dlss5::backends
