#include <corouv/file.h>
#include <corouv/filesystem.h>
#include <corouv/runtime.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

corouv::Task<void> filesystem_case(corouv::UvExecutor& ex,
                                   const std::string& root) {
    const std::string nested = root + "/alpha/beta";
    const std::string file_path = nested + "/note.txt";
    const std::string renamed_path = nested + "/renamed.txt";
    const std::string copied_path = nested + "/copied.txt";
    const std::string symlink_path = nested + "/copied.link";

    co_await corouv::io::create_directories(ex, nested);
    if (!(co_await corouv::io::exists(ex, nested))) {
        throw std::runtime_error("filesystem_test: nested directory missing");
    }

    auto file = co_await corouv::io::open(
        ex, file_path, UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_RDWR, 0644);
    co_await file.write_all("hello-tree");
    co_await file.close();

    const auto st = co_await corouv::io::stat(ex, file_path);
    if (!corouv::io::is_regular_file(st) || st.st_size != 10) {
        throw std::runtime_error("filesystem_test: stat mismatch");
    }

    co_await corouv::io::rename(ex, file_path, renamed_path);
    if ((co_await corouv::io::exists(ex, file_path)) ||
        !(co_await corouv::io::exists(ex, renamed_path))) {
        throw std::runtime_error("filesystem_test: rename mismatch");
    }

    co_await corouv::io::copy_file(ex, renamed_path, copied_path);
    co_await corouv::io::symlink(ex, copied_path, symlink_path);

    const auto link_target = co_await corouv::io::readlink(ex, symlink_path);
    if (link_target != copied_path) {
        throw std::runtime_error("filesystem_test: readlink mismatch");
    }

    const auto real = co_await corouv::io::realpath(ex, symlink_path);
    if (real != copied_path) {
        throw std::runtime_error("filesystem_test: realpath mismatch");
    }

    auto root_dir = co_await corouv::io::open_directory(ex, root);
    auto root_entries = co_await root_dir.read_all();
    co_await root_dir.close();

    if (root_entries.size() != 1 || root_entries.front().name != "alpha" ||
        !root_entries.front().is_directory()) {
        throw std::runtime_error("filesystem_test: root directory listing mismatch");
    }

    auto nested_entries = co_await corouv::io::read_directory(ex, nested);
    std::vector<std::string> names;
    names.reserve(nested_entries.size());
    for (const auto& entry : nested_entries) {
        names.push_back(entry.name);
    }
    std::sort(names.begin(), names.end());
    if (names != std::vector<std::string>{"copied.link", "copied.txt", "renamed.txt"}) {
        throw std::runtime_error("filesystem_test: nested directory listing mismatch");
    }

    co_await corouv::io::remove_file(ex, symlink_path);
    co_await corouv::io::remove_file(ex, copied_path);
    co_await corouv::io::remove_file(ex, renamed_path);
    co_await corouv::io::remove_directory(ex, nested);
    co_await corouv::io::remove_directory(ex, root + "/alpha");
    co_await corouv::io::remove_directory(ex, root);
}

int main() {
    const std::string root = "/tmp/corouv_filesystem_test";

    try {
        corouv::Runtime cleanup_rt;
        cleanup_rt.run(corouv::io::remove_all(cleanup_rt.executor(), root));
    } catch (...) {
    }

    try {
        corouv::Runtime rt;
        rt.run(filesystem_case(rt.executor(), root));
    } catch (...) {
        try {
            corouv::Runtime cleanup_rt;
            cleanup_rt.run(corouv::io::remove_all(cleanup_rt.executor(), root));
        } catch (...) {
        }
        throw;
    }

    try {
        corouv::Runtime cleanup_rt;
        cleanup_rt.run(corouv::io::remove_all(cleanup_rt.executor(), root));
    } catch (...) {
    }
    return 0;
}
