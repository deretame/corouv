#include <corouv/fs.h>
#include <corouv/runtime.h>

#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

corouv::Task<void> write_then_read(corouv::UvExecutor& ex, std::string path) {
    const std::string payload = "hello from corouv fs\n";

    uv_file file = co_await corouv::fs::Open(
        ex.loop(), path, UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_WRONLY, 0644);

    const std::span<const char> buf(payload.data(), payload.size());
    const ssize_t n = co_await corouv::fs::Write(ex.loop(), file, buf, 0);
    co_await corouv::fs::Close(ex.loop(), file);

    if (n != static_cast<ssize_t>(payload.size())) {
        throw std::runtime_error("fs_write_read: short write");
    }

    const std::string out = co_await corouv::fs::read_file(ex, path);
    std::cout << "[fs_write_read] path=" << path << " bytes=" << out.size()
              << "\n";
    std::cout << "[fs_write_read] content=" << out;
}

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "/tmp/corouv-fs-example.txt";

    corouv::Runtime rt;
    rt.run(write_then_read(rt.executor(), std::move(path)));
    return 0;
}
