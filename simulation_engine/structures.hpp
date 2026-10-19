#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "utils.hpp"

enum class OrderSource : uint8_t {  // 0 - оффлайн, 1 - онлайн
    InPerson = 0,
    Online = 1
};

enum class CustomerType : uint8_t {  // 0 - новый клиент, 1 - постоянный клиент
    New = 0,
    Regular = 1
};

struct OrderItem {
    int pizza_id;  // ИД заказываемой пиццы
    std::vector<int> topping_ids;  // массив ИД включаемых добавок
};

struct Order {
    int id;  // ид заказа. не очень полезно
    int timestamp_sec;  // время поступления заказа (в секундах!)
    OrderSource source;  // источник заказа
    CustomerType customer_type;  // вид клиента
    int max_wait_sec;  // сколько клиент готов ждать заказ (в секундах)

    std::vector<OrderItem> items;  // заказы. ИД пицц и ИД добавок

    int delivery_time_min;
};

// структура добавок в пиццу. влияют на времяресурсы и ингредиенты пиццы. (структура неполная за ненадобностью)
struct Topping {
    int id;  // ид добавки (на него указывают Заказы)
    std::string name;  // название добавки (не используется)
    double price_uah;  // цена добавки (для подсчета прибыли)

    double meat_add_g;  // добавляемая цена в мясоресурсах
    double meat_mult = 1.f;  // коэфф цены в мясоресурсах

    double dough_add_g;  // добавляемая цена в тесторесурсах
    double dough_mult = 1.f;  // коэфф цены в тесторесурсах

    double cheese_add_g;  // добавляемая цена в сыроресурсах
    double cheese_mult = 1.f;  // коэфф цены в сыроресурсах

    double veg_add_g;  // добавляемая цена в овощересурсах
    double veg_mult = 1.f;  // коэфф цены в овощересурсах

    double seafood_add_g;  // добавляемая цена в морепродукторесурсах
    double seafood_mult = 1.f;  // коэфф цены в морепродукторесурсах

    int chef_add_sec = 0;  // добавляемое время приготовления НЕ в ПЕЧИ
    double chef_mult = 1.f;  // коэфф времени приготовления НЕ в ПЕЧИ

    int oven_add_sec = 0;  // добавляемая время приготовления в ПЕЧИ
    double oven_mult = 1.f;  // коэфф времени приготовления в ПЕЧИ
};

// структура пицц. требуемые времяресурсы и ингредиенты. (структура неполная за ненадобностью)
struct PizzaRecipe {
    int id;  // ид пиццы (на него указывают Заказы)
    std::string name;  // название пиццы (не используется)
    int radius_cm;  // радиус пиццы (для ПРЕДПОЛОЖЕНИЯ времени)
    double base_price_uah;  // цена пиццы (для подсчета прибыли)

    double meat_g;  // цена в мясоресурсах
    double dough_g;  // цена в тесторесурсах
    double cheese_g;  // цена в сыроресурсах
    double veg_g;  // цена в овощересурсах
    double seafood_g;  // цена в морепродукторесурсах

    int chef_time_sec;  // время приготовления НЕ ПЕЧЬ
    int oven_time_sec;  // время приготовления в ПЕЧИ
};

// динамические политики включения/выключения печей. использовать по минимуму!
struct DynamicOvenPolicyEntry {
    int time;  // колво минут от 00:00 - время перезаписи предыдущей политики
    int loyal_min;  // колво минут Лояльного времени
    int loyal_trigger_min;  // ПРЕДПОЛАГАЕМОЕ время (минуты, очередь), при котором loyal_time_curr = Min(loyal_time_min, loyal_time_update_min)
    int loyal_update_min;  // каким числом минут перезаписывается Лояльное время
    int new_oven_trigger_min;  // ПРЕДПОЛАГАЕМОЕ время (минуты, очередь), при котором включается новая печь
    int new_oven_sustain_min;  // ПРЕДПОЛАГАЕМОЕ время (минуты, очередь), при котором поддерживается включение новой печи
    int new_oven_loyal_min;  // сколько времени мы не тормозим включение печи после new_oven_trigger_min или new_oven_sustain_min
};

// статические политики включения/выключения печей
struct StaticOvenPolicyEntry {
    int time;  // колво минут от 00:00 - время перезаписи предыдущей политики
    int target_ovens;  // целевое количество работающих печей
};

// Таблица порога ожидания по радиусам
struct RadiusWaitTimeEntry {
    int radius_cm;
    int wait_time;
};

// основной конфиг, обязателен для заполнения вручную
struct PizzeriaConfig {
    int opening_time;  // колво минут от 00:00 - время открытия пиццерии
    int closing_time;  // колво минут от 00:00 - время закрытия пиццерии

    // цена ингредиентов для подсчета выручки
    double meat_price_g;  // цена мяса
    double seafood_price_g;  // цена морепродуктов
    double dough_price_g;  // цена теста
    double cheese_price_g;  // цена сыра
    double veg_price_g;  // цена овощей

    // характеристики и цена работы печей
    double oven_cost_min;  // цена работы печи минуту
    double idle_power_ratio = 1.f;  // коэфф цены в простое. не распространяется на период нагрева
    int heat_up_min;  // время нагрева печи
    int cool_down_min;  // время остывания печи
    int oven_capacity = 1;  // вместимость печи (Рекомендуется 1!)

    // все касательно ПРЕДПОЛАГАЕМОГО времени, которое отпугивает клиентов
    std::vector<RadiusWaitTimeEntry> radius_wait_map;
    int est_wait_sec(int r) const {  // ПРЕДПОЛАГАЕМОЕ время приготовления пиццы размера R
        for (const auto& rt : radius_wait_map) {
            if (r <= rt.radius_cm)
                return rt.wait_time;
        }
        return radius_wait_map.back().wait_time;  // массив ДОЛЖЕН БЫТЬ не пустым!!!
    }
    int online_delay_min;  // доп ПРЕДПОЛАГАЕМОЕ ожидание онлайн заказа
    int in_person_delay_min;  // доп ПРЕДПОЛАГАЕМОЕ ожидание личного заказа
    int quant_step_min;  // шаг квантования ПРЕДПОЛАГАЕМОГО времени ожидания
    bool quant_by_clock;  // true: 13:42=>13:45, false: +12min => +15min (только для ПРЕДПОЛАГАЕМОГО времени)

    std::vector<StaticOvenPolicyEntry> static_policy;  // статические политики включения/выключения печей.
    std::vector<DynamicOvenPolicyEntry> dynamic_policy;  // динамические политики включения/выключения печей. использовать по минимуму!
};