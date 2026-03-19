#pragma once

// SQL annotation macros used by code generators.
//
// Priority:
// 1) Future C++ reflection-style attributes when available.
// 2) clang::annotate fallback for libclang-based tooling.
// 3) Empty macros on unsupported compilers.

#if defined(__has_cpp_attribute)

#if defined(COROUV_SQL_FORCE_CLANG_ANNOTATE) && defined(__clang__) && \
    __has_cpp_attribute(clang::annotate)
#define SQL_PK __attribute__((annotate("pk")))
#define SQL_COL(name) __attribute__((annotate("col:" name)))
#define SQL_TABLE(name) __attribute__((annotate("table:" name)))

#elif __has_cpp_attribute(reflect)
#define SQL_PK [[reflect::pk]]
#define SQL_COL(name) [[reflect::column(name)]]
#define SQL_TABLE(name) [[reflect::table(name)]]

#elif defined(__clang__) && __has_cpp_attribute(clang::annotate)
#define SQL_PK __attribute__((annotate("pk")))
#define SQL_COL(name) __attribute__((annotate("col:" name)))
#define SQL_TABLE(name) __attribute__((annotate("table:" name)))

#else
#define SQL_PK
#define SQL_COL(name)
#define SQL_TABLE(name)
#endif

#else
#define SQL_PK
#define SQL_COL(name)
#define SQL_TABLE(name)
#endif
