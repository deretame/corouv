#include <corouv/runtime.h>
#include <corouv/sql/orm.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

corouv::Task<void> sqlite_orm_case(corouv::UvExecutor& ex) {
    corouv::sql::Connection conn(ex);
    co_await conn.connect("sqlite:///:memory:");

    co_await conn.execute(
        "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, age INTEGER)");
    co_await conn.execute(
        "CREATE TABLE profiles(id INTEGER PRIMARY KEY, user_id INTEGER UNIQUE, bio TEXT)");
    co_await conn.execute(
        "CREATE TABLE posts(id INTEGER PRIMARY KEY, user_id INTEGER, title TEXT)");
    co_await conn.execute(
        "CREATE TABLE tags(id INTEGER PRIMARY KEY, name TEXT)");
    co_await conn.execute(
        "CREATE TABLE post_tags(post_id INTEGER, tag_id INTEGER, "
        "PRIMARY KEY(post_id, tag_id))");

    corouv::sql::orm::TableMetaRuntime users;
    users.table = "users";
    users.columns = {"id", "name", "age"};
    users.primary_key = "id";

    corouv::sql::orm::TableMetaRuntime profiles;
    profiles.table = "profiles";
    profiles.columns = {"id", "user_id", "bio"};
    profiles.primary_key = "id";

    corouv::sql::orm::TableMetaRuntime posts;
    posts.table = "posts";
    posts.columns = {"id", "user_id", "title"};
    posts.primary_key = "id";

    corouv::sql::orm::TableMetaRuntime tags;
    tags.table = "tags";
    tags.columns = {"id", "name"};
    tags.primary_key = "id";

    corouv::sql::orm::TableMetaRuntime post_tags;
    post_tags.table = "post_tags";
    post_tags.columns = {"post_id", "tag_id"};

    co_await corouv::sql::orm::insert(
        conn, users, {std::int64_t{1}, std::string("alice"), std::int64_t{30}});
    co_await corouv::sql::orm::insert(
        conn, users, {std::int64_t{2}, std::string("bob"), std::int64_t{25}});

    co_await corouv::sql::orm::insert(
        conn, profiles,
        {std::int64_t{100}, std::int64_t{1}, std::string("alice-bio")});
    co_await corouv::sql::orm::insert(
        conn, profiles,
        {std::int64_t{101}, std::int64_t{2}, std::string("bob-bio")});

    co_await corouv::sql::orm::insert(
        conn, posts,
        {std::int64_t{10}, std::int64_t{1}, std::string("alice-post-1")});
    co_await corouv::sql::orm::insert(
        conn, posts,
        {std::int64_t{11}, std::int64_t{1}, std::string("alice-post-2")});
    co_await corouv::sql::orm::insert(
        conn, posts,
        {std::int64_t{12}, std::int64_t{2}, std::string("bob-post-1")});

    co_await corouv::sql::orm::insert(conn, tags,
                                      {std::int64_t{1}, std::string("cpp")});
    co_await corouv::sql::orm::insert(conn, tags,
                                      {std::int64_t{2}, std::string("db")});
    co_await corouv::sql::orm::insert(conn, tags,
                                      {std::int64_t{3}, std::string("orm")});

    co_await corouv::sql::orm::insert(conn, post_tags,
                                      {std::int64_t{10}, std::int64_t{1}});
    co_await corouv::sql::orm::insert(conn, post_tags,
                                      {std::int64_t{10}, std::int64_t{2}});
    co_await corouv::sql::orm::insert(conn, post_tags,
                                      {std::int64_t{11}, std::int64_t{3}});

    corouv::sql::orm::SelectOptions adults_opt;
    adults_opt.where = corouv::sql::orm::where_ge("users.age", std::int64_t{26});
    adults_opt.order_by.push_back(
        corouv::sql::orm::OrderBy{"users.id", corouv::sql::orm::SortDirection::Asc});
    const auto adults = co_await corouv::sql::orm::select(
        conn, users, std::move(adults_opt));
    require(adults.rows.size() == 1, "sql_orm_test: select/where failed");

    corouv::sql::orm::SelectOptions composed_opt;
    composed_opt.where = corouv::sql::orm::where_and(
        {corouv::sql::orm::where_in(
             "users.id", corouv::sql::Params{std::int64_t{1}, std::int64_t{2}}),
         corouv::sql::orm::where_is_not_null("users.name")});
    const auto composed = co_await corouv::sql::orm::select(
        conn, users, std::move(composed_opt));
    require(composed.rows.size() == 2,
            "sql_orm_test: where_and/where_in failed");

    corouv::sql::orm::OneToOneRelation user_profile;
    user_profile.target_table = "profiles";
    user_profile.target_alias = "pr";
    user_profile.source_key = "id";
    user_profile.target_key = "user_id";

    corouv::sql::orm::SelectOptions one_to_one_opt;
    one_to_one_opt.select_expr = "users.id, users.name, pr.bio";
    one_to_one_opt.where = corouv::sql::orm::where_eq("users.id", std::int64_t{1});
    const auto one_to_one = co_await corouv::sql::orm::join_one_to_one(
        conn, users, user_profile, std::move(one_to_one_opt), "users");
    require(one_to_one.rows.size() == 1,
            "sql_orm_test: one_to_one relation failed");

    corouv::sql::orm::OneToManyRelation user_posts;
    user_posts.target_table = "posts";
    user_posts.target_alias = "p";
    user_posts.source_key = "id";
    user_posts.target_key = "user_id";

    corouv::sql::orm::SelectOptions one_to_many_opt;
    one_to_many_opt.select_expr = "users.id, p.id, p.title";
    one_to_many_opt.where = corouv::sql::orm::where_eq("users.id", std::int64_t{1});
    one_to_many_opt.order_by.push_back(
        corouv::sql::orm::OrderBy{"p.id", corouv::sql::orm::SortDirection::Asc});
    const auto one_to_many = co_await corouv::sql::orm::join_one_to_many(
        conn, users, user_posts, std::move(one_to_many_opt), "users");
    require(one_to_many.rows.size() == 2,
            "sql_orm_test: one_to_many relation failed");

    corouv::sql::orm::ManyToManyRelation post_to_tags;
    post_to_tags.junction_table = "post_tags";
    post_to_tags.junction_alias = "pt";
    post_to_tags.target_table = "tags";
    post_to_tags.target_alias = "tg";
    post_to_tags.source_key = "id";
    post_to_tags.junction_source_key = "post_id";
    post_to_tags.junction_target_key = "tag_id";
    post_to_tags.target_key = "id";

    corouv::sql::orm::SelectOptions many_to_many_opt;
    many_to_many_opt.select_expr = "posts.id, posts.title, tg.name";
    many_to_many_opt.where =
        corouv::sql::orm::where_eq("posts.id", std::int64_t{10});
    many_to_many_opt.order_by.push_back(
        corouv::sql::orm::OrderBy{"tg.id", corouv::sql::orm::SortDirection::Asc});
    const auto many_to_many = co_await corouv::sql::orm::join_many_to_many(
        conn, posts, post_to_tags, std::move(many_to_many_opt), "posts");
    require(many_to_many.rows.size() == 2,
            "sql_orm_test: many_to_many relation failed");

    const auto updated_rows = co_await corouv::sql::orm::update_where(
        conn, users, std::vector<std::string>{"age"},
        corouv::sql::Params{std::int64_t{31}},
        corouv::sql::orm::where_eq("id", std::int64_t{1}));
    require(updated_rows == 1, "sql_orm_test: update_where failed");

    const auto user_one =
        co_await corouv::sql::orm::find_by_pk(conn, users, std::int64_t{1});
    require(user_one.rows.size() == 1,
            "sql_orm_test: find_by_pk after update failed");

    const auto deleted_rows = co_await corouv::sql::orm::delete_where(
        conn, posts, corouv::sql::orm::where_eq("id", std::int64_t{12}));
    require(deleted_rows == 1, "sql_orm_test: delete_where failed");

    const auto should_be_empty =
        co_await corouv::sql::orm::find_all(conn, posts, "id = ?", {std::int64_t{12}});
    require(should_be_empty.rows.empty(),
            "sql_orm_test: delete verification failed");

    std::vector<corouv::sql::orm::JoinSpec> legacy_joins;
    corouv::sql::orm::JoinSpec legacy_join;
    legacy_join.kind = "INNER";
    legacy_join.table = "posts";
    legacy_join.alias = "lp";
    legacy_join.on = "lp.user_id = users.id";
    legacy_joins.push_back(std::move(legacy_join));

    const auto legacy_joined = co_await corouv::sql::orm::join_query(
        conn, users, legacy_joins, "users.id = ?", {std::int64_t{1}},
        "users.name, lp.title");
    require(legacy_joined.rows.size() == 2,
            "sql_orm_test: legacy join_query failed");

    co_await conn.close();
}

}  // namespace

int main() {
    corouv::Runtime rt;
    rt.run(sqlite_orm_case(rt.executor()));
    return 0;
}
