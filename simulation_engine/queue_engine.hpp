#pragma once

#include <cstdint>
#include <vector>
#include "queue_manager.hpp"

struct EngineSolution {
    bool found = false;
    int start_sec = -1;
    int moved_count = 0;
    int64_t total_displacement = 0;
    std::vector<QueueTaskItem> tasks;
};

struct EngineDiagnostics {
    uint64_t peak_bytes = 0;
    uint32_t nodes_expanded = 0;
    uint32_t enclaves_created = 0;
    bool budget_exhausted = false;
};

struct EngineInsertResult {
    EngineSolution left;
    EngineSolution right;
    bool search_limit_hit = false;
    EngineDiagnostics diagnostics;
};

class QueueEngine {
public:
    static bool valid(const std::vector<QueueTaskItem>& tasks,
        const std::vector<uint8_t>& enabled, bool allow_frozen_existing = true);

    static EngineInsertResult insert(const std::vector<QueueTaskItem>& existing,
        const std::vector<uint8_t>& enabled, int target_sec, int duration_sec,
        int temporary_id, int min_sec, int max_sec, const QueueConfig& cfg,
        int current_sec = 0);

    static bool repack(const std::vector<QueueTaskItem>& original,
        const std::vector<uint8_t>& enabled, int closing_stream_id, int preserved_task_id,
        const QueueConfig& cfg, std::vector<QueueTaskItem>& output,
        int current_sec = 0, EngineDiagnostics* diagnostics = nullptr);
};
