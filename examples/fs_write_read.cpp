#include <corouv/file.h>
#include <corouv/runtime.h>

#include <iostream>
#include <stdexcept>
#include <string>

corouv::Task<void> write_then_read(corouv::UvExecutor& ex, std::string path) {
    const std::string payload = "hello from corouv fs\n";

    auto file = co_await corouv::io::open(
        ex, path, UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_RDWR, 0644);
    co_await file.write_all(payload);
    file.rewind();

    const std::string out = co_await file.read_all();
    co_await file.close();

    if (out != payload) {
        throw std::runtime_error("fs_write_read: content mismatch");
    }

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
