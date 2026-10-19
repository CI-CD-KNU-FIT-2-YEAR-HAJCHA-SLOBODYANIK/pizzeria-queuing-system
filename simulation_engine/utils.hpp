#pragma once

#include <utility>
#include <tuple>

inline int time_to_min(const std::pair<int, int>& time) {  // время в минуты. первое - часы, второе - минуты
    return time.first * 60 + time.second;
}

inline std::pair<int, int> min_to_time(int total_minutes) {  // минуты во время. возвращает первое - часы, второе - минуты
    return { total_minutes / 60, total_minutes % 60 };
}

inline int min_to_sec(const std::pair<int, int>& time) {  // минуты и секунды в секунды. первое - минуты, второе - секунды
    return time_to_min(time);  // синоним time_to_min
}

inline std::pair<int, int> sec_to_min(int total_seconds) {  // секунды в минуты и секунды. возвращает первое - минуты, второе - секунды
    return min_to_time(total_seconds); // синоним min_to_time
}

inline int time_to_sec(const std::pair<int, int>& time) {  // часы и минуты в секунды
    return time_to_min(time) * 60;
}
inline int time_to_sec(const std::tuple<int, int, int>& time) {  // время в секунды. первое - часы, второе - минуты, третье - секунды
    return std::get<0>(time) * 3600 + std::get<1>(time) * 60 + std::get<2>(time);
}

inline std::tuple<int, int, int> sec_to_time(int total_seconds) {  // секунды во время. возвращает первое - часы, второе - минуты, третье - секунды
    return { total_seconds / 3600, (total_seconds % 3600) / 60, total_seconds % 60 };
}
// оверрайдить sec_to_time для работы с минуточасами нет смысла