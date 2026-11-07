#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "simulation_engine.cpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using J = nlohmann::json;
using Obj = J::object_t;

namespace {
    std::atomic<bool> signal_stop{ false };
    void on_interrupt(int) { signal_stop.store(true); }

    // Сохраняем UTF-8 при обмене путями с JSON независимо от версии стандарта C++.
    std::string path_utf8(const fs::path& path) {
        const auto u8 = path.u8string();
        return std::string(u8.begin(), u8.end());
    }
    fs::path path_from_utf8(const std::string& utf8) {
#if defined(__cpp_char8_t)
        return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
        return fs::u8path(utf8);
#endif
    }

    // Используем библиотечный парсер с диагностикой исходного файла.
    J read_json(const fs::path& file) {
        std::ifstream f(file, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot open JSON: " + path_utf8(file));
        try {
            J document;
            f >> document;
            if (!f) throw std::runtime_error("Cannot read JSON");
            return document;
        }
        catch (const std::exception& e) {
            throw std::runtime_error(path_utf8(file) + ": " + e.what());
        }
    }
    const J* find_key(const J& value, const std::string& key) {
        if (!value.is_object()) return nullptr;
        auto it = value.find(key);
        return it == value.end() ? nullptr : &it.value();
    }
    J* find_key(J& value, const std::string& key) {
        if (!value.is_object()) return nullptr;
        auto it = value.find(key);
        return it == value.end() ? nullptr : &it.value();
    }
    std::string string(const J& v, const std::string& name) {
        if (!v.is_string()) throw std::runtime_error("Expected string: " + name);
        return v.get<std::string>();
    }
    int integer(const J& v, const std::string& name) {
        if (!v.is_number()) throw std::runtime_error("Expected 32-bit integer: " + name);
        const double n = v.get<double>();
        if (!std::isfinite(n) || std::floor(n) != n ||
            n < double((std::numeric_limits<int>::min)()) ||
            n > double((std::numeric_limits<int>::max)()))
            throw std::runtime_error("Expected 32-bit integer: " + name);
        return int(n);
    }
    double number(const J& v, const std::string& name) {
        if (!v.is_number()) throw std::runtime_error("Expected finite number: " + name);
        const double n = v.get<double>();
        if (!std::isfinite(n)) throw std::runtime_error("Expected finite number: " + name);
        return n;
    }
    bool boolean(const J& v, const std::string& name) {
        if (!v.is_boolean()) throw std::runtime_error("Expected boolean: " + name);
        return v.get<bool>();
    }
    int required_int(const J& v, const std::string& name) { return integer(v.at(name), name); }
    double required_number(const J& v, const std::string& name) { return number(v.at(name), name); }
    int optional_int(const J& v, const std::string& name, int fallback) {
        const J* p = find_key(v, name); return p ? integer(*p, name) : fallback;
    }
    double optional_number(const J& v, const std::string& name, double fallback) {
        const J* p = find_key(v, name); return p ? number(*p, name) : fallback;
    }
    bool optional_bool(const J& v, const std::string& name, bool fallback) {
        const J* p = find_key(v, name); return p ? boolean(*p, name) : fallback;
    }
    std::string required_string(const J& v, const std::string& name) { return string(v.at(name), name); }

    // Конфигурации объединяются рекурсивно. Массивы политик заменяются целиком.
    void deep_merge(J& base, const J & override) {
        if (!override.is_object() || !base.is_object()) { base = override; return; }
        for (const auto& [key, value] : override.items()) {
            auto* current = find_key(base, key);
            if (current && current->is_object() && value.is_object()) deep_merge(base[key], value);
            else base[key] = value;
        }
    }
    fs::path absolute_from(const fs::path& base, const std::string& value) {
        fs::path p = path_from_utf8(value);
        if (p.is_relative()) p = base / p;
        return fs::absolute(p).lexically_normal();
    }
    Resources parse_resources(const J& j) {
        if (!j.is_object()) throw std::runtime_error("Resources must be a JSON object");
        Resources r;
        r.meat = optional_number(j, "meat", 0); r.dough = optional_number(j, "dough", 0);
        r.cheese = optional_number(j, "cheese", 0); r.veg = optional_number(j, "veg", 0);
        r.seafood = optional_number(j, "seafood", 0);
        return r;
    }
    PizzeriaConfig parse_pizzeria_config(const J& j) {
        if (!j.is_object()) throw std::runtime_error("Pizzeria config must be an object");
        PizzeriaConfig c{};
        c.opening_time = required_int(j, "opening_time");
        c.closing_time = required_int(j, "closing_time");
        c.ingredient_price_g = parse_resources(j.at("ingredient_price_g"));
        c.oven_cost_sec = required_number(j, "oven_cost_sec");
        c.idle_power_ratio = optional_number(j, "idle_power_ratio", 1.0);
        c.heat_up_sec = required_int(j, "heat_up_sec");
        c.cool_down_sec = required_int(j, "cool_down_sec");
        c.oven_capacity = optional_int(j, "oven_capacity", 1);
        c.online_delay_sec = required_int(j, "online_delay_sec");
        c.in_person_delay_sec = required_int(j, "in_person_delay_sec");
        c.quant_step_min = required_int(j, "quant_step_min");
        c.quant_by_clock = boolean(j.at("quant_by_clock"), "quant_by_clock");
        c.max_early_prep_sec = optional_int(j, "max_early_prep_sec", 1200);
        c.soft_delay_limit_sec = optional_int(j, "soft_delay_limit_sec", 300);
        for (const auto& row : j.at("radius_wait_map"))
            c.radius_wait_map.push_back({ required_int(row, "radius_cm"), required_int(row, "wait_time") });
        for (const auto& row : j.at("static_policy"))
            c.static_policy.push_back({ required_int(row,"time"),required_int(row,"target_ovens") });
        if (const J* policy = find_key(j, "dynamic_policy")) {
            for (const auto& row : *policy) {
                DynamicOvenPolicyEntry d{};
                d.time = required_int(row, "time");
                d.loyal_sec = optional_int(row, "loyal_sec", 0);
                d.loyal_trigger_sec = optional_int(row, "loyal_trigger_sec", 0);
                d.loyal_update_sec = optional_int(row, "loyal_update_sec", 0);
                d.new_oven_trigger_sec = optional_int(row, "new_oven_trigger_sec", SECONDS_PER_DAY);
                d.new_oven_sustain_sec = optional_int(row, "new_oven_sustain_sec", SECONDS_PER_DAY);
                d.new_oven_loyal_sec = optional_int(row, "new_oven_loyal_sec", 0);
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
            if (c.radius_wait_map[i].wait_time <= 0 || c.radius_wait_map[i].radius_cm <= 0 ||
                (i && c.radius_wait_map[i - 1].radius_cm >= c.radius_wait_map[i].radius_cm))
                throw std::runtime_error("radius_wait_map must have positive values sorted by radius");
        }
        for (const auto& p : c.static_policy)
            if (p.time < 0 || p.time >= SECONDS_PER_DAY || p.target_ovens < 1)
                throw std::runtime_error("Static policy must have at least 1 oven");
        for (const auto& p : c.dynamic_policy)
            if (p.time < 0 || p.time >= SECONDS_PER_DAY || p.loyal_sec < 0 || p.loyal_update_sec < 0 ||
                p.new_oven_loyal_sec < 0) throw std::runtime_error("Invalid dynamic policy");
        return c;
    }
    QueueConfig parse_queue_config(const J& j) {
        if (!j.is_object()) throw std::runtime_error("queue must be an object");
        QueueConfig q;
        if (const J* mode = find_key(j, "mode")) {
            const std::string s = string(*mode, "queue.mode");
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
        q.max_search_nodes = optional_int(j, "max_search_nodes", q.max_search_nodes);
        q.max_repack_nodes = optional_int(j, "max_repack_nodes", q.max_repack_nodes);
        q.max_branch_per_task = optional_int(j, "max_branch_per_task", q.max_branch_per_task);
        q.max_variants = optional_int(j, "max_variants", q.max_variants);
        q.max_time_sec = optional_int(j, "max_time_sec", q.max_time_sec);
        if (q.max_search_nodes <= 0 || q.max_repack_nodes <= 0 || q.max_branch_per_task <= 0 ||
            q.max_variants < 2 || q.max_time_sec < 1 || q.max_time_sec > SECONDS_PER_DAY)
            throw std::runtime_error("Invalid QueueConfig");
        return q;
    }
    J json_resources(const Resources& x) {
        return Obj{ {"meat",x.meat},{"dough",x.dough},{"cheese",x.cheese},{"veg",x.veg},{"seafood",x.seafood} };
    }
    J normalized_config(const PizzeriaConfig& c) {
        Obj o{ {"opening_time",c.opening_time},{"closing_time",c.closing_time},
            {"ingredient_price_g",json_resources(c.ingredient_price_g)},
            {"oven_cost_sec",c.oven_cost_sec},{"idle_power_ratio",c.idle_power_ratio},
            {"heat_up_sec",c.heat_up_sec},{"cool_down_sec",c.cool_down_sec},
            {"oven_capacity",c.oven_capacity},{"online_delay_sec",c.online_delay_sec},
            {"in_person_delay_sec",c.in_person_delay_sec},{"quant_step_min",c.quant_step_min},
            {"quant_by_clock",c.quant_by_clock},{"max_early_prep_sec",c.max_early_prep_sec},
            {"soft_delay_limit_sec",c.soft_delay_limit_sec} };
        J::array_t radius, stat, dyn;
        for (const auto& e : c.radius_wait_map) radius.push_back(Obj{ {"radius_cm",e.radius_cm},{"wait_time",e.wait_time} });
        for (const auto& e : c.static_policy) stat.push_back(Obj{ {"time",e.time},{"target_ovens",e.target_ovens} });
        for (const auto& e : c.dynamic_policy) dyn.push_back(Obj{ {"time",e.time},{"loyal_sec",e.loyal_sec},
            {"loyal_trigger_sec",e.loyal_trigger_sec},{"loyal_update_sec",e.loyal_update_sec},
            {"new_oven_trigger_sec",e.new_oven_trigger_sec},{"new_oven_sustain_sec",e.new_oven_sustain_sec},
            {"new_oven_loyal_sec",e.new_oven_loyal_sec} });
        o["radius_wait_map"] = radius; o["static_policy"] = stat; o["dynamic_policy"] = dyn;
        return o;
    }
    J normalized_queue(const QueueConfig& q) {
        return Obj{ {"mode",q.mode == QueueSearchMode::Fast ? "fast" : "bounded"},
            {"max_search_nodes",q.max_search_nodes},{"max_repack_nodes",q.max_repack_nodes},
            {"max_branch_per_task",q.max_branch_per_task},{"max_variants",q.max_variants},
            {"max_time_sec",q.max_time_sec} };
    }

    // CSV с поддержкой кавычек, экранированных кавычек и переводов строки в поле.
    std::vector<std::vector<std::string>> read_csv(const fs::path& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot open CSV: " + path_utf8(path));
        std::vector<std::vector<std::string>> rows;
        std::vector<std::string> row;
        std::string field;
        bool quoted = false;
        bool at_field_start = true;
        char ch;
        while (f.get(ch)) {
            if (quoted) {
                if (ch == '"') {
                    if (f.peek() == '"') { f.get(ch); field.push_back('"'); }
                    else quoted = false;
                }
                else field.push_back(ch);
            }
            else if (ch == '"' && at_field_start) { quoted = true; at_field_start = false; }
            else if (ch == ',') { row.push_back(field); field.clear(); at_field_start = true; }
            else if (ch == '\n') {
                if (!field.empty() && field.back() == '\r') field.pop_back();
                row.push_back(field); field.clear(); at_field_start = true;
                if (!(row.size() == 1 && row[0].empty())) rows.push_back(std::move(row));
                row.clear();
            }
            else { field.push_back(ch); at_field_start = false; }
        }
        if (quoted) throw std::runtime_error("Unterminated CSV quote: " + path_utf8(path));
        if (!field.empty() || !row.empty()) { row.push_back(field); rows.push_back(std::move(row)); }
        if (rows.empty()) throw std::runtime_error("Empty CSV: " + path_utf8(path));
        if (rows[0][0].compare(0, 3, "\xef\xbb\xbf") == 0) rows[0][0].erase(0, 3);
        const std::size_t n = rows[0].size();
        for (std::size_t i = 1; i < rows.size(); ++i)
            if (rows[i].size() != n) throw std::runtime_error("CSV column count mismatch, row " + std::to_string(i + 1));
        return rows;
    }
    class CsvRow {
        const std::vector<std::string>& row;
        const std::map<std::string, std::size_t>& columns;
    public:
        CsvRow(const std::vector<std::string>& row, const std::map<std::string, std::size_t>& cols) :row(row), columns(cols) {}
        std::string get(const std::string& key, const std::string& fallback = "") const {
            auto it = columns.find(key);
            if (it == columns.end()) return fallback;
            return row[it->second];
        }
        int i(const std::string& key, int fallback = 0, bool required = false) const {
            const auto s = get(key);
            if (s.empty()) { if (required) throw std::runtime_error("CSV missing column/value: " + key); return fallback; }
            std::size_t p = 0; int v = std::stoi(s, &p);
            if (p != s.size()) throw std::runtime_error("Invalid CSV integer in " + key);
            return v;
        }
        double d(const std::string& key, double fallback = 0.0, bool required = false) const {
            const auto s = get(key);
            if (s.empty()) { if (required) throw std::runtime_error("CSV missing column/value: " + key); return fallback; }
            std::size_t p = 0; double v = std::stod(s, &p);
            if (p != s.size() || !std::isfinite(v)) throw std::runtime_error("Invalid CSV number in " + key);
            return v;
        }
    };
    template<typename T, typename Transform>
    std::vector<T> load_table(const fs::path& p, Transform transform) {
        const auto rows = read_csv(p);
        std::map<std::string, std::size_t> cols;
        for (std::size_t i = 0; i < rows[0].size(); ++i)
            if (!cols.emplace(rows[0][i], i).second) throw std::runtime_error("Duplicate CSV header: " + rows[0][i]);
        std::vector<T> result; result.reserve(rows.size() - 1);
        std::set<int> ids;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            auto v = transform(CsvRow(rows[i], cols));
            if (!ids.insert(v.id).second) throw std::runtime_error("Duplicate ID in CSV: " + std::to_string(v.id));
            result.push_back(std::move(v));
        }
        return result;
    }
    std::vector<PizzaRecipe> load_recipes(const fs::path& path) {
        auto result = load_table<PizzaRecipe>(path, [](const CsvRow& r) {
            PizzaRecipe p{};
            p.id = r.i("id", 0, true); p.name = r.get("name");
            p.radius_cm = r.i("radius_cm", 0, true);
            p.price_base = r.d("price_base", 0, true);
            p.chef_time_sec = r.i("chef_time_sec", 0, true);
            p.oven_time_sec = r.i("oven_time_sec", 0, true);
            p.resources_base_g.meat = r.d("meat_g"); p.resources_base_g.dough = r.d("dough_g");
            p.resources_base_g.cheese = r.d("cheese_g"); p.resources_base_g.veg = r.d("veg_g");
            p.resources_base_g.seafood = r.d("seafood_g");
            if (p.id < 0 || p.radius_cm <= 0 || p.chef_time_sec < 0 || p.oven_time_sec <= 0 || p.price_base < 0)
                throw std::runtime_error("Invalid pizza recipe: " + std::to_string(p.id));
            return p;
            });
        if (result.empty()) throw std::runtime_error("No pizza recipes");
        return result;
    }
    std::vector<Topping> load_toppings(const fs::path& path) {
        return load_table<Topping>(path, [](const CsvRow& r) {
            Topping t{};
            t.id = r.i("id", 0, true); t.name = r.get("name"); t.price = r.d("price", 0, true);
            t.chef_add_sec = r.i("chef_add_sec"); t.chef_mult = r.d("chef_mult", 1);
            t.oven_add_sec = r.i("oven_add_sec"); t.oven_mult = r.d("oven_mult", 1);
            t.resources_mult.meat = r.d("meat_mult", 1); t.resources_mult.dough = r.d("dough_mult", 1);
            t.resources_mult.cheese = r.d("cheese_mult", 1); t.resources_mult.veg = r.d("veg_mult", 1);
            t.resources_mult.seafood = r.d("seafood_mult", 1);
            t.resources_add_g.meat = r.d("meat_add_g"); t.resources_add_g.dough = r.d("dough_add_g");
            t.resources_add_g.cheese = r.d("cheese_add_g"); t.resources_add_g.veg = r.d("veg_add_g");
            t.resources_add_g.seafood = r.d("seafood_add_g");
            if (t.id < 0) throw std::runtime_error("Negative topping ID");
            return t;
            });
    }
    Order parse_order(const J& j) {
        Order o{};
        o.id = required_int(j, "id"); o.timestamp_sec = required_int(j, "timestamp_sec");
        auto parse_source = [](const J& v) {
            if (v.is_number()) { int n = integer(v, "source"); if (n == 0)return OrderSource::InPerson; if (n == 1)return OrderSource::Online; }
            else if (v.is_string()) { if (v.get<std::string>() == "in_person")return OrderSource::InPerson; if (v.get<std::string>() == "online")return OrderSource::Online; }
            throw std::runtime_error("source must be in_person/online or 0/1");
            };
        auto parse_customer = [](const J& v) {
            if (v.is_number()) { int n = integer(v, "customer_type"); if (n == 0)return CustomerType::New; if (n == 1)return CustomerType::Regular; }
            else if (v.is_string()) { if (v.get<std::string>() == "new")return CustomerType::New; if (v.get<std::string>() == "regular")return CustomerType::Regular; }
            throw std::runtime_error("customer_type must be new/regular or 0/1");
            };
        o.source = parse_source(j.at("source")); o.customer_type = parse_customer(j.at("customer_type"));
        o.target_time = required_int(j, "target_time"); o.min_wait_sec = required_int(j, "min_wait_sec");
        o.max_wait_sec = required_int(j, "max_wait_sec"); o.delivery_time_sec = required_int(j, "delivery_time_sec");
        for (const auto& t : j.at("tasks")) {
            OrderTask item{}; item.pizza_id = required_int(t, "pizza_id");
            for (const auto& id : t.at("topping_ids")) item.topping_ids.push_back(integer(id, "topping_ids"));
            o.tasks.push_back(std::move(item));
        }
        if (o.id < 0 || o.timestamp_sec < 0 || o.timestamp_sec >= SECONDS_PER_DAY ||
            o.min_wait_sec < 0 || o.max_wait_sec < o.min_wait_sec || o.delivery_time_sec < 0 ||
            o.target_time < 0 || o.tasks.empty()) throw std::runtime_error("Invalid order: " + std::to_string(o.id));
        return o;
    }
    std::vector<Order> load_orders(const fs::path& path) {
        std::vector<Order> orders;
        auto append = [&](const J& v) { orders.push_back(parse_order(v)); };
        if (path.extension() == ".jsonl") {
            std::ifstream f(path, std::ios::binary);
            if (!f) throw std::runtime_error("Cannot open orders: " + path_utf8(path));
            std::string line;
            std::size_t n = 0;
            while (std::getline(f, line)) {
                ++n; if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
                try { append(J::parse(line)); }
                catch (const std::exception& e) { throw std::runtime_error(path_utf8(path) + ":" + std::to_string(n) + ": " + e.what()); }
            }
        }
        else {
            const J data = read_json(path);
            if (!data.is_array()) throw std::runtime_error("orders.json must contain a JSON array");
            orders.reserve(data.size());
            for (const auto& v : data) append(v);
        }
        int last = -1; std::set<int> ids;
        for (const auto& o : orders) {
            if (o.timestamp_sec < last) throw std::runtime_error("Orders must be sorted by timestamp_sec");
            if (!ids.insert(o.id).second) throw std::runtime_error("Duplicate order ID: " + std::to_string(o.id));
            last = o.timestamp_sec;
        }
        return orders;
    }

    struct CLI {
        fs::path manifest;
        int threads = -1;
        bool interactive = false;
        bool overwrite = false;
        std::string orders, recipes, toppings, output, mode;
        std::vector<std::pair<std::string, std::string>> overrides; // run:key=value или all:key=value
    };
    CLI parse_cli(int argc, char** argv) {
        CLI c;
        for (int i = 1; i < argc; ++i) {
            std::string flag = argv[i];
            auto value = [&]() -> std::string { if (i + 1 == argc) throw std::runtime_error("Missing value for " + flag); return argv[++i]; };
            if (flag == "--manifest") c.manifest = path_from_utf8(value());
            else if (flag == "--threads") c.threads = std::stoi(value());
            else if (flag == "--orders") c.orders = value();
            else if (flag == "--recipes") c.recipes = value();
            else if (flag == "--toppings") c.toppings = value();
            else if (flag == "--output-dir") c.output = value();
            else if (flag == "--queue-mode") c.mode = value();
            else if (flag == "--set") { auto s = value(); auto colon = s.find(':'); if (colon == std::string::npos) throw std::runtime_error("--set expects run_id:path=value or all:path=value"); c.overrides.push_back({ s.substr(0,colon),s.substr(colon + 1) }); }
            else if (flag == "--interactive") c.interactive = true;
            else if (flag == "--overwrite") c.overwrite = true;
            else if (flag == "--help" || flag == "-h") {
                std::cout << "Usage: pizzeria --manifest plan.json [--threads N] [--interactive] [--overwrite]\n"
                    << "   [--orders PATH] [--recipes PATH] [--toppings PATH] [--output-dir PATH]\n"
                    << "   [--queue-mode fast|bounded|lite-brute|enclave-tree] [--set all:key=json] [--set run_id:key=json]\n"
                    << "Interactive commands: status, stop RUN_ID, stop-all, wait, exit\n";
                std::exit(0);
            }
            else throw std::runtime_error("Unknown option: " + flag);
        }
        if (c.manifest.empty()) throw std::runtime_error("--manifest required");
        if (c.threads == 0 || c.threads < -1) throw std::runtime_error("--threads must be > 0");
        return c;
    }
    void set_dotted(J& config, const std::string& path, const J& value) {
        if (path.empty()) throw std::runtime_error("Empty --set path");
        J* current = &config;
        std::size_t start = 0;
        for (;;) {
            auto next = path.find('.', start);
            auto key = path.substr(start, next == std::string::npos ? next : next - start);
            if (key.empty()) throw std::runtime_error("Invalid --set path");
            if (next == std::string::npos) { (*current)[key] = value; return; }
            current = &((*current)[key]);
            start = next + 1;
        }
    }
    struct RunSpec {
        std::string id;
        PizzeriaConfig config;
        J config_json;
        fs::path orders_path;
        fs::path report_path;
        int max_ovens = 10;
        bool tables_id_is_index = false;
        std::shared_ptr<const std::vector<Order>> orders;
    };
    struct Project {
        std::vector<RunSpec> runs;
        std::vector<PizzaRecipe> recipes;
        std::vector<Topping> toppings;
        QueueConfig queue;
        J queue_json;
        fs::path recipes_path, toppings_path;
        bool include_oven_events = true;
        int threads = 1;
    };
    Project load_project(const CLI& cli) {
        const fs::path manifest = fs::absolute(cli.manifest).lexically_normal();
        const fs::path base = manifest.parent_path();
        const J root = read_json(manifest);
        if (required_int(root, "version") != 1) throw std::runtime_error("Unsupported manifest version");
        const J& shared = root.at("shared");
        Project project;
        project.recipes_path = absolute_from(base, cli.recipes.empty() ? required_string(shared, "recipes") : cli.recipes);
        project.toppings_path = absolute_from(base, cli.toppings.empty() ? required_string(shared, "toppings") : cli.toppings);
        project.recipes = load_recipes(project.recipes_path);
        project.toppings = load_toppings(project.toppings_path);
        J queue_def = find_key(root, "queue") ? *find_key(root, "queue") : J(Obj{});
        if (!cli.mode.empty()) queue_def["mode"] = cli.mode;
        project.queue = parse_queue_config(queue_def);
        project.queue_json = normalized_queue(project.queue);
        project.include_oven_events = optional_bool(root, "include_oven_events", true);
        project.threads = cli.threads > 0 ? cli.threads : optional_int(root, "parallelism", 1);
        if (project.threads < 1 || project.threads>256) throw std::runtime_error("parallelism must be 1..256");
        const fs::path output = absolute_from(base, cli.output.empty() ? required_string(root, "output_dir") : cli.output);
        fs::create_directories(output);
        J shared_config = J(Obj{});
        if (const J* file = find_key(shared, "config_file"))
            deep_merge(shared_config, read_json(absolute_from(base, string(*file, "config_file"))));
        if (const J* inline_cfg = find_key(shared, "config")) deep_merge(shared_config, *inline_cfg);
        std::map<std::string, std::shared_ptr<const std::vector<Order>>> cached_orders;
        std::set<std::string> ids;
        const J& runs = root.at("runs");
        if (!runs.is_array() || runs.empty()) throw std::runtime_error("runs must be a nonempty array");
        for (const auto& [target, expr] : cli.overrides) {
            if (target == "all") continue;
            bool found = false;
            for (const auto& candidate : runs)
                if (candidate.is_object() && find_key(candidate, "id") && candidate.at("id").is_string() &&
                    candidate.at("id").get<std::string>() == target) found = true;
            if (!found) throw std::runtime_error("Unknown run ID in --set: " + target);
            if (expr.find('=') == std::string::npos) throw std::runtime_error("--set expects key=JSON_VALUE");
        }
        for (const auto& row : runs) {
            RunSpec spec;
            spec.id = required_string(row, "id");
            if (spec.id.empty() || spec.id.size() > 100 || spec.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
                throw std::runtime_error("Run ID may contain only ASCII letters, digits, dash, underscore");
            if (!ids.insert(spec.id).second) throw std::runtime_error("Duplicate run ID: " + spec.id);
            J cfg = shared_config;
            if (const J* file = find_key(row, "config_file")) deep_merge(cfg, read_json(absolute_from(base, string(*file, "config_file"))));
            if (const J* inline_cfg = find_key(row, "config")) deep_merge(cfg, *inline_cfg);
            J special = J(Obj{});
            for (const auto& [target, expr] : cli.overrides) {
                if (target != "all" && target != spec.id) continue;
                auto eq = expr.find('=');
                if (eq == std::string::npos) throw std::runtime_error("--set expects key=JSON_VALUE");
                std::string key = expr.substr(0, eq);
                J value = J::parse(expr.substr(eq + 1));
                if (key == "max_ovens" || key == "tables_id_is_index" || key == "orders")
                    special[key] = value;
                else if (key.compare(0, 7, "config.") == 0) set_dotted(cfg, key.substr(7), value);
                else set_dotted(cfg, key, value);
            }
            spec.config = parse_pizzeria_config(cfg);
            spec.config_json = normalized_config(spec.config);
            spec.max_ovens = optional_int(special, "max_ovens", optional_int(row, "max_ovens", optional_int(root, "max_ovens", 10)));
            spec.tables_id_is_index = optional_bool(special, "tables_id_is_index", optional_bool(row, "tables_id_is_index", optional_bool(root, "tables_id_is_index", false)));
            if (spec.max_ovens < 1 || spec.max_ovens>512) throw std::runtime_error("Invalid max_ovens for " + spec.id);
            for (const auto& e : spec.config.static_policy)
                if (e.target_ovens > spec.max_ovens) throw std::runtime_error("Static policy exceeds max_ovens for " + spec.id);
            std::string orders = cli.orders;
            if (find_key(special, "orders")) orders = string(special.at("orders"), "orders");
            if (orders.empty() && find_key(row, "orders")) orders = string(row.at("orders"), "orders");
            if (orders.empty()) orders = required_string(shared, "orders");
            spec.orders_path = absolute_from(base, orders);
            const auto key = path_utf8(spec.orders_path);
            auto it = cached_orders.find(key);
            if (it == cached_orders.end()) it = cached_orders.emplace(key, std::make_shared<const std::vector<Order>>(load_orders(spec.orders_path))).first;
            spec.orders = it->second;
            spec.report_path = output / path_from_utf8(spec.id + ".json");
            if (!cli.overwrite && fs::exists(spec.report_path))
                throw std::runtime_error("Report already exists (use --overwrite): " + path_utf8(spec.report_path));
            project.runs.push_back(std::move(spec));
        }
        if (project.threads > int(project.runs.size())) project.threads = int(project.runs.size());
        return project;
    }

    struct Job {
        const RunSpec* spec;
        std::atomic<int> state{ 0 };   // 0 queued, 1 running, 2 completed, 3 failed, 4 cancelled
        std::atomic<int> sim_sec{ 0 };
        std::atomic<int> processed{ 0 };
        std::atomic<bool> cancel{ false };
        explicit Job(const RunSpec* s) :spec(s) {}
    };
    const char* state_name(int code) {
        switch (code) {
        case 0:return "queued"; case 1:return "running"; case 2:return "completed";
        case 3:return "failed"; case 4:return "cancelled"; default:return "unknown";
        }
    }
    std::mutex print_mutex;
    void emit(const J& j) {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cout << j.dump() << '\n' << std::flush;
    }
    std::string utc_now() {
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        std::ostringstream os; os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ"); return os.str();
    }

    // Один объект JSON записывается во временный файл и атомарно публикуется после закрытия.
    void write_report(const Project& project, const Job& job, const Logger& logger,
        const std::string& status, const std::string& failure, bool overwrite) {
        const auto& r = *job.spec;
        fs::path temp = r.report_path;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("Cannot create temporary report: " + path_utf8(temp));
            out << "{\"schema_version\":1,\"run_id\":" << J(r.id).dump()
                << ",\"status\":" << J(status).dump()
                << ",\"finished_at_utc\":" << J(utc_now()).dump()
                << ",\"config\":{";
            out << "\"pizzeria\":"; out << r.config_json;
            out << ",\"queue\":"; out << project.queue_json;
            out << ",\"max_ovens\":" << r.max_ovens
                << ",\"tables_id_is_index\":" << (r.tables_id_is_index ? "true" : "false") << "},\"inputs\":{";
            out << "\"recipes\":" << J(path_utf8(project.recipes_path)).dump()
                << ",\"toppings\":" << J(path_utf8(project.toppings_path)).dump()
                << ",\"orders\":" << J(path_utf8(r.orders_path)).dump()
                << ",\"planned_orders\":" << r.orders->size() << "},"
                << "\"progress\":{"
                << "\"simulation_sec\":" << job.sim_sec.load()
                << ",\"processed_orders\":" << job.processed.load()
                << ",\"planned_orders\":" << r.orders->size() << "},"
                << "\"failure\":";
            if (failure.empty()) out << "null"; else out << J(failure).dump();
            out << ",\"results\":";
            logger.write_json(out, project.include_oven_events);
            out << "}\n";
            out.flush();
            if (!out) throw std::runtime_error("Failed writing report: " + path_utf8(temp));
        }
        if (!overwrite && fs::exists(r.report_path)) {
            fs::remove(temp);
            throw std::runtime_error("Refusing to overwrite: " + path_utf8(r.report_path));
        }
        if (!fs::exists(r.report_path)) { fs::rename(temp, r.report_path); return; }
        // Для Windows: существующий отчёт сохраняется как .bak до успешной замены.
        fs::path backup = r.report_path;
        backup += ".bak";
        if (fs::exists(backup)) throw std::runtime_error("Backup exists: " + path_utf8(backup));
        fs::rename(r.report_path, backup);
        try { fs::rename(temp, r.report_path); fs::remove(backup); }
        catch (...) { fs::rename(backup, r.report_path); throw; }
    }

    struct Runner {
        Project project;
        bool overwrite = false;
        std::vector<std::unique_ptr<Job>> jobs;
        std::vector<std::thread> workers;
        std::atomic<std::size_t> next{ 0 };
        std::atomic<int> finished{ 0 };
        std::atomic<bool> stopped{ false };
        Runner(Project p, bool overwrite) :project(std::move(p)), overwrite(overwrite) {
            for (const auto& r : project.runs) jobs.emplace_back(std::make_unique<Job>(&r));
        }
        J progress_counts(const char* event) const {
            const int done = finished.load();
            int succeeded = 0, failed = 0, cancelled = 0;
            for (const auto& j : jobs) {
                switch (j->state.load()) {
                case 2: ++succeeded; break;
                case 3: ++failed; break;
                case 4: ++cancelled; break;
                }
            }
            return Obj{ {"event",event},{"finished",done},{"completed",succeeded},
                {"failed",failed},{"cancelled",cancelled},{"total",int(jobs.size())},
                {"percent",100.0 * done / jobs.size()} };
        }
        void report_progress() { emit(progress_counts("progress")); }
        void run_job(Job& job) {
            job.state = 1;
            emit(Obj{ {"event","started"},{"run_id",job.spec->id} });
            Logger early_log;
            try {
                const auto& r = *job.spec;
                // Локальный SimulationEngine владеет всеми изменяемыми данными этого запуска.
                SimulationEngine engine(r.config, project.recipes, project.toppings, r.max_ovens,
                    r.tables_id_is_index, project.queue);
                if (engine.has_fatal_error()) throw std::runtime_error(engine.last_error());
                for (const auto& order : *r.orders) {
                    if (signal_stop || stopped || job.cancel) break;
                    if (!engine.add_order(order))
                        throw std::runtime_error("Cannot register order " + std::to_string(order.id));
                }
                if (!(signal_stop || stopped || job.cancel)) {
                    // Кооперативная остановка: проверяем флаг между участками суток.
                    // 900 секунд — симулируемое время, а не ожидание в реальном времени.
                    for (int t = 0; t <= SECONDS_PER_DAY; t += 900) {
                        if (signal_stop || stopped || job.cancel) break;
                        engine.advance_to(std::min(t + 899, SECONDS_PER_DAY));
                        job.sim_sec = std::min(engine.current_sec(), SECONDS_PER_DAY);
                        job.processed = int(engine.summary().orders_total);
                        if (engine.has_fatal_error()) break;
                    }
                }
                const bool cancelled = signal_stop || stopped || job.cancel;
                const bool failed = engine.has_fatal_error();
                const std::string state = cancelled ? "cancelled" : failed ? "failed" : "completed";
                const std::string message = failed ? engine.last_error() : "";
                write_report(project, job, engine.logs(), state, message, overwrite);
                job.state = cancelled ? 4 : failed ? 3 : 2;
            }
            catch (const std::exception& e) {
                try {
                    early_log.log_issue(0, job.sim_sec, IssueSeverity::Critical, "RUN_FAILURE", e.what());
                    write_report(project, job, early_log, "failed", e.what(), overwrite);
                }
                catch (const std::exception& output_error) {
                    emit(Obj{ {"event","output_error"},{"run_id",job.spec->id},{"message",output_error.what()} });
                }
                job.state = 3;
            }
            emit(Obj{ {"event","finished"},{"run_id",job.spec->id},
                {"status",state_name(job.state.load())},
                {"output",path_utf8(job.spec->report_path)} });
            ++finished;
            report_progress();
        }
        void start() {
            for (int i = 0; i < project.threads; ++i) workers.emplace_back([this] {
                while (true) {
                    auto index = next.fetch_add(1);
                    if (index >= jobs.size()) return;
                    run_job(*jobs[index]);
                }
                });
        }
        void stop_one(const std::string& id) {
            for (auto& j : jobs) if (j->spec->id == id) { j->cancel = true; emit(Obj{ {"event","stop_requested"},{"run_id",id} }); return; }
            emit(Obj{ {"event","error"},{"message","Unknown run ID: " + id} });
        }
        void stop_all() { stopped = true; for (auto& j : jobs)j->cancel = true; emit(Obj{ {"event","stop_all_requested"} }); }
        void status() {
            J::array_t rows;
            for (const auto& j : jobs) rows.push_back(Obj{ {"id",j->spec->id},{"state",state_name(j->state.load())},
                {"simulation_sec",j->sim_sec.load()},{"processed_orders",j->processed.load()},
                {"planned_orders",j->spec->orders->size()} });
            J report = progress_counts("status");
            report["runs"] = std::move(rows);
            emit(report);
        }
        int join() {
            for (auto& t : workers) if (t.joinable()) t.join();
            status();
            for (const auto& j : jobs) if (j->state != 2) return 2;
            return 0;
        }
    };
} // namespace

int run_program(int argc, char** argv) {
    std::signal(SIGINT, on_interrupt);
#ifdef SIGTERM
    std::signal(SIGTERM, on_interrupt);
#endif
    try {
        const CLI cli = parse_cli(argc, argv);
        Runner runner(load_project(cli), cli.overwrite);
        emit(Obj{ {"event","ready"},{"total",runner.jobs.size()},{"parallelism",runner.project.threads} });
        runner.start();
        if (cli.interactive) {
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line == "status") runner.status();
                else if (line == "stop-all") runner.stop_all();
                else if (line == "wait" || line == "exit") break;
                else if (line.compare(0, 5, "stop ") == 0) runner.stop_one(line.substr(5));
                else if (line == "help") emit(Obj{ {"event","help"},
                    {"commands","status | stop RUN_ID | stop-all | wait | exit"} });
                else if (!line.empty()) emit(Obj{ {"event","error"},{"message","Unknown command"} });
            }
        }
        return runner.join();
    }
    catch (const std::exception& e) {
        emit(Obj{ {"event","fatal"},{"message",e.what()} });
        return 1;
    }
}

#ifdef _WIN32
// Аргументы Windows читаются как Unicode и преобразуются в UTF-8 для JSON и путей.
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> utf8_args;
    std::vector<char*> narrow_argv;
    utf8_args.reserve(argc);
    narrow_argv.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 1;
        std::string arg(size, '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &arg[0], size, nullptr, nullptr) <= 0) return 1;
        arg.pop_back();
        utf8_args.push_back(std::move(arg));
    }
    for (auto& arg : utf8_args) narrow_argv.push_back(arg.data());
    return run_program(argc, narrow_argv.data());
}
#else
int main(int argc, char** argv) { return run_program(argc, argv); }
#endif
