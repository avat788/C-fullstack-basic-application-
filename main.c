#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sqlite3.h>

#define DEFAULT_PORT    8080
#define BACKLOG         16
#define HEADER_BUF_SIZE 8192
#define MAX_BODY_SIZE   1048576   /* 1 MB safety cap on request bodies */

static sqlite3 *db = NULL;

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

/* Case-insensitive substring search, used to pull "Content-Length:" out of
 * the raw request regardless of how the client capitalizes it. */
static const char *find_ci(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return haystack;
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, nlen) == 0) return haystack;
    }
    return NULL;
}

/* Escape a string so it can sit safely inside a JSON string literal. */
static char *json_escape(const char *input) {
    if (!input) input = "";
    size_t len = strlen(input);
    char *out = malloc(len * 6 + 1); /* worst case: every byte -> \u00XX */
    if (!out) die("malloc");
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        switch (c) {
            case '"':  out[j++] = '\\'; out[j++] = '"';  break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
            case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
            case '\t': out[j++] = '\\'; out[j++] = 't';  break;
            default:
                if (c < 0x20) {
                    j += (size_t)sprintf(out + j, "\\u%04x", c);
                } else {
                    out[j++] = (char)c;
                }
        }
    }
    out[j] = '\0';
    return out;
}

/* Pull a string field's value out of a flat JSON object, e.g.
 * extract_json_string_field("{\"title\":\"Buy milk\"}", "title") -> "Buy milk".
 * This is deliberately not a general JSON parser - it only needs to handle
 * the one request shape this server accepts. Returns a malloc'd string, or
 * NULL if the field wasn't found. */
static char *extract_json_string_field(const char *json, const char *field) {
    if (!json) return NULL;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    const char *pos = strstr(json, pattern);
    if (!pos) return NULL;
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;
    if (*pos != ':') return NULL;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;
    if (*pos != '"') return NULL;
    pos++;

    size_t cap = 128, len = 0;
    char *out = malloc(cap);
    if (!out) die("malloc");
    while (*pos && *pos != '"') {
        char c = *pos;
        if (c == '\\' && *(pos + 1)) {
            pos++;
            switch (*pos) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                default:   c = *pos;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            out = realloc(out, cap);
            if (!out) die("realloc");
        }
        out[len++] = c;
        pos++;
    }
    out[len] = '\0';
    return out;
}

/* Read an entire file into a malloc'd, NUL-terminated buffer. Caller frees.
 * Returns NULL if the file can't be opened. */
static char *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); die("malloc"); }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = (long)got;
    return buf;
}

static void send_response(int fd, int status_code, const char *status_text,
                           const char *content_type, const char *body, long body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len);
        status_code, status_text, content_type, body_len);
    if (hlen > 0) {
        if (send(fd, header, (size_t)hlen, 0) < 0) return;
    }
    if (body_len > 0 && body) {
        send(fd, body, (size_t)body_len, 0);
    }
}

static int init_db(const char *path) {
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    const char *sql =
        "CREATE TABLE IF NOT EXISTS tasks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  title TEXT NOT NULL"
        ");";
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static void handle_get_root(int client_fd) {
    long len = 0;
    char *content = read_file("index.html", &len);
    if (!content) {
        const char *msg = "index.html not found next to the server binary";
        send_response(client_fd, 404, "Not Found", "text/plain", msg, (long)strlen(msg));
        return;
    }
    send_response(client_fd, 200, "OK", "text/html; charset=utf-8", content, len);
    free(content);
}

static void handle_get_tasks(int client_fd) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT id, title FROM tasks ORDER BY id ASC;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        const char *msg = "{\"error\":\"database error\"}";
        send_response(client_fd, 500, "Internal Server Error", "application/json", msg, (long)strlen(msg));
        return;
    }

    size_t cap = 1024, len = 0;
    char *json = malloc(cap);
    if (!json) die("malloc");
    json[len++] = '[';

    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const unsigned char *title = sqlite3_column_text(stmt, 1);
        char *escaped = json_escape((const char *)title);

        char entry[512];
        int entry_len = snprintf(entry, sizeof(entry), "%s{\"id\":%d,\"title\":\"%s\"}",
                                  first ? "" : ",", id, escaped);
        free(escaped);

        while (len + (size_t)entry_len + 2 > cap) {
            cap *= 2;
            json = realloc(json, cap);
            if (!json) die("realloc");
        }
        memcpy(json + len, entry, (size_t)entry_len);
        len += (size_t)entry_len;
        first = 0;
    }
    sqlite3_finalize(stmt);

    json[len++] = ']';
    json[len] = '\0';

    send_response(client_fd, 200, "OK", "application/json", json, (long)len);
    free(json);
}

static void handle_post_add(int client_fd, const char *body) {
    char *title = extract_json_string_field(body, "title");
    if (!title || strlen(title) == 0) {
        free(title);
        const char *msg = "{\"error\":\"title is required\"}";
        send_response(client_fd, 400, "Bad Request", "application/json", msg, (long)strlen(msg));
        return;
    }

    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO tasks (title) VALUES (?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(title);
        const char *msg = "{\"error\":\"database error\"}";
        send_response(client_fd, 500, "Internal Server Error", "application/json", msg, (long)strlen(msg));
        return;
    }
    sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        free(title);
        const char *msg = "{\"error\":\"insert failed\"}";
        send_response(client_fd, 500, "Internal Server Error", "application/json", msg, (long)strlen(msg));
        return;
    }

    sqlite3_int64 id = sqlite3_last_insert_rowid(db);
    char *escaped = json_escape(title);
    char resp[512];
    int resp_len = snprintf(resp, sizeof(resp), "{\"id\":%lld,\"title\":\"%s\"}", (long long)id, escaped);
    free(escaped);
    free(title);

    send_response(client_fd, 201, "Created", "application/json", resp, resp_len);
}

static void handle_client(int client_fd) {
    char buf[HEADER_BUF_SIZE + 1];
    size_t total = 0;

    /* Read until we have the full header block, or run out of buffer. */
    while (total < HEADER_BUF_SIZE) {
        ssize_t n = recv(client_fd, buf + total, HEADER_BUF_SIZE - total, 0);
        if (n <= 0) { close(client_fd); return; }
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    char method[8] = {0};
    char path[256] = {0};
    if (sscanf(buf, "%7s %255s", method, path) != 2) {
        const char *msg = "400 Bad Request";
        send_response(client_fd, 400, "Bad Request", "text/plain", msg, (long)strlen(msg));
        close(client_fd);
        return;
    }

    char *header_end = strstr(buf, "\r\n\r\n");
    char *body_start = header_end ? header_end + 4 : NULL;

    long content_length = 0;
    const char *cl = find_ci(buf, "content-length:");
    if (cl && (!header_end || cl < header_end)) {
        content_length = atol(cl + strlen("content-length:"));
    }
    if (content_length < 0) content_length = 0;
    if (content_length > MAX_BODY_SIZE) content_length = MAX_BODY_SIZE;

    char *body = NULL;
    if (content_length > 0 && body_start) {
        body = malloc((size_t)content_length + 1);
        if (!body) die("malloc");
        size_t have = total - (size_t)(body_start - buf);
        size_t to_copy = have < (size_t)content_length ? have : (size_t)content_length;
        memcpy(body, body_start, to_copy);
        size_t copied = to_copy;
        while (copied < (size_t)content_length) {
            ssize_t n = recv(client_fd, body + copied, (size_t)content_length - copied, 0);
            if (n <= 0) break;
            copied += (size_t)n;
        }
        body[copied] = '\0';
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        handle_get_root(client_fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/tasks") == 0) {
        handle_get_tasks(client_fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/add") == 0) {
        handle_post_add(client_fd, body);
    } else {
        const char *msg = "404 Not Found";
        send_response(client_fd, 404, "Not Found", "text/plain", msg, (long)strlen(msg));
    }

    free(body);
    close(client_fd);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN); /* don't die if a client disconnects mid-response */

    const char *port_env = getenv("PORT");
    int port = DEFAULT_PORT;
    if (port_env && *port_env) {
        int parsed = atoi(port_env);
        if (parsed > 0 && parsed <= 65535) port = parsed;
    }

    if (init_db("tasks.db") != 0) {
        fprintf(stderr, "Failed to initialize tasks.db\n");
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) die("socket");

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(server_fd, BACKLOG) < 0) die("listen");

    printf("Task Manager listening on port %d\n", port);
    fflush(stdout);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        handle_client(client_fd);
    }

    sqlite3_close(db);
    close(server_fd);
    return 0;
}
