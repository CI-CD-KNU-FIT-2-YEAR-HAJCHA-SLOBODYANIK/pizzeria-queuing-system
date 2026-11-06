#pragma once

#include <cstdint>
#include <vector>
#include "queue_manager.hpp"

// Внутренний движок: подключается к queue_engine.cpp и queue_manager.cpp.
// Внешние task_id не используются как индексы дерева.
struct EngineSolution {
    bool found = false;
    int start_sec = -1;
    int moved_count = 0;
    int64_t total_displacement = 0;
    std::vector<QueueTaskItem> tasks; // Полный согласованный вариант, включая новую задачу
};

struct EngineInsertResult {
    EngineSolution left;  // Самое близкое начало не позже target_sec
    EngineSolution right; // Самое близкое начало не раньше target_sec
    bool search_limit_hit = false;
};

// При вставке рассматриваем только новые интервалы [start,end), а уже вставленные
// временные задачи с отрицательными ID жёстко закреплены.
class QueueEngine {
public:
    static bool valid(const std::vector<QueueTaskItem>& tasks,
        const std::vector<uint8_t>& enabled, bool allow_frozen_existing = true);

    static EngineInsertResult insert(const std::vector<QueueTaskItem>& existing,
        const std::vector<uint8_t>& enabled, int target_sec, int duration_sec,
        int temporary_id, int min_sec, int max_sec, const QueueConfig& cfg);

    // Полная перепаковка с фиксированной задачей на закрываемом потоке.
    // В случае неуспеха входной массив не изменяется.
    static bool repack(const std::vector<QueueTaskItem>& original,
        const std::vector<uint8_t>& enabled, int closing_stream_id, int preserved_task_id,
        const QueueConfig& cfg, std::vector<QueueTaskItem>& output);
};
