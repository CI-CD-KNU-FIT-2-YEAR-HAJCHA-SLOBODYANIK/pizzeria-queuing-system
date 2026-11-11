#pragma once

namespace {
    struct CLI {
        fs::path manifest;
        int threads = -1;
        bool interactive = false;
        bool overwrite = false;
        std::string orders, recipes, toppings, output, mode;
        std::vector<std::pair<std::string, std::string>> overrides;
    };

    CLI parse_cli(int argc, char** argv) {
        CLI cli;
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 == argc) throw std::runtime_error("Missing value for " + flag);
                return argv[++i];
                };
            if (flag == "--manifest") cli.manifest = path_from_utf8(next());
            else if (flag == "--threads") cli.threads = std::stoi(next());
            else if (flag == "--orders") cli.orders = next();
            else if (flag == "--recipes") cli.recipes = next();
            else if (flag == "--toppings") cli.toppings = next();
            else if (flag == "--output-dir") cli.output = next();
            else if (flag == "--queue-mode") cli.mode = next();
            else if (flag == "--interactive") cli.interactive = true;
            else if (flag == "--overwrite") cli.overwrite = true;
            else if (flag == "--set") {
                const std::string s = next();
                const auto colon = s.find(':');
                if (colon == std::string::npos)
                    throw std::runtime_error("--set expects run_id:path=value or all:path=value");
                cli.overrides.emplace_back(s.substr(0, colon), s.substr(colon + 1));
            }
            else if (flag == "--help" || flag == "-h") {
                std::cout << "Usage: pizzeria --manifest plan.json [--threads N] [--interactive] [--overwrite]\n"
                    << "   [--orders PATH] [--recipes PATH] [--toppings PATH] [--output-dir PATH]\n"
                    << "   [--queue-mode fast|bounded|lite-brute|enclave-tree] [--set all:key=json] [--set run_id:key=json]\n"
                    << "Interactive commands: status, stop RUN_ID, stop-all, wait, exit\n";
                std::exit(0);
            }
            else throw std::runtime_error("Unknown option: " + flag);
        }
        if (cli.manifest.empty()) throw std::runtime_error("--manifest required");
        if (cli.threads == 0 || cli.threads < -1) throw std::runtime_error("--threads must be > 0");
        return cli;
    }

    void set_dotted(J& config, const std::string& path, const J& value) {
        if (path.empty()) throw std::runtime_error("Empty --set path");
        J* current = &config;
        std::size_t start = 0;
        while (true) {
            const auto dot = path.find('.', start);
            const std::string key = path.substr(start, dot - start);
            if (key.empty()) throw std::runtime_error("Invalid --set path");
            if (dot == std::string::npos) {
                (*current)[key] = value;
                return;
            }
            current = &(*current)[key];
            start = dot + 1;
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

    // Спочатку файл налаштувань, потім вбудовані значення (для shared і run).
    void merge_config(J& target, const J& from, const fs::path& base) {
        if (const J* file = find_key(from, "config_file"))
            deep_merge(target, read_json(absolute_from(base, checked<std::string>(*file, "config_file"))));
        if (const J* values = find_key(from, "config")) deep_merge(target, *values);
    }

    // --set може змінювати конфігурацію піцерії або параметри окремого запуску.
    J apply_overrides(J& cfg, const CLI& cli, const std::string& run_id) {
        J special = Obj{};
        for (const auto& [target, expr] : cli.overrides) {
            if (target != "all" && target != run_id) continue;
            const auto eq = expr.find('=');
            if (eq == std::string::npos) throw std::runtime_error("--set expects key=JSON_VALUE");
            const std::string key = expr.substr(0, eq);
            const J value = J::parse(expr.substr(eq + 1));
            if (key == "max_ovens" || key == "tables_id_is_index" || key == "orders")
                special[key] = value;
            else if (key.compare(0, 7, "config.") == 0)
                set_dotted(cfg, key.substr(7), value);
            else
                set_dotted(cfg, key, value);
        }
        return special;
    }

    Project load_project(const CLI& cli) {
        const fs::path manifest = fs::absolute(cli.manifest).lexically_normal();
        const fs::path base = manifest.parent_path();
        const J root = read_json(manifest);
        if (required<int>(root, "version") != 1)
            throw std::runtime_error("Unsupported manifest version");
        const J& shared = root.at("shared");
        Project project;

        const std::string recipes = cli.recipes.empty() ? required<std::string>(shared, "recipes") : cli.recipes;
        const std::string toppings = cli.toppings.empty() ? required<std::string>(shared, "toppings") : cli.toppings;
        project.recipes_path = absolute_from(base, recipes);
        project.toppings_path = absolute_from(base, toppings);
        project.recipes = load_recipes(project.recipes_path);
        project.toppings = load_toppings(project.toppings_path);

        J queue = find_key(root, "queue") ? *find_key(root, "queue") : J(Obj{});
        if (!cli.mode.empty()) queue["mode"] = cli.mode;
        project.queue = parse_queue_config(queue);
        project.queue_json = normalized_queue(project.queue);
        project.include_oven_events = optional(root, "include_oven_events", true);
        project.threads = cli.threads > 0 ? cli.threads : optional(root, "parallelism", 1);
        if (project.threads < 1 || project.threads > 256)
            throw std::runtime_error("parallelism must be 1..256");

        const std::string output_name = cli.output.empty() ? required<std::string>(root, "output_dir") : cli.output;
        const fs::path output = absolute_from(base, output_name);
        fs::create_directories(output);

        J shared_config = Obj{};
        merge_config(shared_config, shared, base);
        const J& runs = root.at("runs");
        if (!runs.is_array() || runs.empty())
            throw std::runtime_error("runs must be a nonempty array");

        // Помилку в ідентифікаторі запуску виявляємо до початку обробки запусків.
        for (const auto& [target, expr] : cli.overrides) {
            if (target == "all") continue;
            bool found = false;
            for (const auto& row : runs) {
                if (const J* id = find_key(row, "id"))
                    if (id->is_string() && id->get<std::string>() == target) found = true;
            }
            if (!found) throw std::runtime_error("Unknown run ID in --set: " + target);
            if (expr.find('=') == std::string::npos)
                throw std::runtime_error("--set expects key=JSON_VALUE");
        }

        std::set<std::string> ids;
        std::map<std::string, std::shared_ptr<const std::vector<Order>>> cached_orders;
        for (const auto& row : runs) {
            RunSpec spec;
            spec.id = required<std::string>(row, "id");
            if (spec.id.empty() || spec.id.size() > 100 ||
                spec.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
                throw std::runtime_error("Run ID may contain only ASCII letters, digits, dash, underscore");
            if (!ids.insert(spec.id).second)
                throw std::runtime_error("Duplicate run ID: " + spec.id);

            J cfg = shared_config;
            merge_config(cfg, row, base);
            const J special = apply_overrides(cfg, cli, spec.id);
            spec.config = parse_pizzeria_config(cfg);
            spec.config_json = normalized_config(spec.config);

            spec.max_ovens = optional(special, "max_ovens",
                optional(row, "max_ovens", optional(root, "max_ovens", 10)));
            spec.tables_id_is_index = optional(special, "tables_id_is_index",
                optional(row, "tables_id_is_index", optional(root, "tables_id_is_index", false)));
            if (spec.max_ovens < 1 || spec.max_ovens > 512)
                throw std::runtime_error("Invalid max_ovens for " + spec.id);
            for (const auto& p : spec.config.static_policy)
                if (p.target_ovens > spec.max_ovens)
                    throw std::runtime_error("Static policy exceeds max_ovens for " + spec.id);

            std::string order_file = cli.orders;
            if (const J* override_value = find_key(special, "orders"))
                order_file = checked<std::string>(*override_value, "orders");
            if (order_file.empty())
                if (const J* local = find_key(row, "orders"))
                    order_file = checked<std::string>(*local, "orders");
            if (order_file.empty()) order_file = required<std::string>(shared, "orders");
            spec.orders_path = absolute_from(base, order_file);
            const std::string key = path_utf8(spec.orders_path);
            auto it = cached_orders.find(key);
            if (it == cached_orders.end()) {
                auto orders = std::make_shared<const std::vector<Order>>(load_orders(spec.orders_path));
                it = cached_orders.emplace(key, std::move(orders)).first;
            }
            spec.orders = it->second;
            spec.report_path = output / path_from_utf8(spec.id + ".json");
            if (!cli.overwrite && fs::exists(spec.report_path))
                throw std::runtime_error("Report already exists (use --overwrite): " + path_utf8(spec.report_path));
            project.runs.push_back(std::move(spec));
        }
        project.threads = std::min(project.threads, int(project.runs.size()));
        return project;
    }

} // namespace
