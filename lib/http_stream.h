/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SR_HTTP_STREAM_H
#define SR_HTTP_STREAM_H
#include "srtypes.h"
#define SR_DEFAULT_USER_AGENT                                                                      \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 "     \
    "Safari/537.36"
error_code http_stream_open(RIP_MANAGER_INFO *rmi, HSOCKET *sock, const char *url,
                            const char *proxy, SR_HTTP_HEADER *info, const char *agent,
                            const char *interface_name);
int http_stream_read(RIP_MANAGER_INFO *rmi, HSOCKET *sock, char *data, int size);
void http_stream_close(HSOCKET *sock);
/* Resolve a playlist or an embedded station URL, relative to base. */
char *http_stream_resolve(const char *base, const char *document, int html);
#endif
