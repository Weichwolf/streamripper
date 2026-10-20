/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "http_stream.h"
#include "findsep.h"
#include "parse.h"
#include "prefs.h"
#include "mchar.h"
#include "audio_frames.h"
#include "cbuf3.h"
#include "http.h"

static void resolve_tests(void)
{
    struct {
        const char *base, *body, *expected;
        int html;
    } cases[] = {
        {"http://localhost/listen.pls?sid=1", "[playlist]\r\nFile1=http://host/stream\r\n",
         "http://host/stream", 0},
        {"https://host/radio/list.m3u", "#EXTM3U\n../stream.mp3\n", "https://host/stream.mp3", 0},
        {"https://host/", "#EXTM3U\nhttps://other/live?token=a=b\n", "https://other/live?token=a=b",
         0},
        {"https://zeno.fm/", "\"https:\\u002F\\u002Fstream.zeno.fm\\u002Fabc123\"",
         "https://stream.zeno.fm/abc123", 1},
        {"https://www.radioswisspop.ch/en",
         "stream_hi_mp3:\"https:\\u002F\\u002Fstream.srg-ssr."
         "ch\\u002Fsrgssr\\u002Frsp\\u002Fmp3\\u002F128\"",
         "https://stream.srg-ssr.ch/srgssr/rsp/mp3/128", 1},
        {"https://host/", "[playlist]\nFile1=file:///etc/passwd\n", NULL, 0},
        {"https://host/", "<html>No player</html>", NULL, 1},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cases); ++i) {
        char *resolved = http_stream_resolve(cases[i].base, cases[i].body, cases[i].html);
        g_assert_cmpstr(resolved, ==, cases[i].expected);
        g_free(resolved);
    }
}

static void metadata_tests(void)
{
    RIP_MANAGER_INFO rmi = {0};
    STREAM_PREFS prefs;
    TRACK_INFO track = {0};
    prefs_get_stream_prefs(&prefs, "stream defaults");
    rmi.prefs = &prefs;
    register_codesets(&rmi, &prefs.cs_opt);
    init_metadata_parser(&rmi, NULL);
    g_strlcpy(track.raw_metadata, "Artist - Progulus Title", sizeof(track.raw_metadata));
    parse_metadata(&rmi, &track);
    g_assert_cmpstr(track.artist, ==, "Artist");
    g_assert_cmpstr(track.title, ==, "Progulus Title");
    parser_free(&rmi);
}

static void unicode_tests(void)
{
    RIP_MANAGER_INFO rmi = {0};
    CODESET_OPTIONS options;
    set_codesets_default(&options);
    register_codesets(&rmi, &options);
    char source[] = "Björk – Jóga / Grüße 東京 🎵";
    char result[256];
    g_assert_cmpint(gstring_from_string(&rmi, result, sizeof(result), source, CODESET_METADATA), >,
                    0);
    g_assert_cmpstr(result, ==, source);
    char legacy[] = "Gr\374\337e \226 \223Musik\224";
    gstring_from_string(&rmi, result, sizeof(result), legacy, CODESET_METADATA);
    g_assert_cmpstr(result, ==, "Grüße – “Musik”");
    char broken[] = "A\xff"
                    "B";
    gstring_from_string(&rmi, result, sizeof(result), broken, CODESET_UTF8);
    g_assert_true(g_utf8_validate(result, -1, NULL));
    for (int capacity = 1; capacity < 32; ++capacity) {
        memset(result, 0x5a, sizeof(result));
        mstrncpy(result, source, capacity);
        g_assert_true(g_utf8_validate(result, -1, NULL));
        g_assert_cmpuint(strlen(result), <, (size_t)capacity);
        g_assert_cmpint(result[capacity], ==, 0x5a);
        int count = string_from_gstring(&rmi, result, capacity, source, CODESET_UTF8);
        g_assert_cmpint(count, ==, (int)strlen(result));
        g_assert_true(g_utf8_validate(result, -1, NULL));
    }
    for (int capacity = 4; capacity < 48; ++capacity) {
        int count = string_from_gstring(&rmi, result, capacity, source, CODESET_ID3);
        g_assert_cmpint(count, <=, capacity - 2);
        if (count) {
            GError *error = NULL;
            char *decoded = g_convert(result, count, "UTF-8", "UTF-16", NULL, NULL, &error);
            g_assert_no_error(error);
            g_assert_nonnull(decoded);
            g_assert_true(g_str_has_prefix(source, decoded));
            g_free(decoded);
        }
    }
    result[0] = 'X';
    g_assert_cmpint(gstring_from_string(&rmi, result, 0, source, CODESET_UTF8), ==, 0);
    g_assert_cmpint(result[0], ==, 'X');
}

typedef error_code (*split_function)(const char *, long, long, long, long, long, long, u_long *,
                                     u_long *);

static void split_tests(const char *fixture_dir)
{
    split_function algorithms[] = {findsep_silence, findsep_silence_2};
    for (size_t algorithm = 0; algorithm < G_N_ELEMENTS(algorithms); ++algorithm) {
        for (int fixture = 0; fixture < 2; ++fixture) {
            const char *name = fixture ? "mono-tone.mp3" : "stereo-gap.mp3";
            char *path = g_build_filename(fixture_dir, name, NULL);
            char *data = NULL;
            gsize length = 0;
            g_assert_true(g_file_get_contents(path, &data, &length, NULL));
            unsigned long end = 0, start = 0;
            int rc =
                algorithms[algorithm](data, (long)length, 1000, 6000, 1000, 0, 0, &end, &start);
            g_assert_cmpint(rc, ==, SR_SUCCESS);
            double seconds = (double)start / (fixture ? 8000.0 : 12000.0);
            printf("split %zu %s: %.3f seconds, bytes %lu/%lu\n", algorithm + 1, name, seconds, end,
                   start);
            g_assert_cmpfloat(seconds, >, fixture ? 2.0 : 3.1);
            g_assert_cmpfloat(seconds, <, fixture ? 6.0 : 4.9);
            g_assert_cmpuint(end + 1, ==, start);
            g_assert_cmpuint(start + 1, <, length);
            g_assert_cmpuint((unsigned char)data[start], ==, 0xff);
            g_assert_cmpuint((unsigned char)data[start + 1] & 0xe0U, ==, 0xe0);
            /* Extreme padding must clamp without unsigned underflow. */
            rc = algorithms[algorithm](data, (long)length, 1000, 6000, 1000, -100000, 100000, &end,
                                       &start);
            g_assert_cmpint(rc, ==, SR_SUCCESS);
            g_assert_cmpuint(end, <, length);
            g_assert_cmpuint(start, <, length);
            g_free(data);
            g_free(path);
        }
        unsigned long end = 0, start = 0;
        g_assert_cmpint(algorithms[algorithm]("garbage", 7, 0, 1000, 100, 0, 0, &end, &start), ==,
                        SR_ERROR_DECODE_FAILURE);
        g_assert_cmpint(algorithms[algorithm](NULL, 0, 0, 1000, 100, 0, 0, &end, &start), ==,
                        SR_ERROR_INVALID_PARAM);
    }
}

static void adts_tests(void)
{
    unsigned char frames[80] = {0};
    /* Ten minimal eight-byte ADTS frames. */
    for (size_t i = 0; i < sizeof(frames); i += 8) {
        unsigned char header[] = {0xff, 0xf1, 0x50, 0x80, 0x01, 0x1f, 0xfc, 0};
        memcpy(frames + i, header, sizeof(header));
    }
    size_t boundary = 0;
    g_assert_true(sr_adts_boundary(frames, sizeof(frames), 43, &boundary));
    g_assert_cmpuint(boundary, ==, 40);
    frames[40] = 0;
    g_assert_true(sr_adts_boundary(frames, sizeof(frames), 43, &boundary));
    g_assert_cmpuint(boundary, ==, 48);
    g_assert_false(sr_adts_boundary(frames, 5, 0, &boundary));
    g_assert_false(sr_adts_boundary(NULL, 5, 0, &boundary));
}

static void buffer_pointer_tests(void)
{
    GList nodes[3] = {0};
    nodes[0].next = &nodes[1];
    nodes[1].prev = &nodes[0];
    nodes[1].next = &nodes[2];
    nodes[2].prev = &nodes[1];
    Cbuf3 buffer = {.chunk_size = 16};
    Cbuf3_pointer start = {.node = &nodes[0], .offset = 7};
    Cbuf3_pointer end = {.node = &nodes[2], .offset = 11};
    unsigned long distance = 0;
    g_assert_cmpint(cbuf3_pointer_subtract(&buffer, &distance, &start, &end), ==, SR_SUCCESS);
    g_assert_cmpuint(distance, ==, 36);
    Cbuf3_pointer next;
    g_assert_cmpint(cbuf3_pointer_add(&buffer, &next, &start, 20), ==, SR_SUCCESS);
    g_assert_true(next.node == &nodes[1]);
    g_assert_cmpuint(next.offset, ==, 11);
}

static void header_tests(void)
{
    SR_HTTP_HEADER info = {
        .content_type = CONTENT_TYPE_MP3, .meta_interval = 8192, .have_icy_name = 1};
    g_strlcpy(info.icy_name, "Grüße 東京", sizeof(info.icy_name));
    char small[16] = {0};
    g_assert_cmpint(http_construct_sc_response(&info, small, sizeof(small), 1), ==,
                    SR_ERROR_BUFFER_TOO_SMALL);
    char header[512];
    g_assert_cmpint(http_construct_sc_response(&info, header, sizeof(header), 1), ==, SR_SUCCESS);
    g_assert_nonnull(strstr(header, "icy-metaint:8192"));
    g_assert_nonnull(strstr(header, "Grüße 東京"));
    char input[] = "HTTP/1.1 200 OK\r\nContent-Type: Audio/AAC\r\nIcy-MetaInt: 16000\r\n\r\n";
    g_assert_cmpint(
        http_parse_sc_header("https://host/live?ref=https://example.org/", input, &info), ==,
        SR_SUCCESS);
    g_assert_cmpint(info.content_type, ==, CONTENT_TYPE_AAC);
    g_assert_cmpint(info.meta_interval, ==, 16000);
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 2);
    resolve_tests();
    header_tests();
    adts_tests();
    buffer_pointer_tests();
    metadata_tests();
    unicode_tests();
    split_tests(argv[1]);
    return 0;
}
