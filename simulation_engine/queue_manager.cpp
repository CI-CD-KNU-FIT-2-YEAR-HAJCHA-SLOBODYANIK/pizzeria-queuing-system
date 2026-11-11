#include "queue_manager.hpp"
#include "queue_engine.hpp"

#include <algorithm>
#include <climits>
#include <stdexcept>
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
    if (!queue_mode_implemented(config_.mode))
        throw std::invalid_argument("Queue algorithm is not implemented");
    if (config_.max_variants < 4) config_.max_variants = 4;
    if (config_.max_search_nodes < 1) config_.max_search_nodes = 1;
    if (config_.max_repack_nodes < 1) config_.max_repack_nodes = 1;
    if (config_.max_branch_per_task < 1) config_.max_branch_per_task = 1;
    ovens_count = 1;
}

bool QueueManager::set_config(const QueueConfig& cfg) {
    if (!current_queue.empty() || !queue_mode_implemented(cfg.mode)) return false;
    config_ = cfg;
    if (config_.max_variants < 4) config_.max_variants = 4;
    if (config_.max_search_nodes < 1) config_.max_search_nodes = 1;
    if (config_.max_repack_nodes < 1) config_.max_repack_nodes = 1;
    if (config_.max_branch_per_task < 1) config_.max_branch_per_task = 1;
    if (config_.max_time_sec <= 0) config_.max_time_sec = SECONDS_PER_DAY;
    drop_variants();
    last_search_peak_bytes_ = last_search_nodes_ = last_search_enclaves_ = 0;
    last_search_limit_hit_ = false;
    memory_peak_ = {};
    if (memory_peak_tracking_) observe_memory_peak(0);
    return true;
}

bool QueueManager::configure_streams(int total_streams, int initially_enabled) {
    if (!current_queue.empty() || total_streams < 1 ||
        initially_enabled < 0 || initially_enabled > total_streams) return false;
    stream_accepting_.assign(total_streams, 0);
    for (int i = 0; i < initially_enabled; ++i) stream_accepting_[i] = 1;
    ovens_count = initially_enabled;
    drop_variants();
    observe_memory_peak(0);
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

bool QueueManager::freeze_stream(int stream_id, int keep_task_id, int curr_time_clear_sec) {
    if (!stream_enabled(stream_id)) return false;
    if (keep_task_id >= 0) {
        auto it = std::find_if(current_queue.begin(), current_queue.end(),
            [&](const auto& t) { return t.task_id == keep_task_id && t.stream_id == stream_id; });
        if (it == current_queue.end()) return false;
    }
    auto enabled = stream_accepting_;
    enabled[stream_id] = 0;
    std::vector<QueueTaskItem> output;
    EngineDiagnostics diagnostics;
    const bool found = QueueEngine::repack(current_queue, enabled, stream_id, keep_task_id,
        config_, output, curr_time_clear_sec, &diagnostics);
    last_search_peak_bytes_ = diagnostics.peak_bytes;
    last_search_nodes_ = diagnostics.nodes_expanded;
    last_search_enclaves_ = diagnostics.enclaves_created;
    last_search_limit_hit_ = diagnostics.budget_exhausted;
    observe_memory_peak(curr_time_clear_sec, &diagnostics);
    if (!found || !validate_current(output, enabled)) return false;
    // Вимкнений потік не містить інших задач, окрім збереженої задачі, що виконується.
    for (const auto& t : output)
        if (t.stream_id == stream_id && t.task_id != keep_task_id) return false;
    sort_queue(output);
    current_queue = std::move(output);
    stream_accepting_ = std::move(enabled);
    --ovens_count;
    observe_memory_peak(curr_time_clear_sec);
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
        // Вимикаємо потоки з кінця. Операція compress є атомарною навіть за кількох вимкнень.
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
            EngineDiagnostics diag;
            const bool found = QueueEngine::repack(candidate, proposed_flags, s, keep,
                config_, repacked, curr_time_clear_sec, &diag);
            last_search_peak_bytes_ = diag.peak_bytes;
            last_search_nodes_ = diag.nodes_expanded;
            last_search_enclaves_ = diag.enclaves_created;
            last_search_limit_hit_ = diag.budget_exhausted;
            observe_memory_peak(curr_time_clear_sec, &diag);
            if (!found || !validate_current(repacked, proposed_flags)) continue;
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
    observe_memory_peak(curr_time_clear_sec);
    drop_variants();
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
        // Підготовка, що ще не почалася, не може стартувати в минулому.
        if (is_locked(task) || task.start_time_sec < curr_time_clear_sec) break;
        const int earliest = std::max({ task.min_deadline_sec, free_from,
            task.start_time_sec - seconds });
        if (earliest > task.start_time_sec) return false;
        if (earliest == task.start_time_sec) break; // Не зміщуємо наступні задачі.
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

void QueueManager::drop_variants() {
    std::deque<Variant> empty;
    queue_variants_.swap(empty); // Звільняємо також залишкові блоки deque.
}

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
    int duration_sec, int& left_id, int& right_id, int min_sec, int max_sec,
    int current_sec) {
    left_id = right_id = -1;
    if (!base || duration_sec <= 0 || current_sec < 0 || min_sec < 0 ||
        max_sec > config_.max_time_sec ||
        min_sec >= max_sec || base->pending_count >= INT_MAX - 1) return { -1, -1 };
    const int new_id = -(base->pending_count + 1);
    const auto solutions = QueueEngine::insert(base->items, stream_accepting_,
        target_sec, duration_sec, new_id, min_sec, max_sec, config_, current_sec);
    last_search_peak_bytes_ = solutions.diagnostics.peak_bytes;
    last_search_nodes_ = solutions.diagnostics.nodes_expanded;
    last_search_enclaves_ = solutions.diagnostics.enclaves_created;
    last_search_limit_hit_ = solutions.search_limit_hit;
    observe_memory_peak(current_sec, &solutions.diagnostics);
    int left_end = -1, right_end = -1;
    if (solutions.left.found) {
        left_end = solutions.left.start_sec + duration_sec;
        left_id = add_variant(solutions.left.tasks, base->pending_count + 1);
        observe_memory_peak(current_sec);
        if (left_id < 0) left_end = -1;
    }
    if (solutions.right.found) {
        right_end = solutions.right.start_sec + duration_sec;
        right_id = add_variant(solutions.right.tasks, base->pending_count + 1);
        if (right_id < 0) right_end = -1;
    }
    observe_memory_peak(current_sec);
    return { left_end, right_end };
}

std::pair<int, int> QueueManager::put_time(int target_sec, int duration_sec,
    int& left_id, int& right_id, int min_sec, int max_sec, int current_sec) {
    Variant base;
    base.items = current_queue;
    return put_impl(&base, target_sec, duration_sec, left_id, right_id,
        min_sec, max_sec, current_sec);
}

std::pair<int, int> QueueManager::put_time_buffered(int queue_id, int target_sec,
    int duration_sec, int& left_id, int& right_id, int min_sec, int max_sec, int current_sec) {
    if (queue_id < 0) return put_time(target_sec, duration_sec,
        left_id, right_id, min_sec, max_sec, current_sec);
    // Копіюємо базовий варіант: додавання нових варіантів може видалити попередній запис deque.
    const Variant* base = variant_at(queue_id);
    if (!base) { left_id = right_id = -1; return { -1, -1 }; }
    Variant snapshot = *base;
    return put_impl(&snapshot, target_sec, duration_sec,
        left_id, right_id, min_sec, max_sec, current_sec);
}

bool QueueManager::apply_variant_items(int id,
    const std::vector<QueueTaskPlacement>& placements, int current_sec) {
    if (current_sec < 0) return false;
    const Variant* v = variant_at(id);
    if (!v || v->pending_count != int(placements.size())) return false;
    auto candidate = v->items;
    std::unordered_set<int> ids;
    for (const auto& old : current_queue) {
        // Лише зафіксовані задачі, які вже почалися, можуть мати час початку в минулому.
        if (!is_locked(old) && old.start_time_sec < current_sec) return false;
        ids.insert(old.task_id);
    }
    for (int n = 0; n < int(placements.size()); ++n) {
        const auto& p = placements[n];
        if (p.task_id < 0 || p.min_deadline_sec < 0 || p.max_deadline_sec < 0 ||
            !ids.insert(p.task_id).second) return false;
        auto it = std::find_if(candidate.begin(), candidate.end(),
            [&](const auto& t) { return t.task_id == -n - 1; });
        if (it == candidate.end()) return false;
        const int64_t end = int64_t(it->start_time_sec) + it->cooking_time_sec;
        if (p.min_deadline_sec > it->start_time_sec || end > p.max_deadline_sec ||
            it->start_time_sec < current_sec || !stream_enabled(it->stream_id)) return false;
        it->task_id = p.task_id;
        it->min_deadline_sec = p.min_deadline_sec;
        it->max_deadline_sec = p.max_deadline_sec;
    }
    // Перевіряємо, що всі раніше прийняті задачі збережені та їхня тривалість не змінилася.
    for (const auto& old : current_queue) {
        auto it = std::find_if(candidate.begin(), candidate.end(),
            [&](const auto& t) { return t.task_id == old.task_id; });
        if (it == candidate.end() || it->cooking_time_sec != old.cooking_time_sec ||
            (is_locked(old) &&
                (it->stream_id != old.stream_id || it->start_time_sec != old.start_time_sec)))
            return false;
    }
    // Раніше закріплені задачі зберігають свої параметри, а нові та ще не розпочаті
    // задачі не можуть починатися раніше поточної секунди.
    for (const auto& task : candidate) {
        if (task.start_time_sec >= current_sec) continue;
        const auto it = std::find_if(current_queue.begin(), current_queue.end(),
            [&](const auto& old) { return old.task_id == task.task_id; });
        if (it == current_queue.end() || !is_locked(*it) ||
            it->start_time_sec != task.start_time_sec || it->stream_id != task.stream_id)
            return false;
    }
    if (!validate_current(candidate, stream_accepting_)) return false;
    sort_queue(candidate);
    current_queue = std::move(candidate);
    observe_memory_peak(current_sec);
    drop_variants();
    return true;
}

bool QueueManager::apply_variant(int id,
    const std::vector<QueueTaskPlacement>& placements, int now) {
    return apply_variant_items(id, placements, now);
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
    // Попередній перевантажений інтерфейс не задає ID: обираємо наступний вільний.
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
void QueueManager::set_memory_peak_tracking(bool enabled) {
    memory_peak_tracking_ = enabled;
    memory_peak_ = {};
    if (enabled) observe_memory_peak(0);
}

void QueueManager::observe_memory_peak(int time_sec, const EngineDiagnostics* diagnostics) {
    if (!memory_peak_tracking_) return;

    const QueueMemoryUsage current = memory_usage();
    memory_peak_.manager_peak_bytes = std::max(memory_peak_.manager_peak_bytes, current.current_bytes);
    memory_peak_.max_queue_tasks = std::max(memory_peak_.max_queue_tasks, uint64_t(current_queue.size()));

    // Оцінюємо пам'ять менеджера й тимчасову пам'ять саме цього пошуку разом.
    const uint64_t search_bytes = diagnostics ? diagnostics->peak_bytes : 0;
    if (diagnostics) {
        ++memory_peak_.searches_observed;
        memory_peak_.search_peak_bytes = std::max(memory_peak_.search_peak_bytes, search_bytes);
    }
    const uint64_t estimated_bytes = current.current_bytes + search_bytes;
    if (estimated_bytes > memory_peak_.estimated_peak_bytes) {
        memory_peak_.estimated_peak_bytes = estimated_bytes;
        memory_peak_.peak_time_sec = time_sec;
        memory_peak_.queue_tasks_at_peak = current_queue.size();
        memory_peak_.nodes_at_peak = diagnostics ? diagnostics->nodes_expanded : 0;
        memory_peak_.enclaves_at_peak = diagnostics ? diagnostics->enclaves_created : 0;
        memory_peak_.budget_exhausted_at_peak = diagnostics && diagnostics->budget_exhausted;
    }
}

QueueMemoryUsage QueueManager::memory_usage() const {
    QueueMemoryUsage m;
    m.mode = config_.mode;
    m.current_bytes = sizeof(*this);
    m.current_bytes += uint64_t(current_queue.capacity()) * sizeof(QueueTaskItem);
    m.current_bytes += uint64_t(stream_accepting_.capacity()) * sizeof(uint8_t);
    // Deque містить блоки та службові вказівники. Оцінка пам’яті є приблизною.
    m.current_bytes += uint64_t(queue_variants_.size()) *
        (sizeof(Variant) + sizeof(void*) * 3);
    for (const Variant& v : queue_variants_)
        m.current_bytes += uint64_t(v.items.capacity()) * sizeof(QueueTaskItem);
    m.last_search_peak_bytes = last_search_peak_bytes_;
    m.last_search_nodes = last_search_nodes_;
    m.last_search_enclaves = last_search_enclaves_;
    m.search_limit_hit = last_search_limit_hit_;
    m.estimated_peak_bytes = m.current_bytes + m.last_search_peak_bytes;
    return m;
}
