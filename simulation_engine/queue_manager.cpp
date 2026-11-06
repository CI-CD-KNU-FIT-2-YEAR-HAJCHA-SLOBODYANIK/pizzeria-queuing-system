#include "queue_manager.hpp"
#include "queue_engine.hpp"

#include <algorithm>
#include <climits>
#include <unordered_set>
#include <utility>

namespace {
    void sort_queue(std::vector<QueueTaskItem>& items) {
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
            if (a.start_time_sec != b.start_time_sec) return a.start_time_sec < b.start_time_sec;
            if (a.stream_id != b.stream_id) return a.stream_id < b.stream_id;
            return a.task_id < b.task_id;
            });
    }

    bool is_locked(const QueueTaskItem& t) {
        return t.min_deadline_sec == t.start_time_sec &&
            t.max_deadline_sec == t.start_time_sec + t.cooking_time_sec;
    }
}

QueueManager::QueueManager(QueueConfig cfg) : config_(cfg) {
    if (config_.max_variants < 4) config_.max_variants = 4;
    if (config_.max_search_nodes < 1) config_.max_search_nodes = 1;
    if (config_.max_repack_nodes < 1) config_.max_repack_nodes = 1;
    if (config_.max_branch_per_task < 1) config_.max_branch_per_task = 1;
    ovens_count = 1;
}

bool QueueManager::set_config(const QueueConfig& cfg) {
    if (!current_queue.empty()) return false;
    config_ = cfg;
    if (config_.max_variants < 4) config_.max_variants = 4;
    if (config_.max_search_nodes < 1) config_.max_search_nodes = 1;
    if (config_.max_repack_nodes < 1) config_.max_repack_nodes = 1;
    if (config_.max_branch_per_task < 1) config_.max_branch_per_task = 1;
    if (config_.max_time_sec <= 0) config_.max_time_sec = SECONDS_PER_DAY;
    drop_variants();
    return true;
}

bool QueueManager::configure_streams(int total_streams, int initially_enabled) {
    if (!current_queue.empty() || total_streams < 1 ||
        initially_enabled < 0 || initially_enabled > total_streams) return false;
    stream_accepting_.assign(total_streams, 0);
    for (int i = 0; i < initially_enabled; ++i) stream_accepting_[i] = 1;
    ovens_count = initially_enabled;
    drop_variants();
    return true;
}

bool QueueManager::stream_enabled(int stream_id) const {
    return stream_id >= 0 && stream_id < int(stream_accepting_.size()) &&
        stream_accepting_[stream_id] != 0;
}

int QueueManager::stream_count() const { return int(stream_accepting_.size()); }

bool QueueManager::enable_stream(int stream_id, int) {
    if (stream_id < 0 || stream_id >= stream_count()) return false;
    if (stream_accepting_[stream_id]) return true;
    stream_accepting_[stream_id] = 1;
    ++ovens_count;
    drop_variants();
    return true;
}

bool QueueManager::validate_current(const std::vector<QueueTaskItem>& items,
    const std::vector<uint8_t>& accepting) const {
    if (!QueueEngine::valid(items, accepting)) return false;
    std::unordered_set<int> seen;
    for (const QueueTaskItem& t : items)
        if (t.task_id < 0 || !seen.insert(t.task_id).second) return false;
    return true;
}

bool QueueManager::freeze_stream(int stream_id, int keep_task_id, int) {
    if (!stream_enabled(stream_id)) return false;
    if (keep_task_id >= 0) {
        auto it = std::find_if(current_queue.begin(), current_queue.end(),
            [&](const auto& t) { return t.task_id == keep_task_id && t.stream_id == stream_id; });
        if (it == current_queue.end()) return false;
    }
    auto enabled = stream_accepting_;
    enabled[stream_id] = 0;
    std::vector<QueueTaskItem> output;
    if (!QueueEngine::repack(current_queue, enabled, stream_id, keep_task_id,
        config_, output) || !validate_current(output, enabled)) return false;
    // Выключенный поток не содержит иных задач, кроме сохранённой выполняющейся.
    for (const auto& t : output)
        if (t.stream_id == stream_id && t.task_id != keep_task_id) return false;
    sort_queue(output);
    current_queue = std::move(output);
    stream_accepting_ = std::move(enabled);
    --ovens_count;
    drop_variants();
    return true;
}

bool QueueManager::compress(int target, int curr_time_clear_sec, bool apply) {
    if (target < 0 || target > stream_count()) return false;
    if (target == ovens_count) return true;
    std::vector<uint8_t> enabled = stream_accepting_;
    auto candidate = current_queue;
    int count = ovens_count;
    if (target > count) {
        for (int s = 0; s < stream_count() && count < target; ++s)
            if (!enabled[s]) { enabled[s] = 1; ++count; }
    }
    else {
        // Выводим потоки с конца. Весь compress атомарен, даже при нескольких отключениях.
        for (int s = stream_count() - 1; s >= 0 && count > target; --s) {
            if (!enabled[s]) continue;
            int keep = -1;
            for (const auto& t : candidate) {
                if (t.stream_id == s && is_locked(t)) {
                    if (keep >= 0) { keep = -2; break; }
                    keep = t.task_id;
                }
            }
            if (keep == -2) continue;
            auto proposed_flags = enabled;
            proposed_flags[s] = 0;
            std::vector<QueueTaskItem> repacked;
            if (!QueueEngine::repack(candidate, proposed_flags, s, keep,
                config_, repacked) || !validate_current(repacked, proposed_flags)) continue;
            candidate = std::move(repacked);
            enabled = std::move(proposed_flags);
            --count;
        }
        if (count != target) return false;
    }
    if (!apply) return true;
    sort_queue(candidate);
    current_queue = std::move(candidate);
    stream_accepting_ = std::move(enabled);
    ovens_count = target;
    drop_variants();
    (void)curr_time_clear_sec;
    return true;
}

bool QueueManager::shift_stream_left(int stream_id, int seconds, int curr_time_clear_sec) {
    if (!stream_enabled(stream_id) || seconds < 0) return false;
    if (seconds == 0) return true;
    auto candidate = current_queue;
    std::vector<int> indices;
    for (int i = 0; i < int(candidate.size()); ++i)
        if (candidate[i].stream_id == stream_id) indices.push_back(i);
    std::sort(indices.begin(), indices.end(), [&](int a, int b) {
        return candidate[a].start_time_sec < candidate[b].start_time_sec;
        });
    int free_from = std::max(0, curr_time_clear_sec);
    bool changed = false;
    for (int i : indices) {
        auto& task = candidate[i];
        // Невыполненная подготовка не может стартовать в прошлом.
        if (is_locked(task) || task.start_time_sec < curr_time_clear_sec) break;
        const int earliest = std::max({ task.min_deadline_sec, free_from,
            task.start_time_sec - seconds });
        if (earliest > task.start_time_sec) return false;
        if (earliest == task.start_time_sec) break; // не смещаем последующие
        task.start_time_sec = earliest;
        free_from = earliest + task.cooking_time_sec;
        changed = true;
    }
    if (!changed) return true;
    if (!validate_current(candidate, stream_accepting_)) return false;
    sort_queue(candidate);
    current_queue = std::move(candidate);
    drop_variants();
    return true;
}

void QueueManager::drop_variants() { queue_variants_.clear(); }

const QueueManager::Variant* QueueManager::variant_at(int id) const {
    if (id < 0) return nullptr;
    for (const auto& v : queue_variants_) if (v.id == id) return &v;
    return nullptr;
}

const std::vector<QueueTaskItem>* QueueManager::get_variant(int id) const {
    const Variant* v = variant_at(id);
    return v ? &v->items : nullptr;
}

int QueueManager::add_variant(std::vector<QueueTaskItem> items, int pending_count) {
    if (next_variant_id_ == INT_MAX) return -1;
    const int id = next_variant_id_++;
    sort_queue(items);
    queue_variants_.push_back({ id, std::move(items), pending_count });
    while (int(queue_variants_.size()) > config_.max_variants) queue_variants_.pop_front();
    return id;
}

std::pair<int, int> QueueManager::put_impl(const Variant* base, int target_sec,
    int duration_sec, int& left_id, int& right_id, int min_sec, int max_sec) {
    left_id = right_id = -1;
    if (!base || duration_sec <= 0 || min_sec < 0 || max_sec > config_.max_time_sec ||
        min_sec >= max_sec || base->pending_count >= INT_MAX - 1) return { -1, -1 };
    const int new_id = -(base->pending_count + 1);
    const auto solutions = QueueEngine::insert(base->items, stream_accepting_,
        target_sec, duration_sec, new_id, min_sec, max_sec, config_);
    int left_end = -1, right_end = -1;
    if (solutions.left.found) {
        left_end = solutions.left.start_sec + duration_sec;
        left_id = add_variant(solutions.left.tasks, base->pending_count + 1);
        if (left_id < 0) left_end = -1;
    }
    if (solutions.right.found) {
        right_end = solutions.right.start_sec + duration_sec;
        right_id = add_variant(solutions.right.tasks, base->pending_count + 1);
        if (right_id < 0) right_end = -1;
    }
    return { left_end, right_end };
}

std::pair<int, int> QueueManager::put_time(int target_sec, int duration_sec,
    int& left_id, int& right_id, int min_sec, int max_sec) {
    Variant base;
    base.items = current_queue;
    return put_impl(&base, target_sec, duration_sec, left_id, right_id, min_sec, max_sec);
}

std::pair<int, int> QueueManager::put_time_buffered(int queue_id, int target_sec,
    int duration_sec, int& left_id, int& right_id, int min_sec, int max_sec) {
    if (queue_id < 0) return put_time(target_sec, duration_sec,
        left_id, right_id, min_sec, max_sec);
    // Копируем базу: добавление вариантов может удалить старую запись deque.
    const Variant* base = variant_at(queue_id);
    if (!base) { left_id = right_id = -1; return { -1, -1 }; }
    Variant snapshot = *base;
    return put_impl(&snapshot, target_sec, duration_sec,
        left_id, right_id, min_sec, max_sec);
}

bool QueueManager::apply_variant_items(int id,
    const std::vector<QueueTaskPlacement>& placements) {
    const Variant* v = variant_at(id);
    if (!v || v->pending_count != int(placements.size())) return false;
    auto candidate = v->items;
    std::unordered_set<int> ids;
    for (const auto& old : current_queue) ids.insert(old.task_id);
    for (int n = 0; n < int(placements.size()); ++n) {
        const auto& p = placements[n];
        if (p.task_id < 0 || p.min_deadline_sec < 0 || p.max_deadline_sec < 0 ||
            !ids.insert(p.task_id).second) return false;
        auto it = std::find_if(candidate.begin(), candidate.end(),
            [&](const auto& t) { return t.task_id == -n - 1; });
        if (it == candidate.end()) return false;
        const int64_t end = int64_t(it->start_time_sec) + it->cooking_time_sec;
        if (p.min_deadline_sec > it->start_time_sec || end > p.max_deadline_sec ||
            !stream_enabled(it->stream_id)) return false;
        it->task_id = p.task_id;
        it->min_deadline_sec = p.min_deadline_sec;
        it->max_deadline_sec = p.max_deadline_sec;
    }
    // Убедиться, что каждое старое обязательство осталось и его длительность не изменена.
    for (const auto& old : current_queue) {
        auto it = std::find_if(candidate.begin(), candidate.end(),
            [&](const auto& t) { return t.task_id == old.task_id; });
        if (it == candidate.end() || it->cooking_time_sec != old.cooking_time_sec ||
            (is_locked(old) &&
                (it->stream_id != old.stream_id || it->start_time_sec != old.start_time_sec)))
            return false;
    }
    if (!validate_current(candidate, stream_accepting_)) return false;
    sort_queue(candidate);
    current_queue = std::move(candidate);
    drop_variants();
    return true;
}

bool QueueManager::apply_variant(int id,
    const std::vector<QueueTaskPlacement>& placements, int) {
    return apply_variant_items(id, placements);
}

bool QueueManager::apply_variant(int id, QueueTaskPlacement& placement, int now) {
    return apply_variant(id, std::vector<QueueTaskPlacement>{placement}, now);
}

bool QueueManager::apply_variant(int id, int start_sec, int min_dl_sec,
    int max_dl_sec, int now) {
    const Variant* variant = variant_at(id);
    if (!variant || variant->pending_count != 1) return false;
    auto it = std::find_if(variant->items.begin(), variant->items.end(),
        [](const auto& t) { return t.task_id == -1; });
    if (it == variant->items.end() || it->start_time_sec != start_sec) return false;
    // Старый перегруженный интерфейс не задаёт ID: выделяем следующий свободный.
    int next_id = 0;
    for (const auto& t : current_queue)
        next_id = std::max(next_id, t.task_id + 1);
    return apply_variant(id, std::vector<QueueTaskPlacement>{{next_id, min_dl_sec, max_dl_sec}}, now);
}

bool QueueManager::remove_task(int task_id, int) {
    auto it = std::find_if(current_queue.begin(), current_queue.end(),
        [&](const auto& t) { return t.task_id == task_id; });
    if (it == current_queue.end()) return false;
    current_queue.erase(it);
    drop_variants();
    return true;
}

bool QueueManager::close_deadlines(int task_id) {
    auto it = std::find_if(current_queue.begin(), current_queue.end(),
        [&](const auto& t) { return t.task_id == task_id; });
    if (it == current_queue.end()) return false;
    const int end = it->start_time_sec + it->cooking_time_sec;
    if (it->min_deadline_sec == it->start_time_sec && it->max_deadline_sec == end)
        return true;
    it->min_deadline_sec = it->start_time_sec;
    it->max_deadline_sec = end;
    drop_variants();
    return true;
}

int QueueManager::next_event(int current_sec, QueueEvent& out_event) {
    out_event = {};
    int best = INT_MAX;
    for (const auto& t : current_queue) {
        const int start = t.start_time_sec;
        const int end = start + t.cooking_time_sec;
        if (start >= current_sec && start < best) {
            best = start;
            out_event = { start, t.task_id, EventType::Start };
        }
        if (end >= current_sec && end < best) {
            best = end;
            out_event = { end, t.task_id, EventType::Finish };
        }
    }
    return best == INT_MAX ? -1 : best - current_sec;
}
