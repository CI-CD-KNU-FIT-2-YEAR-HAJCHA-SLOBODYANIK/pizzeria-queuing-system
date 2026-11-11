#pragma once

namespace {
    Resources parse_resources(const J& j) {
        if (!j.is_object()) throw std::runtime_error("Resources must be a JSON object");
        Resources r{};
        r.meat = optional(j, "meat", 0.0);
        r.dough = optional(j, "dough", 0.0);
        r.cheese = optional(j, "cheese", 0.0);
        r.veg = optional(j, "veg", 0.0);
        r.seafood = optional(j, "seafood", 0.0);
        return r;
    }

    PizzeriaConfig parse_pizzeria_config(const J& j) {
        if (!j.is_object()) throw std::runtime_error("Pizzeria config must be an object");
        PizzeriaConfig c{};
        c.opening_time = required<int>(j, "opening_time");
        c.closing_time = required<int>(j, "closing_time");
        c.ingredient_price_g = parse_resources(j.at("ingredient_price_g"));
        c.oven_cost_sec = required<double>(j, "oven_cost_sec");
        c.idle_power_ratio = optional(j, "idle_power_ratio", 1.0);
        c.heat_up_sec = required<int>(j, "heat_up_sec");
        c.cool_down_sec = required<int>(j, "cool_down_sec");
        c.oven_capacity = optional(j, "oven_capacity", 1);
        c.online_delay_sec = required<int>(j, "online_delay_sec");
        c.in_person_delay_sec = required<int>(j, "in_person_delay_sec");
        c.quant_step_min = required<int>(j, "quant_step_min");
        c.quant_by_clock = required<bool>(j, "quant_by_clock");
        c.max_early_prep_sec = optional(j, "max_early_prep_sec", 1200);
        c.soft_delay_limit_sec = optional(j, "soft_delay_limit_sec", 300);

        for (const auto& row : j.at("radius_wait_map")) {
            c.radius_wait_map.push_back({
                required<int>(row, "radius_cm"),
                required<int>(row, "wait_time")
                });
        }
        for (const auto& row : j.at("static_policy")) {
            c.static_policy.push_back({
                required<int>(row, "time"),
                required<int>(row, "target_ovens")
                });
        }
        if (const J* policy = find_key(j, "dynamic_policy")) {
            for (const auto& row : *policy) {
                DynamicOvenPolicyEntry d{};
                d.time = required<int>(row, "time");
                d.loyal_sec = optional(row, "loyal_sec", 0);
                d.loyal_trigger_sec = optional(row, "loyal_trigger_sec", 0);
                d.loyal_update_sec = optional(row, "loyal_update_sec", 0);
                d.new_oven_trigger_sec = optional(row, "new_oven_trigger_sec", SECONDS_PER_DAY);
                d.new_oven_sustain_sec = optional(row, "new_oven_sustain_sec", SECONDS_PER_DAY);
                d.new_oven_loyal_sec = optional(row, "new_oven_loyal_sec", 0);
                c.dynamic_policy.push_back(d);
            }
        }

        if (c.opening_time < 0 || c.closing_time > SECONDS_PER_DAY || c.opening_time >= c.closing_time ||
            c.oven_capacity < 1 || c.heat_up_sec < 0 || c.cool_down_sec < 0 ||
            c.online_delay_sec < 0 || c.in_person_delay_sec < 0 || c.quant_step_min < 1 ||
            c.oven_cost_sec < 0 || c.idle_power_ratio < 0 || c.max_early_prep_sec < 0 ||
            c.radius_wait_map.empty() || c.static_policy.empty())
            throw std::runtime_error("Invalid PizzeriaConfig: time, oven, estimate, or policy parameters");

        for (std::size_t i = 0; i < c.radius_wait_map.size(); ++i) {
            const auto& r = c.radius_wait_map[i];
            if (r.wait_time <= 0 || r.radius_cm <= 0 ||
                (i && c.radius_wait_map[i - 1].radius_cm >= r.radius_cm))
                throw std::runtime_error("radius_wait_map must have positive values sorted by radius");
        }
        for (const auto& p : c.static_policy)
            if (p.time < 0 || p.time >= SECONDS_PER_DAY || p.target_ovens < 1)
                throw std::runtime_error("Static policy must have at least 1 oven");
        for (const auto& p : c.dynamic_policy)
            if (p.time < 0 || p.time >= SECONDS_PER_DAY || p.loyal_sec < 0 || p.loyal_update_sec < 0 ||
                p.new_oven_loyal_sec < 0)
                throw std::runtime_error("Invalid dynamic policy");
        return c;
    }

    QueueConfig parse_queue_config(const J& j) {
        if (!j.is_object()) throw std::runtime_error("queue must be an object");
        QueueConfig q;
        if (const J* mode = find_key(j, "mode")) {
            const std::string s = checked<std::string>(*mode, "queue.mode");
            if (s == "fast") q.mode = QueueSearchMode::Fast;
            else if (s == "bounded") q.mode = QueueSearchMode::Bounded;
            else if (s == "lite-brute") q.mode = QueueSearchMode::LiteBrute;
            else if (s == "enclave-tree") q.mode = QueueSearchMode::EnclaveTree;
            else throw std::runtime_error("Unknown queue.mode '" + s +
                "'. Supported names: fast, bounded, lite-brute, enclave-tree");
            if (!queue_mode_implemented(q.mode))
                throw std::runtime_error("Queue algorithm '" + s +
                    "' is recognized but NOT IMPLEMENTED; no simulations started");
        }
        q.max_search_nodes = optional(j, "max_search_nodes", q.max_search_nodes);
        q.max_repack_nodes = optional(j, "max_repack_nodes", q.max_repack_nodes);
        q.max_branch_per_task = optional(j, "max_branch_per_task", q.max_branch_per_task);
        q.max_variants = optional(j, "max_variants", q.max_variants);
        q.max_time_sec = optional(j, "max_time_sec", q.max_time_sec);
        if (q.max_search_nodes <= 0 || q.max_repack_nodes <= 0 || q.max_branch_per_task <= 0 ||
            q.max_variants < 2 || q.max_time_sec < 1 || q.max_time_sec > SECONDS_PER_DAY)
            throw std::runtime_error("Invalid QueueConfig");
        return q;
    }

    J json_resources(const Resources& r) {
        return Obj{
            {"meat", r.meat},
            {"dough", r.dough},
            {"cheese", r.cheese},
            {"veg", r.veg},
            {"seafood", r.seafood}
        };
    }

    // Нормалізація потрібна для звіту: він має містити також значення за замовчуванням.
    J normalized_config(const PizzeriaConfig& c) {
        J result = Obj{
            {"opening_time", c.opening_time},
            {"closing_time", c.closing_time},
            {"ingredient_price_g", json_resources(c.ingredient_price_g)},
            {"oven_cost_sec", c.oven_cost_sec},
            {"idle_power_ratio", c.idle_power_ratio},
            {"heat_up_sec", c.heat_up_sec},
            {"cool_down_sec", c.cool_down_sec},
            {"oven_capacity", c.oven_capacity},
            {"online_delay_sec", c.online_delay_sec},
            {"in_person_delay_sec", c.in_person_delay_sec},
            {"quant_step_min", c.quant_step_min},
            {"quant_by_clock", c.quant_by_clock},
            {"max_early_prep_sec", c.max_early_prep_sec},
            {"soft_delay_limit_sec", c.soft_delay_limit_sec}
        };
        result["radius_wait_map"] = J::array();
        result["static_policy"] = J::array();
        result["dynamic_policy"] = J::array();

        for (const auto& e : c.radius_wait_map)
            result["radius_wait_map"].push_back(Obj{ {"radius_cm", e.radius_cm}, {"wait_time", e.wait_time} });
        for (const auto& e : c.static_policy)
            result["static_policy"].push_back(Obj{ {"time", e.time}, {"target_ovens", e.target_ovens} });
        for (const auto& e : c.dynamic_policy) {
            result["dynamic_policy"].push_back(Obj{
                {"time", e.time},
                {"loyal_sec", e.loyal_sec},
                {"loyal_trigger_sec", e.loyal_trigger_sec},
                {"loyal_update_sec", e.loyal_update_sec},
                {"new_oven_trigger_sec", e.new_oven_trigger_sec},
                {"new_oven_sustain_sec", e.new_oven_sustain_sec},
                {"new_oven_loyal_sec", e.new_oven_loyal_sec}
                });
        }
        return result;
    }

    J normalized_queue(const QueueConfig& q) {
        const char* mode = "bounded";
        switch (q.mode) {
        case QueueSearchMode::Fast: mode = "fast"; break;
        case QueueSearchMode::Bounded: mode = "bounded"; break;
        case QueueSearchMode::LiteBrute: mode = "lite-brute"; break;
        case QueueSearchMode::EnclaveTree: mode = "enclave-tree"; break;
        }
        return Obj{
            {"mode", mode},
            {"max_search_nodes", q.max_search_nodes},
            {"max_repack_nodes", q.max_repack_nodes},
            {"max_branch_per_task", q.max_branch_per_task},
            {"max_variants", q.max_variants},
            {"max_time_sec", q.max_time_sec}
        };
    }

} // namespace
