#include <corouv/file.h>
#include <corouv/filesystem.h>
#include <corouv/runtime.h>

#include <span>
#include <stdexcept>
#include <string>

corouv::Task<void> io_file_case(corouv::UvExecutor& ex, const std::string& path) {
    auto file = co_await corouv::io::open(
        ex, path, UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_RDWR, 0644);

    co_await file.write_all("abc");
    if (file.position() != 3) {
        throw std::runtime_error("io_file_test: position after write mismatch");
    }

    const auto size = co_await file.size();
    if (size != 3) {
        throw std::runtime_error("io_file_test: size mismatch");
    }

    const auto patched = co_await file.write_at(1, "Z");
    if (patched != 1) {
        throw std::runtime_error("io_file_test: write_at mismatch");
    }

    file.rewind();

    char head[2] = {};
    const auto first_n = co_await file.read_some(std::span<char>(head, 2));
    if (first_n != 2 || std::string_view(head, first_n) != "aZ") {
        throw std::runtime_error("io_file_test: read_some mismatch");
    }

    const auto tail = co_await file.read_all();
    if (tail != "c") {
        throw std::runtime_error("io_file_test: read_all mismatch");
    }

    co_await file.datasync();
    co_await file.close();

    auto reader = co_await corouv::io::open(ex, path, 0);
    const auto all = co_await reader.read_all();
    if (all != "aZc") {
        throw std::runtime_error("io_file_test: reopened content mismatch");
    }
    co_await reader.close();
}

int main() {
    const std::string path = "/tmp/corouv_io_file_test.txt";

    try {
        corouv::Runtime rt;
        rt.run(io_file_case(rt.executor(), path));
    } catch (...) {
        try {
            corouv::Runtime cleanup_rt;
            cleanup_rt.run(corouv::io::remove_all(cleanup_rt.executor(), path));
        } catch (...) {
        }
        throw;
    }

    try {
        corouv::Runtime cleanup_rt;
        cleanup_rt.run(corouv::io::remove_all(cleanup_rt.executor(), path));
    } catch (...) {
    }
    return 0;
}
