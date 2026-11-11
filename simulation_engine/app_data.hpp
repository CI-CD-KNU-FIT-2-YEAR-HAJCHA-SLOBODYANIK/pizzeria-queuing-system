#pragma once

namespace {
    // Підтримка UTF-8 для шляхів у C++17 і C++20.
    std::string path_utf8(const fs::path& path) {
        const auto value = path.u8string();
        return { value.begin(), value.end() };
    }

    fs::path path_from_utf8(const std::string& value) {
#if defined(__cpp_char8_t)
        return fs::path(std::u8string(value.begin(), value.end()));
#else
        return fs::u8path(value);
#endif
    }

    J read_json(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open JSON: " + path_utf8(path));
        try {
            J data;
            in >> data;
            if (!in) throw std::runtime_error("Cannot read JSON");
            return data;
        }
        catch (const std::exception& e) {
            throw std::runtime_error(path_utf8(path) + ": " + e.what());
        }
    }

    const J* find_key(const J& obj, const std::string& key) {
        if (!obj.is_object()) return nullptr;
        const auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it.value();
    }

    // Суворо перевіряє типи: json.get<int>() сам по собі може відкидати дробову частину.
    template<class T>
    T checked(const J& value, const std::string& name) {
        if constexpr (std::is_same_v<T, int>) {
            if (value.is_number()) {
                const double n = value.get<double>();
                if (std::isfinite(n) && std::floor(n) == n &&
                    n >= double((std::numeric_limits<int>::min)()) &&
                    n <= double((std::numeric_limits<int>::max)())) return int(n);
            }
            throw std::runtime_error("Expected 32-bit integer: " + name);
        }
        else if constexpr (std::is_same_v<T, double>) {
            if (value.is_number()) {
                const double n = value.get<double>();
                if (std::isfinite(n)) return n;
            }
            throw std::runtime_error("Expected finite number: " + name);
        }
        else if constexpr (std::is_same_v<T, bool>) {
            if (value.is_boolean()) return value.get<bool>();
            throw std::runtime_error("Expected boolean: " + name);
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            if (value.is_string()) return value.get<std::string>();
            throw std::runtime_error("Expected string: " + name);
        }
    }

    template<class T>
    T required(const J& obj, const std::string& name) {
        return checked<T>(obj.at(name), name);
    }

    template<class T>
    T optional(const J& obj, const std::string& name, T fallback) {
        const J* value = find_key(obj, name);
        return value ? checked<T>(*value, name) : fallback;
    }

    void deep_merge(J& base, const J& override_values) {
        if (!base.is_object() || !override_values.is_object()) {
            base = override_values;
            return;
        }
        for (const auto& [key, value] : override_values.items()) {
            if (base.contains(key) && base[key].is_object() && value.is_object())
                deep_merge(base[key], value);
            else
                base[key] = value;
        }
    }

    fs::path absolute_from(const fs::path& base, const std::string& value) {
        fs::path path = path_from_utf8(value);
        if (path.is_relative()) path = base / path;
        return fs::absolute(path).lexically_normal();
    }

    // CSV: лапки, подвоєні лапки, коми та перенесення рядків усередині полів.
    std::vector<std::vector<std::string>> read_csv(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open CSV: " + path_utf8(path));
        std::vector<std::vector<std::string>> rows;
        std::vector<std::string> row;
        std::string field;
        bool quoted = false;
        bool at_start = true;
        char ch;

        while (in.get(ch)) {
            if (quoted) {
                if (ch == '"') {
                    if (in.peek() == '"') {
                        in.get(ch);
                        field.push_back('"');
                    }
                    else quoted = false;
                }
                else field.push_back(ch);
            }
            else if (ch == '"' && at_start) {
                quoted = true;
                at_start = false;
            }
            else if (ch == ',') {
                row.push_back(field);
                field.clear();
                at_start = true;
            }
            else if (ch == '\n') {
                if (!field.empty() && field.back() == '\r') field.pop_back();
                row.push_back(field);
                field.clear();
                at_start = true;
                if (!(row.size() == 1 && row[0].empty())) rows.push_back(std::move(row));
                row.clear();
            }
            else {
                field.push_back(ch);
                at_start = false;
            }
        }
        if (quoted) throw std::runtime_error("Unterminated CSV quote: " + path_utf8(path));
        if (!field.empty() || !row.empty()) {
            row.push_back(field);
            rows.push_back(std::move(row));
        }
        if (rows.empty()) throw std::runtime_error("Empty CSV: " + path_utf8(path));
        if (rows[0][0].compare(0, 3, "\xef\xbb\xbf") == 0) rows[0][0].erase(0, 3);
        const auto columns = rows[0].size();
        for (std::size_t i = 1; i < rows.size(); ++i)
            if (rows[i].size() != columns)
                throw std::runtime_error("CSV column count mismatch, row " + std::to_string(i + 1));
        return rows;
    }

    class CsvRow {
        const std::vector<std::string>& row;
        const std::map<std::string, std::size_t>& columns;
    public:
        CsvRow(const std::vector<std::string>& data, const std::map<std::string, std::size_t>& header)
            : row(data), columns(header) {}

        std::string get(const std::string& key) const {
            const auto it = columns.find(key);
            return it == columns.end() ? "" : row[it->second];
        }
        int i(const std::string& key, int fallback = 0, bool required = false) const {
            const std::string s = get(key);
            if (s.empty()) {
                if (required) throw std::runtime_error("CSV missing column/value: " + key);
                return fallback;
            }
            std::size_t pos = 0;
            const int value = std::stoi(s, &pos);
            if (pos != s.size()) throw std::runtime_error("Invalid CSV integer in " + key);
            return value;
        }
        double d(const std::string& key, double fallback = 0, bool required = false) const {
            const std::string s = get(key);
            if (s.empty()) {
                if (required) throw std::runtime_error("CSV missing column/value: " + key);
                return fallback;
            }
            std::size_t pos = 0;
            const double value = std::stod(s, &pos);
            if (pos != s.size() || !std::isfinite(value))
                throw std::runtime_error("Invalid CSV number in " + key);
            return value;
        }
    };

    // Однакові групи інгредієнтів у трьох розділах CSV.
    Resources csv_resources(const CsvRow& row, const std::string& suffix, double fallback = 0.0) {
        Resources r{};
        r.meat = row.d("meat" + suffix, fallback);
        r.dough = row.d("dough" + suffix, fallback);
        r.cheese = row.d("cheese" + suffix, fallback);
        r.veg = row.d("veg" + suffix, fallback);
        r.seafood = row.d("seafood" + suffix, fallback);
        return r;
    }

    template<class T, class Convert>
    std::vector<T> load_table(const fs::path& path, Convert convert) {
        const auto rows = read_csv(path);
        std::map<std::string, std::size_t> header;
        for (std::size_t i = 0; i < rows[0].size(); ++i)
            if (!header.emplace(rows[0][i], i).second)
                throw std::runtime_error("Duplicate CSV header: " + rows[0][i]);

        std::vector<T> result;
        result.reserve(rows.size() - 1);
        std::set<int> ids;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            T item = convert(CsvRow(rows[i], header));
            if (!ids.insert(item.id).second)
                throw std::runtime_error("Duplicate ID in CSV: " + std::to_string(item.id));
            result.push_back(std::move(item));
        }
        return result;
    }

    std::vector<PizzaRecipe> load_recipes(const fs::path& path) {
        auto recipes = load_table<PizzaRecipe>(path, [](const CsvRow& r) {
            PizzaRecipe p{};
            p.id = r.i("id", 0, true);
            p.name = r.get("name");
            p.radius_cm = r.i("radius_cm", 0, true);
            p.price_base = r.d("price_base", 0, true);
            p.chef_time_sec = r.i("chef_time_sec", 0, true);
            p.oven_time_sec = r.i("oven_time_sec", 0, true);
            p.resources_base_g = csv_resources(r, "_g");
            if (p.id < 0 || p.radius_cm <= 0 || p.chef_time_sec < 0 ||
                p.oven_time_sec <= 0 || p.price_base < 0)
                throw std::runtime_error("Invalid pizza recipe: " + std::to_string(p.id));
            return p;
            });
        if (recipes.empty()) throw std::runtime_error("No pizza recipes");
        return recipes;
    }

    std::vector<Topping> load_toppings(const fs::path& path) {
        return load_table<Topping>(path, [](const CsvRow& r) {
            Topping t{};
            t.id = r.i("id", 0, true);
            t.name = r.get("name");
            t.price = r.d("price", 0, true);
            t.chef_add_sec = r.i("chef_add_sec");
            t.chef_mult = r.d("chef_mult", 1);
            t.oven_add_sec = r.i("oven_add_sec");
            t.oven_mult = r.d("oven_mult", 1);
            t.resources_mult = csv_resources(r, "_mult", 1.0);
            t.resources_add_g = csv_resources(r, "_add_g");
            if (t.id < 0) throw std::runtime_error("Negative topping ID");
            return t;
            });
    }

    // Обидва переліки приймають рядок або числове значення 0/1.
    template<class E>
    E parse_choice(const J& value, const char* name, const char* first_name, const char* second_name,
        E first, E second) {
        if (value.is_number()) {
            const int index = checked<int>(value, name);
            if (index == 0) return first;
            if (index == 1) return second;
        }
        else if (value.is_string()) {
            const auto text = value.get<std::string>();
            if (text == first_name) return first;
            if (text == second_name) return second;
        }
        throw std::runtime_error(std::string(name) + " must be " + first_name + "/" + second_name + " or 0/1");
    }

    Order parse_order(const J& j) {
        Order o{};
        o.id = required<int>(j, "id");
        o.timestamp_sec = required<int>(j, "timestamp_sec");
        o.source = parse_choice(j.at("source"), "source", "in_person", "online",
            OrderSource::InPerson, OrderSource::Online);
        o.customer_type = parse_choice(j.at("customer_type"), "customer_type", "new", "regular",
            CustomerType::New, CustomerType::Regular);
        o.target_time = required<int>(j, "target_time");
        o.min_wait_sec = required<int>(j, "min_wait_sec");
        o.max_wait_sec = required<int>(j, "max_wait_sec");
        o.delivery_time_sec = required<int>(j, "delivery_time_sec");
        for (const auto& task : j.at("tasks")) {
            OrderTask item{};
            item.pizza_id = required<int>(task, "pizza_id");
            for (const auto& topping : task.at("topping_ids"))
                item.topping_ids.push_back(checked<int>(topping, "topping_ids"));
            o.tasks.push_back(std::move(item));
        }
        if (o.id < 0 || o.timestamp_sec < 0 || o.timestamp_sec >= SECONDS_PER_DAY ||
            o.min_wait_sec < 0 || o.max_wait_sec < o.min_wait_sec || o.delivery_time_sec < 0 ||
            o.target_time < 0 || o.tasks.empty())
            throw std::runtime_error("Invalid order: " + std::to_string(o.id));
        return o;
    }

    std::vector<Order> load_orders(const fs::path& path) {
        std::vector<Order> orders;
        if (path.extension() == ".jsonl") {
            std::ifstream in(path, std::ios::binary);
            if (!in) throw std::runtime_error("Cannot open orders: " + path_utf8(path));
            std::string line;
            std::size_t line_number = 0;
            while (std::getline(in, line)) {
                ++line_number;
                if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
                try {
                    orders.push_back(parse_order(J::parse(line)));
                }
                catch (const std::exception& e) {
                    throw std::runtime_error(path_utf8(path) + ":" + std::to_string(line_number) + ": " + e.what());
                }
            }
        }
        else {
            const J data = read_json(path);
            if (!data.is_array()) throw std::runtime_error("orders.json must contain a JSON array");
            orders.reserve(data.size());
            for (const auto& item : data) orders.push_back(parse_order(item));
        }
        int previous_time = -1;
        std::set<int> ids;
        for (const auto& order : orders) {
            if (order.timestamp_sec < previous_time)
                throw std::runtime_error("Orders must be sorted by timestamp_sec");
            if (!ids.insert(order.id).second)
                throw std::runtime_error("Duplicate order ID: " + std::to_string(order.id));
            previous_time = order.timestamp_sec;
        }
        return orders;
    }

} // namespace
