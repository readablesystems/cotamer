// examples/jsond.cc
//    A small in-memory notes REST API. Showcases JSON parsing of request
//    bodies (POST/PUT) and JSON generation of responses (objects, arrays,
//    nested values) using nlohmann::json.
//
//    Endpoints:
//      GET /notes          list all notes
//      POST /notes         create note; body = {"title": "...", "body": "..."}
//      GET /notes/<id>     fetch a note
//      PUT /notes/<id>     replace a note
//      DELETE /notes/<id>  delete a note
//      GET /stats          aggregate counts and recent ids
//
//    Try it with examples/jsond-tester, or with curl:
//      curl -s -XPOST localhost:11112/notes \
//           -H 'Content-Type: application/json' \
//           -d '{"title":"hi","body":"first"}'
//      curl -s localhost:11112/notes
//      curl -s localhost:11112/stats

#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#include <nlohmann/json.hpp>
#include <charconv>
#include <map>
#include <unistd.h>

namespace cot = cotamer;
using json = nlohmann::json;


namespace {

struct note {
    uint64_t id;
    std::string title;
    std::string body;
};

// to_json overloads let us write `json(n)` and `j["note"] = n` directly.
[[maybe_unused]] void to_json(json& j, const note& n) {
    j = json{{"id", n.id}, {"title", n.title}, {"body", n.body}};
}


class notes_db {
public:
    note& create(std::string title, std::string body) {
        uint64_t id = next_id_++;
        auto [it, _] = notes_.emplace(id, note{id, std::move(title), std::move(body)});
        return it->second;
    }
    note* find(uint64_t id) {
        auto it = notes_.find(id);
        return it == notes_.end() ? nullptr : &it->second;
    }
    bool erase(uint64_t id) {
        return notes_.erase(id) != 0;
    }
    const std::map<uint64_t, note>& all() const {
        return notes_;
    }
private:
    std::map<uint64_t, note> notes_;
    uint64_t next_id_ = 1;
};


static cot::http_message make_response(unsigned status, const json& body) {
    // The compact form `res.status_code(status).body(body)` would also work:
    // http_message has a `body(const nlohmann::json&)` overload that calls
    // `dump()` and sets Content-Type. We do it by hand here to show the
    // moving parts and to pretty-print the JSON.
    cot::http_message res;
    std::string s = body.dump(2);
    s.push_back('\n');
    res.status_code(status)
       .header("Content-Type", "application/json")
       .body(std::move(s));
    return res;
}

static cot::http_message error_response(unsigned status, std::string_view msg) {
    return make_response(status, json{{"error", msg}, {"status", status}});
}

// Parse "/notes/123" -> 123. The input is `req.path()`, so any query
// string or fragment is already stripped.
static bool parse_note_id(std::string_view path, uint64_t& out) {
    constexpr std::string_view prefix = "/notes/";
    if (!path.starts_with(prefix)) {
        return false;
    }
    auto rest = path.substr(prefix.size());
    if (rest.empty()) {
        return false;
    }
    auto [end, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), out);
    return ec == std::errc{} && end == rest.data() + rest.size();
}


static cot::http_message handle(const cot::http_message& req, notes_db& db) {
    auto method = req.method();
    auto path = req.path();

    // Collection endpoints.
    if (path == "/notes") {
        if (method == HTTP_GET) {
            json arr = json::array();
            for (auto& [_, n] : db.all()) {
                arr.push_back(n);
            }
            return make_response(200, json{{"notes", std::move(arr)}});
        }
        if (method == HTTP_POST) {
            json body;
            try {
                body = json::parse(req.body());
            } catch (const json::parse_error& e) {
                return error_response(400, std::string("invalid JSON: ") + e.what());
            }
            if (!body.is_object()
                || !body.contains("title") || !body["title"].is_string()
                || !body.contains("body")  || !body["body"].is_string()) {
                return error_response(400, "expected {\"title\": string, \"body\": string}");
            }
            note& n = db.create(body["title"].get<std::string>(),
                                body["body"].get<std::string>());
            return make_response(201, json(n));
        }
        return error_response(405, "method not allowed on /notes");
    }

    // Per-id endpoints.
    uint64_t id;
    if (parse_note_id(path, id)) {
        note* n = db.find(id);
        if (method == HTTP_GET) {
            if (!n) {
                return error_response(404, "no such note");
            }
            return make_response(200, json(*n));
        }
        if (method == HTTP_PUT) {
            if (!n) {
                return error_response(404, "no such note");
            }
            json body;
            try {
                body = json::parse(req.body());
            } catch (const json::parse_error& e) {
                return error_response(400, std::string("invalid JSON: ") + e.what());
            }
            // Each field is optional on PUT; missing fields are left alone.
            if (auto it = body.find("title"); it != body.end()) {
                if (!it->is_string()) {
                    return error_response(400, "title must be a string");
                }
                n->title = it->get<std::string>();
            }
            if (auto it = body.find("body"); it != body.end()) {
                if (!it->is_string()) {
                    return error_response(400, "body must be a string");
                }
                n->body = it->get<std::string>();
            }
            return make_response(200, json(*n));
        }
        if (method == HTTP_DELETE) {
            if (!db.erase(id)) {
                return error_response(404, "no such note");
            }
            return make_response(200, json{{"deleted", id}});
        }
        return error_response(405, "method not allowed on /notes/<id>");
    }

    // Aggregate stats endpoint, demonstrating nested JSON generation.
    if (path == "/stats" && method == HTTP_GET) {
        size_t total_bytes = 0;
        json recent = json::array();
        for (auto it = db.all().rbegin(); it != db.all().rend(); ++it) {
            total_bytes += it->second.title.size() + it->second.body.size();
            if (recent.size() < 5) {
                recent.push_back(it->second.id);
            }
        }
        return make_response(200, json{
            {"count", db.all().size()},
            {"total_text_bytes", total_bytes},
            {"recent_ids", std::move(recent)},
            {"server", {{"name", "jsond"}, {"version", "0.1"}}}
        });
    }

    return error_response(404, "no such route");
}


cot::task<> run_one(cot::fd cfd, notes_db& db) {
    cot::http_parser hp(std::move(cfd), HTTP_REQUEST);
    while (true) {
        auto req = co_await hp.receive();
        if (!hp.ok()) {
            break;
        }
        std::cerr << req.method_name() << " " << req.url() << '\n';
        auto res = handle(req, db);
        co_await hp.send(std::move(res));
        if (!hp.should_keep_alive()) {
            break;
        }
    }
}

cot::task<> start(std::string address, notes_db& db) {
    auto lfd = co_await cot::tcp_listen(address);
    while (true) {
        run_one(co_await cot::accept(lfd), db).detach();
    }
}

void usage() {
    fprintf(stderr, "Usage: jsond [-p PORT]\n");
}

} // namespace

int main(int argc, char* argv[]) {
    int opt;
    int port = 11112;
    while ((opt = getopt(argc, argv, "hp:")) != -1) {
        switch (opt) {
        case 'h':
            usage();
            exit(0);
        case 'p':
            port = strtol(optarg, 0, 0);
            break;
        case '?':
        default:
            usage();
            exit(1);
        }
    }
    if (optind != argc) {
        usage();
        exit(1);
    }

    notes_db db;
    cot::set_clock(cot::clock::real_time);
    cot::task<> t = start(std::format("localhost:{}", port), db);
    cot::loop();
}
