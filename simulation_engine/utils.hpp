#pragma once

#include <utility>
#include <tuple>

constexpr int SECONDS_PER_DAY = 86400;

static int ceil_div(int x, int by) {  // цілочисельне ділення з округленням до більшого
    return x <= 0 ? 0 : (x + by - 1) / by;
}

inline int time_to_min(const std::pair<int, int>& time) {  // час у хвилини. перше - години, друге - хвилини
    return time.first * 60 + time.second;
}

inline std::pair<int, int> min_to_time(int total_minutes) {  // хвилини у час. повертає перше - години, друге - хвилини
    return { total_minutes / 60, total_minutes % 60 };
}

inline int min_to_sec(const std::pair<int, int>& time) {  // хвилини і секунди в секунди. перше - хвилини, друге - секунди
    return time_to_min(time);  // синонім time_to_min
}

inline std::pair<int, int> sec_to_min(int total_seconds) {  // секунди в хвилини і секунди. повертає перше - хвилини, друге - секунди
    return min_to_time(total_seconds); // синонім min_to_time
}

inline int time_to_sec(const std::pair<int, int>& time) {  // години і хвилини в секунди
    return time_to_min(time) * 60;
}
inline int time_to_sec(const std::tuple<int, int, int>& time) {  // час у секунди. перше - години, друге - хвилини, третє - секунди
    return std::get<0>(time) * 3600 + std::get<1>(time) * 60 + std::get<2>(time);
}

inline std::tuple<int, int, int> sec_to_time(int total_seconds) {  // секунди у час. повертає перше - години, друге - хвилини, третє - секунди
    return { total_seconds / 3600, (total_seconds % 3600) / 60, total_seconds % 60 };
}
// оверрайдити sec_to_time для роботи з хвилиногодинами немає сенсу лол