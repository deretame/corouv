#include <corouv/file.h>
#include <corouv/filesystem.h>
#include <corouv/runtime.h>

#include <iostream>
#include <string>

corouv::Task<void> filesystem_demo(corouv::UvExecutor& ex, std::string root) {
    const std::string nested = root + "/docs";
    const std::string file_path = nested + "/hello.txt";
    const std::string renamed = nested + "/hello-renamed.txt";
    const std::string copied = nested + "/hello-copy.txt";
    const std::string link = nested + "/hello-link.txt";

    co_await corouv::io::create_directories(ex, nested);

    auto file = co_await corouv::io::open(
        ex, file_path, UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_RDWR, 0644);
    co_await file.write_all("filesystem demo\n");
    co_await file.close();

    co_await corouv::io::rename(ex, file_path, renamed);
    co_await corouv::io::copy_file(ex, renamed, copied);
    co_await corouv::io::symlink(ex, copied, link);

    const auto entries = co_await corouv::io::read_directory(ex, nested);
    std::cout << "[filesystem] entries in " << nested << ":\n";
    for (const auto& entry : entries) {
        std::cout << "  - " << entry.name
                  << (entry.is_directory() ? " [dir]" : " [file]") << "\n";
    }

    std::cout << "[filesystem] readlink=" << co_await corouv::io::readlink(ex, link)
              << "\n";
    std::cout << "[filesystem] realpath=" << co_await corouv::io::realpath(ex, link)
              << "\n";

    auto reader = co_await corouv::io::open(ex, renamed, 0);
    const auto content = co_await reader.read_all();
    co_await reader.close();
    std::cout << "[filesystem] content=" << content;

    co_await corouv::io::remove_file(ex, link);
    co_await corouv::io::remove_file(ex, copied);
    co_await corouv::io::remove_file(ex, renamed);
    co_await corouv::io::remove_directory(ex, nested);
    co_await corouv::io::remove_directory(ex, root);
}

int main(int argc, char** argv) {
    std::string root =
        argc > 1 ? argv[1] : "/tmp/corouv-filesystem-example";

    corouv::Runtime rt;
    try {
        rt.run(corouv::io::remove_all(rt.executor(), root));
    } catch (...) {
    }
    rt.run(filesystem_demo(rt.executor(), root));
    try {
        rt.run(corouv::io::remove_all(rt.executor(), root));
    } catch (...) {
    }
    return 0;
}
