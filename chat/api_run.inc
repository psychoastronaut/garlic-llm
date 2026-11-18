// OpenAI-compatible API jobs (api.c queues them; included in main.c, so it sees the engine and the conversation).
// A job runs on the model between the chat's turns: the request's messages are the whole conversation, so the
// conversation on screen is cleared, and the reply is written to the connection (whole, or as server-sent events).
// The reply is generated as the chat generates its own: several tokens per step with the MTP head where the app has
// it (GARLIC_SPEC), and Circle on the controller stops it. What the screen shows meanwhile is main.c's part
// (api_chat_begin / _reply / _step / _end).

static uint32_t api_ids[16384];   // the prompt of a job (byte-level tokens: at most one per byte of text)
#define API_MAX_TEXT 15000        // bytes of message text a request may carry (the prompt must fit api_ids)

// How many bytes at the start of p (n) are whole UTF-8 characters (the rest waits for the next token).
static size_t api_utf8_whole(const uint8_t* p, size_t n) {
    size_t i = n;
    for (int back = 1; back <= 4 && i > 0; back++) {
        i--;
        uint8_t c = p[i];
        if ((c & 0xC0) == 0x80) continue;   // a continuation byte: keep looking for its lead byte
        size_t need = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        return back < (int)need ? i : n;
    }
    return n;
}

// one server-sent event; -1 if the client went away
static int api_send_event(int fd, const char* json) {
    Sb s;
    sb_init(&s);
    sb_str(&s, "data: ");
    sb_str(&s, json);
    sb_str(&s, "\n\n");
    int r = s.p && !s.oom ? api_send(fd, s.p, s.n) : -1;
    sb_free(&s);
    return r;
}

#define API_IDS 16384
#define API_MAX_TEXT_TOOLS 60000   // (with tools: more bytes may come in, the prompt still has to fit api_ids)

static int api_put(int n, uint32_t id) {
    if (n < 0 || n >= API_IDS - 1) return -1;
    api_ids[n] = id;
    return n + 1;
}

// Text to ids. In a tools prompt the four tags the template writes around calls and results are single tokens
// (added tokens of the vocabulary, not spelled out), as Hugging Face's tokenizer reads them in the text.
static int api_text(int n, const char* z, int tags) {
    static const struct {
        const char* s;
        uint32_t id;
    } tg[] = {{"<tool_call>", TOOL_CALL}, {"</tool_call>", TOOL_CALL_END}, {"<tool_response>", TOOL_RESP},
              {"</tool_response>", TOOL_RESP_END}};
    while (n >= 0 && *z) {
        const char* at = NULL;
        int which = -1;
        for (int i = 0; tags && i < 4; i++) {
            const char* f = strstr(z, tg[i].s);
            if (f && (!at || f < at)) at = f, which = i;
        }
        if (!at) {
            n = encode_into(api_ids, n, API_IDS - 1, z);
            return n >= API_IDS - 1 ? -1 : n;
        }
        if (at > z) {
            char* part = malloc(at - z + 1);
            if (!part) return -1;
            memcpy(part, z, at - z);
            part[at - z] = 0;
            n = encode_into(api_ids, n, API_IDS - 1, part);
            free(part);
            if (n >= API_IDS - 1) return -1;
        }
        n = api_put(n, tg[which].id);
        z = at + strlen(tg[which].s);
    }
    return n;
}

// Builds the prompt: each message as <|im_start|>role\ncontent<|im_end|>\n, then the assistant's turn with the empty
// thought block (as the chat does, enable_thinking=false). Returns the number of ids, or -1 if it does not fit.
// With tools (or messages that carry tool calls or results) it follows Qwen3.5's chat template for them: the tools
// go in the system message, calls follow the assistant's text, results are <tool_response> blocks of a user turn,
// contents are trimmed, and the turns after the last real question start with the empty thought block.
static int api_prompt(const JNode* pool, const ChatRequest* cr) {
    int n = 0, tm = cr->tools >= 0, last_query = -1, idx = 0;
    size_t total = 0;
    for (int m = pool[cr->messages].first; m >= 0; m = pool[m].next, idx++) {   // is it a tools conversation?
        Sb content;
        sb_init(&content);
        char role[16] = "user";
        int ok = api_message(pool, m, role, sizeof(role), &content) == 0;
        if (!strcmp(role, "tool") || json_get(pool, m, "tool_calls") >= 0) tm = 1;
        if (ok && !strcmp(role, "user")) {
            sb_trim(&content);
            if (!(content.n >= 31 && !strncmp(content.p, "<tool_response>", 15) &&
                  !strcmp(content.p + content.n - 16, "</tool_response>")))
                last_query = idx;
        }
        sb_free(&content);
        if (!ok) return -1;
    }
    size_t limit = tm ? API_MAX_TEXT_TOOLS : API_MAX_TEXT;
    int first = pool[cr->messages].first, in_tool = 0;
    if (cr->tools >= 0) {   // the system message: the tools, then the system prompt's text
        Sb sys, c;
        sb_init(&sys);
        sb_init(&c);
        api_tools_prompt(pool, cr->tools, cr->tool_required, &sys);
        char role[16] = "";
        if (api_message(pool, first, role, sizeof(role), &c) == 0 && !strcmp(role, "system")) {
            sb_trim(&c);
            if (c.n) sb_str(&sys, "\n\n"), sb_add(&sys, c.p, c.n);
            first = pool[first].next;
        }
        total += sys.n;
        n = api_put(n, IM_START);
        n = api_text(n, "system\n", 0);
        n = sys.p && !sys.oom && total <= limit ? api_text(n, sys.p, 1) : -1;
        n = api_put(n, IM_END);
        n = api_text(n, "\n", 0);
        sb_free(&sys);
        sb_free(&c);
        if (n < 0) return -1;
    }
    idx = cr->tools >= 0 && first != pool[cr->messages].first ? 1 : 0;
    for (int m = first; m >= 0; m = pool[m].next, idx++) {
        Sb content;
        sb_init(&content);
        char role[16] = "user";
        int ok = api_message(pool, m, role, sizeof(role), &content) == 0;
        if (ok) total += content.n;
        if (!ok || total > limit) {
            sb_free(&content);
            return -1;
        }
        if (strcmp(role, "system") && strcmp(role, "assistant") && strcmp(role, "user") && strcmp(role, "tool"))
            strcpy(role, "user");
        int is_tool = tm && !strcmp(role, "tool");
        if (tm) sb_trim(&content);
        if (in_tool && !is_tool) {   // the results end: the user turn that held them
            n = api_put(n, IM_END);
            n = api_text(n, "\n", 0);
            in_tool = 0;
        }
        if (is_tool) {   // consecutive results share one user turn (the template)
            if (!in_tool) {
                n = api_put(n, IM_START);
                n = api_text(n, "user", 0);
                in_tool = 1;
            }
            n = api_text(n, "\n<tool_response>\n", 1);
            if (content.p) n = api_text(n, content.p, 1);
            n = api_text(n, "\n</tool_response>", 1);
            sb_free(&content);
            if (n < 0) return -1;
            continue;
        }
        Sb calls;
        sb_init(&calls);
        int is_asst = tm && !strcmp(role, "assistant");
        if (is_asst && api_assistant_calls(pool, m, content.n > 0, &calls)) n = -1;
        total += calls.n;
        if (total > limit) n = -1;
        n = api_put(n, IM_START);
        n = api_text(n, role, 0);
        n = api_text(n, "\n", 0);
        if (is_asst && eng.qwen35 && idx > last_query) {   // (Qwen's empty reasoning of the turns after the question)
            n = api_put(n, THINK);
            n = api_text(n, "\n\n", 0);
            n = api_put(n, THINK_END);
            n = api_text(n, "\n\n", 0);
        }
        if (content.p) n = api_text(n, content.p, tm);
        if (calls.p) n = api_text(n, calls.p, 1);
        n = api_put(n, IM_END);
        n = api_text(n, "\n", 0);
        sb_free(&content);
        sb_free(&calls);
        if (n < 0) return -1;
    }
    if (in_tool) {
        n = api_put(n, IM_END);
        n = api_text(n, "\n", 0);
    }
    n = api_put(n, IM_START);
    n = api_text(n, "assistant\n", 0);
    if (eng.qwen35) {
        n = api_put(n, THINK);
        n = api_text(n, "\n\n", 0);
        n = api_put(n, THINK_END);
        n = api_text(n, "\n\n", 0);
    }
    return n;
}

static void api_error(int fd, const char* status_line, const char* message, const char* type, const char* code) {
    Sb s;
    sb_init(&s);
    api_json_error(&s, message, type, code);
    Sb h;
    sb_init(&h);
    sb_str(&h, status_line);
    sb_str(&h, "\r\nContent-Type: application/json\r\nContent-Length: ");
    sb_int(&h, (long long)s.n);
    sb_str(&h, "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
    if (h.p && s.p && api_send(fd, h.p, h.n) == 0) api_send(fd, s.p, s.n);
    sb_free(&h);
    sb_free(&s);
}

static void api_job(ApiJob* job) {
    static unsigned api_count;
    JNode* pool = malloc(sizeof(JNode) * API_MAX_NODES);
    ChatRequest cr;
    const char* err = NULL;
    if (!pool || api_chat_parse(job->body, job->body_len, pool, API_MAX_NODES, &cr, &err)) {
        free(pool);
        api_error(job->fd, "HTTP/1.1 400 Bad Request", err ? err : "Out of memory.", "invalid_request_error",
                  "invalid_body");
        return;
    }
    if (cr.tools >= 0 && !eng.qwen35) {
        free(pool);
        api_error(job->fd, "HTTP/1.1 400 Bad Request", "Tools need Qwen3.5.", "invalid_request_error", "invalid_body");
        return;
    }
    int n = api_prompt(pool, &cr);
    if (n < 0) {
        free(pool);
        api_error(job->fd, "HTTP/1.1 400 Bad Request", "The messages are malformed or too long.",
                  "invalid_request_error", "context_length_exceeded");
        return;
    }
    int tools_on = cr.tools >= 0;   // (the tools stay in the pool: the reply's calls are read against their schemas)
    if (!tools_on) free(pool), pool = NULL;
    if (n + cr.max_tokens + 2 > (int)eng.ctx_limit) {
        free(pool);
        api_error(job->fd, "HTTP/1.1 400 Bad Request", "The messages and max_tokens do not fit the context window.",
                  "invalid_request_error", "context_length_exceeded");
        return;
    }
    // the model answers this request: the conversation on screen starts again from nothing (a request refused
    // above leaves it as it was)
    pos = 0;
    need_close = 0;
    hist_n = hist_done = 0;
    thinking = think_turn = 0;
    sys_pre = 0;
    view.scroll = 0;
    api_chat_begin();   // (clears the conversation: a note says that a request is being answered)
    if (cr.has_seed) seed = cr.seed;
    char id[32];
    snprintf(id, sizeof(id), "chatcmpl-%u", ++api_count);
    const char* model = api_model();
    long long created = (long long)time(NULL);
    say("api: prompt of %d tokens, up to %d new, T %.2f%s", n, cr.max_tokens, cr.temperature,
        cr.stream ? ", streamed" : "");
    engine_reset(&eng);
    if (engine_run(&eng, api_ids, n, 0, (float)cr.temperature, seed++, &next_token)) {
        api_error(job->fd, "HTTP/1.1 500 Internal Server Error", eng.error, "server_error", NULL);
        free(pool);
        pos = 0;
        api_chat_end();
        return;
    }
    pos = n;

    int fd = job->fd, streamed = 0, client_gone = 0, error = 0;
    if (cr.stream) {
        const char* h = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
        if (api_send(fd, h, strlen(h))) client_gone = 1;
        streamed = 1;
        Sb first;
        sb_init(&first);
        api_json_chunk(&first, id, model, created, "assistant", "", 0, NULL);
        if (!client_gone && first.p && api_send_event(fd, first.p)) client_gone = 1;
        sb_free(&first);
    }

    Sb text, pend;   // the reply (all of it), and the bytes of the last tokens not yet a whole character
    sb_init(&text);
    sb_init(&pend);
    int tc_seen = 0;       // tools: a <tool_call> has begun (what follows is held, then read as calls)
    size_t held_from = 0;  // where it begins in text
    int produced = 0, stopped = 0;
    const char* finish = "length";
    // The tokens to write: the one a step drew, after those it kept from the MTP head's draft (the model has read
    // these already). Without the head a step is one token.
    uint32_t q[9];
    int nq = 1, iq = 0;
    q[0] = next_token;
    double t0 = now();
    api_chat_reply();
    while (produced < cr.max_tokens) {
        uint32_t t = q[iq++];
        if (t == IM_END || t == eng.eos2 || t == eng.eos) {
            finish = "stop";
            break;
        }
        const uint8_t* bytes = NULL;
        int nb = tok_piece(&tok, t, &bytes);
        produced++;
        if (tools_on && (t == TOOL_CALL || t == TOOL_CALL_END)) {   // (added tokens: tok_piece gives no text for them)
            bytes = (const uint8_t*)(t == TOOL_CALL ? "<tool_call>" : "</tool_call>");
            nb = t == TOOL_CALL ? 11 : 12;
        }
        if (nb > 0) {
            sb_add(&text, (const char*)bytes, nb);
            sb_add(&pend, (const char*)bytes, nb);
        }
        if (streamed && pend.n) {
            size_t whole = api_utf8_whole((const uint8_t*)pend.p, pend.n);
            if (tools_on && tc_seen) {
                whole = 0;   // (a call has begun: the rest is held in text)
            } else if (tools_on) {   // the text before a <tool_call> goes out; a start of one waits
                long at = api_tag_find(pend.p, pend.n);
                if (at >= 0) {
                    tc_seen = 1;
                    held_from = text.n - pend.n + at;
                    whole = (size_t)at;
                } else {
                    size_t hold = api_tag_holdback(pend.p, pend.n);
                    if (whole > pend.n - hold) whole = pend.n - hold;
                }
            }
            if (whole) {
                Sb ev;
                sb_init(&ev);
                api_json_chunk(&ev, id, model, created, NULL, pend.p, whole, NULL);
                if (ev.p && api_send_event(fd, ev.p)) client_gone = 1;
                sb_free(&ev);
                memmove(pend.p, pend.p + whole, pend.n - whole);
                pend.n -= whole;
                pend.p[pend.n] = 0;
            }
            if (tc_seen) pend.n = 0;   // (the rest is in text)
        }
        if (client_gone || produced == cr.max_tokens) break;
        if (iq < nq) continue;   // (the next token is one the last step kept)
        if (api_chat_step(produced)) {   // Circle: the reply ends here, cut short as by max_tokens
            stopped = 1;
            break;
        }
        int ne = 0;   // tokens of the draft this step keeps
#ifdef GARLIC_SPEC
        if (spec_active(&eng)) {   // (never more than max_tokens: the step keeps at most what is left of them)
            if (engine_spec_step(&eng, t, pos, (float)cr.temperature, seed++, cr.max_tokens - produced + 1, q, &ne,
                                 &next_token)) {
                error = 1;
                break;
            }
        } else
#endif
        if (engine_run(&eng, &t, 1, pos, (float)cr.temperature, seed++, &next_token)) {
            error = 1;
            break;
        }
        pos += 1 + ne;
        q[ne] = next_token;
        nq = ne + 1, iq = 0;
    }
    ApiCall calls[API_MAX_CALLS];
    int nc = 0;
    size_t tend = text.n;   // where the text before the calls ends
    static unsigned api_call_seq;
    if (tools_on && !error && !client_gone && text.p) {
        nc = api_parse_tool_calls(text.p, text.n, pool, cr.tools, calls, API_MAX_CALLS, api_call_seq + 1, &tend);
        api_call_seq += nc;
        if (nc) finish = "tool_calls";
        while (tend > 0 && (text.p[tend - 1] == ' ' || text.p[tend - 1] == '\t' || text.p[tend - 1] == '\r' || text.p[tend - 1] == '\n')) tend--;
    }
    free(pool);
    double dt = now() - t0;
    say("api: %d tokens in %.1f s (%.1f tokens/s), %s", produced, dt, dt > 0 ? produced / dt : 0.0,
        error ? "engine error" : stopped ? "stopped on the console" : finish);

    if (streamed) {
        if (!client_gone && !error) {
            Sb last;
            sb_init(&last);
            if (tools_on && !tc_seen && pend.n) {   // a start of a tag that did not come to be one
                size_t whole = api_utf8_whole((const uint8_t*)pend.p, pend.n);
                if (whole) {
                    api_json_chunk(&last, id, model, created, NULL, pend.p, whole, NULL);
                    if (last.p && api_send_event(fd, last.p)) client_gone = 1;
                    sb_free(&last);
                }
            }
            if (tools_on && tc_seen && !client_gone) {   // the calls, whole (or, if they are not good, the text held)
                if (nc) api_json_chunk_calls(&last, id, model, created, calls, nc);
                else api_json_chunk(&last, id, model, created, NULL, text.p + held_from, text.n - held_from, NULL);
                if (last.p && api_send_event(fd, last.p)) client_gone = 1;
                sb_free(&last);
            }
            if (!client_gone) {
                api_json_chunk(&last, id, model, created, NULL, "", 0, finish);
                if (last.p) api_send_event(fd, last.p);
                sb_free(&last);
                api_send(fd, "data: [DONE]\n\n", 14);
            }
        }
        if (error && !client_gone) {   // the headers are out: the error goes in the stream
            Sb e;
            sb_init(&e);
            api_json_error(&e, eng.error, "server_error", NULL);
            if (e.p) api_send_event(fd, e.p);
            sb_free(&e);
        }
    } else if (error) {
        api_error(fd, "HTTP/1.1 500 Internal Server Error", eng.error, "server_error", NULL);
    } else {
        Sb body, head;
        sb_init(&body);
        sb_init(&head);
        if (nc) api_json_completion_calls(&body, id, model, created, text.p, tend, calls, nc, n, produced);
        else api_json_completion(&body, id, model, created, text.p ? text.p : "", text.n, finish, n, produced);
        sb_str(&head, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ");
        sb_int(&head, (long long)body.n);
        sb_str(&head, "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
        if (head.p && body.p && api_send(fd, head.p, head.n) == 0) api_send(fd, body.p, body.n);
        sb_free(&body);
        sb_free(&head);
    }
    api_calls_free(calls, nc);
    sb_free(&text);
    sb_free(&pend);
    // the engine's state is this request's; the conversation on screen starts again at position 0
    pos = 0;
    need_close = 0;
    api_chat_end();
}
