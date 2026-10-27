#include "astra/version.hpp"

#include <iostream>
#include <cassert>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <fcntl.h>

void test_version_constants() {
    assert(astra::ASTRA_VERSION_MAJOR == 0);
    assert(!astra::ASTRA_VERSION.empty());
    assert(!astra::ASTRA_BANNER.empty());
    std::cout << "[PASS] Version constants check\n";
}

void test_sqlite_in_memory() {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(":memory:", &db);
    assert(rc == SQLITE_OK);
    assert(db != nullptr);

    char* err_msg = nullptr;
    const char* sql = "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT); "
                      "INSERT INTO test (id, name) VALUES (1, 'astra');";
    rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
    assert(rc == SQLITE_OK);

    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, "SELECT name FROM test WHERE id = 1;", -1, &stmt, nullptr);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(stmt);
    assert(rc == SQLITE_ROW);
    std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    assert(name == "astra");

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    std::cout << "[PASS] SQLite3 in-memory database test\n";
}

void test_json_serialization() {
    nlohmann::json j;
    j["name"] = "matrix_mul_cpu";
    j["cpu_slots"] = 2;
    j["ram_mb"] = 256;
    j["device"] = "CpuOnly";

    std::string serialized = j.dump();
    auto parsed = nlohmann::json::parse(serialized);
    assert(parsed["name"] == "matrix_mul_cpu");
    assert(parsed["cpu_slots"] == 2);
    assert(parsed["ram_mb"] == 256);
    std::cout << "[PASS] nlohmann/json serialization and parsing\n";
}

void test_posix_pipe_creation() {
    int fds[2];
    int rc = pipe(fds);
    assert(rc == 0);

    const char msg[] = "ASTRA_IPC_TEST";
    ssize_t written = write(fds[1], msg, sizeof(msg));
    assert(written == sizeof(msg));

    char buf[32] = {0};
    ssize_t read_bytes = read(fds[0], buf, sizeof(buf));
    assert(read_bytes == sizeof(msg));
    assert(std::string_view(buf) == "ASTRA_IPC_TEST");

    close(fds[0]);
    close(fds[1]);
    std::cout << "[PASS] POSIX pipe creation and communication\n";
}

int main() {
    std::cout << "Running ASTRA Phase 0 Sanity Tests...\n";
    test_version_constants();
    test_sqlite_in_memory();
    test_json_serialization();
    test_posix_pipe_creation();
    std::cout << "All Phase 0 tests passed successfully!\n";
    return 0;
}
