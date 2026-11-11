#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>
#include "utils.hpp"

// Елемент черги.
struct QueueTaskItem {
    int min_deadline_sec = -1;
    int max_deadline_sec = -1;
    int cooking_time_sec = 0;
    int start_time_sec = -1;
    int task_id = -1;
    int stream_id = -1;
};

// Дані для закріплення часових обмежень задач.
struct QueueTaskPlacement {
    int task_id = -1;
    int min_deadline_sec = -1;
    int max_deadline_sec = -1;
};

enum class EventType : uint8_t { None = 0, Start = 1, Finish = 2 };
struct QueueEvent {
    int event_time_sec = -1;
    int task_id = -1;
    EventType event_type = EventType::None;
};

enum class QueueSearchMode : uint8_t {
    Fast = 0,
    Bounded = 1,
    LiteBrute = 2,   // DFS за динамічними часовими точками з поверненням і повним уточненням до секунд.
    EnclaveTree = 3  // Дерево перевизначень, стійкі профілі та повторно використовувані анклави.
};

inline bool queue_mode_implemented(QueueSearchMode mode) {
    return mode == QueueSearchMode::Fast || mode == QueueSearchMode::Bounded ||
        mode == QueueSearchMode::LiteBrute || mode == QueueSearchMode::EnclaveTree;
}

struct QueueConfig {
    QueueSearchMode mode = QueueSearchMode::Bounded;
    int max_search_nodes = 600;
    int max_repack_nodes = 3000;
    int max_branch_per_task = 20;
    int max_variants = 16;
    int max_time_sec = SECONDS_PER_DAY;
};

// Приблизна оцінка виділеної пам'яті: sizeof враховує вирівнювання структур,
// а capacity векторів — їхню невикористану місткість. Службові дані алокатора
// та пам'ять сторонніх бібліотек не враховуються. Пік пошуку — для останнього запуску.
struct QueueMemoryUsage {
    uint64_t current_bytes = 0;
    uint64_t last_search_peak_bytes = 0;
    uint64_t estimated_peak_bytes = 0;
    uint32_t last_search_nodes = 0;
    uint32_t last_search_enclaves = 0;
    bool search_limit_hit = false; // Відсутність розв'язку не доведена.
    QueueSearchMode mode = QueueSearchMode::Fast;
};

class QueueManager {
public:
    int ovens_count = 1;
    std::vector<QueueTaskItem> current_queue;

    explicit QueueManager(QueueConfig cfg = {});
    bool set_config(const QueueConfig& cfg);
    const QueueConfig& config() const { return config_; }

    bool configure_streams(int total_streams, int initially_enabled = 0);
    bool stream_enabled(int stream_id) const;
    int stream_count() const;
    bool enable_stream(int stream_id, int curr_time_clear_sec = 0);
    bool freeze_stream(int stream_id, int keep_task_id = -1, int curr_time_clear_sec = 0);
    bool shift_stream_left(int stream_id, int seconds, int curr_time_clear_sec = 0);
    const std::vector<QueueTaskItem>* get_variant(int id) const;
    bool compress(int target, int curr_time_clear_sec = 0, bool apply = true);

    // Повертає КІНЦІ інтервалів та ID пов'язаних буферів (ліворуч/праворуч від цільового початку).
    std::pair<int, int> put_time(int target_sec, int duration_sec, int& left_id, int& right_id,
        int min_sec = 0, int max_sec = SECONDS_PER_DAY, int current_sec = 0);
    std::pair<int, int> put_time_buffered(int queue_id, int target_sec, int duration_sec,
        int& left_id, int& right_id, int min_sec = 0, int max_sec = SECONDS_PER_DAY,
        int current_sec = 0);

    bool remove_task(int task_id, int curr_time_clear_sec = 0);
    bool apply_variant(int id, int start_sec, int min_dl_sec, int max_dl_sec, int curr_time_clear_sec = 0);
    bool apply_variant(int id, QueueTaskPlacement& placement, int curr_time_clear_sec = 0);
    bool apply_variant(int id, const std::vector<QueueTaskPlacement>& placements, int curr_time_clear_sec = 0);
    bool close_deadlines(int task_id);
    int next_event(int current_sec, QueueEvent& out_event);

    // Оцінка виконується лише за зовнішнім запитом, а не на кожному кроці планувальника.
    QueueMemoryUsage memory_usage() const;
    bool last_search_limit_hit() const { return last_search_limit_hit_; }

    // Шаблон використовує наявний метод logger.log_issue без залежності від Logger.
    // Приклад виклику: manager.log_memory_usage(logger, time, IssueSeverity::Info);
    template <class LoggerT, class SeverityT>
    void log_memory_usage(LoggerT& logger, int timestamp_sec, SeverityT severity) const {
        const QueueMemoryUsage m = memory_usage();
        logger.log_issue(0, timestamp_sec, severity, "QUEUE_MEMORY_USAGE",
            "mode=" + std::to_string(int(m.mode)) +
            "; current_bytes=" + std::to_string(m.current_bytes) +
            "; last_search_peak_bytes=" + std::to_string(m.last_search_peak_bytes) +
            "; estimated_peak_bytes=" + std::to_string(m.estimated_peak_bytes) +
            "; nodes=" + std::to_string(m.last_search_nodes) +
            "; enclaves=" + std::to_string(m.last_search_enclaves) +
            "; budget_exhausted=" + std::to_string(int(m.search_limit_hit)));
    }

private:
    struct Variant {
        int id = -1;
        std::vector<QueueTaskItem> items;
        int pending_count = 0;
    };

    QueueConfig config_;
    std::vector<uint8_t> stream_accepting_ = { 1 };
    std::deque<Variant> queue_variants_;
    int next_variant_id_ = 0;
    uint64_t last_search_peak_bytes_ = 0;
    uint32_t last_search_nodes_ = 0;
    uint32_t last_search_enclaves_ = 0;
    bool last_search_limit_hit_ = false;

    bool apply_variant_items(int id, const std::vector<QueueTaskPlacement>& placements, int current_sec);
    void drop_variants();
    int add_variant(std::vector<QueueTaskItem> items, int pending_count);
    const Variant* variant_at(int id) const;
    std::pair<int, int> put_impl(const Variant* base, int target_sec, int duration_sec,
        int& left_id, int& right_id, int min_sec, int max_sec, int current_sec);
    bool validate_current(const std::vector<QueueTaskItem>& items, const std::vector<uint8_t>& accepting) const;
};
