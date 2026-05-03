#include "instance_manager.h"

#include <stdexcept>

// предполагаем интерфейс runtime
class IRuntime {
public:
    virtual ~IRuntime() = default;

    virtual std::string build_command(const InstanceConfig&) const = 0;
    virtual std::string working_dir(const InstanceConfig&) const = 0;

    virtual std::string stop_command() const = 0;

    virtual bool is_ready_log(const std::string& line) const = 0;
};

class RuntimeRegistry {
public:
    virtual const IRuntime* get(const std::string& id) const = 0;
};

// ─────────────────────────────

InstanceManager::InstanceManager(RuntimeRegistry& rr, ProcessManager& pm)
    : runtimeRegistry_(rr), processManager_(pm)
{}

// ─────────────────────────────
// register
// ─────────────────────────────

void InstanceManager::register_instance(const InstanceConfig& config) {
    std::lock_guard lock(mutex_);

    if (instances_.find(config.id) != instances_.end()) {
        throw std::runtime_error("Instance already exists: " + config.id);
    }

    const IRuntime* rt = runtimeRegistry_.get(config.runtime_id);
    if (!rt) {
        throw std::runtime_error("Runtime not found: " + config.runtime_id);
    }

    Instance inst;
    inst.config = config;
    inst.runtime = rt;

    instances_.emplace(config.id, std::move(inst));
}

// ─────────────────────────────
// start
// ─────────────────────────────

bool InstanceManager::start(const std::string& id) {
    std::lock_guard lock(mutex_);

    auto it = instances_.find(id);
    if (it == instances_.end()) return false;

    auto& inst = it->second;

    if (inst.state == InstanceState::RUNNING ||
        inst.state == InstanceState::STARTING)
        return false;

    auto cmd = inst.runtime->build_command(inst.config);
    auto dir = inst.runtime->working_dir(inst.config);

    ProcessManager::Config cfg;
    cfg.command = cmd;
    cfg.working_dir = dir;

    cfg.on_stdout = [this, id](const std::string& line) {
        handle_stdout(id, line);
    };

    cfg.on_exit = [this, id]() {
        handle_exit(id);
    };

    inst.pid = processManager_.start(cfg);
    inst.state = InstanceState::STARTING;
    inst.last_start = std::chrono::steady_clock::now();

    return true;
}

// ─────────────────────────────
// stop (graceful)
// ─────────────────────────────

bool InstanceManager::stop(const std::string& id) {
    std::lock_guard lock(mutex_);

    auto it = instances_.find(id);
    if (it == instances_.end()) return false;

    auto& inst = it->second;

    if (inst.state != InstanceState::RUNNING) return false;

    auto cmd = inst.runtime->stop_command();

    processManager_.write_stdin(inst.pid, cmd + "\n");

    inst.state = InstanceState::STOPPING;
    inst.stop_requested_at = std::chrono::steady_clock::now();

    return true;
}

// ─────────────────────────────
// kill (force)
// ─────────────────────────────

bool InstanceManager::kill(const std::string& id) {
    std::lock_guard lock(mutex_);

    auto it = instances_.find(id);
    if (it == instances_.end()) return false;

    auto& inst = it->second;

    processManager_.stop(inst.pid);

    inst.state = InstanceState::STOPPED;
    return true;
}

// ─────────────────────────────
// restart
// ─────────────────────────────

bool InstanceManager::restart(const std::string& id) {
    if (!stop(id)) return false;

    // тупо — потом можно сделать async + wait
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    return start(id);
}

// ─────────────────────────────
// send command
// ─────────────────────────────

bool InstanceManager::send_command(const std::string& id, const std::string& cmd) {
    std::lock_guard lock(mutex_);

    auto it = instances_.find(id);
    if (it == instances_.end()) return false;

    auto& inst = it->second;

    if (inst.state != InstanceState::RUNNING) return false;

    processManager_.write_stdin(inst.pid, cmd + "\n");
    return true;
}

// ─────────────────────────────
// stdout handler
// ─────────────────────────────

void InstanceManager::handle_stdout(const std::string& id, const std::string& line) {
    std::lock_guard lock(mutex_);

    auto it = instances_.find(id);
    if (it == instances_.end()) return;

    auto& inst = it->second;

    // runtime parsing (READY detection)
    if (inst.state == InstanceState::STARTING &&
        inst.runtime->is_ready_log(line))
    {
        inst.state = InstanceState::RUNNING;
    }

    // TODO: сюда потом:
    // - логирование
    // - web streaming
    // - health check
}

// ─────────────────────────────
// exit handler
// ─────────────────────────────

void InstanceManager::handle_exit(const std::string& id) {
    std::lock_guard lock(mutex_);

    auto it = instances_.find(id);
    if (it == instances_.end()) return;

    auto& inst = it->second;

    if (inst.state == InstanceState::STOPPING) {
        inst.state = InstanceState::STOPPED;
    } else {
        inst.state = InstanceState::CRASHED;
        inst.restart_count++;
    }

    inst.pid = 0;
}

// ─────────────────────────────
// state / exists
// ─────────────────────────────

InstanceState InstanceManager::get_state(const std::string& id) const {
    std::lock_guard lock(mutex_);

    auto it = instances_.find(id);
    if (it == instances_.end())
        return InstanceState::STOPPED;

    return it->second.state;
}

bool InstanceManager::exists(const std::string& id) const {
    std::lock_guard lock(mutex_);
    return instances_.find(id) != instances_.end();
}