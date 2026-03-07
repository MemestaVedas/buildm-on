// github_actions.hpp - GitHub Actions Poller for Buildm-on
#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace gh {

struct WorkflowRun {
    std::string repo;
    std::string name;
    std::string status;       // "queued", "in_progress", "completed"
    std::string conclusion;   // "success", "failure", "cancelled", etc.
    std::string head_branch;
    std::string url;
    int duration_s = 0;
    long long id = 0;
};

class ActionsPoller {
public:
    std::string repo;     // e.g. "torvalds/linux"
    std::string token;    // GitHub API token
    std::vector<WorkflowRun> active_runs;
    std::mutex mtx;
    bool config_loaded = false;

    ActionsPoller() {}

    void load_from_config(const std::string& home) {
        std::string cfg_path = home + "/.buildm-on.toml";
        std::ifstream f(cfg_path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("github_repo") != std::string::npos) {
                auto start = line.find('"');
                auto end = line.rfind('"');
                if (start != std::string::npos && end != std::string::npos && start < end)
                    repo = line.substr(start + 1, end - start - 1);
            }
            if (line.find("github_token") != std::string::npos) {
                auto start = line.find('"');
                auto end = line.rfind('"');
                if (start != std::string::npos && end != std::string::npos && start < end)
                    token = line.substr(start + 1, end - start - 1);
            }
        }
        const char* env_r = getenv("GITHUB_REPOSITORY");
        if (env_r) repo = env_r;
        const char* env_t = getenv("GITHUB_TOKEN");
        if (env_t) token = env_t;
        
        config_loaded = !repo.empty();
    }

    void start_polling() {
        if (!config_loaded) return;
        std::thread([this]() {
            while (true) {
                poll();
                std::this_thread::sleep_for(std::chrono::seconds(15));
            }
        }).detach();
    }

private:
    void poll() {
        auto json_str = fetch_runs();
        if (json_str.empty()) return;
        try {
            auto j = nlohmann::json::parse(json_str);
            std::vector<WorkflowRun> runs;
            if (j.contains("workflow_runs")) {
                for (auto& wr : j["workflow_runs"]) {
                    std::string status = wr.value("status", "");
                    if (status != "queued" && status != "in_progress") continue;
                    
                    WorkflowRun r;
                    r.repo = repo;
                    r.id = wr.value("id", 0LL);
                    r.name = wr.value("name", "Action");
                    r.status = status;
                    r.head_branch = wr.value("head_branch", "");
                    r.url = wr.value("html_url", "");
                    runs.push_back(r);
                }
            }
            std::lock_guard<std::mutex> lk(mtx);
            active_runs = runs;
        } catch (...) {}
    }

    std::string fetch_runs() {
        std::string cmd = "curl -sf -H \"Accept: application/vnd.github.v3+json\" ";
        if (!token.empty()) cmd += "-H \"Authorization: token " + token + "\" ";
        cmd += "\"https://api.github.com/repos/" + repo + "/actions/runs?per_page=10\" 2>/dev/null";
        
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return "";
        std::string out;
        char buf[1024];
        while (fgets(buf, sizeof(buf), pipe)) out += buf;
        pclose(pipe);
        return out;
    }
};

} // namespace gh
