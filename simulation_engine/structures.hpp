#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "utils.hpp"

struct Resources {
    double meat = 0.0;     // м'ясо
    double dough = 0.0;    // тісто
    double cheese = 0.0;   // сир
    double veg = 0.0;      // овочі
    double seafood = 0.0;  // морепродукти

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

// передбачуваний час приготування за радіусами
struct RadiusWaitTimeEntry {
    int radius_cm;
    int wait_time;
};  // ЯКЩО БУДЕ ПЕРЕРОБЛЕНО - ПРИБРАТИ ЦЮ ГАНЬБУ!

enum class OrderSource : uint8_t {  // 0 - офлайн, 1 - онлайн
    InPerson = 0,
    Online = 1
};

enum class CustomerType : uint8_t {  // 0 - новий клієнт, 1 - постійний клієнт
    New = 0,
    Regular = 1
};

// одиниця піци із замовлення
struct OrderTask {
    int pizza_id;                  // ID піци, що замовляється
    std::vector<int> topping_ids;  // масив ID добавок, що додаються
};

// замовлення піци (піц)
struct Order {
    int id;                      // ID замовлення. не дуже корисно
    int timestamp_sec;           // час надходження замовлення (у секундах!)
    OrderSource source;          // джерело замовлення
    CustomerType customer_type;  // тип клієнта
    int target_time;             // схоже з мін вейт, актуально для замовлень "на потім"

    int max_wait_sec;  // скільки клієнт готовий чекати на замовлення (у секундах)
    int min_wait_sec;  // через скільки секунд клієнту потрібне замовлення (у секундах)

    std::vector<OrderTask> tasks;  // замовлення. ID піц та ID добавок

    int delivery_time_sec;  // час на доставку
};

// структура добавок до піци. впливають на час/ресурси та інгредієнти піци. (структура неповна через непотрібність)
struct Topping {
    int id;            // ID добавки (на нього вказують Замовлення)
    std::string name;  // назва добавки (не використовується)
    double price;      // ціна добавки (для розрахунку прибутку)

    Resources resources_mult;   // коеф. ціни в ресурсах. спочатку коеф., потім адд
    Resources resources_add_g;  // ціна в ресурсах, що додається. спочатку коеф., потім адд

    int chef_add_sec = 0;   // час приготування, що додається, НЕ в ПЕЧІ
    double chef_mult = 1.f; // коеф. часу приготування НЕ в ПЕЧІ

    int oven_add_sec = 0;   // час приготування в ПЕЧІ, що додається
    double oven_mult = 1.f; // коеф. часу приготування в ПЕЧІ
};

// структура піц. необхідні час/ресурси та інгредієнти. (структура неповна через непотрібність)
struct PizzaRecipe {
    int id;               // ID піци (на нього вказують Замовлення)
    std::string name;     // назва піци (не використовується)
    int radius_cm;        // радіус піци (для ПЕРЕДБАЧЕННЯ часу)
    double price_base;    // ціна піци (для розрахунку прибутку)

    Resources resources_base_g;  // ціна в ресурсах

    int chef_time_sec;  // час приготування НЕ ПІЧ
    int oven_time_sec;  // час приготування в ПЕЧІ
};

// динамічні політики ввімкнення/вимкнення печей. використовувати по мінімуму!
struct DynamicOvenPolicyEntry {
    int time;  // к-сть секунд від 00:00 - час перезапису попередньої політики
    // лояльний час НЕ віднімається у конкретної піци під час запікання
    int loyal_sec;             // к-сть секунд Лояльного часу
    int loyal_trigger_sec;     // ПЕРЕДБАЧУВАНИЙ час (секунди, черга), за якого loyal_time_curr = Min(loyal_time_sec, loyal_time_update_sec)
    int loyal_update_sec;      // яким числом секунд перезаписується Лояльний час
    int new_oven_trigger_sec;  // ПЕРЕДБАЧУВАНИЙ час (секунди, черга), за якого вмикається нова піч
    int new_oven_sustain_sec;  // ПЕРЕДБАЧУВАНИЙ час (секунди, черга), за якого підтримується ввімкнення нової печі
    int new_oven_loyal_sec;    // скільки часу ми не гальмуємо ввімкнення печі після new_oven_trigger_sec або new_oven_sustain_sec
};

// статичні політики ввімкнення/вимкнення печей
struct StaticOvenPolicyEntry {
    int time;          // к-сть секунд від 00:00 - час перезапису попередньої політики
    int target_ovens;  // цільова кількість працюючих печей
};

// основний конфіг закладу, обов'язковий для заповнення вручну
struct PizzeriaConfig {
    int opening_time;  // к-сть секунд від 00:00 - час відкриття піцерії
    int closing_time;  // к-сть секунд від 00:00 - час закриття піцерії

    // ціна інгредієнтів для розрахунку виручки
    Resources ingredient_price_g;  // ціна ресурсів за грам

    // характеристики та ціна роботи печей
    double oven_cost_sec;           // ціна роботи печі в секунду
    double idle_power_ratio = 1.f;  // коеф. ціни у простої. не поширюється на період нагрівання
    int heat_up_sec;                // час нагрівання печі
    int cool_down_sec;              // час охолодження печі
    int oven_capacity = 1;          // місткість печі (Рекомендується 1!)

    // усе щодо ПЕРЕДБАЧУВАНОГО часу, який відлякує клієнтів
    std::vector<RadiusWaitTimeEntry> radius_wait_map;
    int est_time_sec(int r) const {  // ПЕРЕДБАЧУВАНИЙ час приготування піци розміру R
        for (const auto& rt : radius_wait_map) {
            if (r <= rt.radius_cm)
                return rt.wait_time;
        }
        return radius_wait_map.back().wait_time;  // масив МУСИТЬ БУТИ не порожнім!!!
    }
    int online_delay_sec;     // додаткове ПЕРЕДБАЧУВАНЕ очікування онлайн-замовлення; нижня межа target_time
    int in_person_delay_sec;  // додаткове ПЕРЕДБАЧУВАНЕ очікування особистого замовлення; нижня межа target_time
    int quant_step_min;       // крок квантування ПЕРЕДБАЧУВАНОГО часу очікування (завжди в більший бік)
    bool quant_by_clock;      // true: 13:42=>13:45, false: +12хв => +15хв (тільки для ПЕРЕДБАЧУВАНОГО часу)

    int max_early_prep_sec = 20 * 60;   // лояльна межа допустимого приготування піци заздалегідь під час готування до певного часу
    int soft_delay_limit_sec = 5 * 60;  // нормальний лояльний час запізнення замовлення. не вважається допустимим, але не викликає тривоги.

    std::vector<StaticOvenPolicyEntry> static_policy;    // статичні політики ввімкнення/вимкнення печей.
    std::vector<DynamicOvenPolicyEntry> dynamic_policy;  // динамічні політики ввімкнення/вимкнення печей. використовувати по мінімуму!
};