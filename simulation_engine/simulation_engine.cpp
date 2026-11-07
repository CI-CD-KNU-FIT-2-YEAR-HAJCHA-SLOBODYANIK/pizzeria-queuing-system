#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "logger.hpp"
#include "structures.hpp"
#include "queue_manager.hpp"
#include "utils.hpp"

// Состояние пиццы устанавливается обработчиками событий, а не выводится из времени.
enum class TaskState : uint8_t {
    None = 0,
    Rejected = 1,
    Planned,
    ChefPreparing,
    ChefPrepFinished,
    InOven,
    Done,
    Completed
};

enum class OrderState : uint8_t {
    Pending = 0,
    Rejected,
    Accepted,
    Completed
};

struct TaskRuntime {
    int task_id = -1;                // Уникальный ID задачи в симуляции
    int runtime_order_index = -1;    // Индекс заказа в order_storage
    int order_id = -1;               // родительский заказ
    int pizza_id = -1;               // ид рецепта заказываемой пиццы
    int chef_time_sec = 0;           // время приготовления поваром
    int oven_time_sec = 0;           // время приготовления в печи
    int est_time_sec = 0;            // время предполагаемое для калькуляции менеджером очереди

    // В Planned chef_start_sec — время по плану. После начала это неизменяемый факт.
    int chef_start_sec = -1;         // секунда начала готовки
    int chef_finish_sec = -1;        // Секунда завершения работы повара
    int oven_start_sec = -1;         // секунда начала приготовления в печи
    int oven_finish_sec = -1;        // Секунда фактического завершения выпекания
    int slot_id = -1;                // Слот физической печи; назначен ещё до начала подготовки
    int wait_for_customer_sec = 0;   // Ожидание готовой пиццы до передачи заказа на доставку
    TaskState state = TaskState::None;

    double price = 0.0;              // цена со всеми добавками
    double ingredient_cost = 0.0;    // Себестоимость ингредиентов
    Resources resources;             // цена в продуктах
};

struct OrderRuntime {
    int order_id = -1;
    int order_received_sec = -1;     // время получения заказа
    int min_deadline_sec = -1;       // с какой секунды клиенту нужен заказ
    int max_deadline_sec = -1;       // до какой секунды клиент готов ждать заказ
    int completed_sec = -1;          // Расчётное получение клиентом; может быть в будущем
    int delivery_time_sec = 0;       // Используется при фактическом завершении кухни
    int quoted_end_sec = -1;         // Последнее предложение клиенту, включая отказ
    OrderState state = OrderState::Pending;
    std::vector<int> task_ids;       // ID всех пицц данного заказа
};

// структура слота печи
struct OvenSlotRuntime {
    int oven_id = -1;               // номер(ид) родительской печи
    int slot_id = -1;               // номер(ид) слота (без привязки к печам)
    int total_active_sec = 0;       // время работы печи под нагрузкой (в секундах)
    int total_idle_sec = 0;         // время простоя печи в включенном состоянии (включая нагрев) (в секундах)
    int busy_until_sec = -1;        // секунда освобождения слота печи
    int task_id = -1;               // ид задачи (текущая пицца). <0 если свободно
    int abstract_stream_id = -1;    // Текущий абстрактный поток QueueManager
    bool accepting_tasks = false;   // Принимает ли этот физический слот новые задачи
    bool is_free() const { return task_id < 0; }
};

// структура группы слотов (единой печи)
struct OvenSlotGroupRuntime {
    int oven_id = 0;                // номер(ид) печи
    OvenState state = OvenState::Off; // состояние печи. это те которых 4
    int countdown_timer_sec = 0;    // Таймер до окончания нагрева или охлаждения
    double cycles_count = 0.0;      // Счетчик циклов нагрева/охлаждения (даже неполных)
    int transition_start_sec = -1;  // Начало текущей физической фазы
    int transition_finish_sec = -1; // Время окончания нагрева или охлаждения
    std::vector<OvenSlotRuntime> slots; // ссылки на дочерние слоты

    int countdown_at(int time) const {
        if (state != OvenState::HeatingUp && state != OvenState::CoolingDown) return 0;
        return std::max(0, transition_finish_sec - time);
    }

    // Температура вычисляется из временных границ фазы, а не хранится отдельно.
    float temperature_at(int time) const {
        if (state == OvenState::Active) return 1.f;
        if (state == OvenState::Off) return 0.f;
        const int duration = std::max(1, transition_finish_sec - transition_start_sec);
        const float remaining = float(countdown_at(time)) / duration;
        return state == OvenState::HeatingUp ? 1.f - remaining : remaining;
    }
};

class SimulationEngine {
public:
    SimulationEngine(const PizzeriaConfig& simulation_config,
        const std::vector<PizzaRecipe>& recipes,
        const std::vector<Topping>& toppings,
        int max_ovens = 10,
        bool tables_id_is_index = false,
        const QueueConfig& queue_config = {})
        : config(simulation_config), recipes(recipes), toppings(toppings),
        tables_id_is_index(tables_id_is_index), max_ovens(std::max(1, max_ovens)),
        queue_manager(queue_config) {
        initialize();
    }

    // добавляем заказ. время берется из заказа. будем надеятся что ты отправил заказ вовремя...
    bool add_order(const Order& order) {
        if (fatal_error) return false;
        if (order.id < 0 || order.timestamp_sec < next_sec ||
            order.timestamp_sec >= SECONDS_PER_DAY) {
            report_issue(IssueSeverity::Warning, "INVALID_ORDER_INPUT",
                "Заказ имеет недопустимый ID или время поступления");
            return false;
        }
        if (!incoming_orders.empty() && order.timestamp_sec < incoming_orders.back().timestamp_sec) {
            report_issue(IssueSeverity::Warning, "UNSORTED_ORDERS",
                "Входящие заказы переданы не в хронологическом порядке");
            return false;
        }
        if (!known_order_ids.insert(order.id).second) {
            report_issue(IssueSeverity::Warning, "DUPLICATE_ORDER_ID",
                "Повторно передан ID заказа " + std::to_string(order.id));
            return false;
        }
        incoming_orders.push_back(order);
        return true;
    }

    // Выполняет события до указанной секунды включительно, без посекундного перебора.
    void advance_to(int target_sec) {
        if (fatal_error || target_sec < next_sec) return;
        const int limit = std::min(target_sec, SECONDS_PER_DAY);
        while (next_sec <= limit && !fatal_error) {
            const int time = next_event_time(next_sec);
            if (time > limit) break;
            collect_interval(last_stat_sec, time);
            last_stat_sec = time;
            next_sec = time;
            process_event_time(time);
            if (!fatal_error) next_sec = time + 1;
        }
        if (!fatal_error) {
            collect_interval(last_stat_sec, limit + 1);
            last_stat_sec = limit + 1;
            next_sec = limit + 1;
        }
        run_summary.profit = run_summary.revenue - run_summary.cost_price -
            run_summary.oven_electricity_cost;
        for (OvenSlotGroupRuntime& oven : oven_storage)
            oven.countdown_timer_sec = oven.countdown_at(limit);
        update_summary();
    }

    // Запускает всю смену. Время доставки вычисляется, но не создаёт события за сменой.
    void run() { advance_to(SECONDS_PER_DAY); }

    int current_sec() const { return next_sec; }
    bool has_fatal_error() const { return fatal_error; }
    const std::string& last_error() const { return error_message; }
    const std::vector<TaskRuntime>& tasks() const { return task_storage; }
    const std::vector<OrderRuntime>& orders() const { return order_storage; }
    const std::vector<OvenSlotGroupRuntime>& ovens() const { return oven_storage; }
    const QueueManager& queue() const { return queue_manager; }
    const std::vector<OrderResult>& order_results() const { return logger.order_results; }
    const std::vector<OvenEvent>& oven_events() const { return logger.oven_events; }
    const RunSummary& summary() const { return run_summary; }
    const Logger& logs() const { return logger; }

private:
    PizzeriaConfig config;
    const std::vector<PizzaRecipe>& recipes; // ссылаемся на то что в мейне
    const std::vector<Topping>& toppings;    // ссылаемся на то что в мейне
    bool tables_id_is_index = false;          // это поле только для пицц и топингов

    int max_ovens = 10;
    int next_sec = 0;                         // че это блять
    // Первая ещё не обработанная секунда; нужна для продолжения через advance_to.
    int next_task_id = 0;                     // че это блять
    // Следующий уникальный ID пиццы, совпадающий с индексом task_storage.
    bool fatal_error = false;
    std::string error_message;

    QueueManager queue_manager;               // менеджер потока работающий с очередью
    Logger logger;

    std::vector<Order> incoming_orders;       // чтоб в рантайм симуляции прочитывать не из менйа мы храним себе тут
    std::size_t next_order_index = 0;         // Индекс следующего ещё не обработанного заказа.
    std::unordered_set<int> known_order_ids;  // Проверка повторных ID без обхода истории
    std::vector<TaskRuntime> task_storage;
    std::vector<OrderRuntime> order_storage;
    std::vector<OvenSlotGroupRuntime> oven_storage;
    RunSummary run_summary{};                 // логерская херня

    int active_chefs = 0;                     // Число поваров, выполняющих подготовку прямо сейчас
    int active_orders_count = 0;              // Количество принятых, но не завершённых заказов
    int last_stat_sec = 0;                    // Начало ещё не начисленного интервала статистики

    std::unordered_map<int, int> pizza_popularity;   // сколько раз заказывали ту или иную пиццу
    std::unordered_map<int, int> topping_popularity; // сколько раз заказывали ту или иную добавку
    int source_counts[2][2]{};                        // считаем источники клиентов и их тип новый старый онлайн офлайн

    int dynamic_extra_ovens = 0;              // Печи сверх статической политики, добавленные динамикой
    int dynamic_hold_until_sec = -1;         // До какого времени динамические печи не выключаются
    int last_dynamic_increase_sec = -1;      // Запрещает включить две дополнительные печи в одну секунду
    int last_stream_change_sec = -1;         // Не более одного занятого потока меняет доступность за секунду
    int next_stream_action_sec = -1;         // Следующая попытка применить политику после изменения очереди
    int retirement_allowed_sec = -1;        // Временная граница лояльности при снижении статической мощности
    std::vector<uint8_t> freeze_deferred_reported; // Один Info на поток до успешной разгрузки
    int previous_static_ovens = -1;         // Предыдущее целевое число печей
    int last_earliest_quote_sec = -1;       // Последняя оперативная оценка окончания нового заказа

    // репортуем в логер о проблеме. тут используем автовремя. ВАЖНО использовать всегда разные message!
    void report_issue(IssueSeverity severity, const std::string& code,
        const std::string& message = "", const std::string& recommendation = "") {
        report_issue_at(next_sec, severity, code,
            message + (message.empty() ? "(auto_time!)" : " (auto_time!)"), recommendation);
    }

    // репортуем в логер о проблеме с явной временной отметкой. ВАЖНО использовать всегда разные message!
    void report_issue_at(int time, IssueSeverity severity, const std::string& code,
        const std::string& message = "", const std::string& recommendation = "") {
        logger.log_issue(0, time, severity, code, message, recommendation);
        if (severity == IssueSeverity::Critical) {
            fatal_error = true;
            error_message = message.empty() ? code : message;
        }
    }

    void fail(const std::string& why) { // ну очевидно функция сообщает что мы упали и как мы упали
        report_issue(IssueSeverity::Critical, "SIMULATION_FAILURE", why);
    }

    void initialize() {
        if (config.opening_time < 0 || config.closing_time > SECONDS_PER_DAY ||
            config.opening_time >= config.closing_time || config.oven_capacity < 1 ||
            config.heat_up_sec < 0 || config.cool_down_sec < 0 ||
            config.max_early_prep_sec < 0) {
            fail("Некорректные параметры времени работы или печей");
            return;
        }
        if (config.radius_wait_map.empty()) {
            fail("Не заполнена radius_wait_map");
            return;
        }
        std::sort(config.static_policy.begin(), config.static_policy.end(),
            [](const auto& a, const auto& b) { return a.time < b.time; });
        std::sort(config.dynamic_policy.begin(), config.dynamic_policy.end(),
            [](const auto& a, const auto& b) { return a.time < b.time; });
        for (int i = 0; i < max_ovens; ++i) {
            OvenSlotGroupRuntime oven;
            oven.oven_id = i;
            for (int j = 0; j < config.oven_capacity; ++j) {
                OvenSlotRuntime slot;
                slot.oven_id = i;
                slot.slot_id = i * config.oven_capacity + j;
                slot.abstract_stream_id = slot.slot_id;
                oven.slots.push_back(slot);
            }
            oven_storage.push_back(std::move(oven));
        }
        freeze_deferred_reported.assign(max_ovens * config.oven_capacity, 0);
        if (!queue_manager.configure_streams(max_ovens * config.oven_capacity, 0))
            fail("QueueManager не смог инициализировать потоки");
        if (desired_ovens(config.opening_time) == 0)
            fail("Статическая политика не допускает ноль печей внутри смены");
    }

    // поиск рецепта по ID или по номеру в зависимости от TABLES_ID_IS_INDEX
    const PizzaRecipe* get_recipe(int recipe_id) const {
        if (tables_id_is_index)
            return recipe_id >= 0 && recipe_id < int(recipes.size()) ? &recipes[recipe_id] : nullptr;
        for (const auto& recipe : recipes)
            if (recipe.id == recipe_id) return &recipe;
        return nullptr;
    }

    // поиск добавки по ID или по номеру в зависимости от TABLES_ID_IS_INDEX
    const Topping* get_topping(int topping_id) const {
        if (tables_id_is_index)
            return topping_id >= 0 && topping_id < int(toppings.size()) ? &toppings[topping_id] : nullptr;
        for (const auto& topping : toppings)
            if (topping.id == topping_id) return &topping;
        return nullptr;
    }

    // перемножаем количество каждого ингредиента на стоимость одного грамма.
    double ingredient_cost(const Resources& r) const {
        const Resources& p = config.ingredient_price_g;
        return r.meat * p.meat + r.dough * p.dough + r.cheese * p.cheese +
            r.veg * p.veg + r.seafood * p.seafood;
    }

    static bool valid_resources(const Resources& r) {
        return std::isfinite(r.meat) && r.meat >= 0 &&
            std::isfinite(r.dough) && r.dough >= 0 &&
            std::isfinite(r.cheese) && r.cheese >= 0 &&
            std::isfinite(r.veg) && r.veg >= 0 &&
            std::isfinite(r.seafood) && r.seafood >= 0;
    }

    // Создаёт конкретную пиццу: отсутствие добавки допускается, плохой результат — нет.
    bool make_task(const OrderTask& task_static, int order_id, int task_id, TaskRuntime& out) {
        out.task_id = task_id;
        out.order_id = order_id;
        out.pizza_id = task_static.pizza_id;
        const PizzaRecipe* recipe = get_recipe(task_static.pizza_id);
        if (!recipe) {
            report_issue(IssueSeverity::Warning, "PIZZA_NOT_FOUND",
                "Не найден рецепт пиццы " + std::to_string(task_static.pizza_id) +
                " в заказе " + std::to_string(order_id));
            return false;
        }
        out.chef_time_sec = recipe->chef_time_sec;
        out.oven_time_sec = recipe->oven_time_sec;
        out.est_time_sec = config.est_time_sec(recipe->radius_cm);
        out.price = recipe->price_base;
        out.resources = recipe->resources_base_g;

        // isfinite проверка на НЕбесконечность. нам оно не очень надо, но пусть
        for (int topping_id : task_static.topping_ids) {
            const Topping* topping = get_topping(topping_id);
            if (!topping) {
                report_issue(IssueSeverity::Warning, "TOPPING_NOT_FOUND",
                    "Не найдена добавка " + std::to_string(topping_id) +
                    " для пиццы " + std::to_string(task_id) + "; добавка пропущена");
                continue;
            }
            const double chef_time = out.chef_time_sec * topping->chef_mult;
            const double oven_time = out.oven_time_sec * topping->oven_mult;
            if (!std::isfinite(chef_time) || !std::isfinite(oven_time) ||
                chef_time < 0 || oven_time < 0 ||
                chef_time > std::numeric_limits<int>::max() ||
                oven_time > std::numeric_limits<int>::max()) {
                report_issue(IssueSeverity::Warning, "INVALID_TOPPING_RESULT",
                    "Добавка " + std::to_string(topping_id) +
                    " приводит к некорректному времени пиццы " + std::to_string(task_id));
                return false;
            }
            const long long chef = std::llround(chef_time) + topping->chef_add_sec;
            const long long oven = std::llround(oven_time) + topping->oven_add_sec;
            if (chef < 0 || oven <= 0 || chef > std::numeric_limits<int>::max() ||
                oven > std::numeric_limits<int>::max() ||
                chef + oven > std::numeric_limits<int>::max()) {
                report_issue(IssueSeverity::Warning, "INVALID_TOPPING_RESULT",
                    "Добавка " + std::to_string(topping_id) +
                    " приводит к некорректной длительности пиццы " + std::to_string(task_id));
                return false;
            }
            out.chef_time_sec = chef;
            out.oven_time_sec = oven;
            out.resources *= topping->resources_mult;
            out.resources += topping->resources_add_g;
            out.price += topping->price;
            if (!valid_resources(out.resources) || !std::isfinite(out.price) ||
                !std::isfinite(ingredient_cost(out.resources))) {
                report_issue(IssueSeverity::Warning, "INVALID_TOPPING_RESULT",
                    "Добавка " + std::to_string(topping_id) +
                    " приводит к некорректным ресурсам или цене пиццы " + std::to_string(task_id));
                return false;
            }
        }
        if (out.chef_time_sec < 0 || out.oven_time_sec <= 0 ||
            !valid_resources(out.resources) || !std::isfinite(out.price) || out.price < 0 ||
            !std::isfinite(ingredient_cost(out.resources)) ||
            (long long)out.chef_time_sec + out.oven_time_sec > std::numeric_limits<int>::max()) {
            report_issue(IssueSeverity::Warning, "INVALID_PIZZA_RESULT",
                "Некорректные параметры или ресурсы пиццы " + std::to_string(task_id));
            return false;
        }
        out.ingredient_cost = ingredient_cost(out.resources);
        const int required = out.chef_time_sec + out.oven_time_sec;
        if (out.est_time_sec < required) {
            const int previous = out.est_time_sec;
            out.est_time_sec = required;
            report_issue(IssueSeverity::Info, "EST_TIME_INCREASED",
                "Предполагаемое время пиццы " + std::to_string(task_id) +
                " увеличено с " + std::to_string(previous) + " до " +
                std::to_string(required) + " (повар " + std::to_string(out.chef_time_sec) +
                ", печь " + std::to_string(out.oven_time_sec) + ")");
        }
        return true;
    }

    // буферизированные характеристики расписания(до двух на заказ) предлагаемого клиенту
    // хранят всякую фигню для установки дедлайнов
    struct OrderPlanBuffer {
        int variant_id = -1;
        int ready_end_sec = -1;        // окончание последней пиццы
        int ready_first_end_sec = -1;  // окончание первой готовой пиццы
        int quoted_end_sec = -1;       // предлагаемое клиенту время с доставкой и квантованием
        std::vector<int> task_ends;    // Времена концов по порядку task_ids
    };

    // округляет предложенное время получения
    int quantize_time(const Order& order_static, int kitchen_end_sec) const {
        const int end = kitchen_end_sec + order_static.delivery_time_sec;
        const int quantum = std::max(1, config.quant_step_min) * 60; // всегда округляем минимум до минут. в принципе так и надо
        return config.quant_by_clock
            ? ceil_div(end, quantum) * quantum
            : order_static.timestamp_sec +
            ceil_div(end - order_static.timestamp_sec, quantum) * quantum;
    }

    int requested_time(const Order& order_static) const { // более надежный способ работы с временем, проверка на нулевой таргет
        // Это целевой конец приготовления, а не время получения клиентом.
        const int delay = order_static.source == OrderSource::Online
            ? config.online_delay_sec : config.in_person_delay_sec;
        const int delivery_target = std::max(order_static.target_time > 0
            ? order_static.target_time : order_static.timestamp_sec,
            order_static.timestamp_sec + std::max(0, delay));
        return delivery_target - order_static.delivery_time_sec;
    }

    // регистрация отказа, изменения данных для логера и просто статуса заказа
    void record_rejection(OrderRuntime& order_runtime) {
        if (order_runtime.state != OrderState::Pending) return;
        order_runtime.state = OrderState::Rejected;
        ++run_summary.orders_rejected;
        run_summary.total_pizzas_rejected += order_runtime.task_ids.size();
        for (int task_id : order_runtime.task_ids) { // расчитываем упущенную прибыль
            TaskRuntime& task = task_storage[task_id];
            task.state = TaskState::Rejected;
            run_summary.lost_revenue += task.price;
            run_summary.lost_profit += task.price - task.ingredient_cost -
                task.oven_time_sec * config.oven_cost_sec; // вроде печи тоже считаются. ну да, вроде да
        }
    }

    // Времена предыдущих пицц в цепочке буферов закреплены самим put_time_buffered.
    // Поэтому читаем возвращённые концы, не перебирая внутренние варианты QueueManager.
    bool build_order_plans(const Order& order_static, const OrderRuntime& order_runtime,
        std::pair<OrderPlanBuffer, OrderPlanBuffer>& temp_plans) {
        temp_plans = {};
        if (order_runtime.task_ids.empty()) return false;

        const int target_finish = requested_time(order_static);
        const int last_allowed_end = config.closing_time;
        OrderPlanBuffer curr_plan;
        curr_plan.task_ends.assign(order_runtime.task_ids.size(), -1);

        auto valid_plan = [&](OrderPlanBuffer& plan, int index, int end) {
            if (plan.variant_id < 0 || end < 0) { plan.variant_id = -1; return; }
            const TaskRuntime& task = task_storage[order_runtime.task_ids[index]];
            const int min_start = std::max(config.opening_time, order_runtime.order_received_sec);
            if (end - task.est_time_sec < min_start || end > last_allowed_end) {
                plan.variant_id = -1;
                return;
            }
            plan.task_ends[index] = end;
            plan.ready_end_sec = std::max(plan.ready_end_sec, end);
            plan.ready_first_end_sec = plan.ready_first_end_sec < 0
                ? end : std::min(plan.ready_first_end_sec, end);
            // С уже закреплёнными пиццами разница времён дальше уменьшиться не может.
            if (plan.ready_end_sec - plan.ready_first_end_sec > config.max_early_prep_sec)
                plan.variant_id = -1;
            };

        for (int i = 0; i < int(order_runtime.task_ids.size()); ++i) {
            const TaskRuntime& task = task_storage[order_runtime.task_ids[i]]; // через авто нельзя :_(
            const int duration = task.est_time_sec;
            const int min_start = std::max(config.opening_time, order_runtime.order_received_sec);
            if (duration <= 0 || min_start + duration > last_allowed_end) return false;
            const int target_start = std::clamp(target_finish - duration,
                min_start, last_allowed_end - duration);

            int left_id = -1;
            int right_id = -1;
            // оно так работает, все норм
            const auto ends = curr_plan.variant_id < 0
                ? queue_manager.put_time(target_start, duration, left_id, right_id,
                    min_start, config.closing_time, order_runtime.order_received_sec)
                : queue_manager.put_time_buffered(curr_plan.variant_id, target_start,
                    duration, left_id, right_id, min_start, config.closing_time,
                    order_runtime.order_received_sec);

            OrderPlanBuffer left = curr_plan;
            OrderPlanBuffer right = curr_plan;
            left.variant_id = left_id;
            right.variant_id = right_id;
            valid_plan(left, i, ends.first);
            valid_plan(right, i, ends.second);
            if (left.variant_id < 0 && right.variant_id < 0) return false; // -1, -1

            // на случай последней пиццы. возвращаем все
            if (i + 1 == int(order_runtime.task_ids.size())) {
                if (left.variant_id >= 0)
                    left.quoted_end_sec = quantize_time(order_static, left.ready_end_sec);
                if (right.variant_id >= 0)
                    right.quoted_end_sec = quantize_time(order_static, right.ready_end_sec);
                temp_plans = { std::move(left), std::move(right) };
                return true;
            }

            // Жадный выбор: перед целью — позже, после цели — раньше, на разных сторонах — раньше.
            if (left.variant_id < 0) curr_plan = std::move(right);
            else if (right.variant_id < 0) curr_plan = std::move(left);
            else if (left.ready_end_sec == right.ready_end_sec)
                curr_plan = std::move(right); // правая меньше будет простаивать и сохнуть
            else if (left.ready_end_sec <= target_finish && right.ready_end_sec > target_finish)
                curr_plan = std::move(left);
            else if (right.ready_end_sec <= target_finish && left.ready_end_sec > target_finish)
                curr_plan = std::move(right);
            else if (left.ready_end_sec > target_finish && right.ready_end_sec > target_finish)
                curr_plan = left.ready_end_sec < right.ready_end_sec
                ? std::move(left) : std::move(right);
            else
                curr_plan = left.ready_end_sec > right.ready_end_sec
                ? std::move(left) : std::move(right);
        }
        report_issue(IssueSeverity::Warning, "NO_FINAL_PLAN", "Не сформирован итоговый план заказа");
        return false;
    }

    // клиент выбирает одноиз двух. ничего больше тут он не делает. возвращает план для реализации
    const OrderPlanBuffer* select_client_plan(const Order& order_static,
        const std::pair<OrderPlanBuffer, OrderPlanBuffer>& temp_plans) const {
        const OrderPlanBuffer* best = nullptr;
        const int target = requested_time(order_static) + order_static.delivery_time_sec;
        for (const OrderPlanBuffer* plan : { &temp_plans.first, &temp_plans.second }) {
            if (plan->variant_id < 0) continue;
            const int quoted = plan->quoted_end_sec;
            // спорная тема, но квантование после времени доставки звучит логичнее
            if (quoted < order_static.timestamp_sec + order_static.min_wait_sec ||
                quoted > order_static.timestamp_sec + order_static.max_wait_sec ||
                quoted - order_static.delivery_time_sec > config.closing_time ||
                plan->ready_first_end_sec < quoted - order_static.delivery_time_sec -
                config.max_early_prep_sec) continue;
            // можно сделать так чтоб он всегда брал то что раньше делается, но это не очень верно.
            // текущий лучше симулирует
            if (!best || std::abs(quoted - target) < std::abs(best->quoted_end_sec - target) || // мы ближе к цели
                (std::abs(quoted - target) == std::abs(best->quoted_end_sec - target) &&
                    quoted < best->quoted_end_sec) || // или при равном отдалении мы раньше
                (quoted == best->quoted_end_sec &&
                    plan->ready_first_end_sec > best->ready_first_end_sec)) { // или при равенстве расписаний наше компактнее
                best = plan;
            }
        }
        return best;
    }

    // применяем очереди выставляем дедлайны
    bool apply_accepted_deadlines(const Order& order_static, OrderRuntime& order_runtime,
        const OrderPlanBuffer& temp_plan) {
        std::vector<QueueTaskPlacement> placements; // ну это чисто дедлайны ес че
        placements.reserve(order_runtime.task_ids.size());
        const int max_deadline = temp_plan.quoted_end_sec - order_static.delivery_time_sec;
        const int min_deadline = std::max(config.opening_time,
            max_deadline - config.max_early_prep_sec);
        for (int id : order_runtime.task_ids) {
            const int est_time = task_storage[id].est_time_sec;
            const int earliest_start = std::max({ config.opening_time,
                order_runtime.order_received_sec, min_deadline - est_time });
            placements.push_back({ id, earliest_start, max_deadline });
        }
        // QueueManager проверяет ограничения и применяет весь вариант без частичной записи.
        if (!queue_manager.apply_variant(temp_plan.variant_id, placements,
            order_runtime.order_received_sec)) return false;
        order_runtime.min_deadline_sec = min_deadline; // на удивление, это действительно задается только на этом этапе 0_0
        order_runtime.max_deadline_sec = max_deadline;
        order_runtime.quoted_end_sec = temp_plan.quoted_end_sec;
        return true;
    }

    // Кэш динамической оценки обновляется по уже найденным вариантам, без нового поиска очереди.
    void update_queue_load_estimate(const Order& order_static,
        const std::pair<OrderPlanBuffer, OrderPlanBuffer>& temp_plans) {
        const int delay = order_static.source == OrderSource::Online
            ? config.online_delay_sec : config.in_person_delay_sec;
        if (order_static.target_time > order_static.timestamp_sec + std::max(0, delay))
            return; // Далёкий предварительный заказ не характеризует текущую очередь.
        int estimate = -1;
        if (order_static.source == OrderSource::Online) {
            if (temp_plans.second.variant_id >= 0)
                estimate = temp_plans.second.ready_end_sec; // Онлайн: только правое предложение.
        }
        else {
            for (const OrderPlanBuffer* plan : { &temp_plans.first, &temp_plans.second })
                if (plan->variant_id >= 0 && (estimate < 0 || plan->ready_end_sec < estimate))
                    estimate = plan->ready_end_sec;
        }
        if (estimate >= 0) {
            last_earliest_quote_sec = estimate;
            next_stream_action_sec = order_static.timestamp_sec + 1;
        }
    }

    // Получаем планы, моделируем выбор клиента, затем атомарно фиксируем расписание.
    void accept_order(const Order& order_static, OrderRuntime& order_runtime) {
        std::pair<OrderPlanBuffer, OrderPlanBuffer> temp_plans;
        if (!build_order_plans(order_static, order_runtime, temp_plans)) {
            record_rejection(order_runtime); // не смогли предложить клиенту
            return;
        }
        update_queue_load_estimate(order_static, temp_plans);
        const OrderPlanBuffer* selected_plan = select_client_plan(order_static, temp_plans);
        if (!selected_plan) {
            // Запоминаем последнее предложенное время даже при естественном отказе клиента.
            const int target = requested_time(order_static) + order_static.delivery_time_sec;
            for (const OrderPlanBuffer* plan : { &temp_plans.first, &temp_plans.second }) {
                if (plan->variant_id < 0) continue;
                if (order_runtime.quoted_end_sec < 0 ||
                    std::abs(plan->quoted_end_sec - target) <
                    std::abs(order_runtime.quoted_end_sec - target))
                    order_runtime.quoted_end_sec = plan->quoted_end_sec;
            }
            record_rejection(order_runtime); // отказ от клиента
            return;
        }
        if (!apply_accepted_deadlines(order_static, order_runtime, *selected_plan)) {
            fail("Не удалось применить согласованный вариант заказа No " +
                std::to_string(order_static.id));
            return;
        }
        order_runtime.state = OrderState::Accepted;
        ++active_orders_count;
        for (int id : order_runtime.task_ids) task_storage[id].state = TaskState::Planned;
        apply_runtime_queue_changed(order_runtime.order_received_sec);
    }

    // При поступлении сразу сохраняем заказ и ВСЕ его задачи в runtime.
    void receive_order(const Order& order_static) {
        ++run_summary.orders_total;
        run_summary.total_pizzas_ordered += order_static.tasks.size(); // лучше пока без статиккастов, это делает код непонятным. мы добавим статиккасты на финальном этапе
        const int source_index = order_static.source == OrderSource::Online ? 1 : 0;
        const int customer_index = order_static.customer_type == CustomerType::Regular ? 1 : 0;
        ++source_counts[source_index][customer_index]; // логерская херня, не трогаем.
        for (const auto& pizza : order_static.tasks) {
            ++pizza_popularity[pizza.pizza_id];
            for (int id : pizza.topping_ids) ++topping_popularity[id];
        } // ну тоже логерская херня. даже если заказ неуспешен - пицца все еще популярна! так что оставляем как есть.

        order_storage.emplace_back();
        OrderRuntime& order_runtime = order_storage.back();
        order_runtime.order_id = order_static.id;
        order_runtime.order_received_sec = order_static.timestamp_sec;
        order_runtime.delivery_time_sec = order_static.delivery_time_sec;
        bool recipes_valid = true;
        for (const auto& pizza : order_static.tasks) {
            TaskRuntime task;
            task.task_id = next_task_id++;
            task.runtime_order_index = order_storage.size() - 1;
            task.order_id = order_static.id;
            task.pizza_id = pizza.pizza_id;
            const bool valid = make_task(pizza, order_static.id, task.task_id, task);
            if (!valid) {
                // У некорректной пиццы неизвестная цена не становится потерянной выручкой.
                task.chef_time_sec = task.oven_time_sec = task.est_time_sec = 0;
                task.price = task.ingredient_cost = 0;
                task.resources = Resources{};
            }
            recipes_valid = recipes_valid && valid;
            order_runtime.task_ids.push_back(task.task_id);
            task_storage.push_back(std::move(task));
        }
        const bool order_valid = !order_static.tasks.empty() &&
            order_static.min_wait_sec >= 0 &&
            order_static.max_wait_sec >= order_static.min_wait_sec &&
            order_static.delivery_time_sec >= 0;
        if (!order_valid)
            report_issue(IssueSeverity::Warning, "INVALID_ORDER_PARAMS",
                "Некорректные параметры ожидания или пустой заказ " +
                std::to_string(order_static.id));
        if (!recipes_valid || !order_valid || active_oven_count() == 0 ||
            order_static.timestamp_sec < config.opening_time ||
            order_static.timestamp_sec >= config.closing_time) {
            record_rejection(order_runtime);
            return;
        }
        accept_order(order_static, order_runtime);
    }

    // Статическая политика определяется на начало работы и меняется по её расписанию.
    int desired_ovens(int time) const {
        if (time < config.opening_time || time >= config.closing_time) return 0;
        int desired = max_ovens;
        for (const auto& entry : config.static_policy) {
            if (entry.time > time) break;
            desired = entry.target_ovens;
        }
        return std::clamp(desired, 0, max_ovens);
    }

    // Кэш последней оперативной оценки убывает без новых заказов.
    int est_queue_load(int time) const {
        return last_earliest_quote_sec < 0 ? 0 :
            std::max(0, last_earliest_quote_sec - time);
    }

    // считает только полностью прогретые печи
    int active_oven_count() const {
        int result = 0;
        for (const auto& oven : oven_storage)
            if (oven.state == OvenState::Active) ++result;
        return result;
    }

    // Пустая динамическая политика означает только статическую мощность.
    const DynamicOvenPolicyEntry* dynamic_policy_at(int time) const {
        const DynamicOvenPolicyEntry* result = nullptr;
        for (const auto& policy : config.dynamic_policy) {
            if (policy.time > time) break;
            result = &policy;
        }
        return result;
    }

    // Динамика увеличивает мощность на одну печь за секунду; лишняя печь
    // возвращается к статической норме после порога sustain и периода лояльности.
    int desired_ovens_dynamic(int time) {
        const int baseline = desired_ovens(time);
        if (baseline == 0) return 0;
        const DynamicOvenPolicyEntry* policy = dynamic_policy_at(time);
        if (!policy) return baseline;
        const int load = est_queue_load(time);
        if (policy->new_oven_trigger_sec > 0 &&
            policy->new_oven_trigger_sec < SECONDS_PER_DAY &&
            load >= policy->new_oven_trigger_sec &&
            baseline + dynamic_extra_ovens < max_ovens &&
            last_dynamic_increase_sec != time) {
            ++dynamic_extra_ovens;
            last_dynamic_increase_sec = time;
            dynamic_hold_until_sec = time + std::max(0, policy->new_oven_loyal_sec);
            next_stream_action_sec = time + 1;
        }
        else if (dynamic_extra_ovens > 0) {
            if (policy->new_oven_sustain_sec > 0 &&
                load >= policy->new_oven_sustain_sec) {
                dynamic_hold_until_sec = time + std::max(0, policy->new_oven_loyal_sec);
            }
            else if (time >= dynamic_hold_until_sec && last_dynamic_increase_sec != time) {
                --dynamic_extra_ovens;
                dynamic_hold_until_sec = time + std::max(0, policy->new_oven_loyal_sec);
                if (dynamic_extra_ovens > 0) next_stream_action_sec = time + 1;
            }
        }
        dynamic_extra_ovens = std::min(dynamic_extra_ovens, max_ovens - baseline);
        return baseline + dynamic_extra_ovens;
    }

    // Согласует перестроенное абстрактное расписание с реальными слотами.
    // Начавшие работу задачи сохраняют свой физический слот и точное начало.
    bool apply_runtime_queue_changed(int time) {
        const int total = max_ovens * config.oven_capacity;
        const int streams = queue_manager.stream_count();
        if (streams != total) { fail("Количество абстрактных потоков не совпадает со слотами"); return false; }
        std::vector<int> stream_to_slot(streams, -1);
        std::vector<bool> used(total, false);
        auto get_slot = [&](int id) -> OvenSlotRuntime& {
            return oven_storage[id / config.oven_capacity].slots[id % config.oven_capacity];
            };

        for (const QueueTaskItem& item : queue_manager.current_queue) {
            if (item.task_id < 0 || item.task_id >= int(task_storage.size()) ||
                item.stream_id < 0 || item.stream_id >= streams) {
                fail("Некорректные ID в текущей очереди"); return false;
            }
            const TaskRuntime& task = task_storage[item.task_id];
            if (task.state == TaskState::Planned) continue;
            if (task.state != TaskState::ChefPreparing &&
                task.state != TaskState::ChefPrepFinished &&
                task.state != TaskState::InOven) {
                fail("В очереди находится задача с недопустимым состоянием"); return false;
            }
            const int slot_id = task.slot_id;
            if (slot_id < 0 || slot_id >= total || item.start_time_sec != task.chef_start_sec ||
                (stream_to_slot[item.stream_id] >= 0 && stream_to_slot[item.stream_id] != slot_id) ||
                (used[slot_id] && stream_to_slot[item.stream_id] != slot_id)) {
                fail("Начатая задача была перенесена во времени или на другой слот"); return false;
            }
            stream_to_slot[item.stream_id] = slot_id;
            used[slot_id] = true;
        }

        // Сначала сохраняем прежнюю привязку, затем используем любой свободный горячий слот.
        auto place_stream = [&](int stream_id, bool require_active) {
            if (stream_to_slot[stream_id] >= 0) return true;
            for (int pass = 0; pass < 2; ++pass) {
                for (int id = 0; id < total; ++id) {
                    if (used[id]) continue;
                    const OvenSlotRuntime& slot = get_slot(id);
                    if (pass == 0 && slot.abstract_stream_id != stream_id) continue;
                    if (require_active &&
                        (oven_storage[id / config.oven_capacity].state != OvenState::Active ||
                            !slot.accepting_tasks)) continue;
                    stream_to_slot[stream_id] = id;
                    used[id] = true;
                    return true;
                }
            }
            return false;
            };
        for (const QueueTaskItem& item : queue_manager.current_queue) {
            if (stream_to_slot[item.stream_id] >= 0) continue;
            if (!queue_manager.stream_enabled(item.stream_id) ||
                !place_stream(item.stream_id, true)) {
                fail("Нельзя назначить горячий поток задаче " + std::to_string(item.task_id));
                return false;
            }
        }
        for (int stream_id = 0; stream_id < streams; ++stream_id) {
            if (stream_to_slot[stream_id] >= 0 || !queue_manager.stream_enabled(stream_id)) continue;
            if (!place_stream(stream_id, true)) {
                fail("Нет физического слота для включённого потока");
                return false;
            }
        }
        for (int stream_id = 0; stream_id < streams; ++stream_id)
            if (!place_stream(stream_id, false)) {
                fail("Не удалось восстановить соответствие абстрактных потоков");
                return false;
            }
        for (int stream_id = 0; stream_id < streams; ++stream_id) {
            const int slot_id = stream_to_slot[stream_id];
            OvenSlotRuntime& slot = get_slot(slot_id);
            slot.abstract_stream_id = stream_id;
            slot.accepting_tasks = queue_manager.stream_enabled(stream_id) &&
                oven_storage[slot_id / config.oven_capacity].state == OvenState::Active;
        }
        for (const QueueTaskItem& item : queue_manager.current_queue) {
            TaskRuntime& task = task_storage[item.task_id];
            if (task.state != TaskState::Planned) continue;
            if (item.start_time_sec < time) {
                fail("Перепланирование отправило подготовку пиццы в прошлое");
                return false;
            }
            task.chef_start_sec = item.start_time_sec;
            task.slot_id = stream_to_slot[item.stream_id];
        }
        return true;
    }

    // Фиксируем только реальные изменения печей; температуру считаем по времени.
    void record_oven_event(const OvenSlotGroupRuntime& oven, int time,
        OvenAction action, PolicyTriggerType trigger) {
        OvenEvent event{};
        event.timestamp_sec = time;
        event.oven_id = oven.oven_id;
        event.state = oven.state;
        event.action = action;
        event.trigger_type = trigger;
        event.active_ovens_count = active_oven_count();
        event.temperature_ratio = oven.temperature_at(time);
        event.countdown_timer_sec = oven.countdown_at(time);
        logger.log_oven_event(event);
    }

    // не можем выключить печь пока потоки загружены.
    bool oven_has_reserved_work(const OvenSlotGroupRuntime& oven, int time = -1) const {
        (void)time;
        for (const OvenSlotRuntime& slot : oven.slots) {
            if (!slot.is_free()) return true;
            for (const QueueTaskItem& item : queue_manager.current_queue) {
                if (item.stream_id == slot.abstract_stream_id) return true;
            }
        }
        return false;
    }

    // Печи подчиняются статической мощности, динамика может добавить горячие слоты.
    // Уменьшение сначала замораживает потоки без разрыва принятых интервалов.
    void update_ovens(int time) {
        const int preheat_from = std::max(0, config.opening_time - config.heat_up_sec);
        if (time < preheat_from) return;
        const int baseline = desired_ovens(time < config.opening_time
            ? config.opening_time : time);
        if (time >= config.opening_time && time < config.closing_time && baseline == 0) {
            fail("Статическая политика назначила ноль печей во время смены");
            return;
        }

        // Сначала завершаются физические переходы, затем проверяются новые политики.
        std::vector<bool> just_heated(max_ovens, false);
        for (OvenSlotGroupRuntime& oven : oven_storage) {
            if (oven.transition_finish_sec < 0 || time < oven.transition_finish_sec) continue;
            if (oven.state == OvenState::HeatingUp) {
                oven.state = OvenState::Active;
                oven.transition_finish_sec = -1;
                just_heated[oven.oven_id] = true;
                record_oven_event(oven, time, OvenAction::Heated, PolicyTriggerType::Physics);
            }
            else if (oven.state == OvenState::CoolingDown) {
                oven.state = OvenState::Off;
                oven.transition_finish_sec = -1;
                record_oven_event(oven, time, OvenAction::Off, PolicyTriggerType::Physics);
            }
        }

        const DynamicOvenPolicyEntry* policy = dynamic_policy_at(time);
        if (previous_static_ovens >= 0 && baseline < previous_static_ovens) {
            retirement_allowed_sec = time + (policy ? std::max(0, policy->loyal_sec) : 0);
        }
        if (policy && retirement_allowed_sec > time &&
            est_queue_load(time) <= policy->loyal_trigger_sec)
            retirement_allowed_sec = std::min(retirement_allowed_sec,
                time + std::max(0, policy->loyal_update_sec));
        // После закрытия не держим лишние печи ради временной лояльности.
        if (time >= config.closing_time) retirement_allowed_sec = time;
        previous_static_ovens = baseline;

        const int desired = time < config.opening_time ? baseline : desired_ovens_dynamic(time);
        if (next_stream_action_sec <= time) next_stream_action_sec = -1;

        // Включение новой печи охватывает все её слоты после физического прогрева.
        for (int i = 0; i < desired; ++i) {
            OvenSlotGroupRuntime& oven = oven_storage[i];
            if (oven.state == OvenState::Off) {
                oven.state = config.heat_up_sec == 0 ? OvenState::Active : OvenState::HeatingUp;
                oven.transition_start_sec = time;
                oven.transition_finish_sec = oven.state == OvenState::Active
                    ? -1 : time + config.heat_up_sec;
                just_heated[i] = oven.state == OvenState::Active;
                oven.cycles_count += 0.5;
                record_oven_event(oven, time,
                    oven.state == OvenState::Active ? OvenAction::Heated : OvenAction::HeatStart,
                    i >= baseline ? PolicyTriggerType::Dynamic : PolicyTriggerType::Static);
            }
            if (oven.state == OvenState::Active && just_heated[i]) {
                // При включении новой печи активируем все её слоты одновременно.
                for (OvenSlotRuntime& slot : oven.slots) {
                    if (queue_manager.stream_enabled(slot.abstract_stream_id)) continue;
                    if (!queue_manager.enable_stream(slot.abstract_stream_id, time)) {
                        fail("Не удалось открыть поток новой горячей печи");
                        return;
                    }
                    slot.accepting_tasks = true;
                    freeze_deferred_reported[slot.abstract_stream_id] = 0;
                }
                if (!apply_runtime_queue_changed(time)) return;
            }
            else if (oven.state == OvenState::Active) {
                // Отдельный ранее замороженный поток открывается не быстрее раза в секунду.
                for (OvenSlotRuntime& slot : oven.slots) {
                    if (slot.accepting_tasks || queue_manager.stream_enabled(slot.abstract_stream_id)) continue;
                    if (last_stream_change_sec == time) {
                        next_stream_action_sec = time + 1;
                        break;
                    }
                    if (!queue_manager.enable_stream(slot.abstract_stream_id, time)) {
                        report_issue_at(time, IssueSeverity::Warning, "STREAM_ENABLE_DELAYED",
                            "Не удалось включить поток " + std::to_string(slot.abstract_stream_id));
                        break;
                    }
                    slot.accepting_tasks = true;
                    freeze_deferred_reported[slot.abstract_stream_id] = 0;
                    last_stream_change_sec = time;
                    if (!apply_runtime_queue_changed(time)) return;
                    next_stream_action_sec = time + 1;
                    break;
                }
            }
        }

        // Для полностью пустой печи разрешено заморозить все её потоки сразу.
        if (retirement_allowed_sec < 0 || time >= retirement_allowed_sec) {
            for (int i = max_ovens - 1; i >= desired; --i) {
                OvenSlotGroupRuntime& oven = oven_storage[i];
                if (oven.state != OvenState::Active) continue;
                const bool empty = !oven_has_reserved_work(oven, time);
                for (OvenSlotRuntime& slot : oven.slots) {
                    if (!slot.accepting_tasks) continue;
                    if (!empty && last_stream_change_sec == time) {
                        next_stream_action_sec = time + 1;
                        break;
                    }
                    int keep_task_id = -1;
                    for (const QueueTaskItem& item : queue_manager.current_queue) {
                        if (item.stream_id != slot.abstract_stream_id || item.task_id < 0) continue;
                        const TaskState state = task_storage[item.task_id].state;
                        if (state == TaskState::ChefPreparing ||
                            state == TaskState::ChefPrepFinished || state == TaskState::InOven) {
                            keep_task_id = item.task_id;
                            break;
                        }
                    }
                    if (!queue_manager.freeze_stream(slot.abstract_stream_id, keep_task_id, time)) {
                        const int stream_id = slot.abstract_stream_id;
                        if (!freeze_deferred_reported[stream_id]) {
                            report_issue_at(time, IssueSeverity::Info, "STREAM_FREEZE_DEFERRED",
                                "Не удалось разгрузить поток " + std::to_string(stream_id));
                            freeze_deferred_reported[stream_id] = 1;
                        }
                        break; // Текущая печь остаётся включённой.
                    }
                    freeze_deferred_reported[slot.abstract_stream_id] = 0;
                    slot.accepting_tasks = false;
                    if (!empty) last_stream_change_sec = time;
                    if (!apply_runtime_queue_changed(time)) return;
                    if (!empty) { next_stream_action_sec = time + 1; break; }
                }
            }
        }

        // Печь охлаждается, только когда все слоты перестали принимать работу и освободились.
        for (int i = max_ovens - 1; i >= desired; --i) {
            OvenSlotGroupRuntime& oven = oven_storage[i];
            if (oven.state != OvenState::Active) continue;
            bool frozen = true;
            for (const OvenSlotRuntime& slot : oven.slots)
                if (slot.accepting_tasks) frozen = false;
            if (!frozen || oven_has_reserved_work(oven, time)) continue;
            oven.state = config.cool_down_sec == 0 ? OvenState::Off : OvenState::CoolingDown;
            oven.transition_start_sec = time;
            oven.transition_finish_sec = time + config.cool_down_sec;
            oven.cycles_count += 0.5;
            record_oven_event(oven, time,
                oven.state == OvenState::Off ? OvenAction::Off : OvenAction::CoolStart,
                i >= baseline ? PolicyTriggerType::Dynamic : PolicyTriggerType::Static);
        }
    }

    // Запускает конкретную подготовку. Начало и физический слот после этого неизменны.
    void start_task(int task_id, int time) {
        if (task_id < 0 || task_id >= int(task_storage.size())) {
            fail("Некорректный ID задачи при начале подготовки"); return;
        }
        TaskRuntime& task = task_storage[task_id];
        if (task.state != TaskState::Planned || task.chef_start_sec != time || task.slot_id < 0) {
            fail("Начало подготовки не совпало с расписанием задачи " + std::to_string(task_id));
            return;
        }
        if (!queue_manager.close_deadlines(task_id)) {
            fail("Не удалось зафиксировать время задачи " + std::to_string(task_id));
            return;
        }
        task.state = TaskState::ChefPreparing;
        task.chef_finish_sec = time + task.chef_time_sec;
        if (task.chef_time_sec > 0) ++active_chefs;
        apply_runtime_queue_changed(time);
    }

    // В момент окончания работы повара пицца сразу занимает уже назначенный физический слот.
    void start_oven_task(int task_id, int time) {
        TaskRuntime& task = task_storage[task_id];
        if (task.state != TaskState::ChefPreparing || task.chef_finish_sec != time) {
            fail("Некорректное событие окончания подготовки " + std::to_string(task_id));
            return;
        }
        task.state = TaskState::ChefPrepFinished;
        if (task.chef_time_sec > 0) --active_chefs;
        if (task.slot_id < 0 || task.slot_id >= max_ovens * config.oven_capacity) {
            fail("Не назначен физический слот пицце " + std::to_string(task_id));
            return;
        }
        OvenSlotGroupRuntime& oven = oven_storage[task.slot_id / config.oven_capacity];
        OvenSlotRuntime& slot = oven.slots[task.slot_id % config.oven_capacity];
        // Замороженный слот может закончить уже начатую задачу.
        if (oven.state != OvenState::Active || !slot.is_free()) {
            fail("Назначенная печь занята или не прогрета для задачи " + std::to_string(task_id));
            return;
        }
        task.state = TaskState::InOven;
        task.oven_start_sec = time;
        slot.task_id = task_id;
        slot.busy_until_sec = time + task.oven_time_sec;
    }

    // Заказ завершён на кухне: расчётное получение клиентом может быть в будущем.
    void finish_order(OrderRuntime& order_runtime, int time) {
        if (order_runtime.state != OrderState::Accepted) return;
        for (int id : order_runtime.task_ids)
            if (task_storage[id].state != TaskState::Done) return;
        if (order_runtime.max_deadline_sec < 0) {
            fail("Нет согласованного дедлайна заказа " + std::to_string(order_runtime.order_id));
            return;
        }
        // Даже при ранней готовности нельзя вручить заказ раньше разрешённого.
        const int handoff = std::max(time, order_runtime.max_deadline_sec);
        order_runtime.completed_sec = handoff + order_runtime.delivery_time_sec;
        order_runtime.state = OrderState::Completed;
        --active_orders_count;
        ++run_summary.orders_fulfilled;
        run_summary.total_pizzas_fulfilled += order_runtime.task_ids.size();

        OrderResult result{};
        result.order_id = order_runtime.order_id;
        result.timestamp_sec = order_runtime.completed_sec;
        result.arrival_time_sec = order_runtime.quoted_end_sec - order_runtime.order_received_sec;
        result.total_wait_sec = order_runtime.completed_sec - order_runtime.order_received_sec;
        for (int i = 0; i < int(order_runtime.task_ids.size()); ++i) {
            TaskRuntime& task = task_storage[order_runtime.task_ids[i]];
            task.wait_for_customer_sec = std::max(0, handoff - task.oven_finish_sec);
            task.state = TaskState::Completed;
            TaskResult r{};
            r.task_index = i;
            r.pizza_id = task.pizza_id;
            r.oven_id = task.slot_id / config.oven_capacity;
            r.chef_prep_sec = task.chef_time_sec;
            r.wait_before_oven_sec = task.oven_start_sec - task.chef_finish_sec;
            r.oven_bake_sec = task.oven_finish_sec - task.oven_start_sec;
            r.wait_for_customer_sec = task.wait_for_customer_sec;
            r.resources_g = task.resources;
            r.price = task.price;
            r.cost = task.ingredient_cost;
            result.tasks.push_back(r);
            result.total_price += task.price;
            result.total_cost += task.ingredient_cost;
            result.resources_total__g += task.resources;
        }
        run_summary.revenue += result.total_price;
        run_summary.cost_price += result.total_cost;
        logger.log_order(result);
    }

    // Завершает выпечку, удаляет задачу из QueueManager и освобождает её физический слот.
    void finish_task(int task_id, int time) {
        if (task_id < 0 || task_id >= int(task_storage.size())) {
            fail("Некорректный ID завершённой задачи"); return;
        }
        TaskRuntime& task = task_storage[task_id];
        if (task.state != TaskState::InOven || task.slot_id < 0) {
            fail("Завершение задачи, которая не выпекалась"); return;
        }
        OvenSlotRuntime& slot = oven_storage[task.slot_id / config.oven_capacity]
            .slots[task.slot_id % config.oven_capacity];
        if (slot.task_id != task_id || slot.busy_until_sec != time) {
            fail("Фактическое завершение не совпало со временем задачи " + std::to_string(task_id));
            return;
        }
        int reserved_finish = -1;
        int stream_id = -1;
        for (const QueueTaskItem& item : queue_manager.current_queue) {
            if (item.task_id != task_id) continue;
            reserved_finish = item.start_time_sec + item.cooking_time_sec;
            stream_id = item.stream_id;
            break;
        }
        if (reserved_finish < time || stream_id < 0) {
            fail("Задача закончилась за пределами резерва " + std::to_string(task_id));
            return;
        }
        // Физическая выпечка может закончиться раньше зарезервированного интервала.
        if (!queue_manager.remove_task(task_id, time)) {
            fail("QueueManager не смог удалить задачу " + std::to_string(task_id));
            return;
        }
        task.state = TaskState::Done;
        task.oven_finish_sec = time;
        slot.task_id = -1;
        slot.busy_until_sec = -1;
        const int unused = reserved_finish - time;
        if (unused > 0 && slot.accepting_tasks &&
            !queue_manager.shift_stream_left(stream_id, unused, time)) {
            report_issue_at(time, IssueSeverity::Warning, "STREAM_SHIFT_FAILED",
                "Не удалось сдвинуть поток " + std::to_string(stream_id));
        }
        if (!apply_runtime_queue_changed(time)) return;
        finish_order(order_storage[task.runtime_order_index], time);
        next_stream_action_sec = time + 1; // Очередь и мощность могли измениться.
    }

    // Обрабатывает все начала подготовки и её окончания в одну секунду.
    // Не сканирует историю: рассматриваются только задачи активной очереди.
    void process_due_tasks(int time) {
        while (!fatal_error) {
            std::vector<int> starting;
            std::vector<int> prepared;
            for (const QueueTaskItem& item : queue_manager.current_queue) {
                if (item.task_id < 0) continue;
                const TaskRuntime& task = task_storage[item.task_id];
                if (task.state == TaskState::Planned && task.chef_start_sec == time)
                    starting.push_back(item.task_id);
                else if (task.state == TaskState::ChefPreparing && task.chef_finish_sec == time)
                    prepared.push_back(item.task_id);
            }
            if (starting.empty() && prepared.empty()) return;
            // Сначала завершаем уже начатую подготовку, освобождая поваров.
            for (int id : prepared) {
                start_oven_task(id, time);
                if (fatal_error) return;
            }
            for (int id : starting) {
                start_task(id, time);
                if (fatal_error) return;
            }
            // Подготовка длительностью ноль вызывает событие печи в ту же секунду.
        }
    }

    // Начисляет электричество и загрузку сразу за весь неизменный временной интервал.
    void collect_interval(int from_sec, int to_sec) {
        const int delta = std::max(0, to_sec - from_sec);
        if (delta == 0) return;
        for (OvenSlotGroupRuntime& oven : oven_storage) {
            bool baking = false;
            for (OvenSlotRuntime& slot : oven.slots) {
                if (!slot.is_free()) {
                    baking = true;
                    slot.total_active_sec += delta;
                }
                else if (oven.state == OvenState::Active || oven.state == OvenState::HeatingUp)
                    slot.total_idle_sec += delta;
            }
            if (oven.state == OvenState::Active) {
                if (baking) {
                    run_summary.total_oven_active_sec += delta;
                    run_summary.oven_electricity_cost += delta * config.oven_cost_sec;
                }
                else {
                    run_summary.total_oven_idle_sec += delta;
                    run_summary.oven_electricity_cost += delta * config.oven_cost_sec * config.idle_power_ratio;
                }
            }
            else if (oven.state == OvenState::HeatingUp) {
                run_summary.total_oven_idle_sec += delta;
                run_summary.oven_electricity_cost += delta * config.oven_cost_sec;
            }
        }
    }

    // Пиковые метрики проверяются при изменении состояния, а не каждую секунду.
    void collect_event_metrics(int time) {
        int baking_ovens = 0;
        for (const OvenSlotGroupRuntime& oven : oven_storage) {
            for (const OvenSlotRuntime& slot : oven.slots) {
                if (!slot.is_free()) { ++baking_ovens; break; }
            }
        }
        if (baking_ovens > run_summary.max_active_ovens) {
            run_summary.max_active_ovens = baking_ovens;
            run_summary.max_active_ovens_timestamp_sec = time;
        }
        if (active_chefs > run_summary.max_active_chefs) {
            run_summary.max_active_chefs = active_chefs;
            run_summary.max_active_chefs_timestamp_sec = time;
        }
        const int wait = est_queue_load(time);
        if (wait > run_summary.max_est_queue_wait_sec) {
            run_summary.max_est_queue_wait_sec = wait;
            run_summary.max_est_queue_timestamp_sec = time;
        }
        if (active_orders_count > run_summary.max_actual_queue_count) {
            run_summary.max_actual_queue_count = active_orders_count;
            run_summary.max_actual_queue_timestamp_sec = time;
        }
        // Нулевая мощность заменяется единицей только для безопасного деления.
        const int capacity = std::max(1, active_oven_count() * config.oven_capacity);
        int remaining = 0;
        for (const QueueTaskItem& q : queue_manager.current_queue)
            remaining += std::max(0, q.cooking_time_sec - std::max(0, time - q.start_time_sec));
        // Показатель имеет размерность секунд работы на слот, не процентов.
        const double load = double(remaining) / capacity;
        if (load > run_summary.max_baking_load_ratio) {
            run_summary.max_baking_load_ratio = load;
            run_summary.max_baking_load_orders = active_orders_count;
            run_summary.max_baking_load_ovens = active_oven_count();
            run_summary.max_baking_load_timestamp_sec = time;
        }
    }

    // Пересчитывает итоговую статистику по хранимой истории, но не в цикле событий.
    void update_summary() {
        run_summary.per_oven_stats.clear();
        run_summary.total_oven_cycles = 0;
        for (const auto& oven : oven_storage) {
            OvenSummary item{};
            item.oven_id = oven.oven_id;
            for (const auto& slot : oven.slots) {
                item.active_sec += slot.total_active_sec;
                item.idle_sec += slot.total_idle_sec;
            }
            item.cycles = oven.cycles_count;
            const int total = item.active_sec + item.idle_sec;
            item.load_percent = total > 0 ? 100.0 * item.active_sec / total : 0;
            run_summary.per_oven_stats.push_back(item);
            run_summary.total_oven_cycles += oven.cycles_count;
        }
        const int total = run_summary.total_oven_active_sec + run_summary.total_oven_idle_sec;
        run_summary.total_oven_load_percent = total > 0 ?
            100.0 * run_summary.total_oven_active_sec / total : 0;
        run_summary.total_meat_g = run_summary.total_dough_g = run_summary.total_cheese_g = 0;
        run_summary.total_veg_g = run_summary.total_seafood_g = 0;
        long long completed_wait = 0;
        for (const auto& result : logger.order_results) {
            completed_wait += result.total_wait_sec;
            const Resources& res = result.resources_total__g;
            run_summary.total_meat_g += res.meat;
            run_summary.total_dough_g += res.dough;
            run_summary.total_cheese_g += res.cheese;
            run_summary.total_veg_g += res.veg;
            run_summary.total_seafood_g += res.seafood;
        }
        run_summary.avg_wait_sec = logger.order_results.empty() ? 0 :
            completed_wait / logger.order_results.size();

        double total_strain = 0;
        double total_margin = 0;
        int measured = 0;
        // incoming_orders и order_storage соответствуют друг другу по индексу.
        for (int i = 0; i < int(order_storage.size()); ++i) {
            const OrderRuntime& runtime = order_storage[i];
            const Order& source = incoming_orders[i];
            if (runtime.quoted_end_sec < 0 || source.max_wait_sec <= source.min_wait_sec) continue;
            // 0% соответствует минимальному ожиданию, 100% — максимальному.
            const int quote_wait = runtime.quoted_end_sec - runtime.order_received_sec;
            const int actual_wait = runtime.state == OrderState::Completed ?
                runtime.completed_sec - runtime.order_received_sec : quote_wait;
            const int range = source.max_wait_sec - source.min_wait_sec;
            total_strain += 100.0 * (actual_wait - source.min_wait_sec) / range;
            total_margin += 100.0 * (source.max_wait_sec - quote_wait) / range;
            ++measured;
        }
        run_summary.avg_patience_strain_percent = measured ? total_strain / measured : 0;
        run_summary.avg_patience_margin_percent = measured ? total_margin / measured : 0;
        for (const auto& [id, count] : pizza_popularity)
            if (run_summary.most_popular_pizza_id < 0 ||
                count > pizza_popularity.at(run_summary.most_popular_pizza_id))
                run_summary.most_popular_pizza_id = id;
        for (const auto& [id, count] : topping_popularity)
            if (run_summary.most_popular_topping_id < 0 ||
                count > topping_popularity.at(run_summary.most_popular_topping_id))
                run_summary.most_popular_topping_id = id;
        if (run_summary.orders_total > 0) {
            run_summary.online_new_orders_percent = 100.0 * source_counts[1][0] / run_summary.orders_total;
            run_summary.online_regular_orders_percent = 100.0 * source_counts[1][1] / run_summary.orders_total;
            run_summary.in_person_new_orders_percent = 100.0 * source_counts[0][0] / run_summary.orders_total;
            run_summary.in_person_regular_orders_percent = 100.0 * source_counts[0][1] / run_summary.orders_total;
        }
        // Потенциальная прибыль — модельная оценка, не бухгалтерский факт.
        const double potential_profit = run_summary.profit + run_summary.lost_profit;
        run_summary.profit_realization_percent = potential_profit > 0 ?
            100.0 * run_summary.profit / potential_profit : 0;
        logger.finalize_summary(run_summary);
    }

    // Выбирает ближайшую временную метку среди входящих заказов, печей,
    // политик и текущей очереди. Завершённые задачи в истории не обходятся.
    int next_event_time(int from) const {
        int next = SECONDS_PER_DAY + 1;
        auto consider = [&](int time) {
            if (time >= from && time <= SECONDS_PER_DAY) next = std::min(next, time);
            };
        if (next_order_index < incoming_orders.size())
            consider(incoming_orders[next_order_index].timestamp_sec);
        consider(std::max(0, config.opening_time - config.heat_up_sec));
        consider(config.opening_time);
        consider(config.closing_time);
        for (const auto& policy : config.static_policy)
            if (policy.time >= config.opening_time) consider(policy.time);
        for (const auto& policy : config.dynamic_policy)
            if (policy.time >= config.opening_time) consider(policy.time);
        consider(retirement_allowed_sec);
        consider(next_stream_action_sec);
        if (dynamic_extra_ovens > 0) consider(dynamic_hold_until_sec);
        if (last_earliest_quote_sec >= 0) {
            const DynamicOvenPolicyEntry* policy = dynamic_policy_at(from);
            if (policy && dynamic_extra_ovens > 0 && policy->new_oven_sustain_sec > 0)
                consider(last_earliest_quote_sec - policy->new_oven_sustain_sec + 1);
        }
        for (const OvenSlotGroupRuntime& oven : oven_storage) {
            consider(oven.transition_finish_sec);
            for (const OvenSlotRuntime& slot : oven.slots)
                if (slot.task_id >= 0) consider(slot.busy_until_sec);
        }
        for (const QueueTaskItem& item : queue_manager.current_queue) {
            if (item.task_id < 0) continue;
            const TaskRuntime& task = task_storage[item.task_id];
            if (task.state == TaskState::Planned) consider(task.chef_start_sec);
            else if (task.state == TaskState::ChefPreparing) consider(task.chef_finish_sec);
        }
        return next;
    }

    // В одну секунду: завершаем выпечку, применяем политику, запускаем старые
    // задачи, затем обрабатываем входящие заказы по одному с их немедленным запуском.
    void process_event_time(int time) {
        std::vector<int> finished;
        for (const OvenSlotGroupRuntime& oven : oven_storage)
            for (const OvenSlotRuntime& slot : oven.slots)
                if (slot.task_id >= 0 && slot.busy_until_sec == time)
                    finished.push_back(slot.task_id);
        for (int id : finished) {
            finish_task(id, time);
            if (fatal_error) return;
        }

        // Совпадение статической и динамической политик допустимо; повтор
        // внутри одного вида в ту же секунду неоднозначен.
        const int static_matches = std::count_if(config.static_policy.begin(),
            config.static_policy.end(), [&](const StaticOvenPolicyEntry& p) { return p.time == time; });
        const int dynamic_matches = std::count_if(config.dynamic_policy.begin(),
            config.dynamic_policy.end(), [&](const DynamicOvenPolicyEntry& p) { return p.time == time; });
        if (static_matches > 1 || dynamic_matches > 1)
            report_issue_at(time, IssueSeverity::Info, "SAME_SECOND_POLICY",
                "Несколько политик одного типа получили одинаковое время");
        update_ovens(time);
        if (fatal_error) return;
        process_due_tasks(time);
        if (fatal_error) return;

        // Заказы с одинаковой секундой проходят полный цикл по очереди один за другим.
        while (next_order_index < incoming_orders.size() &&
            incoming_orders[next_order_index].timestamp_sec == time) {
            const Order& order_static = incoming_orders[next_order_index++];
            receive_order(order_static);
            if (fatal_error) return;
            process_due_tasks(time);
            if (fatal_error) return;
        }
        collect_event_metrics(time);
        run_summary.profit = run_summary.revenue - run_summary.cost_price -
            run_summary.oven_electricity_cost;
    }
};