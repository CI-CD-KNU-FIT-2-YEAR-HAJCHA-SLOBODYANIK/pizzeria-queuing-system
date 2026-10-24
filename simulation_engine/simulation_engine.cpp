#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

#include "logger.hpp"
#include "structures.hpp"

size_t MAX_OVEN_SLOTS = 1;  // вместимость одной печи (количество слотов для выпечки)
size_t MAX_OVENS = 10;  // количество печей в пиццерии
bool TABLES_ID_IS_INDEX = false;

Logger logger;

PizzeriaConfig config;

// Менеджер очереди заказов в рантайме
class QueueManager {
public:
    QueueManager() = default;

    // Расчет предполагаемого времени ожидания/приготовления для нового заказа
    bool est_wait_time_sec(int& est_time, const Order& order) const {
        return false;
        // остановились тут. тут всрато будет...
    }

    // Постановка заказа в рабочую очередь
    void order_push(const Order& order) {
        orders_.push_back(order);
    }

    // Получение времени приготовления заказа по его ID
    int get_prep_time_sec(int order_id) const {
        return 0;
    }

    // Доступ к текущему массиву заказов
    const std::vector<Order>& get_orders() const {
        return orders_;
    }

private:
    std::vector<Order> orders_;

    struct PlaneTask {
        TaskRuntime& task;
        int chef_time_start;
        int oven_time_start;
    };
};

// поиск рецепта по ID или по номеру в зависимости от TABLES_ID_IS_INDEX
const PizzaRecipe* get_recipe(const std::vector<PizzaRecipe>& recipes, int recipe_id, int time = -1) {
    if (TABLES_ID_IS_INDEX) {
        if (recipe_id >= 0 && recipe_id < recipes.size()) {
            return &(recipes[recipe_id]);
        }
        if (recipe_id < 0) {
            logger.log_issue(0, time, IssueSeverity::Warning, "RECIPE_NOT_FOUND",
                "Пицца имела отрицательный номер рецепта, а именно " + std::to_string(recipe_id) + "!",
                "Проверьте генератор заказов (рецепты)");
            return nullptr;
        }
        logger.log_issue(0, time, IssueSeverity::Warning, "RECIPE_NOT_FOUND",
            "Пицца под номером " + std::to_string(recipe_id) + " не была найдена в таблице рецептов!",
            "Проверьте генератор или расширьте таблицу рецептов");
        return nullptr;
    }
    else
        for (const auto& r : recipes) {
            if (r.id == recipe_id)
                return &r;
        }
    logger.log_issue(0, time, IssueSeverity::Warning, "RECIPE_NOT_FOUND",
        "Пицца с ID " + std::to_string(recipe_id) + " не была найдена в таблице рецептов!",
        "Проверьте генератор или расширьте таблицу рецептов");
    return nullptr;
}

// поиск добавки по ID или по номеру в зависимости от TABLES_ID_IS_INDEX
const Topping* get_topping(const std::vector<Topping>& toppings, int topping_id, int time = -1) {
    if (TABLES_ID_IS_INDEX) {
        if (topping_id >= 0 && topping_id < toppings.size()) {
            return &(toppings[topping_id]);
        }
        if (topping_id < 0) {
            logger.log_issue(0, time, IssueSeverity::Warning, "RECIPE_NOT_FOUND",
                "Пицца имела отрицательный номер добавки, а именно " + std::to_string(topping_id) + "!",
                "Проверьте генератор заказов (добавки)");
            return nullptr;
        }
        logger.log_issue(0, time, IssueSeverity::Warning, "TOPPING_NOT_FOUND",
            "Добавка под номером " + std::to_string(topping_id) + " не была найдена в таблице добавок!",
            "Проверьте генератор или расширьте таблицу добавок");
        return nullptr;
    }
    else
        for (const auto& t : toppings) {
            if (t.id == topping_id)
                return &t;
        }
    logger.log_issue(0, time, IssueSeverity::Warning, "TOPPING_NOT_FOUND",
        "Добавка с ID " + std::to_string(topping_id) + " не была найдена в таблице добавок!",
        "Проверьте генератор или расширьте таблицу добавок");
    return nullptr;
}

// состояния пиццы в заказе
enum class TaskState : uint8_t {
    None = 0,
    Planned,
    ChefPreparing,
    ChefPrepFinished,
    InOven,
    Done
};

struct TaskRuntime {
    int order_id = -1; // родительский заказ
    int pizza_id = -1; // ид заказываемой пиццы. с заказом не связано.
    int chef_time_sec = 0;  // время приготовления поваром
    int oven_time_sec = 0;  // время приготовления в печи

    int chef_start_sec = -1;  // секунда начала готовки
    int oven_start_sec = -1;  // секунда начала приготовления в печи
    double price = 0;  // цена со всеми добавками

    Resources resources;  // цена в продуктах

    TaskRuntime(int pizza_ID, int order_ID,
        const std::vector<PizzaRecipe>& recipes, int recipe_id, const std::vector<Topping>& toppings, const std::vector<int>& toppings_id,
        int time = -1) {

        order_id = order_ID;
        pizza_id = pizza_ID;

        const PizzaRecipe* pizza = get_recipe(recipes, recipe_id, time);
        if (pizza) {
            chef_time_sec = pizza->chef_time_sec;
            oven_time_sec = pizza->oven_time_sec;
            price = pizza->price_base;
            resources = pizza->resources_base_g;
        }

        for (auto t_id : toppings_id) {
            const Topping* t = get_topping(toppings, t_id, time);
            if (t)
                apply_topping(*t);
        }
    }

    TaskState state(int time = -1) const { // состояние пиццы. если ты, еблан, запустил без аргумента - ТВОЯ ПРОБЛЕМА
        if (oven_start_sec >= 0 && time >= oven_start_sec) {
            if (time >= oven_start_sec + oven_time_sec)
                return TaskState::Done;
            else
                return TaskState::InOven;
        }
        else if (chef_start_sec >= 0 && time >= chef_start_sec) {
            if (time >= chef_start_sec + chef_time_sec)
                return TaskState::ChefPrepFinished;
            else
                return TaskState::ChefPreparing;
        }
        return TaskState::Planned;
    }

private:
    bool apply_topping(const Topping& topping) {
        chef_time_sec = (int)std::round(chef_time_sec * topping.chef_mult) + topping.chef_add_sec;
        oven_time_sec = (int)std::round(oven_time_sec * topping.oven_mult) + topping.oven_add_sec;

        price += topping.price;

        resources *= topping.resources_mult;
        resources += topping.resources_add_g;

        return true;
    }
};

struct OrderRuntime {
    int order_id = -1;

    int order_received_sec = -1;  // время получения заказа
    int est_time_sec = 0;  // предполагаемое время ожидания (число в вакууме, не на таймлайне)

    int min_deadline_sec = 0;  // с какой секунды клиенту нужен заказ
    int max_deadline_sec = 0;  // до какой секунды клиент готов ждать заказ

    std::vector<TaskRuntime*> tasks;

    OrderRuntime(const Order& order, const std::vector<PizzaRecipe>& recipes, const std::vector<Topping>& toppings, int time) {
        order_id = order.id;
        order_received_sec = time;  // делать заказы с временем -1 это вообще бред

        min_deadline_sec = order_received_sec + order.min_wait_sec - order.delivery_time_sec;
        max_deadline_sec = order_received_sec + order.max_wait_sec - order.delivery_time_sec;
    }

private:
    int est_time_calc(const std::vector<PizzaRecipe>& recipes, const std::vector<OrderTask>& tasks, int time = -1) {
        std::vector<int> arrival_only_me;
        arrival_only_me.reserve(tasks.size());
        for (const auto& t : tasks) {
            const PizzaRecipe* r = get_recipe(recipes, t.pizza_id, time);
            if (!r) continue;
            arrival_only_me.push_back(config.est_wait_sec(r->radius_cm));
        }
        // а дальше работает аривал калькулятор
        return 0;
    }
};

// структура группы слотов (единой печи)
struct OvenSlotGroupRuntime {
    int oven_id = 0;  // номер(ид) печи

    OvenState state = OvenState::Off;  // состояние печи. это те которых 4
    float temperature_ratio = 0.0f;  // Температура в диапазоне [0.0, 1.0]
    int countdown_timer_sec = 0;  // Таймер обратного отсчета текущей фазы (нагрев/остывание)
    double cycles_count = 0.0;  // Счетчик циклов нагрева/охлаждения (даже неполных)

    std::vector<OvenSlotRuntime> slots;  // ссылки на дочерние слоты 
};

// структура слота печи
struct OvenSlotRuntime {
    int oven_id = -1;  // номер(ид) родительской печи
    int slot_id = -1;  // номер(ид) слота (без привязки к печам)

    int total_active_sec = 0;  // время работы печи под нагрузкой (в секундах)
    int total_idle_sec = 0;  // время простоя печи в включенном состоянии (включая нагрев) (в секундах)

    int busy_until_sec = -1;  // секунда освобождения слота печи
    int task_id = -1;  // ид задачи (текущая пицца). <0 если свободно
    bool is_free() const { return task_id < 0; }
};

struct ShefRuntime {
    int target_slot_id = -1;  // слот, в который повар хочет положить пиццу

    int busy_until_sec = -1;  // секунда завершения работы повара
    int task_id = -1;  // ид задачи (текущая пицца)

    ShefRuntime(int task, int slot, const std::vector<PizzaRecipe>& recipes, const std::vector<Topping>& toppings, const std::vector<OvenSlotGroupRuntime>& slots) {
        target_slot_id = slot;
        task_id = task;
    }
};