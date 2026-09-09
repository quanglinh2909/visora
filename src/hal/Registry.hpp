#pragma once

// The registry lives in core — it is not a hardware concept (see
// core/Registry.hpp). These aliases keep the spelling backends actually use:
//
//   static hal::Register<hal::ImageOps> reg{{ ... }};
//
// which reads better than mixing namespaces at every registration site.

#include "core/Registry.hpp"

namespace visora::hal {

using core::BackendStatus;
using core::Probe;
using core::Registry;
using core::Register;

}  // namespace visora::hal
