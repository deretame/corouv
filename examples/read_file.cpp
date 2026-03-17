#include <corouv/fs.h>
#include <corouv/loop.h>
#include <corouv/sync_wait.h>

#include <async_simple/coro/Lazy.h>

#include <iostream>
#include <string>

async_simple::coro::Lazy<void> demo_read_file(corouv::UvExecutor& ex,
                                              std::string path) {
    const auto s = co_await corouv::fs::read_file(ex, path);
    std::cout << "[read_file] path=" << path << " bytes=" << s.size() << "\n";
}

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "CMakeLists.txt";

    corouv::Loop loop;
    corouv::UvExecutor ex(loop.raw());

    corouv::run(ex, demo_read_file(ex, path));

    ex.shutdown();
    loop.run(UV_RUN_DEFAULT);

    return 0;
}
