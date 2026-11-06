#include "logger.hpp"
#include <nlohmann/json.hpp>

#include <ostream>

void Logger::log_issue(const LogNotification& notification) {
    notifications.push_back(notification);
}
void Logger::log_issue(int thread_id, int timestamp_sec, IssueSeverity severity,
    const std::string& code, const std::string& message, const std::string& recommendation) {
    notifications.push_back({ thread_id, timestamp_sec, severity, code, message, recommendation });
}
void Logger::log_oven_event(const OvenEvent& event) {
    oven_events.push_back(event);
}
void Logger::log_order(const OrderResult& result) {
    order_results.push_back(result);
}
void Logger::finalize_summary(const RunSummary& run_summary) {
    summary = run_summary;
}

namespace {
    using J = nlohmann::json;
    using Obj = J::object_t;

    J resources_json(const Resources& r) {
        return Obj{ {"meat",r.meat},{"dough",r.dough},{"cheese",r.cheese},
            {"veg",r.veg},{"seafood",r.seafood} };
    }
    J notification_json(const LogNotification& x) {
        const char* label = x.severity == IssueSeverity::Critical ? "critical" :
            x.severity == IssueSeverity::Warning ? "warning" : "info";
        return Obj{ {"thread_id",x.thread_id},{"timestamp_sec",x.timestamp_sec},
            {"severity",int(x.severity)},{"severity_name",label},{"code",x.code},
            {"message",x.message},{"recommendation",x.recommendation} };
    }
    J task_json(const TaskResult& x) {
        return Obj{ {"task_index",x.task_index},{"pizza_id",x.pizza_id},{"oven_id",x.oven_id},
            {"chef_prep_sec",x.chef_prep_sec},{"wait_before_oven_sec",x.wait_before_oven_sec},
            {"oven_bake_sec",x.oven_bake_sec},{"wait_for_customer_sec",x.wait_for_customer_sec},
            {"resources_g",resources_json(x.resources_g)},{"price",x.price},{"cost",x.cost} };
    }
    J order_json(const OrderResult& x) {
        Obj o{ {"order_id",x.order_id},{"timestamp_sec",x.timestamp_sec},
            {"arrival_time_sec",x.arrival_time_sec},{"total_wait_sec",x.total_wait_sec},
            {"total_price",x.total_price},{"total_cost",x.total_cost},
            {"resources_total__g",resources_json(x.resources_total__g)} };
        J::array_t tasks;
        tasks.reserve(x.tasks.size());
        for (const auto& t : x.tasks) tasks.push_back(task_json(t));
        o["tasks"] = std::move(tasks);
        return o;
    }
    J oven_event_json(const OvenEvent& x) {
        return Obj{ {"timestamp_sec",x.timestamp_sec},{"oven_id",x.oven_id},
            {"state",int(x.state)},{"action",int(x.action)},
            {"trigger_type",int(x.trigger_type)},{"active_ovens_count",x.active_ovens_count},
            {"temperature_ratio",double(x.temperature_ratio)},
            {"countdown_timer_sec",x.countdown_timer_sec} };
    }
    J oven_summary_json(const OvenSummary& x) {
        return Obj{ {"oven_id",x.oven_id},{"active_sec",x.active_sec},{"idle_sec",x.idle_sec},
            {"cycles",x.cycles},{"load_percent",x.load_percent} };
    }
    J summary_json(const RunSummary& x) {
        Obj o;
#define FIELD(field) o[#field] = x.field
        FIELD(revenue);
        FIELD(cost_price);
        FIELD(oven_electricity_cost);
        FIELD(profit);
        FIELD(lost_revenue);
        FIELD(lost_profit);
        FIELD(profit_realization_percent);
        FIELD(orders_total);
        FIELD(total_pizzas_ordered);
        FIELD(orders_fulfilled);
        FIELD(total_pizzas_fulfilled);
        FIELD(orders_rejected);
        FIELD(total_pizzas_rejected);
        FIELD(avg_wait_sec);
        FIELD(avg_patience_strain_percent);
        FIELD(avg_patience_margin_percent);
        FIELD(total_oven_active_sec);
        FIELD(total_oven_idle_sec);
        FIELD(total_oven_cycles);
        FIELD(total_oven_load_percent);
        FIELD(max_active_chefs);
        FIELD(max_active_chefs_timestamp_sec);
        FIELD(max_active_ovens);
        FIELD(max_active_ovens_timestamp_sec);
        FIELD(max_est_queue_wait_sec);
        FIELD(max_est_queue_timestamp_sec);
        FIELD(max_actual_queue_count);
        FIELD(max_actual_queue_timestamp_sec);
        FIELD(max_baking_load_ratio);
        FIELD(max_baking_load_orders);
        FIELD(max_baking_load_ovens);
        FIELD(max_baking_load_timestamp_sec);
        FIELD(total_meat_g);
        FIELD(total_dough_g);
        FIELD(total_cheese_g);
        FIELD(total_veg_g);
        FIELD(total_seafood_g);
        FIELD(most_popular_pizza_id);
        FIELD(most_popular_topping_id);
        FIELD(online_new_orders_percent);
        FIELD(online_regular_orders_percent);
        FIELD(in_person_new_orders_percent);
        FIELD(in_person_regular_orders_percent);
#undef FIELD
        J::array_t ovens;
        ovens.reserve(x.per_oven_stats.size());
        for (const auto& s : x.per_oven_stats) ovens.push_back(oven_summary_json(s));
        o["per_oven_stats"] = std::move(ovens);
        return o;
    }
    template <typename T, typename F>
    void write_array(std::ostream& out, const std::vector<T>& data, F transform) {
        out << '[';
        bool first = true;
        for (const auto& v : data) {
            if (!first) out << ',';
            first = false;
            out << transform(v);
        }
        out << ']';
    }
} // namespace

// Записывает полный отчёт потоково: память не удваивается при большом количестве заказов.
void Logger::write_json(std::ostream& out, bool include_oven_events) const {
    out << "{\"summary\":";
    out << summary_json(summary);
    out << ",\"notifications\":";
    write_array(out, notifications, notification_json);
    out << ",\"order_results\":";
    write_array(out, order_results, order_json);
    out << ",\"oven_events\":";
    if (include_oven_events) write_array(out, oven_events, oven_event_json);
    else out << "[]";
    out << '}';
}
