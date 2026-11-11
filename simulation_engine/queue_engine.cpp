#include "queue_engine.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <functional>
#include <set>
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
            auto test = [&](int start, int stream) {
                auto t2 = t;
                t2.start_time_sec = start;
                t2.stream_id = stream;
                placed.push_back(t2);
                const bool found = dfs(pos + 1);
                placed.pop_back();
                return found;
                };
            for (int i = 0; i < int(possibilities.size()) && i < branches; ++i) {
                if (test(possibilities[i].start, possibilities[i].stream))return true;
                if (exhaustive && visited >= cfg.max_repack_nodes)return false;
            }
            if (exhaustive && cfg.mode == QueueSearchMode::LiteBrute) {
                // Дополнительный посекундный перебор с откатом, не создавая
                // массива всех секунд и не ограничивая его опорными точками.
                for (int stream = 0; stream<int(enabled.size()); ++stream) {
                    if (!enabled[stream])continue;
                    for (const Gap& gap : free_gaps(placed, stream, -1, t.cooking_time_sec, lo, t.max_deadline_sec)) {
                        const int a = std::max(lo, gap.lo), b = std::min(hi, gap.hi);
                        if (a > b)continue;
                        const int center = clamp_int(t.start_time_sec, a, b);
                        for (int delta = 0; delta <= std::max(center - a, b - center); ++delta) {
                            if (visited >= cfg.max_repack_nodes)return false;
                            const int alternatives[] = { center + delta,center - delta };
                            for (int side = 0; side < (delta == 0 ? 1 : 2); ++side) {
                                const int start = alternatives[side];
                                if (start<a || start>b)continue;
                                const bool already = std::any_of(possibilities.begin(), possibilities.end(),
                                    [&](const CandidateMove& m) {return m.start == start && m.stream == stream; });
                                if (already)continue;
                                if (test(start, stream))return true;
                            }
                        }
                    }
                }
            }
            return false;
        }
    };

    // Полный перебор с откатом: задача не создаёт отдельной копии расписания в каждом узле.
    // Новые временные метки возникают при добавлении каждого очередного интервала.
    struct LiteBruteSearch {
        const std::vector<QueueTaskItem>& original;
        const std::vector<uint8_t>& enabled;
        const QueueConfig& cfg;
        EngineInsertResult& result;
        int target, duration, temp_id, lo, hi, now;
        int64_t examined = 0;
        const int64_t budget;
        int peak_depth = 0;
        std::vector<int> order;
        std::vector<QueueTaskItem> placed;
        std::vector<QueueTaskItem> assembled;
        std::set<int> times;
        std::set<int> visited_times;
        bool stop = false;

        LiteBruteSearch(const std::vector<QueueTaskItem>& src,
            const std::vector<uint8_t>& flags, const QueueConfig& config,
            EngineInsertResult& out, int target_sec, int length, int id,
            int min_start, int max_end, int current)
            : original(src), enabled(flags), cfg(config), result(out), target(target_sec),
            duration(length), temp_id(id), lo(min_start), hi(max_end - length), now(current),
            budget(std::max(1, config.max_search_nodes)) {
            order.reserve(src.size());
            placed.reserve(src.size() + 1);
            assembled.resize(src.size());
            for (int i = 0; i<int(src.size()); ++i) {
                if (fixed(src[i]) || src[i].start_time_sec < now)
                    placed.push_back(src[i]);
                else order.push_back(i);
            }
            // Сначала ограниченные по времени задачи: это обрезает дерево раньше.
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                const auto& x = original[a]; const auto& y = original[b];
                const int sx = x.max_deadline_sec - x.min_deadline_sec - x.cooking_time_sec;
                const int sy = y.max_deadline_sec - y.min_deadline_sec - y.cooking_time_sec;
                return sx == sy ? x.start_time_sec < y.start_time_sec : sx < sy;
                });
        }

        void add_time(int t) {
            if (t >= lo && t <= hi && times.size() < size_t(budget * 3 + 16)) times.insert(t);
        }
        void from_task(int start, int length) {
            add_time(start);
            add_time(start + length);
            add_time(start - duration);
            add_time(start + length - duration);
        }
        void initial_times() {
            add_time(target); add_time(lo); add_time(hi);
            for (const auto& t : original) from_task(t.start_time_sec, t.cooking_time_sec);
        }
        void tick() {
            ++examined;
            peak_depth = std::max(peak_depth, int(placed.size()));
            if (examined >= budget) { stop = true; result.search_limit_hit = true; }
        }
        bool fits(const QueueTaskItem& t) const {
            for (const auto& p : placed)
                if (p.stream_id == t.stream_id && overlaps(p, t)) return false;
            return true;
        }
        bool dfs(int position, int probe_start, int probe_stream) {
            if (stop) return false;
            if (position == int(order.size())) {
                // В исходном порядке восстановление требуется только на листьях.
                assembled = original;
                for (size_t n = 0; n < order.size(); ++n) {
                    const auto& task = placed[placed.size() - order.size() + n];
                    assembled[order[n]] = task;
                }
                int moved; int64_t displacement;
                measure(original, assembled, moved, displacement);
                EngineSolution candidate;
                candidate.found = true;
                candidate.start_sec = probe_start;
                candidate.moved_count = moved;
                candidate.total_displacement = displacement;
                candidate.tasks = assembled;
                candidate.tasks.push_back({ probe_start,probe_start + duration,duration,
                    probe_start,temp_id,probe_stream });
                if (probe_start <= target && better(candidate, result.left, true))
                    result.left = candidate;
                if (probe_start >= target && better(candidate, result.right, false))
                    result.right = std::move(candidate);
                return moved == 0 && probe_start == target;
            }
            const QueueTaskItem& task = original[order[position]];
            const int first = std::max(now, task.min_deadline_sec);
            const int last = task.max_deadline_sec - task.cooking_time_sec;
            if (first > last) return false;
            for (int s = 0; s<int(enabled.size()) && !stop; ++s) {
                if (!enabled[s]) continue;
                const auto gaps = free_gaps(placed, s, -1, task.cooking_time_sec, first, task.max_deadline_sec);
                for (const Gap& g : gaps) {
                    if (stop) break;
                    const int a = std::max(first, g.lo), b = std::min(last, g.hi);
                    if (a > b) continue;
                    // Метки появляются по мере построения конкретной временной ветви.
                    std::vector<int> marks = { task.start_time_sec,a,b,
                        probe_start - task.cooking_time_sec,probe_start + duration };
                    for (const auto& p : placed) {
                        marks.push_back(p.start_time_sec - task.cooking_time_sec);
                        marks.push_back(p.start_time_sec + p.cooking_time_sec);
                    }
                    marks.erase(std::remove_if(marks.begin(), marks.end(),
                        [&](int v) {return v<a || v>b; }), marks.end());
                    std::sort(marks.begin(), marks.end(), [&](int x, int y) {
                        const auto dx = std::abs(int64_t(x) - task.start_time_sec);
                        const auto dy = std::abs(int64_t(y) - task.start_time_sec);
                        if (dx != dy)return dx < dy;
                        return x > y; // Сначала вправо; после неуспеха ветка идёт влево.
                        });
                    marks.erase(std::unique(marks.begin(), marks.end()), marks.end());
                    auto try_start = [&](int start)->bool {
                        tick();
                        if (stop)return false;
                        QueueTaskItem value = task;
                        value.start_time_sec = start;
                        value.stream_id = s;
                        placed.push_back(value);
                        from_task(start, task.cooking_time_sec);
                        const bool done = dfs(position + 1, probe_start, probe_stream);
                        placed.pop_back();  // Откат только одного назначения.
                        return done;
                        };
                    for (int mark : marks) {
                        if (stop)break;
                        if (try_start(mark))return true;
                    }
                    // Если опорных точек недостаточно, перебираем все секунды диапазона.
                    // Никаких массивов длиной в сутки не создаётся.
                    if (!stop) {
                        std::unordered_set<int> anchors(marks.begin(), marks.end());
                        const int center = clamp_int(task.start_time_sec, a, b);
                        const int width = std::max(center - a, b - center);
                        for (int delta = 0; delta <= width && !stop; ++delta) {
                            const int right = center + delta;
                            if (right <= b && !anchors.count(right) && try_start(right))return true;
                            const int left = center - delta;
                            if (delta && left >= a && !anchors.count(left) && try_start(left))return true;
                        }
                    }
                }
            }
            return false;
        }
        void run() {
            initial_times();
            int delta = 1;
            bool right_next = true;
            while (!stop) {
                int point = -1;
                if (!times.empty()) {
                    auto it = times.lower_bound(target);
                    auto candidate = times.end();
                    if (it != times.end())candidate = it;
                    if (it != times.begin()) {
                        auto left = std::prev(it);
                        if (candidate == times.end() || target - *left < *candidate - target)
                            candidate = left;
                    }
                    point = *candidate;
                    times.erase(candidate);
                }
                else {
                    // Полное уточнение после исчерпания динамически созданных меток.
                    // Сначала проверяем секунды справа, затем симметрично слева.
                    while (delta <= std::max(target - lo, hi - target)) {
                        const int candidate = right_next ? target + delta : target - delta;
                        if (!right_next)++delta;
                        right_next = !right_next;
                        if (candidate >= lo && candidate <= hi && !visited_times.count(candidate)) {
                            point = candidate;
                            break;
                        }
                    }
                    if (point < 0)break;
                }
                if (!visited_times.insert(point).second)continue;
                for (int stream = 0; stream<int(enabled.size()) && !stop; ++stream) {
                    if (!enabled[stream])continue;
                    tick(); // Учитываем в бюджете также проверку точки при отсутствии подвижных задач.
                    if (stop)break;
                    QueueTaskItem probe{ point,point + duration,duration,point,temp_id,stream };
                    if (!fits(probe))continue;
                    placed.push_back(probe);
                    if (dfs(0, point, stream)) { stop = true; }
                    placed.pop_back();
                }
                if (result.left.found && result.right.found &&
                    result.left.start_sec == target && result.right.start_sec == target &&
                    result.left.moved_count == 0 && result.right.moved_count == 0)break;
            }
            result.diagnostics.nodes_expanded = uint32_t(std::min<int64_t>(examined, UINT32_MAX));
            // Рабочие стеки, массивы и узлы std::set приблизительно по sizeof.
            const uint64_t bytes = sizeof(*this) + uint64_t(placed.capacity() + assembled.capacity()) * sizeof(QueueTaskItem) +
                uint64_t(order.capacity()) * sizeof(int) +
                uint64_t(times.size() + visited_times.size()) * (sizeof(int) + 3 * sizeof(void*) + 32) +
                uint64_t(peak_depth) * sizeof(void*) * 12;
            result.diagnostics.peak_bytes = std::max(result.diagnostics.peak_bytes, bytes);
            result.diagnostics.budget_exhausted = result.search_limit_hit;
        }
    };

    // Узлы анклавного дерева содержат только родителя и диапазон перемещений в арене.
    struct EnclaveMove { int index, start, stream; };
    struct EnclaveNode { int parent = -1, begin = 0, count = 0, depth = 0; int64_t priority = 0; };
    struct OccupancyStep { int begin, end, count; };
    struct StableProfile {
        int node = -1;
        std::vector<OccupancyStep> timeline;
        std::vector<uint64_t> changed_mask;
    };
    struct EnclavePatch {
        int source_begin = 0, source_end = 0;
        std::vector<EnclaveMove> before, after;
        std::vector<uint64_t> changed_mask;
    };

    struct EnclaveTreeSearch {
        const std::vector<QueueTaskItem>& original;
        const std::vector<uint8_t>& enabled;
        const QueueConfig& cfg;
        EngineInsertResult& result;
        int target, duration, temporary_id, lo, hi, now;
        std::vector<EnclaveNode> nodes;
        std::vector<EnclaveMove> move_arena;
        std::vector<StableProfile> stable;
        std::vector<EnclavePatch> enclaves;
        std::unordered_set<std::string> seen;
        using Work = std::pair<int64_t, int>;
        std::priority_queue<Work, std::vector<Work>, std::greater<Work>> frontier;
        uint64_t max_bytes = 0;
        uint32_t expanded = 0;

        EnclaveTreeSearch(const std::vector<QueueTaskItem>& src, const std::vector<uint8_t>& flags,
            const QueueConfig& config, EngineInsertResult& out, int tgt, int len, int id,
            int low, int high, int current)
            :original(src), enabled(flags), cfg(config), result(out), target(tgt), duration(len),
            temporary_id(id), lo(low), hi(high), now(current) {
            const int reserve = std::max(2, cfg.max_search_nodes);
            nodes.reserve(reserve);
            move_arena.reserve(reserve * 2);
            nodes.push_back({ -1,0,0,0,0 });
            seen.insert(signature(original));
            frontier.push({ 0,0 });
        }

        std::vector<QueueTaskItem> reconstruct(int node) const {
            std::vector<QueueTaskItem> tasks = original;
            std::vector<int> chain;
            for (int id = node; id > 0; id = nodes[id].parent)chain.push_back(id);
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const auto& n = nodes[*it];
                for (int j = n.begin; j < n.begin + n.count; ++j) {
                    const auto& m = move_arena[j];
                    tasks[m.index].start_time_sec = m.start;
                    tasks[m.index].stream_id = m.stream;
                }
            }
            return tasks;
        }
        static std::vector<OccupancyStep> usage_profile(const std::vector<QueueTaskItem>& tasks) {
            std::vector<std::pair<int, int>> events;
            events.reserve(tasks.size() * 2);
            for (const auto& t : tasks) {
                events.push_back({ t.start_time_sec,1 });
                events.push_back({ t.start_time_sec + t.cooking_time_sec,-1 });
            }
            std::sort(events.begin(), events.end());
            std::vector<OccupancyStep> profile;
            int active = 0, prev = 0;
            for (size_t i = 0; i < events.size();) {
                const int instant = events[i].first;
                if (instant > prev && active > 0)profile.push_back({ prev,instant,active });
                while (i < events.size() && events[i].first == instant)active += events[i++].second;
                prev = instant;
            }
            return profile;
        }
        std::vector<uint64_t> mask(const std::vector<QueueTaskItem>& state) const {
            std::vector<uint64_t> bits((state.size() + 63) / 64, 0);
            for (size_t i = 0; i < state.size(); ++i)
                if (state[i].start_time_sec != original[i].start_time_sec ||
                    state[i].stream_id != original[i].stream_id)bits[i / 64] |= 1ULL << (i % 64);
            return bits;
        }
        int conflict_count(const std::vector<QueueTaskItem>& state) const {
            int conflicts = 0;
            for (int i = 0; i<int(state.size()); ++i)
                for (int j = 0; j < i; ++j)
                    if (state[i].stream_id == state[j].stream_id && overlaps(state[i], state[j]))++conflicts;
            return conflicts;
        }
        std::vector<int> relevant(const std::vector<QueueTaskItem>& state) const {
            std::vector<int> list = blockers(state, target, duration);
            for (int i = 0; i<int(state.size()); ++i) {
                if (fixed(state[i]) || state[i].start_time_sec < now)continue;
                for (int j = 0; j<int(state.size()); ++j) {
                    if (i != j && state[i].stream_id == state[j].stream_id && overlaps(state[i], state[j])) {
                        if (std::find(list.begin(), list.end(), i) == list.end())list.insert(list.begin(), i);
                        break;
                    }
                }
            }
            if (list.size() > 16)list.resize(16);
            return list;
        }
        void register_stable(int node, const std::vector<QueueTaskItem>& state) {
            StableProfile v;
            v.node = node;
            v.timeline = usage_profile(state);
            v.changed_mask = mask(state);
            stable.push_back(std::move(v));
            if (node == 0 || enclaves.size() >= size_t(std::max(1, cfg.max_search_nodes / 3)))return;
            EnclavePatch patch;
            for (size_t i = 0; i < state.size(); ++i) {
                if (state[i].start_time_sec == original[i].start_time_sec &&
                    state[i].stream_id == original[i].stream_id)continue;
                patch.before.push_back({ int(i),original[i].start_time_sec,original[i].stream_id });
                patch.after.push_back({ int(i),state[i].start_time_sec,state[i].stream_id });
                if (patch.after.size() > 8)return;
                const auto& old = original[i];
                if (patch.after.size() == 1)patch.source_begin = old.start_time_sec;
                patch.source_begin = std::min(patch.source_begin, old.start_time_sec);
                patch.source_end = std::max(patch.source_end, old.start_time_sec + old.cooking_time_sec);
            }
            if (patch.after.empty())return;
            patch.changed_mask = stable.back().changed_mask;
            enclaves.push_back(std::move(patch));
        }
        void note_memory() {
            uint64_t b = sizeof(*this) + uint64_t(nodes.capacity()) * sizeof(EnclaveNode) +
                uint64_t(move_arena.capacity()) * sizeof(EnclaveMove) +
                uint64_t(stable.capacity()) * sizeof(StableProfile) +
                uint64_t(enclaves.capacity()) * sizeof(EnclavePatch) +
                uint64_t(seen.size()) * (sizeof(std::string) + sizeof(void*) * 4 + original.size() * 8) +
                uint64_t(frontier.size()) * sizeof(Work);
            for (const auto& s : stable)
                b += uint64_t(s.timeline.capacity()) * sizeof(OccupancyStep) +
                uint64_t(s.changed_mask.capacity()) * sizeof(uint64_t);
            for (const auto& e : enclaves)
                b += uint64_t(e.before.capacity() + e.after.capacity()) * sizeof(EnclaveMove) +
                uint64_t(e.changed_mask.capacity()) * sizeof(uint64_t);
            max_bytes = std::max(max_bytes, b);
        }
        bool push(int parent, const std::vector<EnclaveMove>& patch,
            const std::vector<QueueTaskItem>& changed) {
            if (int(nodes.size()) >= std::max(2, cfg.max_search_nodes))return false;
            const std::string key = signature(changed);
            if (!seen.insert(key).second)return false;
            int moved; int64_t displacement;
            measure(original, changed, moved, displacement);
            EngineInsertResult local;
            if (QueueEngine::valid(changed, enabled))
                evaluate(original, changed, enabled, target, duration, temporary_id, lo, hi, local);
            const auto dist = best_distance(local, target);
            const int conflicts = conflict_count(changed);
            const int64_t score = (dist >= INT64_MAX / 4 ? int64_t(1000000) : dist) * 4 +
                int64_t(conflicts) * 10000 + moved * 20 + std::min<int64_t>(displacement, 5000);
            const int id = int(nodes.size());
            const int begin = int(move_arena.size());
            move_arena.insert(move_arena.end(), patch.begin(), patch.end());
            nodes.push_back({ parent,begin,int(patch.size()),nodes[parent].depth + 1,score });
            frontier.push({ score,id });
            return true;
        }
        void apply_enclaves(int node, const std::vector<QueueTaskItem>& state) {
            const std::vector<uint64_t> changed = mask(state);
            for (const auto& enc : enclaves) {
                // Маска быстро исключает группы, затрагивающие уже перенесённые задачи.
                bool intersect = false;
                for (size_t word = 0; word < changed.size(); ++word)
                    if ((changed[word] & enc.changed_mask[word]) != 0) { intersect = true; break; }
                if (intersect)continue;
                bool matching = true;
                for (const auto& m : enc.before)
                    if (state[m.index].start_time_sec != m.start ||
                        state[m.index].stream_id != m.stream) {
                        matching = false; break;
                    }
                if (!matching)continue;
                auto changed = state;
                for (const auto& m : enc.after) {
                    changed[m.index].start_time_sec = m.start;
                    changed[m.index].stream_id = m.stream;
                }
                if (QueueEngine::valid(changed, enabled))push(node, enc.after, changed);
            }
        }
        void run() {
            const int cap = std::max(1, cfg.max_search_nodes);
            while (!frontier.empty() && int(expanded) < cap) {
                const auto [priority, node] = frontier.top(); frontier.pop();
                (void)priority;
                ++expanded;
                auto state = reconstruct(node);
                const bool valid = QueueEngine::valid(state, enabled);
                if (valid) {
                    register_stable(node, state);
                    evaluate(original, state, enabled, target, duration, temporary_id, lo, hi, result);
                    if (result.left.found && result.right.found &&
                        result.left.start_sec == target && result.right.start_sec == target &&
                        result.left.moved_count == 0 && result.right.moved_count == 0)break;
                }
                if (node > 0 && valid && !enclaves.empty())apply_enclaves(node, state);
                auto candidate_tasks = relevant(state);
                if (valid && !stable.empty()) {
                    // Профиль устойчивого состояния показывает насыщенные интервалы.
                    // Проверка точной совместимости всё равно происходит по stream_id.
                    const int available = int(std::count_if(enabled.begin(), enabled.end(),
                        [](uint8_t on) {return on != 0; }));
                    for (const OccupancyStep& step : stable.back().timeline) {
                        if (step.count < available || step.end <= target || step.begin >= target + duration)
                            continue;
                        std::stable_sort(candidate_tasks.begin(), candidate_tasks.end(),
                            [&](int a, int b) {
                                const bool overlaps_a = state[a].start_time_sec<step.end &&
                                    state[a].start_time_sec + state[a].cooking_time_sec>step.begin;
                                const bool overlaps_b = state[b].start_time_sec<step.end &&
                                    state[b].start_time_sec + state[b].cooking_time_sec>step.begin;
                                return overlaps_a > overlaps_b;
                            });
                        break;
                    }
                }
                for (int idx : candidate_tasks) {
                    if (fixed(state[idx]) || state[idx].start_time_sec < now)continue;
                    const auto proposed = choices(state, enabled, idx, target, duration,
                        std::max(1, cfg.max_branch_per_task), now);
                    for (const auto& p : proposed) {
                        auto moved = state;
                        moved[idx].start_time_sec = p.start;
                        moved[idx].stream_id = p.stream;
                        if (moved[idx].start_time_sec < now ||
                            moved[idx].start_time_sec < moved[idx].min_deadline_sec ||
                            int64_t(moved[idx].start_time_sec) + moved[idx].cooking_time_sec >
                            moved[idx].max_deadline_sec)continue;
                        push(node, { {idx,p.start,p.stream} }, moved);
                    }
                    if (int(nodes.size()) >= cap)break;
                }
                note_memory();
                if (int(nodes.size()) >= cap)break;
            }
            const bool exact_zero = result.left.found && result.right.found &&
                result.left.start_sec == target && result.right.start_sec == target &&
                result.left.moved_count == 0 && result.right.moved_count == 0;
            result.search_limit_hit = !exact_zero && (!frontier.empty() ||
                int(nodes.size()) >= std::max(2, cfg.max_search_nodes));
            result.diagnostics.budget_exhausted = result.search_limit_hit;
            result.diagnostics.nodes_expanded = expanded;
            result.diagnostics.enclaves_created = uint32_t(enclaves.size());
            note_memory();
            result.diagnostics.peak_bytes = std::max(result.diagnostics.peak_bytes, max_bytes);
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
        max_sec > cfg.max_time_sec || max_sec - std::max(min_sec, current_sec) < duration ||
        !valid(original, enabled)) return result;
    min_sec = std::max(min_sec, current_sec);
    // Устаревшее расписание не должно становиться базой нового плана.
    for (const auto& t : original)
        if (!fixed(t) && t.start_time_sec < current_sec) return result;

    if (std::none_of(enabled.begin(), enabled.end(), [](uint8_t value) {return value != 0; }))
        return result;
    evaluate(original, original, enabled, target, duration, temporary_id, min_sec, max_sec, result);
    if (result.left.found && result.right.found &&
        result.left.start_sec == target && result.right.start_sec == target &&
        result.left.moved_count == 0 && result.right.moved_count == 0) return result;
    if (cfg.mode == QueueSearchMode::LiteBrute) {
        LiteBruteSearch search(original, enabled, cfg, result, target, duration,
            temporary_id, min_sec, max_sec, current_sec);
        search.run();
        return result;
    }
    if (cfg.mode == QueueSearchMode::EnclaveTree) {
        EnclaveTreeSearch search(original, enabled, cfg, result, target, duration,
            temporary_id, min_sec, max_sec, current_sec);
        search.run();
        return result;
    }
    if (cfg.mode == QueueSearchMode::Fast ||
        (result.left.found && result.left.start_sec == target &&
            result.right.found && result.right.start_sec == target)) return result;

    std::priority_queue<SearchNode, std::vector<SearchNode>, SearchWorse> frontier;
    std::unordered_set<std::string> seen;
    frontier.push({ original, 0, 0, best_distance(result, target), 0 });
    seen.insert(signature(original));
    int serial = 1;
    int expanded = 0;
    uint64_t peak_memory = 0;
    const int budget = std::max(1, cfg.max_search_nodes);
    while (!frontier.empty() && expanded < budget && int(seen.size()) < budget * 8) {
        SearchNode node = frontier.top(); frontier.pop();
        ++expanded;
        peak_memory = std::max(peak_memory,
            uint64_t(frontier.size()) * (sizeof(SearchNode) + original.size() * sizeof(QueueTaskItem)) +
            uint64_t(seen.size()) * (sizeof(std::string) + sizeof(void*) * 4 + original.size() * 8));
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
                    if (expanded >= 8) {
                        result.diagnostics.nodes_expanded = expanded;
                        result.diagnostics.peak_bytes = peak_memory + sizeof(frontier);
                        return result;
                    }
                }
                const int64_t distance = best_distance(local, target);
                const int64_t priority = distance * 16 + moved * 5 + std::min<int64_t>(displacement, 1000);
                frontier.push({ std::move(changed), moved, displacement, priority, serial++ });
            }
        }
    }
    result.search_limit_hit = !frontier.empty();
    result.diagnostics.budget_exhausted = result.search_limit_hit;
    result.diagnostics.nodes_expanded = expanded;
    // Ограниченный режим хранит полные состояния в каждой вершине очереди.
    result.diagnostics.peak_bytes = peak_memory + sizeof(frontier);
    return result;
}

bool QueueEngine::repack(const std::vector<QueueTaskItem>& original,
    const std::vector<uint8_t>& enabled, int closing_stream_id, int preserved_task_id,
    const QueueConfig& cfg, std::vector<QueueTaskItem>& output, int current_sec,
    EngineDiagnostics* diagnostics) {
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
        if (diagnostics) {
            diagnostics->nodes_expanded = solver.visited;
            diagnostics->peak_bytes = sizeof(solver) +
                uint64_t(solver.placed.capacity() + solver.result.capacity()) * sizeof(QueueTaskItem) +
                uint64_t(solver.move_list.capacity()) * sizeof(RepackItem);
        }
        if (!valid(solver.result, enabled)) return false;
        output = std::move(solver.result);
        return true;
    }
    if (diagnostics) {
        diagnostics->nodes_expanded = solver.visited;
        diagnostics->peak_bytes = sizeof(solver) +
            uint64_t(solver.placed.capacity() + solver.result.capacity()) * sizeof(QueueTaskItem) +
            uint64_t(solver.move_list.capacity()) * sizeof(RepackItem);
    }
    if (cfg.mode != QueueSearchMode::Fast) {
        solver.visited = 0;
        solver.exhaustive = true;
        if (solver.dfs(0)) {
            output = std::move(solver.result);
            if (diagnostics) diagnostics->nodes_expanded = solver.visited;
            return valid(output, enabled);
        }
        if (diagnostics) diagnostics->nodes_expanded = solver.visited;
    }
    if (diagnostics) diagnostics->budget_exhausted = solver.visited >= cfg.max_repack_nodes;
    return false;
}