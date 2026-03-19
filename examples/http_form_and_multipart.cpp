#include <corouv/http.h>
#include <corouv/multipart.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>

#include <iostream>
#include <string>

corouv::Task<void> http_form_and_multipart_demo(corouv::UvExecutor& ex) {
    corouv::http::Server server(
        ex,
        [](corouv::http::Request request)
            -> corouv::Task<corouv::http::Response> {
            corouv::http::Response response;
            response.headers.push_back({"content-type", "text/plain"});

            if (request.method == "POST" && request.target == "/form") {
                const auto fields =
                    corouv::http::parse_form_urlencoded_request(request);
                const auto name = corouv::http::find_form_value(fields, "name");
                const auto tags = corouv::http::find_form_values(fields, "tag");

                response.body = "form name=";
                response.body.append(name.has_value() ? std::string(*name) : "<none>");
                response.body.append(" tags=");
                response.body.append(std::to_string(tags.size()));
                co_return response;
            }

            if (request.method == "POST" && request.target == "/upload") {
                const auto form = corouv::multipart::parse_request(request);
                response.body = "multipart parts=" + std::to_string(form.parts.size());
                if (!form.parts.empty()) {
                    response.body.append(" first=");
                    response.body.append(form.parts.front().name);
                }
                co_return response;
            }

            response.status = 404;
            response.reason = corouv::http::reason_phrase(404);
            response.body = "not found";
            response.keep_alive = false;
            co_return response;
        },
        corouv::http::ServerOptions{
            .host = "127.0.0.1",
            .port = 0,
        });

    co_await server.listen();
    std::cout << "[http_form_multipart] listening on " << server.host() << ":"
              << server.port() << "\n";

    auto group = co_await corouv::make_task_group();
    (void)group.spawn(server.serve());

    corouv::http::Client client(ex);
    co_await client.connect(server.host(), server.port());

    corouv::http::FormFields form_fields;
    form_fields.push_back({"name", "alice"});
    form_fields.push_back({"tag", "cpp"});
    form_fields.push_back({"tag", "coroutine"});

    corouv::http::Request form_request;
    form_request.method = "POST";
    form_request.target = "/form";
    form_request.headers.push_back(
        {"content-type", "application/x-www-form-urlencoded"});
    form_request.body = corouv::http::serialize_form_urlencoded(form_fields);

    const auto form_response = co_await client.request(form_request);
    std::cout << "[http_form_multipart] form status=" << form_response.status
              << " body=" << form_response.body << "\n";

    corouv::multipart::FormData upload_form;
    corouv::multipart::Part title_part;
    title_part.name = "title";
    title_part.body = "demo";
    upload_form.parts.push_back(std::move(title_part));

    corouv::multipart::Part file_part;
    file_part.name = "file";
    file_part.filename = "hello.txt";
    file_part.content_type = "text/plain";
    file_part.body = "hello multipart";
    upload_form.parts.push_back(std::move(file_part));

    const std::string boundary = "corouv-demo-boundary";
    corouv::http::Request upload_request;
    upload_request.method = "POST";
    upload_request.target = "/upload";
    upload_request.headers.push_back(
        {"content-type", corouv::multipart::build_content_type(boundary)});
    upload_request.body = corouv::multipart::serialize(upload_form, boundary);

    const auto upload_response = co_await client.request(upload_request);
    std::cout << "[http_form_multipart] upload status=" << upload_response.status
              << " body=" << upload_response.body << "\n";

    client.close();
    server.close();
    co_await group.wait();
}

int main() {
    corouv::Runtime rt;
    rt.run(http_form_and_multipart_demo(rt.executor()));
    return 0;
}
