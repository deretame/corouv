#include <corouv/runtime.h>
#include <corouv/web.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string header_value(const corouv::web::Headers& headers,
                         std::string_view name) {
    for (const auto& entry : headers) {
        if (entry.name == name) {
            return entry.value;
        }
    }
    return {};
}

}  // namespace

corouv::Task<void> web_router_params_mount_middleware_case() {
    using corouv::web::Request;
    using corouv::web::RequestContext;
    using corouv::web::Response;
    using corouv::web::Router;

    Router api;
    api.use([](RequestContext ctx, corouv::web::Next next)
                -> corouv::Task<Response> {
        if (!ctx.has_param("userId")) {
            throw std::runtime_error("web_router_test: middleware lost userId");
        }
        auto response = co_await next(std::move(ctx));
        response.headers.push_back({"x-api-mw", "1"});
        co_return response;
    });

    api.get("/posts/:postId", [](RequestContext ctx) -> corouv::Task<Response> {
        const auto user_id = ctx.param("userId");
        const auto post_id = ctx.param("postId");
        if (!user_id.has_value() || !post_id.has_value()) {
            throw std::runtime_error(
                "web_router_test: expected userId and postId params");
        }

        Response response;
        response.body = std::string(*user_id) + ":" + std::string(*post_id) +
                        " " + std::string(ctx.path());
        co_return response;
    });

    api.not_found([](RequestContext ctx) -> corouv::Task<Response> {
        Response response;
        response.status = 404;
        response.reason = corouv::web::reason_phrase(404);
        response.body = "api-missing " + ctx.request.target;
        response.keep_alive = false;
        co_return response;
    });

    Router app;
    app.use([](RequestContext ctx, corouv::web::Next next)
                -> corouv::Task<Response> {
        auto response = co_await next(std::move(ctx));
        response.headers.push_back({"x-app-mw", "1"});
        co_return response;
    });

    app.get("/users/:userId/profile",
            [](RequestContext ctx) -> corouv::Task<Response> {
                const auto user_id = ctx.param("userId");
                if (!user_id.has_value()) {
                    throw std::runtime_error(
                        "web_router_test: expected userId on direct route");
                }

                Response response;
                response.body = "profile " + std::string(*user_id);
                co_return response;
            });

    app.mount("/users/:userId", api);

    Request profile_request;
    profile_request.method = "GET";
    profile_request.target = "/users/alice/profile";

    const auto profile_response = co_await app.handle(std::move(profile_request));
    if (profile_response.status != 200 ||
        profile_response.body != "profile alice") {
        throw std::runtime_error("web_router_test: direct param route mismatch");
    }
    if (header_value(profile_response.headers, "x-app-mw") != "1") {
        throw std::runtime_error(
            "web_router_test: app middleware did not run on direct route");
    }

    Request post_request;
    post_request.method = "GET";
    post_request.target = "/users/bob/posts/42?expand=1";

    const auto post_response = co_await app.handle(std::move(post_request));
    if (post_response.status != 200 ||
        post_response.body != "bob:42 /posts/42") {
        throw std::runtime_error("web_router_test: mount route mismatch status=" +
                                 std::to_string(post_response.status) +
                                 " body=" + post_response.body);
    }
    if (header_value(post_response.headers, "x-app-mw") != "1" ||
        header_value(post_response.headers, "x-api-mw") != "1") {
        throw std::runtime_error(
            "web_router_test: middleware chain mismatch on mount");
    }

    Request missing_request;
    missing_request.method = "GET";
    missing_request.target = "/users/bob/missing?x=1";

    const auto missing_response = co_await app.handle(std::move(missing_request));
    if (missing_response.status != 404 ||
        missing_response.body != "api-missing /missing?x=1") {
        throw std::runtime_error(
            "web_router_test: mounted not_found target mismatch");
    }
    if (header_value(missing_response.headers, "x-app-mw") != "1") {
        throw std::runtime_error(
            "web_router_test: app middleware did not wrap mounted not_found");
    }
}

corouv::Task<void> web_request_context_parse_helpers_case() {
    using corouv::web::Request;
    using corouv::web::RequestContext;
    using corouv::web::Response;
    using corouv::web::Router;

    Router app;
    app.post("/form", [](RequestContext ctx) -> corouv::Task<Response> {
        const auto fields = ctx.parse_form();
        const auto name = corouv::http::find_form_value(fields, "name");
        if (!name.has_value()) {
            throw std::runtime_error("web_router_test: parse_form missing name");
        }

        Response response;
        response.body = "form " + std::string(*name);
        co_return response;
    });

    app.post("/multipart", [](RequestContext ctx) -> corouv::Task<Response> {
        const auto form = ctx.parse_multipart();
        if (form.parts.size() != 2 || form.parts[0].name != "title" ||
            form.parts[0].body != "hello" || form.parts[1].name != "upload" ||
            !form.parts[1].filename.has_value() ||
            *form.parts[1].filename != "a.txt") {
            throw std::runtime_error(
                "web_router_test: parse_multipart content mismatch");
        }

        Response response;
        response.body = "multipart " + form.parts[1].body;
        co_return response;
    });

    Request form_request;
    form_request.method = "POST";
    form_request.target = "/form";
    form_request.headers.push_back(
        {"Content-Type", "application/x-www-form-urlencoded"});
    form_request.body = "name=alice&x=1";

    const auto form_response = co_await app.handle(std::move(form_request));
    if (form_response.status != 200 || form_response.body != "form alice") {
        throw std::runtime_error("web_router_test: parse_form helper mismatch");
    }

    corouv::multipart::FormData form;
    corouv::multipart::Part title;
    title.name = "title";
    title.body = "hello";
    form.parts.push_back(std::move(title));

    corouv::multipart::Part upload;
    upload.name = "upload";
    upload.filename = "a.txt";
    upload.body = "payload";
    form.parts.push_back(std::move(upload));

    const std::string boundary = "web-helper-boundary";
    Request multipart_request;
    multipart_request.method = "POST";
    multipart_request.target = "/multipart";
    multipart_request.headers.push_back(
        {"Content-Type", corouv::multipart::build_content_type(boundary)});
    multipart_request.body = corouv::multipart::serialize(form, boundary);

    const auto multipart_response =
        co_await app.handle(std::move(multipart_request));
    if (multipart_response.status != 200 ||
        multipart_response.body != "multipart payload") {
        throw std::runtime_error(
            "web_router_test: parse_multipart helper mismatch");
    }
}

corouv::Task<void> web_router_method_not_allowed_case() {
    using corouv::web::Request;
    using corouv::web::Response;
    using corouv::web::Router;

    Router app;
    app.get("/resource", [](corouv::web::RequestContext)
                           -> corouv::Task<Response> {
        Response response;
        response.body = "get";
        co_return response;
    });
    app.post("/resource", [](corouv::web::RequestContext)
                            -> corouv::Task<Response> {
        Response response;
        response.body = "post";
        co_return response;
    });
    app.any("/open", [](corouv::web::RequestContext) -> corouv::Task<Response> {
        Response response;
        response.body = "open";
        co_return response;
    });

    Request put_request;
    put_request.method = "PUT";
    put_request.target = "/resource";

    const auto put_response = co_await app.handle(std::move(put_request));
    const auto allow = header_value(put_response.headers, "Allow");
    if (put_response.status != 405 || allow != "GET, POST") {
        throw std::runtime_error("web_router_test: 405 allow mismatch");
    }

    Request any_request;
    any_request.method = "DELETE";
    any_request.target = "/open";

    const auto any_response = co_await app.handle(std::move(any_request));
    if (any_response.status != 200 || any_response.body != "open") {
        throw std::runtime_error("web_router_test: any route mismatch");
    }

    Request missing_request;
    missing_request.method = "PUT";
    missing_request.target = "/missing";

    const auto missing_response = co_await app.handle(std::move(missing_request));
    if (missing_response.status != 404) {
        throw std::runtime_error("web_router_test: missing route should be 404");
    }
}

int main() {
    corouv::Runtime rt;
    rt.run(web_router_params_mount_middleware_case());
    rt.run(web_request_context_parse_helpers_case());
    rt.run(web_router_method_not_allowed_case());
    return 0;
}
