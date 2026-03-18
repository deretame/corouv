#include <corouv/fs.h>
#include <corouv/filesystem.h>
#include <corouv/runtime.h>

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>

corouv::Task<void> fs_roundtrip(corouv::UvExecutor& ex, const std::string& path) {
    static constexpr const char kPayload[] = "corouv-fs-roundtrip\n";

    uv_file file = co_await corouv::fs::Open(
        ex.loop(), path, UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_WRONLY, 0644);

    const std::span<const char> buf(kPayload, sizeof(kPayload) - 1);
    const ssize_t n = co_await corouv::fs::Write(ex.loop(), file, buf, 0);
    co_await corouv::fs::Close(ex.loop(), file);

    if (n != static_cast<ssize_t>(buf.size())) {
        throw std::runtime_error("fs_test: short write");
    }

    const std::string read_back = co_await corouv::fs::read_file(ex, path);
    if (read_back != std::string(kPayload, sizeof(kPayload) - 1)) {
        throw std::runtime_error("fs_test: content mismatch");
    }
}

int main() {
    const std::string path = "/tmp/corouv_fs_test.txt";

    try {
        corouv::Runtime rt;
        rt.run(fs_roundtrip(rt.executor(), path));
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
