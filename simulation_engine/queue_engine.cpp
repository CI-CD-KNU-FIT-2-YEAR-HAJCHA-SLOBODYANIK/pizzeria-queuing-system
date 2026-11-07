#include "queue_engine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <string>
#include <unordered_set>
#include <utility>

namespace {
    struct Gap { int lo, hi; }; // Допустимые начала для задачи в свободном интервале

    int clamp_int(int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); }

    bool fixed(const QueueTaskItem& t) {
        return t.task_id < 0 || (t.min_deadline_sec == t.start_time_sec &&
            t.max_deadline_sec == t.start_time_sec + t.cooking_time_sec);
    }

    bool overlaps(const QueueTaskItem& a, const QueueTaskItem& b) {
        return a.start_time_sec < b.start_time_sec + b.cooking_time_sec &&
            b.start_time_sec < a.start_time_sec + a.cooking_time_sec;
    }

    std::vector<Gap> free_gaps(const std::vector<QueueTaskItem>& tasks, int stream,
        int except, int duration, int lo, int hi) {
        std::vector<std::pair<int, int>> intervals;
        intervals.reserve(tasks.size());
        for (int i = 0; i < int(tasks.size()); ++i) {
            if (i == except || tasks[i].stream_id != stream) continue;
            intervals.emplace_back(tasks[i].start_time_sec,
                tasks[i].start_time_sec + tasks[i].cooking_time_sec);
        }
        std::sort(intervals.begin(), intervals.end());
        std::vector<Gap> gaps;
        int cursor = lo;
        for (auto [a, b] : intervals) {
            if (a > cursor && a - duration >= cursor)
                gaps.push_back({ cursor, a - duration });
            cursor = std::max(cursor, b);
            if (cursor > hi) break;
        }
        if (cursor <= hi - duration) gaps.push_back({ cursor, hi - duration });
        return gaps;
    }

    void measure(const std::vector<QueueTaskItem>& original,
        const std::vector<QueueTaskItem>& tasks, int& moved, int64_t& displacement) {
        moved = 0; displacement = 0;
        for (int i = 0; i < int(original.size()); ++i) {
            if (tasks[i].start_time_sec != original[i].start_time_sec ||
                tasks[i].stream_id != original[i].stream_id) {
                ++moved;
                displacement += std::abs(int64_t(tasks[i].start_time_sec) - original[i].start_time_sec);
                if (tasks[i].stream_id != original[i].stream_id) displacement += 1;
            }
        }
    }

    bool better(const EngineSolution& candidate, const EngineSolution& previous,
        bool left) {
        if (!candidate.found) return false;
        if (!previous.found) return true;
        if (candidate.start_sec != previous.start_sec)
            return left ? candidate.start_sec > previous.start_sec : candidate.start_sec < previous.start_sec;
        if (candidate.moved_count != previous.moved_count)
            return candidate.moved_count < previous.moved_count;
        return candidate.total_displacement < previous.total_displacement;
    }

    void evaluate(const std::vector<QueueTaskItem>& original,
        const std::vector<QueueTaskItem>& tasks, const std::vector<uint8_t>& enabled,
        int target, int duration, int temporary_id, int lo, int hi, EngineInsertResult& result) {
        int moved; int64_t displacement;
        measure(original, tasks, moved, displacement);
        for (int stream = 0; stream < int(enabled.size()); ++stream) {
            if (!enabled[stream]) continue;
            for (const Gap& gap : free_gaps(tasks, stream, -1, duration, lo, hi)) {
                for (int side = 0; side < 2; ++side) {
                    const int start = side == 0 ? std::min(target, gap.hi) : std::max(target, gap.lo);
                    if (start < gap.lo || start > gap.hi) continue;
                    EngineSolution found;
                    found.found = true;
                    found.start_sec = start;
                    found.moved_count = moved;
                    found.total_displacement = displacement;
                    EngineSolution& best = side == 0 ? result.left : result.right;
                    if (!better(found, best, side == 0)) continue;
                    found.tasks = tasks;
                    found.tasks.push_back({ start, start + duration, duration, start, temporary_id, stream });
                    best = std::move(found);
                }
            }
        }
    }

    std::string signature(const std::vector<QueueTaskItem>& tasks) {
        std::string s;
        s.reserve(tasks.size() * 8);
        for (const auto& t : tasks) {
            const int32_t values[2] = { t.start_time_sec, t.stream_id };
            for (int32_t v : values) {
                const uint32_t u = uint32_t(v);
                for (int n = 0; n < 4; ++n) s.push_back(char((u >> (n * 8)) & 255));
            }
        }
        return s;
    }

    struct CandidateMove { int start, stream; int64_t cost; };

    // Генерирует только границы свободных интервалов и времена рядом с целью.
    // Посекундный перебор всей смены не требуется.
    std::vector<CandidateMove> choices(const std::vector<QueueTaskItem>& tasks,
        const std::vector<uint8_t>& enabled, int idx, int target, int probe_duration,
        int cap, int current_sec) {
        const QueueTaskItem& t = tasks[idx];
        std::vector<CandidateMove> out;
        if (fixed(t)) return out;
        const int lo = std::max(t.min_deadline_sec, current_sec);
        const int hi = t.max_deadline_sec - t.cooking_time_sec;
        if (lo > hi) return out;
        for (int stream = 0; stream < int(enabled.size()); ++stream) {
            if (!enabled[stream]) continue;
            for (const Gap& gap : free_gaps(tasks, stream, idx, t.cooking_time_sec,
                lo, t.max_deadline_sec)) {
                const int a = std::max(lo, gap.lo);
                const int b = std::min(hi, gap.hi);
                if (a > b) continue;
                const int anchors[] = { a, b, clamp_int(t.start_time_sec, a, b),
                    clamp_int(target - t.cooking_time_sec, a, b),
                    clamp_int(target + probe_duration, a, b) };
                for (int start : anchors) {
                    if (start == t.start_time_sec && stream == t.stream_id) continue;
                    const int64_t cost = std::abs(int64_t(start) - t.start_time_sec) * 2 +
                        (stream == t.stream_id ? 0 : 1);
                    out.push_back({ start, stream, cost });
                }
            }
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (a.cost != b.cost) return a.cost < b.cost;
            if (a.start != b.start) return a.start < b.start;
            return a.stream < b.stream;
            });
        out.erase(std::unique(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return a.start == b.start && a.stream == b.stream;
            }), out.end());
        if (int(out.size()) > cap) out.resize(cap);
        return out;
    }

    struct SearchNode {
        std::vector<QueueTaskItem> tasks;
        int moved = 0;
        int64_t displacement = 0;
        int64_t priority = 0;
        int serial = 0;
    };
    struct SearchWorse {
        bool operator()(const SearchNode& a, const SearchNode& b) const {
            if (a.priority != b.priority) return a.priority > b.priority;
            return a.serial > b.serial;
        }
    };

    int64_t best_distance(const EngineInsertResult& r, int target) {
        int64_t d = std::numeric_limits<int64_t>::max() / 2;
        if (r.left.found) d = std::min(d, int64_t(target) - r.left.start_sec);
        if (r.right.found) d = std::min(d, int64_t(r.right.start_sec) - target);
        return d;
    }

    std::vector<int> blockers(const std::vector<QueueTaskItem>& tasks, int target, int duration) {
        std::vector<std::pair<int64_t, int>> entries;
        for (int i = 0; i < int(tasks.size()); ++i) {
            if (fixed(tasks[i])) continue;
            const int a = tasks[i].start_time_sec;
            const int b = a + tasks[i].cooking_time_sec;
            // Приоритет задач, пересекающих целевой интервал.
            const bool crossing = a < target + duration && target < b;
            const int64_t gap = crossing ? 0 : (b <= target ? target - b : a - (target + duration));
            entries.emplace_back(gap * 100000 + std::abs(int64_t(a) - target), i);
        }
        std::sort(entries.begin(), entries.end());
        std::vector<int> ids;
        for (int i = 0; i < int(entries.size()) && i < 10; ++i) ids.push_back(entries[i].second);
        return ids;
    }

    struct RepackItem { int index, slack, original_start; };

    // Размещает интервалы с жёсткой фиксацией уже выполняющихся задач.
    // Ветка всегда валидна; откат не меняет original.
    struct RepackSearch {
        const std::vector<QueueTaskItem>& original;
        const std::vector<uint8_t>& enabled;
        const QueueConfig& cfg;
        std::vector<QueueTaskItem> result;
        std::vector<QueueTaskItem> placed;
        std::vector<RepackItem> move_list;
        int visited = 0;
        bool exhaustive = false;
        int current_sec = 0;

        bool dfs(int pos) {
            if (++visited > (exhaustive ? cfg.max_repack_nodes : 1 + int(move_list.size()))) return false;
            if (pos == int(move_list.size())) {
                result = placed;
                return true;
            }
            const QueueTaskItem& t = original[move_list[pos].index];
            const int lo = std::max(t.min_deadline_sec, current_sec);
            const int hi = t.max_deadline_sec - t.cooking_time_sec;
            if (lo > hi) return false;
            std::vector<CandidateMove> possibilities;
            for (int s = 0; s < int(enabled.size()); ++s) {
                if (!enabled[s]) continue;
                for (const Gap& gap : free_gaps(placed, s, -1, t.cooking_time_sec, lo, t.max_deadline_sec)) {
                    const int a = std::max(lo, gap.lo);
                    const int b = std::min(hi, gap.hi);
                    if (a > b) continue;
                    const int candidate[] = { clamp_int(t.start_time_sec, a, b), a, b };
                    for (int start : candidate) {
                        const int64_t cost = std::abs(int64_t(t.start_time_sec) - start) * 2 +
                            (s == t.stream_id ? 0 : 1);
                        possibilities.push_back({ start, s, cost });
                    }
                }
            }
            std::sort(possibilities.begin(), possibilities.end(), [](auto a, auto b) {
                if (a.cost != b.cost) return a.cost < b.cost;
                if (a.start != b.start) return a.start < b.start;
                return a.stream < b.stream;
                });
            possibilities.erase(std::unique(possibilities.begin(), possibilities.end(),
                [](auto a, auto b) { return a.start == b.start && a.stream == b.stream; }), possibilities.end());
            const int branches = exhaustive ? std::max(1, cfg.max_branch_per_task) : 1;
            for (int i = 0; i < int(possibilities.size()) && i < branches; ++i) {
                auto t2 = t;
                t2.start_time_sec = possibilities[i].start;
                t2.stream_id = possibilities[i].stream;
                placed.push_back(t2);
                if (dfs(pos + 1)) return true;
                placed.pop_back();
            }
            return false;
        }
    };
} // namespace (только внутренние детали реализации)

bool QueueEngine::valid(const std::vector<QueueTaskItem>& tasks,
    const std::vector<uint8_t>& enabled, bool allow_frozen_existing) {
    for (int i = 0; i < int(tasks.size()); ++i) {
        const auto& t = tasks[i];
        if (t.cooking_time_sec <= 0 || t.stream_id < 0 || t.stream_id >= int(enabled.size()) ||
            t.start_time_sec < 0 || t.min_deadline_sec > t.start_time_sec ||
            int64_t(t.start_time_sec) + t.cooking_time_sec > t.max_deadline_sec ||
            (!enabled[t.stream_id] && (!allow_frozen_existing || t.task_id < 0))) return false;
        for (int j = 0; j < i; ++j)
            if (tasks[j].stream_id == t.stream_id && overlaps(tasks[j], t)) return false;
    }
    return true;
}

EngineInsertResult QueueEngine::insert(const std::vector<QueueTaskItem>& original,
    const std::vector<uint8_t>& enabled, int target, int duration, int temporary_id,
    int min_sec, int max_sec, const QueueConfig& cfg, int current_sec) {
    EngineInsertResult result;
    if (!queue_mode_implemented(cfg.mode)) return result;
    if (duration <= 0 || min_sec < 0 || current_sec < 0 ||
        max_sec > cfg.max_time_sec || max_sec - min_sec < duration ||
        min_sec < current_sec || !valid(original, enabled)) return result;
    // Устаревшее расписание не должно становиться базой нового плана.
    for (const auto& t : original)
        if (!fixed(t) && t.start_time_sec < current_sec) return result;

    evaluate(original, original, enabled, target, duration, temporary_id, min_sec, max_sec, result);
    if (cfg.mode == QueueSearchMode::Fast ||
        (result.left.found && result.left.start_sec == target &&
            result.right.found && result.right.start_sec == target)) return result;

    std::priority_queue<SearchNode, std::vector<SearchNode>, SearchWorse> frontier;
    std::unordered_set<std::string> seen;
    frontier.push({ original, 0, 0, best_distance(result, target), 0 });
    seen.insert(signature(original));
    int serial = 1;
    int expanded = 0;
    const int budget = std::max(1, cfg.max_search_nodes);
    while (!frontier.empty() && expanded < budget && int(seen.size()) < budget * 8) {
        SearchNode node = frontier.top(); frontier.pop();
        ++expanded;
        const std::vector<int> ids = blockers(node.tasks, target, duration);
        for (int idx : ids) {
            for (const CandidateMove& move : choices(node.tasks, enabled, idx, target,
                duration, std::max(1, cfg.max_branch_per_task), current_sec)) {
                auto changed = node.tasks;
                changed[idx].start_time_sec = move.start;
                changed[idx].stream_id = move.stream;
                const std::string key = signature(changed);
                if (!seen.insert(key).second) continue;
                int moved; int64_t displacement;
                measure(original, changed, moved, displacement);
                EngineInsertResult local;
                evaluate(original, changed, enabled, target, duration, temporary_id,
                    min_sec, max_sec, local);
                if (better(local.left, result.left, true)) result.left = std::move(local.left);
                if (better(local.right, result.right, false)) result.right = std::move(local.right);
                if (result.left.found && result.right.found &&
                    result.left.start_sec == target && result.right.start_sec == target) {
                    // Целевое начало является нижней границей ошибки по времени.
                    if (expanded >= 8) return result;
                }
                const int64_t distance = best_distance(local, target);
                const int64_t priority = distance * 16 + moved * 5 + std::min<int64_t>(displacement, 1000);
                frontier.push({ std::move(changed), moved, displacement, priority, serial++ });
            }
        }
    }
    result.search_limit_hit = !frontier.empty();
    return result;
}

bool QueueEngine::repack(const std::vector<QueueTaskItem>& original,
    const std::vector<uint8_t>& enabled, int closing_stream_id, int preserved_task_id,
    const QueueConfig& cfg, std::vector<QueueTaskItem>& output, int current_sec) {
    if (current_sec < 0 || !queue_mode_implemented(cfg.mode)) return false;
    RepackSearch solver{ original, enabled, cfg, {}, {}, {}, 0, false, current_sec };
    for (int i = 0; i < int(original.size()); ++i) {
        const auto& t = original[i];
        const bool pinned = t.task_id == preserved_task_id || fixed(t);
        // Прошедшую незакреплённую подготовку нельзя переносить задним числом.
        if (!pinned && t.start_time_sec < current_sec) return false;
        if (pinned) {
            // Другую уже закреплённую задачу запрещено выталкивать с закрытого потока.
            if (t.stream_id == closing_stream_id && t.task_id != preserved_task_id) return false;
            solver.placed.push_back(t);
        }
        else {
            solver.move_list.push_back({ i, t.max_deadline_sec - t.min_deadline_sec - t.cooking_time_sec,
                t.start_time_sec });
        }
    }
    if (!valid(solver.placed, enabled)) return false;
    std::sort(solver.move_list.begin(), solver.move_list.end(), [](auto a, auto b) {
        if (a.slack != b.slack) return a.slack < b.slack;
        return a.original_start < b.original_start;
        });
    if (solver.dfs(0)) {
        if (!valid(solver.result, enabled)) return false;
        output = std::move(solver.result);
        return true;
    }
    if (cfg.mode == QueueSearchMode::Bounded) {
        solver.visited = 0;
        solver.exhaustive = true;
        if (solver.dfs(0)) {
            output = std::move(solver.result);
            return valid(output, enabled);
        }
    }
    return false;
}