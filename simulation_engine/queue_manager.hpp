#pragma once

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>
#include "utils.hpp"

// единица очереди
struct QueueTaskItem {
    int min_deadline_sec = -1;  // минимальный допустимый дедлайн (начало)
    int max_deadline_sec = -1;  // максимальный допустимый дедлайн (конец)
    int cooking_time_sec = 0;  // время приготовления (время на себя)
    int start_time_sec = -1;  // время начала при текущей очереди
    int task_id = -1;  // ID задачи (пиццы в заказе); отрицательный только во временном буфере
    int stream_id = -1;  // Абстрактный поток QueueManager; физический слот назначает SimulationEngine
};

// данные для фиксации дедлайнов задачам
struct QueueTaskPlacement {
    int task_id = -1;
    int min_deadline_sec = -1;
    int max_deadline_sec = -1;
};

// 0 - ошибка, 1 - начало, 2 - завершение (по идее его мы не возвращаем)
enum class EventType : uint8_t {
    None = 0,  // ошибка?
    Start = 1,
    Finish = 2
};

// событие на таймлайне очереди
struct QueueEvent {
    int event_time_sec = -1;
    int task_id = -1;
    EventType event_type = EventType::None;
};

// Быстрый режим не перемещает принятые задачи при вставке. Углублённый
// ищет ограниченное число перестановок и может улучшить ближайшие времена.
enum class QueueSearchMode : uint8_t {
    Fast = 0,
    Bounded = 1,
    LiteBrute = 2,  // Зарезервировано, алгоритм пока не реализован
    EnclaveTree = 3 // Зарезервировано, алгоритм пока не реализован
};

inline bool queue_mode_implemented(QueueSearchMode mode) {
    return mode == QueueSearchMode::Fast || mode == QueueSearchMode::Bounded;
}

struct QueueConfig {
    QueueSearchMode mode = QueueSearchMode::Bounded;
    int max_search_nodes = 600;       // Ограничение памяти и времени дерева поиска
    int max_repack_nodes = 3000;      // Ограничение дерева при закрытии потоков
    int max_branch_per_task = 20;     // Максимум кандидатов переноса одной задачи
    int max_variants = 16;           // Храним только последние буферы
    int max_time_sec = SECONDS_PER_DAY;
};

// менеджер управления очередью в рантайме
class QueueManager {
public:
    int ovens_count = 1;  // колво открытых для новых задач потоков
    std::vector<QueueTaskItem> current_queue;  // текущая активная очередь

    explicit QueueManager(QueueConfig cfg = {});
    bool set_config(const QueueConfig& cfg); // Менять алгоритм разрешено только при пустой очереди
    const QueueConfig& config() const { return config_; }

    // Инициализирует постоянный набор абстрактных потоков; разрешено ноль открытых.
    // Вызывается один раз до поступления задач. stream_id лежит в [0, total_streams).
    bool configure_streams(int total_streams, int initially_enabled = 0);
    bool stream_enabled(int stream_id) const;
    int stream_count() const;
    bool enable_stream(int stream_id, int curr_time_clear_sec = 0);

    // Закрывает один поток для будущих задач. keep_task_id остаётся без изменений,
    // остальные задачи атомарно переносятся в разрешённые потоки в пределах дедлайнов.
    bool freeze_stream(int stream_id, int keep_task_id = -1, int curr_time_clear_sec = 0);

    // После раннего завершения задачи освобождённый резерв используется слева направо.
    // Если очередную задачу нельзя сдвинуть, следующие за ней не перемещаются.
    bool shift_stream_left(int stream_id, int seconds, int curr_time_clear_sec = 0);

    // Возвращённый указатель действителен только до следующей мутации менеджера.
    const std::vector<QueueTaskItem>* get_variant(int id) const;

    // попытка установить колво потоков. сток перезаписывает, иначе подать третий аргумент фолс
    bool compress(int target, int curr_time_clear_sec = 0, bool apply = true);

    // поиск подходящего времени вставки в активную очередь слева и справа от целевого. вощвращает концы
    std::pair<int, int> put_time(int target_sec, int duration_sec, int& left_id, int& right_id,
        int min_sec = 0, int max_sec = SECONDS_PER_DAY, int current_sec = 0);
    // лефт айди и райт айди - айди буферизированных очередей куда вставилась задача с временем инт фирст инт секонд

    // поиск подходящего времени вставки в буферрную очередь слева и справа от целевого. вощвращает концы
    std::pair<int, int> put_time_buffered(int queue_id, int target_sec, int duration_sec,
        int& left_id, int& right_id, int min_sec = 0, int max_sec = SECONDS_PER_DAY,
        int current_sec = 0);
    // если очередь айди меньше 0 то мы просто делаем для активной

    // удаление задачи из текущей очереди по ее ID (выполнена/отменена). сбрасывает варианты!
    bool remove_task(int task_id, int curr_time_clear_sec = 0);

    // активирует план по ID варианта, возвращает фолс если план устарел или отсутствует
    bool apply_variant(int id, int start_sec, int min_dl_sec, int max_dl_sec, int curr_time_clear_sec = 0);
    bool apply_variant(int id, QueueTaskPlacement& placement, int curr_time_clear_sec = 0);
    // Применение атомарно: новые задачи связываются с placements по порядку вставки.
    // Предыдущие вставки внутри put_time_buffered остаются неподвижными.
    bool apply_variant(int id, const std::vector<QueueTaskPlacement>& placements, int curr_time_clear_sec = 0);

    // Закрывает дедлайны задачи ровно по текущему интервалу; после этого перенос запрещён.
    bool close_deadlines(int task_id);

    // возвращает время в секундах до след события, записывает в аргумент событие
    int next_event(int current_sec, QueueEvent& out_event);

private:
    struct Variant {
        int id = -1;
        std::vector<QueueTaskItem> items;
        int pending_count = 0; // Временные ID: -1, -2, ... по порядку вставки
    };

    QueueConfig config_;
    std::vector<uint8_t> stream_accepting_ = { 1 };
    std::deque<Variant> queue_variants_;
    int next_variant_id_ = 0;

    bool apply_variant_items(int id, const std::vector<QueueTaskPlacement>& placements,
        int current_sec);
    void drop_variants();
    int add_variant(std::vector<QueueTaskItem> items, int pending_count);
    const Variant* variant_at(int id) const;
    std::pair<int, int> put_impl(const Variant* base, int target_sec, int duration_sec,
        int& left_id, int& right_id, int min_sec, int max_sec, int current_sec);
    bool validate_current(const std::vector<QueueTaskItem>& items, const std::vector<uint8_t>& accepting) const;
};