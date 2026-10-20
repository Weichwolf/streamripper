/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <curl/curl.h>
#include <string.h>
#include <stdio.h>
#include "http_stream.h"
#include "http.h"
#include "errors.h"

#define SR_BODY_LIMIT (1024U * 1024U)
#define SR_AUDIO_BUFFER (4U * CURL_MAX_WRITE_SIZE)
#define SR_RESOLVE_LIMIT 10

struct sr_http_stream {
    CURL *easy;
    CURLM *multi;
    struct curl_slist *headers;
    struct curl_slist *aliases;
    char response[MAX_HEADER_LEN];
    size_t response_size;
    char body[SR_AUDIO_BUFFER];
    size_t length;
    size_t offset;
    int paused;
    int done;
    CURLcode result;
    char error[CURL_ERROR_SIZE];
};

static size_t receive_header(char *data, size_t size, size_t count, void *opaque)
{
    struct sr_http_stream *s = opaque;
    size_t n = size * count;
    if (n >= 5 && (!memcmp(data, "HTTP/", 5) || !memcmp(data, "ICY ", 4)))
        s->response_size = 0;
    if (n >= sizeof(s->response) - s->response_size)
        return 0;
    memcpy(s->response + s->response_size, data, n);
    s->response_size += n;
    s->response[s->response_size] = '\0';
    return n;
}

static size_t receive_body(char *data, size_t size, size_t count, void *opaque)
{
    struct sr_http_stream *s = opaque;
    size_t n = size * count;
    if (n > sizeof(s->body) - s->length) {
        s->paused = 1;
        return CURL_WRITEFUNC_PAUSE;
    }
    memcpy(s->body + s->length, data, n);
    s->length += n;
    return n;
}

void http_stream_close(HSOCKET *sock)
{
    struct sr_http_stream *s = sock->http;
    if (!s)
        return;
    if (s->multi && s->easy)
        curl_multi_remove_handle(s->multi, s->easy);
    curl_easy_cleanup(s->easy);
    curl_multi_cleanup(s->multi);
    curl_slist_free_all(s->headers);
    curl_slist_free_all(s->aliases);
    g_free(s);
    sock->http = NULL;
    sock->closed = TRUE;
}

/* Only the ripping thread owns curl handles. The abort pipe wakes its poll. */
static int pump(RIP_MANAGER_INFO *rmi, struct sr_http_stream *s)
{
    int running, remaining;
    CURLMsg *message;
    struct curl_waitfd abort_fd;
    if (s->paused && s->length == 0) {
        s->paused = 0;
        if (curl_easy_pause(s->easy, CURLPAUSE_CONT) != CURLE_OK)
            return SR_ERROR_RECV_FAILED;
    }
    if (curl_multi_perform(s->multi, &running) != CURLM_OK)
        return SR_ERROR_RECV_FAILED;
    while ((message = curl_multi_info_read(s->multi, &remaining))) {
        if (message->msg == CURLMSG_DONE) {
            s->done = 1;
            s->result = message->data.result;
        }
    }
    if (s->length || s->done)
        return SR_SUCCESS;
    memset(&abort_fd, 0, sizeof(abort_fd));
#if __UNIX__
    abort_fd.fd = rmi->abort_pipe[0];
    abort_fd.events = CURL_WAIT_POLLIN;
    if (curl_multi_poll(s->multi, &abort_fd, 1, 200, NULL) != CURLM_OK)
        return SR_ERROR_RECV_FAILED;
    if (abort_fd.revents & CURL_WAIT_POLLIN)
        return SR_ERROR_ABORT_PIPE_SIGNALLED;
#else
    (void)rmi;
    if (curl_multi_poll(s->multi, NULL, 0, 200, NULL) != CURLM_OK)
        return SR_ERROR_RECV_FAILED;
#endif
    return SR_SUCCESS;
}

static int transfer_error(struct sr_http_stream *s)
{
    if (s->result == CURLE_OK)
        return SR_SUCCESS;
    fprintf(stderr, "HTTP: %s\n", s->error[0] ? s->error : curl_easy_strerror(s->result));
    return s->result == CURLE_OPERATION_TIMEDOUT ? SR_ERROR_TIMEOUT : SR_ERROR_RECV_FAILED;
}

int http_stream_read(RIP_MANAGER_INFO *rmi, HSOCKET *sock, char *data, int size)
{
    struct sr_http_stream *s = sock->http;
    int received = 0;
    gint64 last_data = g_get_monotonic_time();
    while (received < size) {
        size_t available = s->length - s->offset;
        if (available) {
            size_t n = MIN(available, (size_t)(size - received));
            memcpy(data + received, s->body + s->offset, n);
            s->offset += n;
            received += (int)n;
            last_data = g_get_monotonic_time();
            if (s->offset == s->length)
                s->offset = s->length = 0;
        } else if (s->done) {
            int rc = transfer_error(s);
            return rc < 0 ? rc : received;
        } else {
            int rc = pump(rmi, s);
            if (rc < 0)
                return rc;
            if (rmi->prefs->timeout > 0 &&
                g_get_monotonic_time() - last_data > (gint64)rmi->prefs->timeout * G_USEC_PER_SEC)
                return SR_ERROR_TIMEOUT;
        }
    }
    return received;
}

static char *absolute_url(const char *base, const char *value)
{
    CURLU *url = curl_url();
    char *resolved = NULL, *scheme = NULL, *result = NULL;
    if (url && curl_url_set(url, CURLUPART_URL, base, 0) == CURLUE_OK &&
        curl_url_set(url, CURLUPART_URL, value, 0) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        (!strcmp(scheme, "http") || !strcmp(scheme, "https")) &&
        curl_url_get(url, CURLUPART_URL, &resolved, 0) == CURLUE_OK)
        result = g_strdup(resolved);
    curl_free(scheme);
    curl_free(resolved);
    curl_url_cleanup(url);
    return result;
}

char *http_stream_resolve(const char *base, const char *document, int html)
{
    char *result = NULL;
    if (!html && strstr(document, "#EXT-X-"))
        return NULL;
    if (html) {
        /* Nuxt serializes slashes as JSON Unicode escapes. No mount IDs are fixed. */
        GString *decoded = g_string_new(NULL);
        const char *p = document;
        while (*p) {
            if (!strncmp(p, "\\u002F", 6) || !strncmp(p, "\\u002f", 6)) {
                g_string_append_c(decoded, '/');
                p += 6;
            } else if (!strncmp(p, "\\/", 2)) {
                g_string_append_c(decoded, '/');
                p += 2;
            } else {
                g_string_append_c(decoded, *p++);
            }
        }
        GRegex *pattern = g_regex_new("https?://(?:stream\\.zeno\\.fm/[A-Za-z0-9_-]+|"
                                      "stream\\.srg-ssr\\.ch/srgssr/rsp/mp3/[0-9]+)",
                                      0, 0, NULL);
        GMatchInfo *match = NULL;
        if (g_regex_match(pattern, decoded->str, 0, &match))
            result = g_match_info_fetch(match, 0);
        g_match_info_free(match);
        g_regex_unref(pattern);
        g_string_free(decoded, TRUE);
    } else {
        char **lines = g_strsplit(document, "\n", -1);
        for (size_t i = 0; lines[i]; ++i) {
            char *line = g_strstrip(lines[i]);
            if (!g_ascii_strncasecmp(line, "File", 4)) {
                char *equals = strchr(line, '=');
                if (!equals)
                    continue;
                line = g_strstrip(equals + 1);
            } else if (!*line || *line == '#' || *line == '[' ||
                       (strchr(line, '=') && !strchr(line, '?') && !strstr(line, "://"))) {
                continue;
            }
            result = absolute_url(base, line);
            if (result)
                break;
        }
        g_strfreev(lines);
    }
    return result;
}

static int start_transfer(RIP_MANAGER_INFO *rmi, HSOCKET *sock, const char *url, const char *proxy,
                          const char *agent, const char *interface_name)
{
    struct sr_http_stream *s = g_new0(struct sr_http_stream, 1);
    sock->http = s;
    sock->closed = FALSE;
    s->easy = curl_easy_init();
    s->multi = curl_multi_init();
    if (!s->easy || !s->multi) {
        http_stream_close(sock);
        return SR_ERROR_CANT_ALLOC_MEMORY;
    }
    s->headers = curl_slist_append(NULL, "Icy-MetaData: 1");
    s->aliases = curl_slist_append(NULL, "ICY 200 OK");
#define SET_OPTION(option, value)                                                                  \
    do {                                                                                           \
        if (curl_easy_setopt(s->easy, option, value) != CURLE_OK) {                                \
            http_stream_close(sock);                                                               \
            return SR_ERROR_INVALID_PARAM;                                                         \
        }                                                                                          \
    } while (0)
    const char *ca_bundle = getenv("CURL_CA_BUNDLE");
    if (ca_bundle && *ca_bundle)
        SET_OPTION(CURLOPT_CAINFO, ca_bundle);
    SET_OPTION(CURLOPT_URL, url);
    SET_OPTION(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    SET_OPTION(CURLOPT_ACCEPT_ENCODING, "");
    SET_OPTION(CURLOPT_PROTOCOLS_STR, "http,https");
    SET_OPTION(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    SET_OPTION(CURLOPT_FOLLOWLOCATION, 1L);
    SET_OPTION(CURLOPT_MAXREDIRS, 10L);
    SET_OPTION(CURLOPT_NOSIGNAL, 1L);
    SET_OPTION(CURLOPT_CONNECTTIMEOUT, (long)MAX(rmi->prefs->timeout, 1));
    SET_OPTION(CURLOPT_LOW_SPEED_LIMIT, 1L);
    SET_OPTION(CURLOPT_LOW_SPEED_TIME, (long)MAX(rmi->prefs->timeout, 1));
    SET_OPTION(CURLOPT_USERAGENT, *agent ? agent : SR_DEFAULT_USER_AGENT);
    SET_OPTION(CURLOPT_HTTPHEADER, s->headers);
    SET_OPTION(CURLOPT_HTTP200ALIASES, s->aliases);
    SET_OPTION(CURLOPT_HEADERFUNCTION, receive_header);
    SET_OPTION(CURLOPT_HEADERDATA, s);
    SET_OPTION(CURLOPT_WRITEFUNCTION, receive_body);
    SET_OPTION(CURLOPT_WRITEDATA, s);
    SET_OPTION(CURLOPT_ERRORBUFFER, s->error);
    if (proxy)
        SET_OPTION(CURLOPT_PROXY, proxy);
    if (interface_name && *interface_name)
        SET_OPTION(CURLOPT_INTERFACE, interface_name);
#undef SET_OPTION
    if (curl_multi_add_handle(s->multi, s->easy) != CURLM_OK)
        return SR_ERROR_CONNECT_FAILED;
    while (!s->length && !s->done) {
        int rc = pump(rmi, s);
        if (rc < 0)
            return rc;
    }
    return s->done ? transfer_error(s) : SR_SUCCESS;
}

error_code http_stream_open(RIP_MANAGER_INFO *rmi, HSOCKET *sock, const char *url,
                            const char *proxy, SR_HTTP_HEADER *info, const char *agent,
                            const char *interface_name)
{
    char *current = absolute_url(url, url);
    int rc = SR_ERROR_INVALID_URL;
    if (!current)
        return rc;
    for (int hop = 0; hop < SR_RESOLVE_LIMIT; ++hop) {
        char *type = NULL, *effective = NULL;
        long status = 0;
        rc = start_transfer(rmi, sock, current, proxy, agent, interface_name);
        if (rc < 0)
            break;
        struct sr_http_stream *s = sock->http;
        curl_easy_getinfo(s->easy, CURLINFO_CONTENT_TYPE, &type);
        char content_type[256];
        g_strlcpy(content_type, type ? type : "", sizeof(content_type));
        for (char *letter = content_type; *letter; ++letter)
            *letter = g_ascii_tolower(*letter);
        type = content_type;
        curl_easy_getinfo(s->easy, CURLINFO_EFFECTIVE_URL, &effective);
        curl_easy_getinfo(s->easy, CURLINFO_RESPONSE_CODE, &status);
        if (status >= 400) {
            rc = http_parse_sc_header(effective, s->response, info);
            if (rc == SR_SUCCESS)
                rc = SR_ERROR_CONNECT_FAILED;
            break;
        }
        int html = type && g_str_has_prefix(type, "text/html");
        int playlist = (type && (strstr(type, "scpls") || strstr(type, "mpegurl"))) ||
                       strstr(effective, ".pls") || strstr(effective, ".m3u");
        if (!html && !playlist) {
            if (*type && !g_str_has_prefix(type, "audio/mpeg") &&
                !g_str_has_prefix(type, "audio/mp3") && !g_str_has_prefix(type, "audio/aac") &&
                !g_str_has_prefix(type, "audio/ogg") &&
                !g_str_has_prefix(type, "application/ogg") &&
                !g_str_has_prefix(type, "application/octet-stream")) {
                rc = SR_ERROR_UNSUPPORTED_FORMAT;
                break;
            }
            rc = http_parse_sc_header(effective, s->response, info);
            if (rc == SR_SUCCESS && info->meta_interval > 1024 * 1024)
                rc = SR_ERROR_INVALID_METADATA;
            break;
        }
        GString *body = g_string_new(NULL);
        while (body->len < SR_BODY_LIMIT) {
            char block[4096];
            int n = http_stream_read(rmi, sock, block, sizeof(block));
            if (n < 0) {
                rc = n;
                break;
            }
            g_string_append_len(body, block, n);
            if (n < (int)sizeof(block))
                break;
        }
        if (strstr(body->str, "#EXT-X-"))
            rc = SR_ERROR_UNSUPPORTED_FORMAT;
        char *next = rc < 0 || body->len >= SR_BODY_LIMIT
                         ? NULL
                         : http_stream_resolve(effective, body->str, html);
        g_string_free(body, TRUE);
        http_stream_close(sock);
        if (!next) {
            if (rc >= 0)
                rc = SR_ERROR_PARSE_FAILURE;
            break;
        }
        g_free(current);
        current = next;
        rc = SR_ERROR_INVALID_URL; /* A cycle must fail when the limit is reached. */
    }
    g_free(current);
    if (rc < 0)
        http_stream_close(sock);
    return rc;
}
