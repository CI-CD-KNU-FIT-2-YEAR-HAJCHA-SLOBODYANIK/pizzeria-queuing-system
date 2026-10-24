#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "utils.hpp"

struct Resources {
    double meat = 0.0;  // мясо
    double dough = 0.0;  // тесто
    double cheese = 0.0;  // сыр
    double veg = 0.0;  // овощи
    double seafood = 0.0;  // морепродукты

    Resources& operator+=(const Resources& rhs) {
        meat += rhs.meat; dough += rhs.dough; cheese += rhs.cheese;
        veg += rhs.veg; seafood += rhs.seafood;
        return *this;
    }

    Resources& operator*=(const Resources& rhs) {
        meat *= rhs.meat; dough *= rhs.dough; cheese *= rhs.cheese;
        veg *= rhs.veg; seafood *= rhs.seafood;
        return *this;
    }

    friend Resources operator+(Resources lhs, const Resources& rhs) { return lhs += rhs; }
    friend Resources operator*(Resources lhs, const Resources& rhs) { return lhs *= rhs; }
};

// предполагаемого времени приготовления по радиусам
struct RadiusWaitTimeEntry {
    int radius_cm;
    int wait_time;
};  // ЕСЛИ БУДЕТ ПЕРЕРАБОТАНО - УБРАТЬ СЕЙ ПОЗОР!

enum class OrderSource : uint8_t {  // 0 - оффлайн, 1 - онлайн
    InPerson = 0,
    Online = 1
};

enum class CustomerType : uint8_t {  // 0 - новый клиент, 1 - постоянный клиент
    New = 0,
    Regular = 1
};

// единица пиццы из заказа
struct OrderTask {
    int pizza_id;  // ИД заказываемой пиццы
    std::vector<int> topping_ids;  // массив ИД включаемых добавок
};

// заказ пиццы (пицц)
struct Order {
    int id;  // ид заказа. не очень полезно
    int timestamp_sec;  // время поступления заказа (в секундах!)
    OrderSource source;  // источник заказа
    CustomerType customer_type;  // вид клиента
    int max_wait_sec;  // сколько клиент готов ждать заказ (в секундах)
    int min_wait_sec;  // через сколько секунд клиенту нужен заказ (в секундах)

    std::vector<OrderTask> tasks;  // заказы. ИД пицц и ИД добавок

    int delivery_time_sec;  // время на доставку
};

// структура добавок в пиццу. влияют на времяресурсы и ингредиенты пиццы. (структура неполная за ненадобностью)
struct Topping {
    int id;  // ид добавки (на него указывают Заказы)
    std::string name;  // название добавки (не используется)
    double price;  // цена добавки (для подсчета прибыли)

    Resources resources_mult;  // коэфф цены в ресурсах. сперва коэфф потом адд
    Resources resources_add_g;  // добавляемая цена в ресурсах. сперва коэфф потом адд

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
    double price_base;  // цена пиццы (для подсчета прибыли)

    Resources resources_base_g;  // цена в ресурсах

    int chef_time_sec;  // время приготовления НЕ ПЕЧЬ
    int oven_time_sec;  // время приготовления в ПЕЧИ
};

// динамические политики включения/выключения печей. использовать по минимуму!
struct DynamicOvenPolicyEntry {
    int time;  // колво секунд от 00:00 - время перезаписи предыдущей политики
    // лояльное время НЕ вычитается у конкретной пиццы во время запекания
    int loyal_sec;  // колво секунд Лояльного времени
    int loyal_trigger_sec;  // ПРЕДПОЛАГАЕМОЕ время (секунды, очередь), при котором loyal_time_curr = Min(loyal_time_sec, loyal_time_update_sec)
    int loyal_update_sec;  // каким числом секунд перезаписывается Лояльное время
    int new_oven_trigger_sec;  // ПРЕДПОЛАГАЕМОЕ время (секунды, очередь), при котором включается новая печь
    int new_oven_sustain_sec;  // ПРЕДПОЛАГАЕМОЕ время (секунды, очередь), при котором поддерживается включение новой печи
    int new_oven_loyal_sec;  // сколько времени мы не тормозим включение печи после new_oven_trigger_sec или new_oven_sustain_sec
};

// статические политики включения/выключения печей
struct StaticOvenPolicyEntry {
    int time;  // колво секунд от 00:00 - время перезаписи предыдущей политики
    int target_ovens;  // целевое количество работающих печей
};

// основной конфиг заведения, обязателен для заполнения вручную
struct PizzeriaConfig {
    int opening_time;  // колво секунд от 00:00 - время открытия пиццерии
    int closing_time;  // колво секунд от 00:00 - время закрытия пиццерии

    // цена ингредиентов для подсчета выручки
    Resources ingredient_price_g;  // цена ресурсов за грамм

    // характеристики и цена работы печей
    double oven_cost_sec;  // цена работы печи в секунду
    double idle_power_ratio = 1.f;  // коэфф цены в простое. не распространяется на период нагрева
    int heat_up_sec;  // время нагрева печи
    int cool_down_sec;  // время остывания печи
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
    int online_delay_sec;  // доп ПРЕДПОЛАГАЕМОЕ ожидание онлайн заказа
    int in_person_delay_sec;  // доп ПРЕДПОЛАГАЕМОЕ ожидание личного заказа
    int quant_step_min;  // шаг квантования ПРЕДПОЛАГАЕМОГО времени ожидания (всегда в большую сторону)
    bool quant_by_clock;  // true: 13:42=>13:45, false: +12min => +15min (только для ПРЕДПОЛАГАЕМОГО времени)

    int max_early_prep_sec = 20 * 60;  // лояльное граница допустимого приготовления пиццы заранее при готовке к определеннму времени
    int soft_delay_limit_sec = 5 * 60;  // нормальное лояльное время запаздания заказа. не считается допустимым, но не вызывает тревогу.

    std::vector<StaticOvenPolicyEntry> static_policy;  // статические политики включения/выключения печей.
    std::vector<DynamicOvenPolicyEntry> dynamic_policy;  // динамические политики включения/выключения печей. использовать по минимуму!
};