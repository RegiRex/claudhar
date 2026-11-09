// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// The memory figures of the "Claudhar memory" log line besides the GPU's: what the process
// holds in RAM, which includes the GPU driver's shader compiler, and what the device has
// left. Android ends a game without a crash of its own when memory runs out, so these show
// how close a session came. They are read from /proc by the emulation thread once per log
// interval, never per frame.

#pragma once

#include <string>

namespace Claudhar::Diagnostics {

/// "process N MiB resident, device M MiB available", either part left out when the system
/// does not report it, or an empty string when it reports neither.
std::string ProcessMemoryReport();

} // namespace Claudhar::Diagnostics
