#pragma once

namespace {
    std::atomic<bool> signal_stop{ false };
    void on_interrupt(int) { signal_stop = true; }
    enum class JobState { Queued, Running, Completed, Failed, Cancelled };

    const char* state_name(JobState state) {
        switch (state) {
        case JobState::Queued: return "queued";
        case JobState::Running: return "running";
        case JobState::Completed: return "completed";
        case JobState::Failed: return "failed";
        case JobState::Cancelled: return "cancelled";
        }
        return "unknown";
    }

    struct Job {
        const RunSpec* spec;
        std::atomic<JobState> state{ JobState::Queued };
        std::atomic<int> sim_sec{ 0 };
        std::atomic<int> processed{ 0 };
        std::atomic<bool> cancel{ false };

        explicit Job(const RunSpec* run) : spec(run) {}
    };

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
        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return out.str();
    }

    // Результати записуються потоково; готовий звіт замінює попередній через .tmp/.bak.
    void write_report(const Project& project, const Job& job, const Logger& logger,
        const std::string& status, const std::string& failure, bool overwrite) {
        const auto& run = *job.spec;
        fs::path temp = run.report_path;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("Cannot create temporary report: " + path_utf8(temp));
            out << "{\"schema_version\":1,\"run_id\":" << J(run.id).dump()
                << ",\"status\":" << J(status).dump()
                << ",\"finished_at_utc\":" << J(utc_now()).dump()
                << ",\"config\":{\"pizzeria\":" << run.config_json
                << ",\"queue\":" << project.queue_json
                << ",\"max_ovens\":" << run.max_ovens
                << ",\"tables_id_is_index\":" << J(run.tables_id_is_index)
                << "},\"inputs\":{\"recipes\":" << J(path_utf8(project.recipes_path)).dump()
                << ",\"toppings\":" << J(path_utf8(project.toppings_path)).dump()
                << ",\"orders\":" << J(path_utf8(run.orders_path)).dump()
                << ",\"planned_orders\":" << run.orders->size()
                << "},\"progress\":{\"simulation_sec\":" << job.sim_sec.load()
                << ",\"processed_orders\":" << job.processed.load()
                << ",\"planned_orders\":" << run.orders->size()
                << "},\"failure\":";
            if (failure.empty()) out << "null";
            else out << J(failure).dump();
            out << ",\"results\":";
            logger.write_json(out, project.include_oven_events);
            out << "}\n";
            out.flush();
            if (!out) throw std::runtime_error("Failed writing report: " + path_utf8(temp));
        }

        if (!overwrite && fs::exists(run.report_path)) {
            fs::remove(temp);
            throw std::runtime_error("Refusing to overwrite: " + path_utf8(run.report_path));
        }
        if (!fs::exists(run.report_path)) {
            fs::rename(temp, run.report_path);
            return;
        }
        fs::path backup = run.report_path;
        backup += ".bak";
        if (fs::exists(backup)) throw std::runtime_error("Backup exists: " + path_utf8(backup));
        fs::rename(run.report_path, backup);
        try {
            fs::rename(temp, run.report_path);
            fs::remove(backup);
        }
        catch (...) {
            fs::rename(backup, run.report_path);
            throw;
        }
    }

    struct Runner {
        Project project;
        bool overwrite;
        std::deque<Job> jobs;
        std::vector<std::thread> workers;
        std::atomic<std::size_t> next{ 0 };
        std::atomic<int> finished{ 0 };
        std::atomic<bool> stopped{ false };

        Runner(Project p, bool replace) : project(std::move(p)), overwrite(replace) {
            for (const auto& run : project.runs)
                jobs.emplace_back(&run);
        }

        bool should_stop(const Job& job) const {
            return signal_stop || stopped || job.cancel;
        }

        J progress_counts(const char* event) const {
            int completed = 0, failed = 0, cancelled = 0;
            for (const auto& job : jobs) {
                switch (job.state.load()) {
                case JobState::Completed: ++completed; break;
                case JobState::Failed: ++failed; break;
                case JobState::Cancelled: ++cancelled; break;
                default: break;
                }
            }
            const int done = finished.load();
            return Obj{
                {"event", event},
                {"finished", done},
                {"completed", completed},
                {"failed", failed},
                {"cancelled", cancelled},
                {"total", int(jobs.size())},
                {"percent", 100.0 * done / jobs.size()}
            };
        }

        void run_job(Job& job) {
            job.state = JobState::Running;
            emit(Obj{ {"event", "started"}, {"run_id", job.spec->id} });
            Logger early_log;
            try {
                const RunSpec& run = *job.spec;
                SimulationEngine engine(run.config, project.recipes, project.toppings,
                    run.max_ovens, run.tables_id_is_index, project.queue);
                if (engine.has_fatal_error()) throw std::runtime_error(engine.last_error());
                for (const auto& order : *run.orders) {
                    if (should_stop(job)) break;
                    if (!engine.add_order(order))
                        throw std::runtime_error("Cannot register order " + std::to_string(order.id));
                }
                if (!should_stop(job)) {
                    // Крок у 900 секунд симуляції; перевірка зупинки між кроками.
                    for (int t = 0; t <= SECONDS_PER_DAY; t += 900) {
                        if (should_stop(job)) break;
                        engine.advance_to(std::min(t + 899, SECONDS_PER_DAY));
                        job.sim_sec = std::min(engine.current_sec(), SECONDS_PER_DAY);
                        job.processed = int(engine.summary().orders_total);
                        if (engine.has_fatal_error()) break;
                    }
                }
                const bool cancelled = should_stop(job);
                const bool failed = engine.has_fatal_error();
                const JobState final_state = cancelled ? JobState::Cancelled :
                    failed ? JobState::Failed : JobState::Completed;
                // Зберігаємо діагностику черги один раз після завершення або зупинки запуску.
                engine.log_queue_memory_usage();
                write_report(project, job, engine.logs(), state_name(final_state),
                    failed ? engine.last_error() : "", overwrite);
                job.state = final_state;
            }
            catch (const std::exception& e) {
                try {
                    early_log.log_issue(0, job.sim_sec, IssueSeverity::Critical, "RUN_FAILURE", e.what());
                    write_report(project, job, early_log, "failed", e.what(), overwrite);
                }
                catch (const std::exception& error) {
                    emit(Obj{ {"event", "output_error"}, {"run_id", job.spec->id}, {"message", error.what()} });
                }
                job.state = JobState::Failed;
            }
            emit(Obj{
                {"event", "finished"},
                {"run_id", job.spec->id},
                {"status", state_name(job.state.load())},
                {"output", path_utf8(job.spec->report_path)}
                });
            ++finished;
            emit(progress_counts("progress"));
        }

        void start() {
            for (int i = 0; i < project.threads; ++i) {
                workers.emplace_back([this] {
                    while (true) {
                        const auto index = next.fetch_add(1);
                        if (index >= jobs.size()) return;
                        run_job(jobs[index]);
                    }
                    });
            }
        }

        void stop_one(const std::string& id) {
            for (auto& job : jobs) {
                if (job.spec->id != id) continue;
                job.cancel = true;
                emit(Obj{ {"event", "stop_requested"}, {"run_id", id} });
                return;
            }
            emit(Obj{ {"event", "error"}, {"message", "Unknown run ID: " + id} });
        }

        void stop_all() {
            stopped = true;
            for (auto& job : jobs) job.cancel = true;
            emit(Obj{ {"event", "stop_all_requested"} });
        }

        void status() {
            J rows = J::array();
            for (const auto& job : jobs) {
                rows.push_back(Obj{
                    {"id", job.spec->id},
                    {"state", state_name(job.state.load())},
                    {"simulation_sec", job.sim_sec.load()},
                    {"processed_orders", job.processed.load()},
                    {"planned_orders", job.spec->orders->size()}
                    });
            }
            J report = progress_counts("status");
            report["runs"] = std::move(rows);
            emit(report);
        }

        int join() {
            for (auto& worker : workers)
                if (worker.joinable()) worker.join();
            status();
            for (const auto& job : jobs)
                if (job.state != JobState::Completed) return 2;
            return 0;
        }
    };

} // namespace
