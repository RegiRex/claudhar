// Copyright 2026 Claudhar Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

// Formats and logs the draw ledger; see draw_ledger.h.

#include <fmt/format.h>

#include "claudhar/shading/draw_ledger.h"
#include "common/logging/log.h"

namespace Claudhar::Shading {

std::string DrawLedger::Summary() const {
    return fmt::format(
        "drawn {} (specialized GPU-vertex {}, specialized CPU-vertex {}, ubershader {}), "
        "empty {}, invalid {}, CPU-vertex fallbacks {}, pipelines building {}, "
        "compile_waits {}, skipped {}",
        Drawn(), specialized_gpu_vertices, specialized_cpu_vertices, ubershader, empty, invalid,
        cpu_vertex_fallbacks, building.load(std::memory_order_relaxed), compile_waits, skipped);
}

void DrawLedger::LogPeriodically(std::chrono::seconds interval) {
    const auto now = std::chrono::steady_clock::now();
    const u64 rule_breaks = compile_waits + skipped;
    const bool rule_broken = rule_breaks != logged_rule_breaks;
    if (!rule_broken && (Drawn() == logged_drawn || now - last_log < interval)) {
        return;
    }
    last_log = now;
    logged_drawn = Drawn();
    logged_rule_breaks = rule_breaks;
    // Claudhar log line: one per interval at most, plus one when a rule counter changes.
    if (rule_broken) {
        LOG_WARNING(Render_Vulkan, "Claudhar shading: {}", Summary());
    } else {
        LOG_INFO(Render_Vulkan, "Claudhar shading: {}", Summary());
    }
}

} // namespace Claudhar::Shading
