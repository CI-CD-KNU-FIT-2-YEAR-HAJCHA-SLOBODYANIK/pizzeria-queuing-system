#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
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

#include "app_data.hpp"
#include "app_config.hpp"
#include "app_project.hpp"
#include "app_runner.hpp"

int run_program(int argc, char** argv) {
    std::signal(SIGINT, on_interrupt);
#ifdef SIGTERM
    std::signal(SIGTERM, on_interrupt);
#endif
    try {
        const CLI cli = parse_cli(argc, argv);
        Runner runner(load_project(cli), cli.overwrite);
        emit(Obj{
            {"event", "ready"},
            {"total", runner.jobs.size()},
            {"parallelism", runner.project.threads}
            });
        runner.start();
        if (cli.interactive) {
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line == "status") runner.status();
                else if (line == "stop-all") runner.stop_all();
                else if (line == "wait" || line == "exit") break;
                else if (line.compare(0, 5, "stop ") == 0) runner.stop_one(line.substr(5));
                else if (line == "help")
                    emit(Obj{ {"event", "help"}, {"commands", "status | stop RUN_ID | stop-all | wait | exit"} });
                else if (!line.empty())
                    emit(Obj{ {"event", "error"}, {"message", "Unknown command"} });
            }
        }
        return runner.join();
    }
    catch (const std::exception& e) {
        emit(Obj{ {"event", "fatal"}, {"message", e.what()} });
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> utf8_args;
    std::vector<char*> args;
    utf8_args.reserve(argc);
    args.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 1;
        std::string s(size, '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), size, nullptr, nullptr) <= 0) return 1;
        s.pop_back();
        utf8_args.push_back(std::move(s));
    }
    for (auto& s : utf8_args) args.push_back(s.data());
    return run_program(argc, args.data());
}
#else
int main(int argc, char** argv) { return run_program(argc, argv); }
#endif
