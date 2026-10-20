/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "http_stream.h"
#include "socklib.h"

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    RIP_MANAGER_INFO rmi = {0};
    STREAM_PREFS prefs = {0};
    HSOCKET sock = {.closed = TRUE};
    SR_HTTP_HEADER info;
    prefs.timeout = 2;
    rmi.prefs = &prefs;
    if (pipe(rmi.abort_pipe) != 0)
        return 2;
    if (socklib_init() != SR_SUCCESS)
        return 2;
    int rc = http_stream_open(&rmi, &sock, argv[1], NULL, &info, "", NULL);
    if (rc == SR_SUCCESS) {
        unsigned char data[2048];
        int n;
        size_t total = 0;
        while ((n = socklib_recvall(&rmi, &sock, (char *)data, sizeof(data), 2)) > 0) {
            for (int i = 0; i < n; ++i) {
                if (data[i] != (unsigned char)((total + (size_t)i) % 251))
                    return 3;
            }
            total += (size_t)n;
        }
        if (n < 0 || total != 250000 || info.meta_interval != 8192)
            rc = SR_ERROR_RECV_FAILED;
    }
    http_stream_close(&sock);
    close(rmi.abort_pipe[0]);
    close(rmi.abort_pipe[1]);
    socklib_cleanup();
    if (rc < 0)
        fprintf(stderr, "request failed: %d\n", rc);
    return rc < 0 ? 1 : 0;
}
