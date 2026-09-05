// SPDX-License-Identifier: MIT
// backends/interface/backend_registry.h — static backend registration.
//
// Concrete backends self-register at static-init time. The probe/Blender
// layer selects a backend by name or takes the default; it never contains
// backend-specific names itself.

#pragma once

#include <string>
#include <vector>

#include "neural_backend.h"

namespace blender_dlss5::backends {

using BackendFactory = INeuralRenderingBackend* (*)();

bool RegisterBackend(const char* name, BackendFactory factory);
INeuralRenderingBackend* CreateBackendByName(const char* name);  // nullptr if absent
INeuralRenderingBackend* CreateDefaultBackend();                 // first registered
std::vector<std::string> ListBackends();

}  // namespace blender_dlss5::backends
