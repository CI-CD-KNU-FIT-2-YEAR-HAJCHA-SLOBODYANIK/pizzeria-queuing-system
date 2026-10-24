#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "structures.hpp"

// уровни проблем, фиксируемых движком симуляции
enum class IssueSeverity : uint8_t {
    Level1 = 1, Info = 1,  // уровень 1: необязательные рекомендации. результаты работы симуляции полностью ликвидны
    Level2 = 2, Warning = 2,  // уровень 2: некоторое нарушение симуляции. некоторая степень погрешности результатов симуляции
    Level3 = 3, Critical = 3  // Уровень 3: фатальные сбои симуляции. невозможность симулирования либо обесценивание результатов симуляции
};

// запись о зарегистрированной проблеме
struct LogNotification {
    int thread_id;  // ид потока, в котором произошла проблема
    int timestamp_sec;  // время (в секундах) когда произошла проблема
    IssueSeverity severity;  // уровень критичности проблемы
    std::string code;  // строка кода?
    std::string message;  // сообщение
    std::string recommendation;  // рекомендация
};


// детализированные метрики по каждой пицце в заказе
struct TaskResult {
    int task_index;  // ИД из конкретных пицц в заказах
    int pizza_id;  // ИД типа пиццы
    int oven_id;  // номер печи где выпекалась пицца
    int chef_prep_sec = 0;  // время приготовления поваром ДО печи
    int wait_before_oven_sec = 0;  // время ожидания свободной печи
    int oven_bake_sec = 0;  // время выпекания в ПЕЧИ

    Resources resources_g;  // расход продуктов в граммах

    double price = 0.0;  // цена КЛИЕНТУ
    double cost = 0.0;  // стоимость НАМ ингредиентов (БЕЗ учета печей)
};

// финальный результат обработки заказа
struct OrderResult {
    int order_id;  // ИД заказа
    int timestamp_sec;  // время (в секундах) когда произошло событие
    int arrival_time_sec = 0;  // предполагаемое время ожидания
    int total_wait_sec = 0;  // общее время ожидания
    double total_price = 0.0;  // общая цена КЛИЕНТУ
    double total_cost = 0.0;  // общая стоимость НАМ ингредиентов (БЕЗ учета печей)

    Resources resources_total__g;  // суммарно продуктов на заказ в граммах

    std::vector<TaskResult> tasks;  // массив пицц в заказе
};

// подытог состояний печи. лучше подавать как часть RunSummary
struct OvenSummary {
    int oven_id;  // ИД печи
    int active_sec = 0;  // суммарно секунд работала конкретная печь
    int idle_sec = 0;  // суммарно секунд ожидала конкретная печь (с учетом времени нагрева)
    double cycles = 0.0;  // циклы нагревов/охлаждений этой печи (включая неполные)
    double load_percent = 0.0;  // процент(100) загруженности этой печи
};

// агрегированная финансово-операционная статистика
struct RunSummary {
    // базовые финансовые показатели
    double revenue = 0.0;  // выручка - суммарная цена проданных пицц
    double cost_price = 0.0;  // себестоимость ингредиентов
    double oven_electricity_cost = 0.0;  // затраты на печи
    double profit = 0.0;  // прибыль (зарплаты поварам не считаем)
    double lost_revenue = 0.0;  // упущенная выручка из-за отказов
    double lost_profit = 0.0;  // упущенная прибыль (с учетом трат на электроэнергию)
    double profit_realization_percent = 0.0;  // процент(100) полученной прибыли от потенциальной (с учетом цены за временя в печи)

    // количественные метрики заказов и пицц
    int orders_total = 0;  // всего заказов
    int total_pizzas_ordered = 0;  // количество заказываемых пицц
    int orders_fulfilled = 0;  // выполнено заказов
    int total_pizzas_fulfilled = 0;  // приготовлено пицц
    int orders_rejected = 0;  // отказались от заказа
    int total_pizzas_rejected = 0;  // пицц не потребовалось готовить по причине отказа от заказа

    // время ожидания и удовлетворенность
    int avg_wait_sec = 0;  // среднее время ожидания
    double avg_patience_strain_percent = 0.0;  // средняя нагрузка на лимит ожидания клиентов, процент (100) (отказы >100%)
    double avg_patience_margin_percent = 0.0;  // средний запас лояльности входящего потока клиентов, процент (100)

    // рабочие параметры и статистика печей
    int total_oven_active_sec = 0;  // суммарно секунд работали печи
    int total_oven_idle_sec = 0;  // суммарно секунд ожидали печи (с учетом времени нагрева)
    double total_oven_cycles = 0.0;  // циклы нагревов/охлаждений печей (неполные циклы тоже считаем)
    double total_oven_load_percent = 0.0;  // процент(100) загруженности печей к ожиданию

    std::vector<OvenSummary> per_oven_stats;  // статистика по каждой конкретной печи

    // пиковое использование оборудования и персонала
    int max_active_chefs = 0;  // максимальное количество одновременно задействованных поваров
    int max_active_chefs_timestamp_sec = 0;  // временная метка пика одновременно задействованных поваров (первого из такого)
    int max_active_ovens = 0;  // максимальное количество одновременно задействованных печей
    int max_active_ovens_timestamp_sec = 0;  // временная метка пика одновременно задействованных печей (первого из таких)

    // пиковые очереди
    int max_est_queue_wait_sec = 0;  // максимальное ПРЕДПОЛАГАЕМОЕ время ожидания в очереди
    int max_est_queue_timestamp_sec = 0;  // временная метка максимальной ПРЕДПОЛАГАЕМОЙ очереди
    int max_actual_queue_count = 0;  // максимальная де-факто очередь заказов
    int max_actual_queue_timestamp_sec = 0;  // временная метка максимальной де-факто очереди

    // пиковая нагрузка на единицу печей (похожа на max_actual_queue_count но немного другая)
    double max_baking_load_ratio = 0.0;  // результат операции (ЗАКАЗЫ * ВРЕМЯ_ВЫПЕКАНИЯ) / ПЕЧИ
    int max_baking_load_orders = 0;  // количество заказов при максимальной нагрузке
    int max_baking_load_ovens = 0;  // количество активных печей при максимальной нагрузке
    int max_baking_load_timestamp_sec = 0;  // временная метка пиковой нагрузки

    // суммарный расход ингредиентов
    double total_meat_g = 0.0;  // общие расходы мяса в граммах
    double total_dough_g = 0.0;  // общие расходы теста в граммах
    double total_cheese_g = 0.0;  // общие расходы сыра в граммах
    double total_veg_g = 0.0;  // общие расходы овощей в граммах
    double total_seafood_g = 0.0;  // общие расходы морепродуктов в граммах

    // фигня по приколу
    int most_popular_pizza_id = -1;  // самая популярная пицца (БЕСПОЛЕЗНАЯ ИНФОРМАЦИЯ)
    int most_popular_topping_id = -1;  // самая популярная добавка (БЕСПОЛЕЗНАЯ ИНФОРМАЦИЯ)

    double online_new_orders_percent = 0.0;  // процент(100) онлайн заказов от НОВЫХ клиентов
    double online_regular_orders_percent = 0.0;  // процент(100) онлайн заказов от ПОСТОЯННЫХ клиентов
    double in_person_new_orders_percent = 0.0;  // процент(100) оффлайн заказов от НОВЫХ клиентов
    double in_person_regular_orders_percent = 0.0;  // процент(100) оффлайн заказов от ПОСТОЯННЫХ клиентов
};

// состояние печи
enum class OvenState : uint8_t {
    Off = 0,  // выключена и холодная
    HeatingUp = 1,  // нагревается
    Active = 2,  // готова и рабочая
    CoolingDown = 3  // остывает
};

// действие печи
enum class OvenAction : uint8_t {
    HeatStart = 0,  // начало нагрева (включение)
    Heated = 1,  // печь разогрелась и готова
    CoolStart = 2,  // начало остывания (выключение)
    Off = 3,  // печь полностью остыла
    PowerOnCountdownUpdate = 4,  // запуск или обновление отсчета таймера на включение
    ShutdownCountdownUpdate = 5  // запуск или обновление отсчета таймера на выключение
};

// источник срабатывания (почему изменилось состояние печи)
enum class PolicyTriggerType : uint8_t {
    Initial = 0,  // базовая инициализация при открытии
    Static = 1,  // расписание (статическая политика по времени)
    Dynamic = 2,  // триггер загрузки очереди (динамическая политика)
    Physics = 3  // отдельно для нагрева или охлаждения
};

// событие влияющее на состояние печи
struct OvenEvent {
    int timestamp_sec;  // время события в секундах
    int oven_id;  // ИД печи
    OvenState state;  // текущее состояние печи
    OvenAction action;  // что произошло с печью
    PolicyTriggerType trigger_type;  // тип политики, вызвавшей изменение
    int active_ovens_count;  // количество активных печей ПОСЛЕ этого события (под угрозой удаления)

    float temperature_ratio = 0.0f;  // текущая температура печи (от 0.0 - комнатная, до 1.0 - рабочей)
    int countdown_timer_sec = 0;  // счетчик/лояльное время на включение или выключение печи в секундах
};

// класс фиксации событий и метрик симуляции
class Logger {
public:
    std::vector<LogNotification> notifications;  // массив технических уведомлений и ошибок движка
    std::vector<OvenEvent> oven_events;  // хронологический лог событий состояний печей
    std::vector<OrderResult> order_results;  // детализированные результаты по обработанным заказам
    RunSummary summary;  // финальная агрегированная аналитика прогона

    // технические метрики
    void log_issue(const LogNotification& notification);  // запись сбоя, некорректных данных или предупреждения движка
    void log_issue(int thread_id, int timestamp_sec, IssueSeverity severity, const std::string& code, const std::string& message, const std::string& recommendation = "");  // прямая запись проблемы без явного создания структуры

    // аналитические метрики
    void log_oven_event(const OvenEvent& event);  // запись физического переключения или обновления таймера печи
    void log_order(const OrderResult& result);  // запись финального результата обработки заказа
    void finalize_summary(const RunSummary& run_summary);  // сохранение итогового аналитического отчета симуляции
};