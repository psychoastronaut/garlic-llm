// OpenAI-compatible HTTP API, protocol part (see api_proto.h).
#include "api_proto.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- growing string -----------------------------------------------------------------------------------------------

void sb_init(Sb* s) {
    s->p = NULL;
    s->n = s->cap = 0;
    s->oom = 0;
}

void sb_free(Sb* s) {
    free(s->p);
    sb_init(s);
}

void sb_add(Sb* s, const char* p, size_t n) {
    if (s->oom || n == 0) return;
    if (s->n + n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 256;
        while (cap < s->n + n + 1) cap *= 2;
        char* q = realloc(s->p, cap);
        if (!q) {
            s->oom = 1;
            return;
        }
        s->p = q;
        s->cap = cap;
    }
    memcpy(s->p + s->n, p, n);
    s->n += n;
    s->p[s->n] = 0;
}

void sb_str(Sb* s, const char* z) { sb_add(s, z, strlen(z)); }

void sb_json_str(Sb* s, const char* p, size_t n) {
    sb_add(s, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        switch (c) {
        case '"': sb_add(s, "\\\"", 2); break;
        case '\\': sb_add(s, "\\\\", 2); break;
        case '\n': sb_add(s, "\\n", 2); break;
        case '\r': sb_add(s, "\\r", 2); break;
        case '\t': sb_add(s, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                sb_add(s, esc, 6);
            } else {
                sb_add(s, (const char*)&p[i], 1);   // UTF-8 bytes pass through
            }
        }
    }
    sb_add(s, "\"", 1);
}

void sb_int(Sb* s, long long v) {
    char b[32];
    snprintf(b, sizeof(b), "%lld", v);
    sb_str(s, b);
}

// ---- HTTP request -------------------------------------------------------------------------------------------------

static int ci_eq(const char* a, size_t n, const char* b) {
    size_t m = strlen(b);
    if (n != m) return 0;
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (x != y) return 0;
    }
    return 1;
}

int api_http_parse(const char* buf, size_t len, HttpRequest* req) {
    memset(req, 0, sizeof(*req));
    size_t head = 0, term = 4;
    for (size_t i = 0; i + 1 < len; i++) {
        if (i + 3 < len && !memcmp(buf + i, "\r\n\r\n", 4)) {
            head = i + 4;
            break;
        }
        if (!memcmp(buf + i, "\n\n", 2)) {
            head = i + 2;
            term = 2;
            break;
        }
    }
    if (!head) return len > 16384 ? -1 : 0;
    (void)term;
    // request line: METHOD SP target SP HTTP/1.x
    const char* p = buf;
    const char* end = buf + head;
    const char* eol = memchr(p, '\n', end - p);
    if (!eol) return -1;
    const char* sp1 = memchr(p, ' ', eol - p);
    if (!sp1 || sp1 - p >= (long)sizeof(req->method)) return -1;
    memcpy(req->method, p, sp1 - p);
    const char* sp2 = memchr(sp1 + 1, ' ', eol - sp1 - 1);
    if (!sp2) return -1;
    size_t plen = sp2 - (sp1 + 1);
    const char* q = memchr(sp1 + 1, '?', plen);
    if (q) plen = q - (sp1 + 1);
    if (plen == 0 || plen >= sizeof(req->path)) return -1;
    memcpy(req->path, sp1 + 1, plen);
    req->path[plen] = 0;
    if (memchr(sp2 + 1, 'H', eol - sp2 - 1) == NULL) return -1;
    // headers
    p = eol + 1;
    while (p < end) {
        eol = memchr(p, '\n', end - p);
        if (!eol) break;
        const char* line_end = eol;
        if (line_end > p && line_end[-1] == '\r') line_end--;
        if (line_end == p) break;
        const char* colon = memchr(p, ':', line_end - p);
        if (colon) {
            const char* v = colon + 1;
            while (v < line_end && (*v == ' ' || *v == '\t')) v++;
            size_t vlen = line_end - v;
            if (ci_eq(p, colon - p, "content-length")) {
                char num[24];
                if (vlen == 0 || vlen >= sizeof(num)) return -1;
                memcpy(num, v, vlen);
                num[vlen] = 0;
                char* ep;
                long cl = strtol(num, &ep, 10);
                if (*ep || cl < 0 || cl > 1 << 20) return -1;
                req->content_length = cl;
            } else if (ci_eq(p, colon - p, "authorization")) {
                size_t k = vlen < sizeof(req->auth) - 1 ? vlen : sizeof(req->auth) - 1;
                memcpy(req->auth, v, k);
                req->auth[k] = 0;
            }
        }
        p = eol + 1;
    }
    req->head_len = head;
    if (len < head + (size_t)req->content_length) return 0;
    req->body = buf + head;
    req->body_len = (size_t)req->content_length;
    return 1;
}

// ---- JSON reader --------------------------------------------------------------------------------------------------

typedef struct {
    const char* s;
    size_t n, i;
    JNode* pool;
    int max, used, depth;
} JP;

static void jp_ws(JP* P) {
    while (P->i < P->n && (P->s[P->i] == ' ' || P->s[P->i] == '\t' || P->s[P->i] == '\n' || P->s[P->i] == '\r')) P->i++;
}

static int jp_new(JP* P) {
    if (P->used >= P->max) return -1;
    JNode* d = &P->pool[P->used];
    memset(d, 0, sizeof(*d));
    d->first = d->next = -1;
    return P->used++;
}

// raw string at P->i (on the opening quote): sets node s/slen, moves past the closing quote
static int jp_string(JP* P, int node, const char** s, size_t* len) {
    if (P->i >= P->n || P->s[P->i] != '"') return -1;
    P->i++;
    size_t start = P->i;
    while (P->i < P->n) {
        char c = P->s[P->i];
        if (c == '\\') {
            P->i += 2;
            continue;
        }
        if (c == '"') {
            *s = P->s + start;
            *len = P->i - start;
            P->i++;
            (void)node;
            return 0;
        }
        P->i++;
    }
    return -1;
}

static int jp_value(JP* P, int* out);

static int jp_value(JP* P, int* out) {
    jp_ws(P);
    if (P->i >= P->n || ++P->depth > 64) return -1;
    int r = -1;
    char c = P->s[P->i];
    int idx = jp_new(P);
    if (idx < 0) goto done;
    JNode* d = &P->pool[idx];
    if (c == '{' || c == '[') {
        int obj = c == '{';
        d->type = obj ? J_OBJ : J_ARR;
        P->i++;
        jp_ws(P);
        int last = -1;
        if (P->i < P->n && P->s[P->i] == (obj ? '}' : ']')) {
            P->i++;
            r = 0;
            goto done_ok;
        }
        for (;;) {
            const char* key = NULL;
            size_t klen = 0;
            if (obj) {
                jp_ws(P);
                if (jp_string(P, idx, &key, &klen)) goto done;
                jp_ws(P);
                if (P->i >= P->n || P->s[P->i] != ':') goto done;
                P->i++;
            }
            int child;
            if (jp_value(P, &child)) goto done;
            P->pool[child].key = key;
            P->pool[child].klen = klen;
            if (last < 0) P->pool[idx].first = child;
            else P->pool[last].next = child;
            last = child;
            P->pool[idx].count++;
            jp_ws(P);
            if (P->i >= P->n) goto done;
            if (P->s[P->i] == ',') {
                P->i++;
                continue;
            }
            if (P->s[P->i] == (obj ? '}' : ']')) {
                P->i++;
                break;
            }
            goto done;
        }
    done_ok:
        r = 0;
    } else if (c == '"') {
        d->type = J_STR;
        if (jp_string(P, idx, &d->s, &d->slen)) goto done;
        r = 0;
    } else if (c == 't' || c == 'f' || c == 'n') {
        const char* lit = c == 't' ? "true" : c == 'f' ? "false" : "null";
        size_t m = strlen(lit);
        if (P->n - P->i < m || memcmp(P->s + P->i, lit, m)) goto done;
        P->i += m;
        d->type = c == 'n' ? J_NULL : J_BOOL;
        d->b = c == 't';
        r = 0;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = P->i;
        while (P->i < P->n && strchr("+-0123456789.eE", P->s[P->i])) P->i++;
        size_t m = P->i - start;
        if (m == 0 || m > 40) goto done;
        char num[48];
        memcpy(num, P->s + start, m);
        num[m] = 0;
        char* ep;
        d->num = strtod(num, &ep);
        if (*ep) goto done;
        d->type = J_NUM;
        r = 0;
    }
done:
    if (r == 0) *out = idx;
    P->depth--;
    return r;
}

int json_parse(const char* s, size_t n, JNode* pool, int max) {
    JP P = {s, n, 0, pool, max, 0, 0};
    int root;
    if (jp_value(&P, &root)) return -1;
    jp_ws(&P);
    if (P.i != n) return -1;
    return root;
}

int json_get(const JNode* pool, int obj, const char* key) {
    if (obj < 0 || pool[obj].type != J_OBJ) return -1;
    size_t m = strlen(key);
    for (int c = pool[obj].first; c >= 0; c = pool[c].next)
        if (pool[c].klen == m && !memcmp(pool[c].key, key, m)) return c;
    return -1;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void utf8_put(Sb* out, uint32_t cp) {
    char b[4];
    if (cp < 0x80) {
        b[0] = (char)cp;
        sb_add(out, b, 1);
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        sb_add(out, b, 2);
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        sb_add(out, b, 3);
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        sb_add(out, b, 4);
    }
}

// the 4 hex digits at p (p[0..3]); -1 if not hex
static long hex4(const char* p, const char* end) {
    if (end - p < 4) return -1;
    long v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexv(p[i]);
        if (h < 0) return -1;
        v = v * 16 + h;
    }
    return v;
}

// the decoded text of raw JSON string contents (p, n)
static void raw_str_sb(const char* p, size_t n, Sb* out) {
    const char* end = p + n;
    while (p < end) {
        if (*p != '\\') {
            sb_add(out, p++, 1);
            continue;
        }
        p++;
        if (p >= end) break;
        char c = *p++;
        switch (c) {
        case 'n': sb_add(out, "\n", 1); break;
        case 't': sb_add(out, "\t", 1); break;
        case 'r': sb_add(out, "\r", 1); break;
        case 'b': sb_add(out, "\b", 1); break;
        case 'f': sb_add(out, "\f", 1); break;
        case 'u': {
            long cp = hex4(p, end);
            if (cp < 0) return;
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                long lo = hex4(p + 2, end);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            utf8_put(out, (uint32_t)cp);
            break;
        }
        default: sb_add(out, &c, 1); break;   // \" \\ \/
        }
    }
}

void json_str_sb(const JNode* pool, int node, Sb* out) {
    if (node < 0 || pool[node].type != J_STR) return;
    raw_str_sb(pool[node].s, pool[node].slen, out);
}

// a member name, decoded and written again as a JSON string
static void key_json(const JNode* pool, int node, Sb* out) {
    Sb k;
    sb_init(&k);
    raw_str_sb(pool[node].key, pool[node].klen, &k);
    sb_json_str(out, k.p ? k.p : "", k.n);
    sb_free(&k);
}

int api_message(const JNode* pool, int msg, char* role, size_t rcap, Sb* out) {
    if (msg < 0 || pool[msg].type != J_OBJ) return -1;
    int r = json_get(pool, msg, "role");
    if (r < 0 || pool[r].type != J_STR) return -1;
    Sb rs;
    sb_init(&rs);
    json_str_sb(pool, r, &rs);
    size_t k = rs.n < rcap - 1 ? rs.n : rcap - 1;
    if (rs.n) memcpy(role, rs.p, k);
    role[k] = 0;
    sb_free(&rs);
    int c = json_get(pool, msg, "content");
    if (c < 0 || pool[c].type == J_NULL) return 0;
    if (pool[c].type == J_STR) {
        json_str_sb(pool, c, out);
        return 0;
    }
    if (pool[c].type != J_ARR) return -1;
    for (int part = pool[c].first; part >= 0; part = pool[part].next) {
        if (pool[part].type != J_OBJ) return -1;
        int t = json_get(pool, part, "text");
        if (t >= 0 && pool[t].type == J_STR) json_str_sb(pool, t, out);
    }
    return 0;
}

// ---- tool calling -------------------------------------------------------------------------------------------------

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void sb_trim(Sb* s) {
    if (!s->p) return;
    size_t a = 0, b = s->n;
    while (a < b && is_ws(s->p[a])) a++;
    while (b > a && is_ws(s->p[b - 1])) b--;
    memmove(s->p, s->p + a, b - a);
    s->n = b - a;
    s->p[s->n] = 0;
}

void api_json_dump(const JNode* pool, int node, Sb* out, int spaced) {
    const JNode* d = &pool[node];
    switch (d->type) {
    case J_NULL: sb_str(out, "null"); break;
    case J_BOOL: sb_str(out, d->b ? "true" : "false"); break;
    case J_NUM: {
        char b[40];
        if (!(d->num == d->num) || d->num > 1e300 || d->num < -1e300) strcpy(b, "null");
        else if (d->num < 1e15 && d->num > -1e15 && d->num == floor(d->num)) snprintf(b, sizeof(b), "%lld", (long long)d->num);
        else {   // the shortest digits that read back the same (as Python's repr)
            for (int prec = 15; prec <= 17; prec++) {
                snprintf(b, sizeof(b), "%.*g", prec, d->num);
                if (strtod(b, NULL) == d->num) break;
            }
        }
        sb_str(out, b);
        break;
    }
    case J_STR: {
        Sb t;
        sb_init(&t);
        json_str_sb(pool, node, &t);
        sb_json_str(out, t.p ? t.p : "", t.n);
        sb_free(&t);
        break;
    }
    default: {
        int obj = d->type == J_OBJ;
        sb_str(out, obj ? "{" : "[");
        for (int c = d->first; c >= 0; c = pool[c].next) {
            if (c != d->first) sb_str(out, spaced ? ", " : ",");
            if (obj) {
                key_json(pool, c, out);
                sb_str(out, spaced ? ": " : ":");
            }
            api_json_dump(pool, c, out, spaced);
        }
        sb_str(out, obj ? "}" : "]");
    }
    }
}

// Qwen3.5's chat template, tools part (verbatim from chat_template.jinja of Qwen/Qwen3.5-9B).
void api_tools_prompt(const JNode* pool, int tools, int required, Sb* out) {
    sb_str(out, "# Tools\n\nYou have access to the following functions:\n\n<tools>");
    for (int t = pool[tools].first; t >= 0; t = pool[t].next) {
        sb_str(out, "\n");
        api_json_dump(pool, t, out, 1);
    }
    sb_str(out, "\n</tools>");
    sb_str(out, "\n\nIf you choose to call a function ONLY reply in the following format with NO suffix:\n\n"
                "<tool_call>\n<function=example_function_name>\n<parameter=example_parameter_1>\nvalue_1\n</parameter>\n"
                "<parameter=example_parameter_2>\nThis is the value for the second parameter\nthat can span\nmultiple lines\n"
                "</parameter>\n</function>\n</tool_call>\n\n<IMPORTANT>\nReminder:\n"
                "- Function calls MUST follow the specified format: an inner <function=...></function> block must be nested "
                "within <tool_call></tool_call> XML tags\n"
                "- Required parameters MUST be specified\n"
                "- You may provide optional reasoning for your function call in natural language BEFORE the function call, "
                "but NOT after\n"
                "- If there is no function call available, answer the question like normal with your current knowledge and "
                "do not tell the user about function calls\n");
    // (not in the template: tool_choice "required", best effort)
    if (required) sb_str(out, "- You MUST call one of the functions above in this reply\n");
    sb_str(out, "</IMPORTANT>");
}

// The parameters of one call (obj: an object node of pool), as the template writes them.
static void render_params(const JNode* pool, int obj, Sb* out) {
    for (int c = pool[obj].first; c >= 0; c = pool[c].next) {
        sb_str(out, "<parameter=");
        raw_str_sb(pool[c].key, pool[c].klen, out);
        sb_str(out, ">\n");
        switch (pool[c].type) {
        case J_STR: json_str_sb(pool, c, out); break;
        case J_OBJ:
        case J_ARR: api_json_dump(pool, c, out, 1); break;
        case J_BOOL: sb_str(out, pool[c].b ? "True" : "False"); break;   // (Python's str(), as in the template)
        case J_NULL: sb_str(out, "None"); break;
        default: api_json_dump(pool, c, out, 1);
        }
        sb_str(out, "\n</parameter>\n");
    }
}

int api_assistant_calls(const JNode* pool, int msg, int has_content, Sb* out) {
    int tc = json_get(pool, msg, "tool_calls");
    if (tc < 0 || pool[tc].type == J_NULL) return 0;
    if (pool[tc].type != J_ARR) return -1;
    int first = 1;
    for (int c = pool[tc].first; c >= 0; c = pool[c].next) {
        if (pool[c].type != J_OBJ) return -1;
        int fn = json_get(pool, c, "function");
        if (fn < 0 || pool[fn].type != J_OBJ) fn = c;
        int nm = json_get(pool, fn, "name");
        if (nm < 0 || pool[nm].type != J_STR) return -1;
        sb_str(out, first ? (has_content ? "\n\n" : "") : "\n");
        first = 0;
        sb_str(out, "<tool_call>\n<function=");
        json_str_sb(pool, nm, out);
        sb_str(out, ">\n");
        int a = json_get(pool, fn, "arguments");
        if (a >= 0 && pool[a].type == J_OBJ) {
            render_params(pool, a, out);
        } else if (a >= 0 && pool[a].type == J_STR) {   // OpenAI: the arguments are a JSON text
            Sb txt;
            sb_init(&txt);
            json_str_sb(pool, a, &txt);
            JNode* ap = malloc(sizeof(JNode) * 512);
            int root = ap && txt.p ? json_parse(txt.p, txt.n, ap, 512) : -1;
            if (root >= 0 && ap[root].type == J_OBJ) render_params(ap, root, out);   // (anything else: no parameters)
            free(ap);
            sb_free(&txt);
        }
        sb_str(out, "</function>\n</tool_call>");
    }
    return out->oom ? -1 : 0;
}

static int lit_at(const char* p, const char* end, const char* lit) {
    size_t m = strlen(lit);
    return (size_t)(end - p) >= m && !memcmp(p, lit, m);
}

static const char* lit_find(const char* p, const char* end, const char* lit) {
    size_t m = strlen(lit);
    for (; (size_t)(end - p) >= m; p++)
        if (*p == lit[0] && !memcmp(p, lit, m)) return p;
    return NULL;
}

static const char* skip_ws(const char* p, const char* end) {
    while (p < end && is_ws(*p)) p++;
    return p;
}

long api_tag_find(const char* p, size_t n) {
    const char* f = lit_find(p, p + n, "<tool_call>");
    return f ? (long)(f - p) : -1;
}

size_t api_tag_holdback(const char* p, size_t n) {
    for (size_t k = n < 10 ? n : 10; k > 0; k--)   // (a whole tag is not "held back": it is found)
        if (!memcmp(p + n - k, "<tool_call>", k)) return k;
    return 0;
}

static void note_type(const JNode* t, int* has_string, const char** other, size_t* olen) {
    if (t->slen == 6 && !memcmp(t->s, "string", 6)) *has_string = 1;
    else if (!(t->slen == 4 && !memcmp(t->s, "null", 4)) && !*other) *other = t->s, *olen = t->slen;
}

// The type the schema of function fn gives to its parameter pn ("string", "number"...), or NULL.
static const char* schema_type(const JNode* pool, int tools, const Sb* fn, const char* pn, size_t* len) {
    if (tools < 0 || !fn->p) return NULL;
    for (int t = pool[tools].first; t >= 0; t = pool[t].next) {
        int f = json_get(pool, t, "function");
        int nm = json_get(pool, f, "name");
        if (nm < 0 || pool[nm].type != J_STR || pool[nm].slen != fn->n || memcmp(pool[nm].s, fn->p, fn->n)) continue;
        int prop = json_get(pool, json_get(pool, json_get(pool, f, "parameters"), "properties"), pn);
        // the types the property allows: "type" (a name or a list) and those of anyOf / oneOf
        int has_string = 0;
        const char* other = NULL;
        size_t olen = 0;
        int srcs[3] = {json_get(pool, prop, "type"), -1, -1};
        for (int i = 0; i < 3; i++) {
            int s = i == 0 ? srcs[0] : json_get(pool, prop, i == 1 ? "anyOf" : "oneOf");
            if (s < 0) continue;
            if (i == 0 && pool[s].type == J_STR) note_type(&pool[s], &has_string, &other, &olen);
            else if (pool[s].type == J_ARR)
                for (int e = pool[s].first; e >= 0; e = pool[e].next) {
                    int ty = i == 0 ? e : json_get(pool, e, "type");   // (an anyOf entry: {"type": ...})
                    if (ty >= 0 && pool[ty].type == J_STR) note_type(&pool[ty], &has_string, &other, &olen);
                }
        }
        // only strings (null aside): a string, whatever it looks like; mixed with other types, the other one (a guess
        // from the text)
        if (other) {
            *len = olen;
            return other;
        }
        if (has_string) {
            *len = 6;
            return "string";
        }
        return NULL;
    }
    return NULL;
}

static int json_number_ok(const char* s, size_t n) {   // -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
    size_t i = 0;
    if (i < n && s[i] == '-') i++;
    if (i >= n || s[i] < '0' || s[i] > '9') return 0;
    if (s[i] == '0') i++;
    else while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    if (i < n && s[i] == '.') {
        size_t d = ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == d) return 0;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        size_t d = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == d) return 0;
    }
    return i == n;
}

// One parameter's value (as text) in the JSON of the arguments.
static void param_value(Sb* out, const char* v, size_t n, const char* ty, size_t tlen) {
    int is_string = ty && tlen == 6 && !memcmp(ty, "string", 6);
    const char* a = skip_ws(v, v + n);
    const char* b = v + n;
    while (b > a && is_ws(b[-1])) b--;
    size_t m = b - a;
    if (!is_string && m) {
        if ((m == 4 && !memcmp(a, "true", 4)) || (m == 5 && !memcmp(a, "false", 5)) || (m == 4 && !memcmp(a, "null", 4)) ||
            json_number_ok(a, m)) {
            sb_add(out, a, m);
            return;
        }
        if (ty && m == 4 && !memcmp(a, "True", 4)) return sb_str(out, "true");   // (the template writes Python's str())
        if (ty && m == 5 && !memcmp(a, "False", 5)) return sb_str(out, "false");
        if (ty && m == 4 && !memcmp(a, "None", 4)) return sb_str(out, "null");
        if (*a == '{' || *a == '[') {
            JNode* pool = malloc(sizeof(JNode) * 256);
            int root = pool ? json_parse(a, m, pool, 256) : -1;
            if (root >= 0) {
                api_json_dump(pool, root, out, 0);
                free(pool);
                return;
            }
            free(pool);
        }
    }
    sb_json_str(out, v, n);
}

// One block (between <tool_call> and </tool_call>): <function=NAME> <parameter=ARG> VALUE </parameter>... </function>
static int parse_block(const char* p, const char* end, const JNode* pool, int tools, ApiCall* c) {
    p = skip_ws(p, end);
    if (!lit_at(p, end, "<function=")) return -1;
    p += 10;
    const char* gt = memchr(p, '>', end - p);
    if (!gt) return -1;
    sb_add(&c->name, p, gt - p);
    sb_trim(&c->name);
    if (!c->name.n || memchr(c->name.p, '<', c->name.n)) return -1;
    p = gt + 1;
    sb_str(&c->args, "{");
    int np = 0;
    for (;;) {
        p = skip_ws(p, end);
        if (!lit_at(p, end, "<parameter=")) break;
        p += 11;
        gt = memchr(p, '>', end - p);
        if (!gt) return -1;
        Sb pn;
        sb_init(&pn);
        sb_add(&pn, p, gt - p);
        sb_trim(&pn);
        const char* close = lit_find(gt + 1, end, "</parameter>");
        if (!close || !pn.n) {
            sb_free(&pn);
            return -1;
        }
        const char* v = gt + 1;
        if (v < close && *v == '\n') v++;   // (the template puts a line break before and after the value)
        const char* ve = close;
        if (ve > v && ve[-1] == '\n') ve--;
        if (np++) sb_str(&c->args, ",");
        sb_json_str(&c->args, pn.p, pn.n);
        sb_str(&c->args, ":");
        size_t tl = 0;
        const char* ty = schema_type(pool, tools, &c->name, pn.p, &tl);
        param_value(&c->args, v, ve - v, ty, tl);
        sb_free(&pn);
        p = close + 12;
    }
    p = skip_ws(p, end);
    if (lit_at(p, end, "</function>")) p = skip_ws(p + 11, end);
    if (p != end) return -1;
    sb_str(&c->args, "}");
    return c->name.oom || c->args.oom ? -1 : 0;
}

int api_parse_tool_calls(const char* text, size_t n, const JNode* pool, int tools, ApiCall* calls, int max,
                         unsigned first_id, size_t* text_end) {
    const char* end = text + n;
    const char* p = text;
    int k = 0;
    *text_end = n;
    while (k < max) {
        const char* open = lit_find(p, end, "<tool_call>");
        if (!open) break;
        const char* body = open + 11;
        const char* close = lit_find(body, end, "</tool_call>");
        const char* body_end = close;
        const char* next = close ? close + 12 : end;
        if (!close) {   // the turn ended before </tool_call>: fine if the call itself is whole
            if (!lit_find(body, end, "</function>")) break;
            body_end = end;
        }
        ApiCall* c = &calls[k];
        sb_init(&c->name);
        sb_init(&c->args);
        c->id = first_id + k;
        if (parse_block(body, body_end, pool, tools, c)) {
            sb_free(&c->name);
            sb_free(&c->args);
            break;
        }
        if (!k) *text_end = open - text;
        k++;
        p = next;
    }
    return k;
}

void api_calls_free(ApiCall* calls, int n) {
    for (int i = 0; i < n; i++) {
        sb_free(&calls[i].name);
        sb_free(&calls[i].args);
    }
}

// ---- JSON writers -------------------------------------------------------------------------------------------------

void api_json_models(Sb* out, const char* model, long long created) {
    sb_str(out, "{\"object\":\"list\",\"data\":[{\"id\":");
    sb_json_str(out, model, strlen(model));
    sb_str(out, ",\"object\":\"model\",\"created\":");
    sb_int(out, created);
    sb_str(out, ",\"owned_by\":\"garlic-llm\"}]}");
}

static void calls_json(Sb* out, const ApiCall* calls, int n, int indexed) {
    sb_str(out, "[");
    for (int i = 0; i < n; i++) {
        if (i) sb_str(out, ",");
        sb_str(out, "{");
        if (indexed) {
            sb_str(out, "\"index\":");
            sb_int(out, i);
            sb_str(out, ",");
        }
        sb_str(out, "\"id\":\"call_");
        sb_int(out, calls[i].id);
        sb_str(out, "\",\"type\":\"function\",\"function\":{\"name\":");
        sb_json_str(out, calls[i].name.p ? calls[i].name.p : "", calls[i].name.n);
        sb_str(out, ",\"arguments\":");
        sb_json_str(out, calls[i].args.p ? calls[i].args.p : "", calls[i].args.n);
        sb_str(out, "}}");
    }
    sb_str(out, "]");
}

static void completion(Sb* out, const char* id, const char* model, long long created, const char* text, size_t tlen,
                       const ApiCall* calls, int ncalls, const char* finish, int prompt_tokens, int completion_tokens) {
    sb_str(out, "{\"id\":");
    sb_json_str(out, id, strlen(id));
    sb_str(out, ",\"object\":\"chat.completion\",\"created\":");
    sb_int(out, created);
    sb_str(out, ",\"model\":");
    sb_json_str(out, model, strlen(model));
    sb_str(out, ",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":");
    if (ncalls > 0 && tlen == 0) sb_str(out, "null");
    else sb_json_str(out, text, tlen);
    if (ncalls > 0) {
        sb_str(out, ",\"tool_calls\":");
        calls_json(out, calls, ncalls, 0);
    }
    sb_str(out, "},\"finish_reason\":");
    sb_json_str(out, finish, strlen(finish));
    sb_str(out, "}],\"usage\":{\"prompt_tokens\":");
    sb_int(out, prompt_tokens);
    sb_str(out, ",\"completion_tokens\":");
    sb_int(out, completion_tokens);
    sb_str(out, ",\"total_tokens\":");
    sb_int(out, prompt_tokens + completion_tokens);
    sb_str(out, "}}");
}

void api_json_completion(Sb* out, const char* id, const char* model, long long created, const char* text, size_t tlen,
                         const char* finish, int prompt_tokens, int completion_tokens) {
    completion(out, id, model, created, text, tlen, NULL, 0, finish, prompt_tokens, completion_tokens);
}

void api_json_completion_calls(Sb* out, const char* id, const char* model, long long created, const char* text,
                               size_t tlen, const ApiCall* calls, int n, int prompt_tokens, int completion_tokens) {
    completion(out, id, model, created, text, tlen, calls, n, "tool_calls", prompt_tokens, completion_tokens);
}

void api_json_chunk_calls(Sb* out, const char* id, const char* model, long long created, const ApiCall* calls, int n) {
    sb_str(out, "{\"id\":");
    sb_json_str(out, id, strlen(id));
    sb_str(out, ",\"object\":\"chat.completion.chunk\",\"created\":");
    sb_int(out, created);
    sb_str(out, ",\"model\":");
    sb_json_str(out, model, strlen(model));
    sb_str(out, ",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":");
    calls_json(out, calls, n, 1);
    sb_str(out, "},\"finish_reason\":null}]}");
}

void api_json_chunk(Sb* out, const char* id, const char* model, long long created, const char* role, const char* text,
                    size_t tlen, const char* finish) {
    sb_str(out, "{\"id\":");
    sb_json_str(out, id, strlen(id));
    sb_str(out, ",\"object\":\"chat.completion.chunk\",\"created\":");
    sb_int(out, created);
    sb_str(out, ",\"model\":");
    sb_json_str(out, model, strlen(model));
    sb_str(out, ",\"choices\":[{\"index\":0,\"delta\":{");
    int any = 0;
    if (role) {
        sb_str(out, "\"role\":");
        sb_json_str(out, role, strlen(role));
        any = 1;
    }
    if (tlen) {
        if (any) sb_str(out, ",");
        sb_str(out, "\"content\":");
        sb_json_str(out, text, tlen);
    }
    sb_str(out, "},\"finish_reason\":");
    if (finish) sb_json_str(out, finish, strlen(finish));
    else sb_str(out, "null");
    sb_str(out, "}]}");
}

void api_json_error(Sb* out, const char* message, const char* type, const char* code) {
    sb_str(out, "{\"error\":{\"message\":");
    sb_json_str(out, message, strlen(message));
    sb_str(out, ",\"type\":");
    sb_json_str(out, type, strlen(type));
    sb_str(out, ",\"code\":");
    if (code) sb_json_str(out, code, strlen(code));
    else sb_str(out, "null");
    sb_str(out, "}}");
}

// ---- chat request -------------------------------------------------------------------------------------------------

int api_chat_parse(const char* body, size_t len, JNode* pool, int max, ChatRequest* req, const char** err) {
    memset(req, 0, sizeof(*req));
    req->temperature = 0.7;
    req->max_tokens = 512;
    req->messages = -1;
    req->root = json_parse(body, len, pool, max);
    if (req->root < 0 || pool[req->root].type != J_OBJ) {
        *err = "The body must be a JSON object.";
        return -1;
    }
    int m = json_get(pool, req->root, "messages");
    if (m < 0 || pool[m].type != J_ARR || pool[m].count < 1) {
        *err = "'messages' must be a non-empty array.";
        return -1;
    }
    for (int c = pool[m].first; c >= 0; c = pool[c].next) {
        if (pool[c].type != J_OBJ) {
            *err = "Each message must be an object with 'role' and 'content'.";
            return -1;
        }
        req->n_messages++;
    }
    req->messages = m;
    int t = json_get(pool, req->root, "temperature");
    if (t >= 0 && pool[t].type != J_NULL) {
        if (pool[t].type != J_NUM || pool[t].num < 0 || pool[t].num > 2) {
            *err = "'temperature' must be a number from 0 to 2.";
            return -1;
        }
        req->temperature = pool[t].num;
    }
    int mt = json_get(pool, req->root, "max_completion_tokens");
    if (mt < 0) mt = json_get(pool, req->root, "max_tokens");
    if (mt >= 0 && pool[mt].type != J_NULL) {
        if (pool[mt].type != J_NUM || pool[mt].num < 1 || pool[mt].num > 16384) {
            *err = "'max_tokens' must be a number from 1 to 16384.";
            return -1;
        }
        req->max_tokens = (int)pool[mt].num;
    }
    int s = json_get(pool, req->root, "stream");
    if (s >= 0 && pool[s].type != J_NULL) {
        if (pool[s].type != J_BOOL) {
            *err = "'stream' must be true or false.";
            return -1;
        }
        req->stream = pool[s].b;
    }
    int n = json_get(pool, req->root, "n");
    if (n >= 0 && pool[n].type == J_NUM && pool[n].num != 1) {
        *err = "Only n = 1 is supported.";
        return -1;
    }
    // tools: put in the prompt unless tool_choice is "none" ("auto" and the rest: the model decides)
    req->tools = -1;
    int none = 0;
    int tc = json_get(pool, req->root, "tool_choice");
    if (tc >= 0 && pool[tc].type == J_STR) {
        if (pool[tc].slen == 4 && !memcmp(pool[tc].s, "none", 4)) none = 1;
        else if (pool[tc].slen == 8 && !memcmp(pool[tc].s, "required", 8)) req->tool_required = 1;
        else if (!(pool[tc].slen == 4 && !memcmp(pool[tc].s, "auto", 4))) {
            *err = "'tool_choice' must be \"none\", \"auto\", \"required\" or a function.";
            return -1;
        }
    } else if (tc >= 0 && pool[tc].type == J_OBJ) {
        req->tool_required = 1;   // a named function: asked for as "required" (the name is not enforced)
    } else if (tc >= 0 && pool[tc].type != J_NULL) {
        *err = "'tool_choice' must be a string or an object.";
        return -1;
    }
    int tl = json_get(pool, req->root, "tools");
    if (tl >= 0 && pool[tl].type != J_NULL) {
        if (pool[tl].type != J_ARR) {
            *err = "'tools' must be an array.";
            return -1;
        }
        for (int c = pool[tl].first; c >= 0; c = pool[c].next) {
            int f = json_get(pool, c, "function");
            int nm = json_get(pool, f, "name");
            if (nm < 0 || pool[nm].type != J_STR || pool[nm].slen == 0) {
                *err = "Each tool must be {\"type\":\"function\",\"function\":{\"name\":...}}.";
                return -1;
            }
        }
        if (pool[tl].count > 0 && !none) req->tools = tl;
    }
    if (req->tools < 0) req->tool_required = 0;
    int sd = json_get(pool, req->root, "seed");
    if (sd >= 0 && pool[sd].type == J_NUM && pool[sd].num >= 0) {
        req->seed = (unsigned)pool[sd].num;
        req->has_seed = 1;
    }
    return 0;
}
