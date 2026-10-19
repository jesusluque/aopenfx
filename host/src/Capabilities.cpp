// Copyright (c) 2026 aopenfx contributors.
#include "aofx_host/Capabilities.h"

#include <cstdio>

namespace aofx::host {

void Logger::info(const std::string& line) { std::fprintf(stderr, "[info ] %s\n", line.c_str()); }
void Logger::warn(const std::string& line) { std::fprintf(stderr, "[warn ] %s\n", line.c_str()); }
void Logger::error(const std::string& line) { std::fprintf(stderr, "[error] %s\n", line.c_str()); }

std::vector<std::string> Capabilities::names() const {
    std::vector<std::string> out{"kernels", "scratch", "keep", "borrow", "read", "publish"};
    if (media != nullptr) {
        out.emplace_back("media");
    }
    if (models != nullptr) {
        out.emplace_back("models");
    }
    for (const EngineBackend* engine : engines) {
        if (engine != nullptr) {
            out.emplace_back("engine:" + engine->name());
        }
    }
    return out;
}

}   // namespace aofx::host
