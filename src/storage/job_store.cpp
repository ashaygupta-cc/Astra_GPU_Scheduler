#include "astra/job_store.hpp"

#include <sqlite3.h>
#include <mutex>
#include <iostream>
#include <sstream>

namespace astra {

namespace {

// RAII helper for SQLite prepared statements
class ScopedStmt {
public:
    explicit ScopedStmt(sqlite3_stmt* stmt = nullptr) : stmt_(stmt) {}
    ~ScopedStmt() {
        if (stmt_) {
            sqlite3_finalize(stmt_);
        }
    }

    ScopedStmt(const ScopedStmt&) = delete;
    ScopedStmt& operator=(const ScopedStmt&) = delete;

    ScopedStmt(ScopedStmt&& other) noexcept : stmt_(other.stmt_) {
        other.stmt_ = nullptr;
    }

    ScopedStmt& operator=(ScopedStmt&& other) noexcept {
        if (this != &other) {
            if (stmt_) sqlite3_finalize(stmt_);
            stmt_ = other.stmt_;
            other.stmt_ = nullptr;
        }
        return *this;
    }

    sqlite3_stmt* get() const noexcept { return stmt_; }
    sqlite3_stmt** receive() noexcept {
        if (stmt_) {
            sqlite3_finalize(stmt_);
            stmt_ = nullptr;
        }
        return &stmt_;
    }

    explicit operator bool() const noexcept { return stmt_ != nullptr; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

int64_t to_ms(const std::chrono::system_clock::time_point& tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point from_ms(int64_t ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

} // namespace

struct JobStore::Impl {
    sqlite3* db = nullptr;
    std::mutex mtx;

    ~Impl() {
        close();
    }

    void close() {
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
    }

    bool exec_sql(const char* sql, std::string* error_out = nullptr) {
        char* err_msg = nullptr;
        int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            if (error_out && err_msg) {
                *error_out = err_msg;
            }
            if (err_msg) sqlite3_free(err_msg);
            return false;
        }
        return true;
    }

    bool run_migrations(std::string* error_out = nullptr) {
        const char* create_mig_table = 
            "CREATE TABLE IF NOT EXISTS schema_migrations ("
            "  version INTEGER PRIMARY KEY,"
            "  applied_at_ms INTEGER NOT NULL"
            ");";
        if (!exec_sql(create_mig_table, error_out)) return false;

        // Check current version
        int current_version = 0;
        ScopedStmt check_stmt;
        int rc = sqlite3_prepare_v2(db, "SELECT MAX(version) FROM schema_migrations;", -1, check_stmt.receive(), nullptr);
        if (rc == SQLITE_OK && sqlite3_step(check_stmt.get()) == SQLITE_ROW) {
            if (sqlite3_column_type(check_stmt.get(), 0) != SQLITE_NULL) {
                current_version = sqlite3_column_int(check_stmt.get(), 0);
            }
        }

        if (current_version < 1) {
            const char* v1_sql = 
                "BEGIN TRANSACTION;"
                "CREATE TABLE IF NOT EXISTS jobs ("
                "  id TEXT PRIMARY KEY,"
                "  name TEXT NOT NULL,"
                "  executable_path TEXT NOT NULL,"
                "  argv_json TEXT NOT NULL,"
                "  priority INTEGER NOT NULL,"
                "  device TEXT NOT NULL,"
                "  cpu_slots INTEGER NOT NULL,"
                "  ram_mb INTEGER NOT NULL,"
                "  gpu_memory_mb INTEGER NOT NULL,"
                "  state TEXT NOT NULL,"
                "  max_retries INTEGER NOT NULL,"
                "  retry_count INTEGER NOT NULL,"
                "  checkpointable INTEGER NOT NULL,"
                "  assigned_worker_id INTEGER NOT NULL,"
                "  worker_pid INTEGER NOT NULL,"
                "  exit_code INTEGER NOT NULL,"
                "  failure_reason TEXT NOT NULL,"
                "  workflow_id TEXT NOT NULL,"
                "  submitted_time_ms INTEGER NOT NULL,"
                "  enqueued_time_ms INTEGER NOT NULL,"
                "  started_time_ms INTEGER NOT NULL,"
                "  completed_time_ms INTEGER NOT NULL,"
                "  deadline_ms INTEGER"
                ");"
                "CREATE TABLE IF NOT EXISTS job_dependencies ("
                "  job_id TEXT NOT NULL,"
                "  dependency_id TEXT NOT NULL,"
                "  PRIMARY KEY (job_id, dependency_id)"
                ");"
                "CREATE TABLE IF NOT EXISTS attempts ("
                "  attempt_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "  job_id TEXT NOT NULL,"
                "  attempt_number INTEGER NOT NULL,"
                "  worker_id INTEGER NOT NULL,"
                "  pid INTEGER NOT NULL,"
                "  start_time_ms INTEGER NOT NULL,"
                "  end_time_ms INTEGER NOT NULL,"
                "  exit_code INTEGER NOT NULL,"
                "  error_message TEXT NOT NULL"
                ");"
                "CREATE TABLE IF NOT EXISTS workers ("
                "  id INTEGER PRIMARY KEY,"
                "  pid INTEGER NOT NULL,"
                "  state TEXT NOT NULL,"
                "  current_job_id TEXT NOT NULL,"
                "  restart_count INTEGER NOT NULL,"
                "  last_heartbeat_ms INTEGER NOT NULL,"
                "  started_time_ms INTEGER NOT NULL"
                ");"
                "CREATE TABLE IF NOT EXISTS events ("
                "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "  timestamp_ms INTEGER NOT NULL,"
                "  type TEXT NOT NULL,"
                "  job_id TEXT NOT NULL,"
                "  worker_id INTEGER NOT NULL,"
                "  payload_json TEXT NOT NULL"
                ");"
                "CREATE TABLE IF NOT EXISTS workflows ("
                "  id TEXT PRIMARY KEY,"
                "  name TEXT NOT NULL,"
                "  state TEXT NOT NULL,"
                "  created_time_ms INTEGER NOT NULL"
                ");"
                "INSERT INTO schema_migrations (version, applied_at_ms) VALUES (1, 1000);"
                "COMMIT;";
            if (!exec_sql(v1_sql, error_out)) return false;
        }

        return true;
    }
};

JobStore::JobStore() : impl_(std::make_unique<Impl>()) {}

JobStore::~JobStore() = default;

JobStore::JobStore(JobStore&&) noexcept = default;
JobStore& JobStore::operator=(JobStore&&) noexcept = default;

bool JobStore::open(const std::string& db_path, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->close();

    int rc = sqlite3_open(db_path.c_str(), &impl_->db);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        impl_->close();
        return false;
    }

    // Enable WAL mode & foreign keys for high performance & integrity
    impl_->exec_sql("PRAGMA journal_mode=WAL;");
    impl_->exec_sql("PRAGMA synchronous=NORMAL;");
    impl_->exec_sql("PRAGMA foreign_keys=ON;");

    return impl_->run_migrations(error_out);
}

void JobStore::close() {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->close();
}

bool JobStore::is_open() const {
    return impl_->db != nullptr;
}

int JobStore::get_schema_version(std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return -1;
    }

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, "SELECT MAX(version) FROM schema_migrations;", -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return -1;
    }
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt.get(), 0) != SQLITE_NULL) {
            return sqlite3_column_int(stmt.get(), 0);
        }
    }
    return 0;
}

bool JobStore::save_job(const Job& job, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return false;
    }

    const char* sql = 
        "INSERT INTO jobs ("
        "  id, name, executable_path, argv_json, priority, device, cpu_slots, ram_mb, gpu_memory_mb,"
        "  state, max_retries, retry_count, checkpointable, assigned_worker_id, worker_pid,"
        "  exit_code, failure_reason, workflow_id, submitted_time_ms, enqueued_time_ms,"
        "  started_time_ms, completed_time_ms, deadline_ms"
        ") VALUES ("
        "  ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23"
        ") ON CONFLICT(id) DO UPDATE SET "
        "  name = excluded.name,"
        "  executable_path = excluded.executable_path,"
        "  argv_json = excluded.argv_json,"
        "  priority = excluded.priority,"
        "  device = excluded.device,"
        "  cpu_slots = excluded.cpu_slots,"
        "  ram_mb = excluded.ram_mb,"
        "  gpu_memory_mb = excluded.gpu_memory_mb,"
        "  state = excluded.state,"
        "  max_retries = excluded.max_retries,"
        "  retry_count = excluded.retry_count,"
        "  checkpointable = excluded.checkpointable,"
        "  assigned_worker_id = excluded.assigned_worker_id,"
        "  worker_pid = excluded.worker_pid,"
        "  exit_code = excluded.exit_code,"
        "  failure_reason = excluded.failure_reason,"
        "  workflow_id = excluded.workflow_id,"
        "  submitted_time_ms = excluded.submitted_time_ms,"
        "  enqueued_time_ms = excluded.enqueued_time_ms,"
        "  started_time_ms = excluded.started_time_ms,"
        "  completed_time_ms = excluded.completed_time_ms,"
        "  deadline_ms = excluded.deadline_ms;";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }

    nlohmann::json argv_json = job.argv;
    std::string argv_str = argv_json.dump();
    std::string dev_str = std::string(to_string(job.resources.device));
    std::string state_str = std::string(to_string(job.state));

    sqlite3_bind_text(stmt.get(), 1, job.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, job.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, job.executable_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, argv_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 5, job.priority);
    sqlite3_bind_text(stmt.get(), 6, dev_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 7, static_cast<int>(job.resources.cpu_slots));
    sqlite3_bind_int64(stmt.get(), 8, static_cast<sqlite3_int64>(job.resources.ram_mb));
    sqlite3_bind_int64(stmt.get(), 9, static_cast<sqlite3_int64>(job.resources.gpu_memory_mb));
    sqlite3_bind_text(stmt.get(), 10, state_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 11, job.max_retries);
    sqlite3_bind_int(stmt.get(), 12, job.retry_count);
    sqlite3_bind_int(stmt.get(), 13, job.checkpointable ? 1 : 0);
    sqlite3_bind_int(stmt.get(), 14, job.assigned_worker_id);
    sqlite3_bind_int(stmt.get(), 15, job.worker_pid);
    sqlite3_bind_int(stmt.get(), 16, job.exit_code);
    sqlite3_bind_text(stmt.get(), 17, job.failure_reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 18, job.workflow_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 19, to_ms(job.submitted_time));
    sqlite3_bind_int64(stmt.get(), 20, to_ms(job.enqueued_time));
    sqlite3_bind_int64(stmt.get(), 21, to_ms(job.started_time));
    sqlite3_bind_int64(stmt.get(), 22, to_ms(job.completed_time));
    if (job.deadline.has_value()) {
        sqlite3_bind_int64(stmt.get(), 23, to_ms(*job.deadline));
    } else {
        sqlite3_bind_null(stmt.get(), 23);
    }

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }

    // Save dependencies
    for (const auto& dep_id : job.dependencies) {
        ScopedStmt dep_stmt;
        const char* dep_sql = "INSERT OR IGNORE INTO job_dependencies (job_id, dependency_id) VALUES (?1, ?2);";
        sqlite3_prepare_v2(impl_->db, dep_sql, -1, dep_stmt.receive(), nullptr);
        sqlite3_bind_text(dep_stmt.get(), 1, job.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(dep_stmt.get(), 2, dep_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(dep_stmt.get());
    }

    return true;
}

bool JobStore::update_job_state(const std::string& job_id, JobState state, int exit_code, 
                                const std::string& failure_reason, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return false;
    }

    const char* sql = "UPDATE jobs SET state = ?1, exit_code = ?2, failure_reason = ?3 WHERE id = ?4;";
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }

    std::string state_str = std::string(to_string(state));
    sqlite3_bind_text(stmt.get(), 1, state_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 2, exit_code);
    sqlite3_bind_text(stmt.get(), 3, failure_reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, job_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }
    return true;
}

bool JobStore::update_job_worker(const std::string& job_id, int worker_id, pid_t pid, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return false;
    }

    const char* sql = "UPDATE jobs SET assigned_worker_id = ?1, worker_pid = ?2 WHERE id = ?3;";
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }

    sqlite3_bind_int(stmt.get(), 1, worker_id);
    sqlite3_bind_int(stmt.get(), 2, pid);
    sqlite3_bind_text(stmt.get(), 3, job_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }
    return true;
}

static Job parse_job_row(sqlite3_stmt* stmt) {
    Job job;
    job.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    job.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    job.executable_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

    const char* argv_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    if (argv_str) {
        auto parsed = nlohmann::json::parse(argv_str, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_array()) {
            job.argv = parsed.get<std::vector<std::string>>();
        }
    }

    job.priority = sqlite3_column_int(stmt, 4);
    const char* dev_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    job.resources.device = device_requirement_from_string(dev_str ? dev_str : "").value_or(DeviceRequirement::CpuOnly);
    job.resources.cpu_slots = static_cast<unsigned int>(sqlite3_column_int(stmt, 6));
    job.resources.ram_mb = static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
    job.resources.gpu_memory_mb = static_cast<uint64_t>(sqlite3_column_int64(stmt, 8));

    const char* state_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
    job.state = job_state_from_string(state_str ? state_str : "").value_or(JobState::Submitted);
    job.max_retries = sqlite3_column_int(stmt, 10);
    job.retry_count = sqlite3_column_int(stmt, 11);
    job.checkpointable = (sqlite3_column_int(stmt, 12) != 0);
    job.assigned_worker_id = sqlite3_column_int(stmt, 13);
    job.worker_pid = sqlite3_column_int(stmt, 14);
    job.exit_code = sqlite3_column_int(stmt, 15);

    const char* fail_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
    job.failure_reason = fail_str ? fail_str : "";
    const char* wf_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 17));
    job.workflow_id = wf_str ? wf_str : "";

    job.submitted_time = from_ms(sqlite3_column_int64(stmt, 18));
    job.enqueued_time = from_ms(sqlite3_column_int64(stmt, 19));
    job.started_time = from_ms(sqlite3_column_int64(stmt, 20));
    job.completed_time = from_ms(sqlite3_column_int64(stmt, 21));

    if (sqlite3_column_type(stmt, 22) != SQLITE_NULL) {
        job.deadline = from_ms(sqlite3_column_int64(stmt, 22));
    }

    return job;
}

std::optional<Job> JobStore::get_job(const std::string& job_id, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return std::nullopt;
    }

    const char* sql = 
        "SELECT id, name, executable_path, argv_json, priority, device, cpu_slots, ram_mb, gpu_memory_mb,"
        "       state, max_retries, retry_count, checkpointable, assigned_worker_id, worker_pid,"
        "       exit_code, failure_reason, workflow_id, submitted_time_ms, enqueued_time_ms,"
        "       started_time_ms, completed_time_ms, deadline_ms "
        "FROM jobs WHERE id = ?1;";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return std::nullopt;
    }

    sqlite3_bind_text(stmt.get(), 1, job_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        Job job = parse_job_row(stmt.get());

        // Load dependencies
        ScopedStmt dep_stmt;
        sqlite3_prepare_v2(impl_->db, "SELECT dependency_id FROM job_dependencies WHERE job_id = ?1;", -1, dep_stmt.receive(), nullptr);
        sqlite3_bind_text(dep_stmt.get(), 1, job_id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(dep_stmt.get()) == SQLITE_ROW) {
            job.dependencies.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(dep_stmt.get(), 0)));
        }

        // Load attempts
        ScopedStmt att_stmt;
        sqlite3_prepare_v2(impl_->db, 
            "SELECT attempt_id, attempt_number, worker_id, pid, start_time_ms, end_time_ms, exit_code, error_message "
            "FROM attempts WHERE job_id = ?1 ORDER BY attempt_number ASC;", -1, att_stmt.receive(), nullptr);
        sqlite3_bind_text(att_stmt.get(), 1, job_id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(att_stmt.get()) == SQLITE_ROW) {
            JobAttempt att;
            att.attempt_id = static_cast<uint64_t>(sqlite3_column_int64(att_stmt.get(), 0));
            att.job_id = job_id;
            att.attempt_number = sqlite3_column_int(att_stmt.get(), 1);
            att.worker_id = sqlite3_column_int(att_stmt.get(), 2);
            att.pid = sqlite3_column_int(att_stmt.get(), 3);
            att.start_time = from_ms(sqlite3_column_int64(att_stmt.get(), 4));
            att.end_time = from_ms(sqlite3_column_int64(att_stmt.get(), 5));
            att.exit_code = sqlite3_column_int(att_stmt.get(), 6);
            const char* err = reinterpret_cast<const char*>(sqlite3_column_text(att_stmt.get(), 7));
            att.error_message = err ? err : "";
            job.attempts.push_back(std::move(att));
        }

        return job;
    }

    return std::nullopt;
}

std::vector<Job> JobStore::get_all_jobs(std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<Job> jobs;
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return jobs;
    }

    const char* sql = 
        "SELECT id, name, executable_path, argv_json, priority, device, cpu_slots, ram_mb, gpu_memory_mb,"
        "       state, max_retries, retry_count, checkpointable, assigned_worker_id, worker_pid,"
        "       exit_code, failure_reason, workflow_id, submitted_time_ms, enqueued_time_ms,"
        "       started_time_ms, completed_time_ms, deadline_ms "
        "FROM jobs ORDER BY submitted_time_ms ASC;";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return jobs;
    }

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        jobs.push_back(parse_job_row(stmt.get()));
    }

    for (auto& j : jobs) {
        ScopedStmt dep_stmt;
        sqlite3_prepare_v2(impl_->db, "SELECT dependency_id FROM job_dependencies WHERE job_id = ?1;", -1, dep_stmt.receive(), nullptr);
        sqlite3_bind_text(dep_stmt.get(), 1, j.id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(dep_stmt.get()) == SQLITE_ROW) {
            j.dependencies.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(dep_stmt.get(), 0)));
        }
    }

    return jobs;
}

std::vector<Job> JobStore::get_jobs_by_state(JobState state, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<Job> jobs;
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return jobs;
    }

    const char* sql = 
        "SELECT id, name, executable_path, argv_json, priority, device, cpu_slots, ram_mb, gpu_memory_mb,"
        "       state, max_retries, retry_count, checkpointable, assigned_worker_id, worker_pid,"
        "       exit_code, failure_reason, workflow_id, submitted_time_ms, enqueued_time_ms,"
        "       started_time_ms, completed_time_ms, deadline_ms "
        "FROM jobs WHERE state = ?1 ORDER BY submitted_time_ms ASC;";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return jobs;
    }

    std::string state_str = std::string(to_string(state));
    sqlite3_bind_text(stmt.get(), 1, state_str.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        jobs.push_back(parse_job_row(stmt.get()));
    }

    for (auto& j : jobs) {
        ScopedStmt dep_stmt;
        sqlite3_prepare_v2(impl_->db, "SELECT dependency_id FROM job_dependencies WHERE job_id = ?1;", -1, dep_stmt.receive(), nullptr);
        sqlite3_bind_text(dep_stmt.get(), 1, j.id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(dep_stmt.get()) == SQLITE_ROW) {
            j.dependencies.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(dep_stmt.get(), 0)));
        }
    }

    return jobs;
}

bool JobStore::delete_job(const std::string& job_id, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return false;
    }

    const char* sql = "DELETE FROM jobs WHERE id = ?1;";
    ScopedStmt stmt;
    sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    sqlite3_bind_text(stmt.get(), 1, job_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }

    ScopedStmt dep_stmt;
    sqlite3_prepare_v2(impl_->db, "DELETE FROM job_dependencies WHERE job_id = ?1 OR dependency_id = ?1;", -1, dep_stmt.receive(), nullptr);
    sqlite3_bind_text(dep_stmt.get(), 1, job_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(dep_stmt.get());

    return true;
}

bool JobStore::purge_completed_jobs(std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return false;
    }

    const char* sql = 
        "DELETE FROM job_dependencies WHERE job_id IN (SELECT id FROM jobs WHERE state IN ('SUCCEEDED', 'FAILED', 'CANCELLED', 'Succeeded', 'Failed', 'Cancelled'))"
        "   OR dependency_id IN (SELECT id FROM jobs WHERE state IN ('SUCCEEDED', 'FAILED', 'CANCELLED', 'Succeeded', 'Failed', 'Cancelled'));"
        "DELETE FROM attempts WHERE job_id IN (SELECT id FROM jobs WHERE state IN ('SUCCEEDED', 'FAILED', 'CANCELLED', 'Succeeded', 'Failed', 'Cancelled'));"
        "DELETE FROM jobs WHERE state IN ('SUCCEEDED', 'FAILED', 'CANCELLED', 'Succeeded', 'Failed', 'Cancelled');";
    return impl_->exec_sql(sql, error_out);
}

bool JobStore::add_dependency(const std::string& job_id, const std::string& depends_on_job_id, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return false;
    }

    const char* sql = "INSERT OR IGNORE INTO job_dependencies (job_id, dependency_id) VALUES (?1, ?2);";
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }

    sqlite3_bind_text(stmt.get(), 1, job_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, depends_on_job_id.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }
    return true;
}

std::vector<std::string> JobStore::get_dependencies(const std::string& job_id, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<std::string> deps;
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return deps;
    }

    const char* sql = "SELECT dependency_id FROM job_dependencies WHERE job_id = ?1;";
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return deps;
    }

    sqlite3_bind_text(stmt.get(), 1, job_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        deps.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0)));
    }
    return deps;
}

bool JobStore::record_attempt(const JobAttempt& attempt, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return false;
    }

    const char* sql = 
        "INSERT INTO attempts ("
        "  job_id, attempt_number, worker_id, pid, start_time_ms, end_time_ms, exit_code, error_message"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }

    sqlite3_bind_text(stmt.get(), 1, attempt.job_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 2, attempt.attempt_number);
    sqlite3_bind_int(stmt.get(), 3, attempt.worker_id);
    sqlite3_bind_int(stmt.get(), 4, attempt.pid);
    sqlite3_bind_int64(stmt.get(), 5, to_ms(attempt.start_time));
    sqlite3_bind_int64(stmt.get(), 6, to_ms(attempt.end_time));
    sqlite3_bind_int(stmt.get(), 7, attempt.exit_code);
    sqlite3_bind_text(stmt.get(), 8, attempt.error_message.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }
    return true;
}

std::vector<JobAttempt> JobStore::get_attempts_for_job(const std::string& job_id, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<JobAttempt> attempts;
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return attempts;
    }

    const char* sql = 
        "SELECT attempt_id, job_id, attempt_number, worker_id, pid, start_time_ms, end_time_ms, exit_code, error_message "
        "FROM attempts WHERE job_id = ?1 ORDER BY attempt_number ASC;";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return attempts;
    }

    sqlite3_bind_text(stmt.get(), 1, job_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        JobAttempt a;
        a.attempt_id = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 0));
        a.job_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        a.attempt_number = sqlite3_column_int(stmt.get(), 2);
        a.worker_id = sqlite3_column_int(stmt.get(), 3);
        a.pid = sqlite3_column_int(stmt.get(), 4);
        a.start_time = from_ms(sqlite3_column_int64(stmt.get(), 5));
        a.end_time = from_ms(sqlite3_column_int64(stmt.get(), 6));
        a.exit_code = sqlite3_column_int(stmt.get(), 7);
        const char* msg = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 8));
        a.error_message = msg ? msg : "";
        attempts.push_back(std::move(a));
    }
    return attempts;
}

bool JobStore::save_worker(const WorkerInfo& worker, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return false;
    }

    const char* sql = 
        "INSERT INTO workers (id, pid, state, current_job_id, restart_count, last_heartbeat_ms, started_time_ms) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  pid = excluded.pid,"
        "  state = excluded.state,"
        "  current_job_id = excluded.current_job_id,"
        "  restart_count = excluded.restart_count,"
        "  last_heartbeat_ms = excluded.last_heartbeat_ms,"
        "  started_time_ms = excluded.started_time_ms;";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }

    std::string state_str = std::string(to_string(worker.state));
    sqlite3_bind_int(stmt.get(), 1, worker.id);
    sqlite3_bind_int(stmt.get(), 2, worker.pid);
    sqlite3_bind_text(stmt.get(), 3, state_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, worker.current_job_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 5, worker.restart_count);
    sqlite3_bind_int64(stmt.get(), 6, to_ms(worker.last_heartbeat));
    sqlite3_bind_int64(stmt.get(), 7, to_ms(worker.started_time));

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }
    return true;
}

std::optional<WorkerInfo> JobStore::get_worker(int worker_id, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return std::nullopt;
    }

    const char* sql = 
        "SELECT id, pid, state, current_job_id, restart_count, last_heartbeat_ms, started_time_ms "
        "FROM workers WHERE id = ?1;";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return std::nullopt;
    }

    sqlite3_bind_int(stmt.get(), 1, worker_id);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        WorkerInfo w;
        w.id = sqlite3_column_int(stmt.get(), 0);
        w.pid = sqlite3_column_int(stmt.get(), 1);
        const char* s_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        w.state = worker_state_from_string(s_str ? s_str : "").value_or(WorkerState::Idle);
        const char* j_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        w.current_job_id = j_str ? j_str : "";
        w.restart_count = sqlite3_column_int(stmt.get(), 4);
        w.last_heartbeat = from_ms(sqlite3_column_int64(stmt.get(), 5));
        w.started_time = from_ms(sqlite3_column_int64(stmt.get(), 6));
        return w;
    }
    return std::nullopt;
}

std::vector<WorkerInfo> JobStore::get_all_workers(std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<WorkerInfo> workers;
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return workers;
    }

    const char* sql = 
        "SELECT id, pid, state, current_job_id, restart_count, last_heartbeat_ms, started_time_ms "
        "FROM workers ORDER BY id ASC;";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return workers;
    }

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        WorkerInfo w;
        w.id = sqlite3_column_int(stmt.get(), 0);
        w.pid = sqlite3_column_int(stmt.get(), 1);
        const char* s_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        w.state = worker_state_from_string(s_str ? s_str : "").value_or(WorkerState::Idle);
        const char* j_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        w.current_job_id = j_str ? j_str : "";
        w.restart_count = sqlite3_column_int(stmt.get(), 4);
        w.last_heartbeat = from_ms(sqlite3_column_int64(stmt.get(), 5));
        w.started_time = from_ms(sqlite3_column_int64(stmt.get(), 6));
        workers.push_back(w);
    }
    return workers;
}

bool JobStore::record_event(const Event& event, std::uint64_t* out_event_id, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return false;
    }

    const char* sql = 
        "INSERT INTO events (timestamp_ms, type, job_id, worker_id, payload_json) "
        "VALUES (?1, ?2, ?3, ?4, ?5);";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }

    std::string type_str = std::string(to_string(event.type));
    sqlite3_bind_int64(stmt.get(), 1, to_ms(event.timestamp));
    sqlite3_bind_text(stmt.get(), 2, type_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, event.job_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 4, event.worker_id);
    sqlite3_bind_text(stmt.get(), 5, event.payload_json.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }

    if (out_event_id) {
        *out_event_id = static_cast<uint64_t>(sqlite3_last_insert_rowid(impl_->db));
    }
    return true;
}

std::vector<Event> JobStore::get_events(const std::string& job_id, size_t limit, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<Event> events;
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return events;
    }

    std::string sql;
    if (job_id.empty()) {
        sql = "SELECT id, timestamp_ms, type, job_id, worker_id, payload_json FROM events ORDER BY id ASC LIMIT ?1;";
    } else {
        sql = "SELECT id, timestamp_ms, type, job_id, worker_id, payload_json FROM events WHERE job_id = ?1 ORDER BY id ASC LIMIT ?2;";
    }

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return events;
    }

    if (job_id.empty()) {
        sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(limit));
    } else {
        sqlite3_bind_text(stmt.get(), 1, job_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(limit));
    }

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        Event e;
        e.id = static_cast<uint64_t>(sqlite3_column_int64(stmt.get(), 0));
        e.timestamp = from_ms(sqlite3_column_int64(stmt.get(), 1));
        const char* t_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        e.type = event_type_from_string(t_str ? t_str : "").value_or(EventType::JOB_SUBMITTED);
        const char* j_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        e.job_id = j_str ? j_str : "";
        e.worker_id = sqlite3_column_int(stmt.get(), 4);
        const char* p_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
        e.payload_json = p_str ? p_str : "";
        events.push_back(std::move(e));
    }

    return events;
}

bool JobStore::save_workflow(const WorkflowRecord& wf, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return false;
    }

    const char* sql = "INSERT INTO workflows (id, name, state, created_time_ms) VALUES (?1, ?2, ?3, ?4) "
                      "ON CONFLICT(id) DO UPDATE SET name = excluded.name, state = excluded.state;";
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }

    sqlite3_bind_text(stmt.get(), 1, wf.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, wf.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, wf.state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 4, to_ms(wf.created_time));

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }
    return true;
}

bool JobStore::update_workflow_state(const std::string& wf_id, const std::string& state, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return false;
    }

    const char* sql = "UPDATE workflows SET state = ?1 WHERE id = ?2;";
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }

    sqlite3_bind_text(stmt.get(), 1, state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, wf_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return false;
    }
    return true;
}

std::optional<WorkflowRecord> JobStore::get_workflow(const std::string& wf_id, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return std::nullopt;
    }

    const char* sql = "SELECT id, name, state, created_time_ms FROM workflows WHERE id = ?1;";
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return std::nullopt;
    }

    sqlite3_bind_text(stmt.get(), 1, wf_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        WorkflowRecord wf;
        wf.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        wf.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        wf.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        wf.created_time = from_ms(sqlite3_column_int64(stmt.get(), 3));
        return wf;
    }

    return std::nullopt;
}

std::vector<WorkflowRecord> JobStore::get_all_workflows(std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<WorkflowRecord> list;
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return list;
    }

    const char* sql = "SELECT id, name, state, created_time_ms FROM workflows ORDER BY created_time_ms DESC;";
    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return list;
    }

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        WorkflowRecord wf;
        wf.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        wf.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        wf.state = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        wf.created_time = from_ms(sqlite3_column_int64(stmt.get(), 3));
        list.push_back(std::move(wf));
    }

    return list;
}

std::vector<Job> JobStore::get_jobs_by_workflow(const std::string& wf_id, std::string* error_out) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<Job> jobs;
    if (!impl_->db) {
        if (error_out) *error_out = "Database is not open";
        return jobs;
    }

    const char* sql = 
        "SELECT id, name, executable_path, argv_json, priority, device, cpu_slots, ram_mb, gpu_memory_mb,"
        "       state, max_retries, retry_count, checkpointable, assigned_worker_id, worker_pid,"
        "       exit_code, failure_reason, workflow_id, submitted_time_ms, enqueued_time_ms,"
        "       started_time_ms, completed_time_ms, deadline_ms "
        "FROM jobs WHERE workflow_id = ?1 ORDER BY submitted_time_ms ASC;";

    ScopedStmt stmt;
    int rc = sqlite3_prepare_v2(impl_->db, sql, -1, stmt.receive(), nullptr);
    if (rc != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(impl_->db);
        return jobs;
    }

    sqlite3_bind_text(stmt.get(), 1, wf_id.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        jobs.push_back(parse_job_row(stmt.get()));
    }

    for (auto& j : jobs) {
        ScopedStmt dep_stmt;
        sqlite3_prepare_v2(impl_->db, "SELECT dependency_id FROM job_dependencies WHERE job_id = ?1;", -1, dep_stmt.receive(), nullptr);
        sqlite3_bind_text(dep_stmt.get(), 1, j.id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(dep_stmt.get()) == SQLITE_ROW) {
            j.dependencies.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(dep_stmt.get(), 0)));
        }
    }

    return jobs;
}

} // namespace astra
