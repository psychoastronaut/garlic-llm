// Host tests for chat/api_proto.c (the OpenAI-compatible API's protocol part).
// Build and run: gcc -std=c99 -Wall -Wextra -g -fsanitize=address,undefined -I chat tests/api_proto_test.c chat/api_proto.c -o build/api_proto_test && build/api_proto_test
#include <stdio.h>
#include <string.h>

#include "api_proto.h"

static int fails, checks;
#define CHECK(c)                                                              \
    do {                                                                      \
        checks++;                                                             \
        if (!(c)) {                                                           \
            fails++;                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);               \
        }                                                                     \
    } while (0)

static void test_http(void) {
    HttpRequest r;
    const char* post = "POST /v1/chat/completions?x=1 HTTP/1.1\r\nHost: ps4\r\nContent-Type: application/json\r\n"
                       "content-length: 13\r\nAuthorization: Bearer abc\r\n\r\n{\"a\":\"b\"}    ";
    CHECK(api_http_parse(post, strlen(post), &r) == 1);
    CHECK(!strcmp(r.method, "POST"));
    CHECK(!strcmp(r.path, "/v1/chat/completions"));
    CHECK(r.content_length == 13);
    CHECK(r.body_len == 13 && !memcmp(r.body, "{\"a\":\"b\"}    ", 13));
    CHECK(!strcmp(r.auth, "Bearer abc"));

    const char* partial = "POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc";
    CHECK(api_http_parse(partial, strlen(partial), &r) == 0);
    const char* head_only = "GET /v1/models HTTP/1.1\r\nHost: a\r\n";
    CHECK(api_http_parse(head_only, strlen(head_only), &r) == 0);
    const char* get = "GET /v1/models HTTP/1.1\r\nHost: a\r\n\r\n";
    CHECK(api_http_parse(get, strlen(get), &r) == 1);
    CHECK(!strcmp(r.path, "/v1/models") && r.content_length == 0);
    const char* bad = "GET\r\n\r\n";
    CHECK(api_http_parse(bad, strlen(bad), &r) == -1);
    const char* badcl = "POST / HTTP/1.1\r\nContent-Length: nope\r\n\r\n";
    CHECK(api_http_parse(badcl, strlen(badcl), &r) == -1);
    const char* bigcl = "POST / HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n";
    CHECK(api_http_parse(bigcl, strlen(bigcl), &r) == -1);
}

static void test_json(void) {
    JNode pool[64];
    const char* doc = "{ \"a\" : [1, 2.5, -3e2, true, false, null], \"s\": \"x\\\"y\\\\\\n\\u00e9\\ud83d\\ude00\" }";
    int root = json_parse(doc, strlen(doc), pool, 64);
    CHECK(root >= 0);
    int a = json_get(pool, root, "a");
    CHECK(a >= 0 && pool[a].type == J_ARR && pool[a].count == 6);
    int c = pool[a].first;
    CHECK(pool[c].type == J_NUM && pool[c].num == 1);
    c = pool[c].next;
    CHECK(pool[c].num == 2.5);
    c = pool[c].next;
    CHECK(pool[c].num == -300);
    c = pool[c].next;
    CHECK(pool[c].type == J_BOOL && pool[c].b == 1);
    int s = json_get(pool, root, "s");
    Sb out;
    sb_init(&out);
    json_str_sb(pool, s, &out);
    CHECK(out.n == strlen("x\"y\\\n\xc3\xa9\xf0\x9f\x98\x80"));
    CHECK(out.p && !memcmp(out.p, "x\"y\\\n\xc3\xa9\xf0\x9f\x98\x80", out.n));
    sb_free(&out);

    CHECK(json_parse("{\"a\":}", 6, pool, 64) == -1);
    CHECK(json_parse("[1,2", 4, pool, 64) == -1);
    CHECK(json_parse("{} x", 4, pool, 64) == -1);
    CHECK(json_parse("[1,2]", 5, pool, 2) == -1);   // pool too small
    CHECK(json_get(pool, root, "missing") == -1);
}

static void test_chat(void) {
    JNode pool[API_MAX_NODES];
    ChatRequest req;
    const char* err = NULL;
    const char* body =
        "{\"model\":\"x\",\"messages\":[{\"role\":\"system\",\"content\":\"Be brief.\"},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Hi \"},{\"type\":\"text\",\"text\":\"there\"}]}],"
        "\"temperature\":0.2,\"max_tokens\":64,\"stream\":true,\"seed\":7}";
    CHECK(api_chat_parse(body, strlen(body), pool, API_MAX_NODES, &req, &err) == 0);
    CHECK(req.n_messages == 2 && req.stream == 1 && req.max_tokens == 64 && req.has_seed && req.seed == 7);
    CHECK(req.temperature > 0.19 && req.temperature < 0.21);
    char role[16];
    Sb content;
    sb_init(&content);
    int m = pool[req.messages].first;
    CHECK(api_message(pool, m, role, sizeof(role), &content) == 0 && !strcmp(role, "system"));
    CHECK(content.n == 9 && !memcmp(content.p, "Be brief.", 9));
    sb_free(&content);
    sb_init(&content);
    m = pool[m].next;
    CHECK(api_message(pool, m, role, sizeof(role), &content) == 0 && !strcmp(role, "user"));
    CHECK(content.n == 8 && !memcmp(content.p, "Hi there", 8));
    sb_free(&content);

    const char* defaults = "{\"messages\":[{\"role\":\"user\",\"content\":\"yo\"}]}";
    CHECK(api_chat_parse(defaults, strlen(defaults), pool, API_MAX_NODES, &req, &err) == 0);
    CHECK(req.max_tokens == 512 && req.stream == 0 && !req.has_seed);

    const char* bad[] = {
        "[]",
        "{\"messages\":[]}",
        "{\"messages\":[1]}",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"n\":2}",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"temperature\":3}",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"max_tokens\":0}",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"stream\":\"yes\"}",
        "{\"messages\":[{\"role\":\"user\"",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(api_chat_parse(bad[i], strlen(bad[i]), pool, API_MAX_NODES, &req, &err) == -1);
        CHECK(err != NULL);
    }
}

// ---- tool calling ---------------------------------------------------------------------------------------------------
static const char* TOOLS =
    "[{\"type\":\"function\",\"function\":{\"name\":\"t\",\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"n\":{\"type\":\"number\"},\"b\":{\"type\":\"boolean\"},\"o\":{\"type\":\"object\"},\"a\":{\"type\":\"array\"},"
    "\"s\":{\"type\":\"string\"},\"u\":{}}}}}]";

// the arguments of the one call in reply (the schema above), or "(none)" if it holds no call
static const char* args_of(const char* reply) {
    static JNode pool[256];
    static char out[1024];
    int tools = json_parse(TOOLS, strlen(TOOLS), pool, 256);
    ApiCall calls[API_MAX_CALLS];
    size_t te;
    int n = api_parse_tool_calls(reply, strlen(reply), pool, tools, calls, API_MAX_CALLS, 1, &te);
    snprintf(out, sizeof(out), "%s", n ? calls[0].args.p : "(none)");
    api_calls_free(calls, n);
    return out;
}

static void test_tool_calls(void) {
    JNode pool[256];
    int tools = json_parse(TOOLS, strlen(TOOLS), pool, 256);
    ApiCall calls[API_MAX_CALLS];
    size_t te = 0;
    const char* r = "Let me look.\n\n<tool_call>\n<function=read>\n<parameter=path>\n/tmp/a b.txt\n</parameter>\n"
                    "<parameter=limit>\n20\n</parameter>\n</function>\n</tool_call>\n"
                    "<tool_call>\n<function=ls>\n</function>\n</tool_call>";
    int n = api_parse_tool_calls(r, strlen(r), pool, tools, calls, API_MAX_CALLS, 5, &te);
    CHECK(n == 2 && te == 14 && !memcmp(r, "Let me look.\n\n", 14));
    CHECK(!strcmp(calls[0].name.p, "read") && !strcmp(calls[0].args.p, "{\"path\":\"/tmp/a b.txt\",\"limit\":20}"));
    CHECK(!strcmp(calls[1].name.p, "ls") && !strcmp(calls[1].args.p, "{}"));
    CHECK(calls[0].id == 5 && calls[1].id == 6);
    Sb s;
    sb_init(&s);
    api_json_completion_calls(&s, "c", "m", 1, "Let me look.", 12, calls, n, 10, 20);
    CHECK(strstr(s.p, "\"content\":\"Let me look.\",\"tool_calls\":[{\"id\":\"call_5\",\"type\":\"function\",\"function\":"
                      "{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"/tmp/a b.txt\\\",\\\"limit\\\":20}\"}},"
                      "{\"id\":\"call_6\"") != NULL);
    CHECK(strstr(s.p, "\"finish_reason\":\"tool_calls\"") != NULL);
    sb_free(&s);
    sb_init(&s);
    api_json_completion_calls(&s, "c", "m", 1, "", 0, calls, n, 10, 20);
    CHECK(strstr(s.p, "\"content\":null,\"tool_calls\":[") != NULL);
    sb_free(&s);
    sb_init(&s);
    api_json_chunk_calls(&s, "c", "m", 1, calls, 2);
    CHECK(strstr(s.p, "\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_5\"") != NULL);
    CHECK(strstr(s.p, ",{\"index\":1,\"id\":\"call_6\"") != NULL && strstr(s.p, "\"finish_reason\":null") != NULL);
    sb_free(&s);
    api_calls_free(calls, n);

    // the types of the values: by the schema where there is one, else by what they look like
    const char* H = "<tool_call>\n<function=t>\n";
    char buf[512];
#define ARGS(params) (snprintf(buf, sizeof(buf), "%s%s</function>\n</tool_call>", H, params), args_of(buf))
    CHECK(!strcmp(ARGS("<parameter=n>\n42\n</parameter>\n<parameter=b>\ntrue\n</parameter>\n"), "{\"n\":42,\"b\":true}"));
    CHECK(!strcmp(ARGS("<parameter=n>\n-1.5e3\n</parameter>\n"), "{\"n\":-1.5e3}"));
    CHECK(!strcmp(ARGS("<parameter=n>\n+5\n</parameter>\n"), "{\"n\":\"+5\"}"));       // not JSON: a string
    CHECK(!strcmp(ARGS("<parameter=n>\n007\n</parameter>\n"), "{\"n\":\"007\"}"));
    CHECK(!strcmp(ARGS("<parameter=b>\nFalse\n</parameter>\n"), "{\"b\":false}"));    // (the template writes Python's)
    CHECK(!strcmp(ARGS("<parameter=b>\nmaybe\n</parameter>\n"), "{\"b\":\"maybe\"}"));
    CHECK(!strcmp(ARGS("<parameter=o>\n{\"a\": [1, 2.5, null], \"k\": \"v\\\"\"}\n</parameter>\n"),
                  "{\"o\":{\"a\":[1,2.5,null],\"k\":\"v\\\"\"}}"));
    CHECK(!strcmp(ARGS("<parameter=a>\n[1, \"x\"]\n</parameter>\n"), "{\"a\":[1,\"x\"]}"));
    CHECK(!strcmp(ARGS("<parameter=o>\n{broken\n</parameter>\n"), "{\"o\":\"{broken\"}"));
    CHECK(!strcmp(ARGS("<parameter=s>\n123\n</parameter>\n<parameter=u>\n123\n</parameter>\n"), "{\"s\":\"123\",\"u\":123}"));
    CHECK(!strcmp(ARGS("<parameter=s>\ntrue\n</parameter>\n<parameter=u>\nplain words\n</parameter>\n"),
                  "{\"s\":\"true\",\"u\":\"plain words\"}"));
    // strings: quotes, backslashes, line breaks (kept, only the template's own break at each end goes), unicode
    CHECK(!strcmp(ARGS("<parameter=s>\nsay \"hi\"\\n\n  indented\n\ncaf\xc3\xa9 \xf0\x9f\x98\x80\n</parameter>\n"),
                  "{\"s\":\"say \\\"hi\\\"\\\\n\\n  indented\\n\\ncaf\xc3\xa9 \xf0\x9f\x98\x80\"}"));
    CHECK(!strcmp(ARGS("<parameter=s>\n\n</parameter>\n"), "{\"s\":\"\"}"));
    CHECK(!strcmp(ARGS("<parameter=s>\nno line breaks</parameter>\n"), "{\"s\":\"no line breaks\"}"));
    CHECK(!strcmp(ARGS("<parameter=s>\nx\n</parameter>\n<parameter=s>\ny\n</parameter>\n"), "{\"s\":\"x\",\"s\":\"y\"}"));

    // not a call: no tag, cut off, no function, a parameter that never closes, text inside, an empty name
    CHECK(!strcmp(args_of("just an answer"), "(none)"));
    CHECK(!strcmp(args_of("<tool_call>\n<function=t>\n<parameter=s>\nabc"), "(none)"));
    CHECK(!strcmp(args_of("<tool_call>\n<function=t>\n<parameter=s>\nabc\n</parameter>\n"), "(none)"));   // no </function>
    CHECK(!strcmp(args_of("<tool_call>\nhello\n</tool_call>"), "(none)"));
    CHECK(!strcmp(args_of("<tool_call>\n<function=t>\n<parameter=s>\nabc\n</function>\n</tool_call>"), "(none)"));
    CHECK(!strcmp(args_of("<tool_call>\n<function=>\n</function>\n</tool_call>"), "(none)"));
    CHECK(!strcmp(args_of("<tool_call>\n<function=t>\nstray text\n</function>\n</tool_call>"), "(none)"));
    CHECK(!strcmp(args_of("<tool_call><function=t></function></tool_call>"), "{}"));
    CHECK(!strcmp(args_of("<tool_call>\n<function=t>\n</function>"), "{}"));   // the turn ended right after the call
    CHECK(!strcmp(args_of(""), "(none)"));
    // a good call followed by a cut one: the good one counts
    n = api_parse_tool_calls("<tool_call><function=a></function></tool_call><tool_call><function=b><parameter=x>\n1",
                             84, pool, tools, calls, API_MAX_CALLS, 1, &te);
    CHECK(n == 1 && te == 0 && !strcmp(calls[0].name.p, "a"));
    api_calls_free(calls, n);
    // more calls than the limit
    char many[2048] = "";
    for (int i = 0; i < 12; i++) strcat(many, "<tool_call><function=f></function></tool_call>");
    n = api_parse_tool_calls(many, strlen(many), pool, -1, calls, API_MAX_CALLS, 1, &te);
    CHECK(n == API_MAX_CALLS);
    api_calls_free(calls, n);

    // a name or a value as large as the reply allows
    static char big[40000];
    int k = snprintf(big, sizeof(big), "<tool_call><function=");
    memset(big + k, 'N', 20000), k += 20000;
    k += snprintf(big + k, sizeof(big) - k, "><parameter=p>\n");
    memset(big + k, 'v', 15000), k += 15000;
    snprintf(big + k, sizeof(big) - k, "\n</parameter></function></tool_call>");
    n = api_parse_tool_calls(big, strlen(big), pool, tools, calls, API_MAX_CALLS, 1, &te);
    CHECK(n == 1 && calls[0].name.n == 20000 && calls[0].args.n == 15000 + 8);
    api_calls_free(calls, n);

    // tags split across tokens
    CHECK(api_tag_find("ab<tool_call>c", 14) == 2 && api_tag_find("ab<tool_cal", 11) == -1);
    CHECK(api_tag_holdback("ab<tool_cal", 11) == 9 && api_tag_holdback("ab<", 3) == 1 && api_tag_holdback("ab", 2) == 0);
    CHECK(api_tag_holdback("a<b", 3) == 0 && api_tag_holdback("<tool_call", 10) == 10);
}

static void test_tool_request(void) {
    static JNode pool[API_MAX_NODES];
    ChatRequest req;
    const char* err = NULL;
    const char* tool = "{\"type\":\"function\",\"function\":{\"name\":\"f\",\"parameters\":{}}}";
    char body[1024];
    snprintf(body, sizeof(body), "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":[%s]%%s}", tool);
    char b2[1100];
#define TOOLREQ(extra) (snprintf(b2, sizeof(b2), body, extra), api_chat_parse(b2, strlen(b2), pool, API_MAX_NODES, &req, &err))
    CHECK(TOOLREQ("") == 0 && req.tools >= 0 && !req.tool_required);
    CHECK(TOOLREQ(",\"tool_choice\":\"auto\"") == 0 && req.tools >= 0 && !req.tool_required);
    CHECK(TOOLREQ(",\"tool_choice\":\"none\"") == 0 && req.tools < 0);
    CHECK(TOOLREQ(",\"tool_choice\":\"required\"") == 0 && req.tools >= 0 && req.tool_required);
    CHECK(TOOLREQ(",\"tool_choice\":{\"type\":\"function\",\"function\":{\"name\":\"f\"}}") == 0 && req.tool_required);
    CHECK(TOOLREQ(",\"tool_choice\":null") == 0 && req.tools >= 0);
    CHECK(TOOLREQ(",\"tool_choice\":\"maybe\"") == -1);
    CHECK(TOOLREQ(",\"tool_choice\":3") == -1);
    const char* empty = "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":[],\"tool_choice\":\"required\"}";
    CHECK(api_chat_parse(empty, strlen(empty), pool, API_MAX_NODES, &req, &err) == 0 && req.tools < 0 && !req.tool_required);
    const char* nul = "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":null}";
    CHECK(api_chat_parse(nul, strlen(nul), pool, API_MAX_NODES, &req, &err) == 0 && req.tools < 0);
    const char* bad[] = {
        "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":{}}",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":[1]}",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":[{\"type\":\"function\"}]}",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":[{\"function\":{\"name\":\"\"}}]}",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"a\"}],\"tools\":[{\"function\":{\"name\":3}}]}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(api_chat_parse(bad[i], strlen(bad[i]), pool, API_MAX_NODES, &req, &err) == -1);

    // the tools block, and a message that carries calls
    const char* doc = "{\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"f\",\"description\":\"d \\\"q\\\"\\n\\u00e9\","
                      "\"parameters\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\",\"default\":1.5}},"
                      "\"required\":[]}}}],\"m\":{\"role\":\"assistant\",\"content\":\"hi\",\"tool_calls\":["
                      "{\"id\":\"c\",\"type\":\"function\",\"function\":{\"name\":\"f\",\"arguments\":\"{\\\"x\\\":2,\\\"y\\\":\\\"s\\\\n\\\","
                      "\\\"z\\\":null,\\\"w\\\":[1,\\\"a\\\"]}\"}}]}}";
    int root = json_parse(doc, strlen(doc), pool, API_MAX_NODES);
    Sb s;
    sb_init(&s);
    api_tools_prompt(pool, json_get(pool, root, "tools"), 0, &s);
    CHECK(strstr(s.p, "<tools>\n{\"type\": \"function\", \"function\": {\"name\": \"f\", \"description\": \"d \\\"q\\\"\\n\xc3\xa9\", "
                      "\"parameters\": {\"type\": \"object\", \"properties\": {\"x\": {\"type\": \"number\", \"default\": 1.5}}, "
                      "\"required\": []}}}\n</tools>\n\nIf you choose") != NULL);
    sb_free(&s);
    sb_init(&s);
    CHECK(api_assistant_calls(pool, json_get(pool, root, "m"), 1, &s) == 0);
    CHECK(!strcmp(s.p, "\n\n<tool_call>\n<function=f>\n<parameter=x>\n2\n</parameter>\n<parameter=y>\ns\n\n</parameter>\n"
                       "<parameter=z>\nNone\n</parameter>\n<parameter=w>\n[1, \"a\"]\n</parameter>\n</function>\n</tool_call>"));
    sb_free(&s);
    sb_init(&s);
    const char* plain = "{\"role\":\"assistant\",\"content\":\"x\"}";
    root = json_parse(plain, strlen(plain), pool, API_MAX_NODES);
    CHECK(api_assistant_calls(pool, root, 1, &s) == 0 && s.n == 0);
    sb_free(&s);
}

static void test_writers(void) {
    Sb s;
    sb_init(&s);
    api_json_completion(&s, "chatcmpl-1", "Qwen3.5-9B", 1700000000, "Hola \"mundo\"\n", 13, "stop", 5, 4);
    CHECK(s.p && strstr(s.p, "\"content\":\"Hola \\\"mundo\\\"\\n\"") != NULL);
    CHECK(strstr(s.p, "\"total_tokens\":9") != NULL);
    CHECK(strstr(s.p, "\"finish_reason\":\"stop\"") != NULL);
    sb_free(&s);

    sb_init(&s);
    api_json_chunk(&s, "c", "m", 1, "assistant", "", 0, NULL);
    CHECK(!strcmp(s.p, "{\"id\":\"c\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"m\","
                       "\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\"},\"finish_reason\":null}]}"));
    sb_free(&s);

    sb_init(&s);
    api_json_error(&s, "bad \x01 input", "invalid_request_error", "bad_body");
    CHECK(!strcmp(s.p, "{\"error\":{\"message\":\"bad \\u0001 input\",\"type\":\"invalid_request_error\","
                       "\"code\":\"bad_body\"}}"));
    sb_free(&s);

    sb_init(&s);
    api_json_models(&s, "Qwen3.5-9B", 0);
    CHECK(strstr(s.p, "\"data\":[{\"id\":\"Qwen3.5-9B\",\"object\":\"model\"") != NULL);
    sb_free(&s);
}

int main(void) {
    test_http();
    test_json();
    test_chat();
    test_writers();
    test_tool_calls();
    test_tool_request();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
