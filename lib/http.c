/* This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <curl/curl.h>
#include "sr_compat.h"
#include "srtypes.h"
#include "socklib.h"
#include "http.h"
#include "http_stream.h"
#include "mchar.h"  /* for substrn_until, etc. */
#include "debug.h"

/******************************************************************************
 * Function prototypes
 *****************************************************************************/
static char* make_auth_header(const char *header_name, 
			      const char *username, const char *password);
static error_code
http_parse_url(const char *url, URLINFO *urlinfo);

/******************************************************************************
 * Private Vars
 *****************************************************************************/
#define MAX_PLS_LEN 8192
#define MAX_M3U_LEN 8192


/******************************************************************************
 * Public functions
 *****************************************************************************/
/* Connect to a shoutcast type stream, leaves when it's about to 
   get the header info */
error_code
http_sc_connect (RIP_MANAGER_INFO* rmi,
		 HSOCKET *sock, const char *url, const char *proxyurl, 
		 SR_HTTP_HEADER *info, char *useragent, char *if_name)
{
    return http_stream_open(rmi, sock, url, proxyurl, info, useragent, if_name);
}

/******************************************************************************
 * Private functions
 *****************************************************************************/
/* Parse only the legacy request/header fields here; libcurl owns transport. */
static error_code
http_parse_url(const char *input, URLINFO *info)
{
    CURLU *url = curl_url();
    char *normalized = strstr(input, "://") ? g_strdup(input) : g_strconcat("http://", input, NULL);
    char *host = NULL, *port = NULL, *path = NULL, *query = NULL;
    char *user = NULL, *password = NULL, *scheme = NULL;
    error_code result = SR_ERROR_INVALID_URL;
    memset(info, 0, sizeof(*info));
    if (!url || curl_url_set(url, CURLUPART_URL, normalized, 0) != CURLUE_OK ||
        curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK ||
        (strcmp(scheme, "http") && strcmp(scheme, "https")) ||
        curl_url_get(url, CURLUPART_HOST, &host, 0) != CURLUE_OK ||
        curl_url_get(url, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) != CURLUE_OK ||
        curl_url_get(url, CURLUPART_PATH, &path, 0) != CURLUE_OK)
        goto done;
    if (g_strlcpy(info->host, host, sizeof(info->host)) >= sizeof(info->host))
        goto done;
    info->port = (u_short)strtoul(port, NULL, 10);
    g_strlcpy(info->path, path, sizeof(info->path));
    if (curl_url_get(url, CURLUPART_QUERY, &query, 0) == CURLUE_OK) {
        g_strlcat(info->path, "?", sizeof(info->path));
        g_strlcat(info->path, query, sizeof(info->path));
    }
    if (curl_url_get(url, CURLUPART_USER, &user, CURLU_URLDECODE) == CURLUE_OK)
        g_strlcpy(info->username, user, sizeof(info->username));
    if (curl_url_get(url, CURLUPART_PASSWORD, &password, CURLU_URLDECODE) == CURLUE_OK)
        g_strlcpy(info->password, password, sizeof(info->password));
    result = SR_SUCCESS;
done:
    curl_free(host);
    curl_free(port);
    curl_free(path);
    curl_free(query);
    curl_free(user);
    curl_free(password);
    curl_free(scheme);
    curl_url_cleanup(url);
    g_free(normalized);
    return result;
}

error_code
http_construct_sc_request(const char *url, const char* proxyurl, char *buffer, char *useragent)
{
    int ret;
    URLINFO ui;
    URLINFO proxyui;
    char myurl[MAX_URL_LEN];
    if ((ret = http_parse_url(url, &ui)) != SR_SUCCESS)
	return ret;

    if (proxyurl) {
	sprintf(myurl, "http://%s:%d%s", ui.host, ui.port, ui.path);
	if ((ret = http_parse_url(proxyurl, &proxyui)) != SR_SUCCESS)
	    return ret;
    } else {
	strcpy(myurl, ui.path);
    }

#if defined (commentout)
    /* This is the old header */
    snprintf(buffer, MAX_HEADER_LEN,
	     "GET %s HTTP/1.0\r\n"
	     "Host: %s:%d\r\n"
	     "User-Agent: %s\r\n"
	     "Icy-MetaData:1\r\n", 
	     myurl, 
	     ui.host, 
	     ui.port, 
	     useragent[0] ? useragent : "Streamripper/1.x");
#endif

    /* This is the header suggested Florian Stoehr */
    snprintf(buffer, MAX_HEADER_LEN,
	     "GET %s HTTP/1.1\r\n"
	     "Accept: */*\r\n"
	     "Cache-Control: no-cache\r\n"
	     "User-Agent: %s\r\n"
	     "Icy-Metadata: 1\r\n"
	     "Connection: close\r\n"
	     "Host: %s:%d\r\n",
	     myurl,
	     useragent[0] ? useragent: "Streamripper/1.x",
	     ui.host,
	     ui.port);

    // http authentication (not proxy, see below for that)
    if (ui.username[0] && ui.password[0]) {
	char *authbuf = make_auth_header("Authorization", 
					 ui.username,
					 ui.password);
	if (g_strlcat(buffer, authbuf, MAX_HEADER_LEN) >= MAX_HEADER_LEN) {
            g_free(authbuf);
            return SR_ERROR_BUFFER_TOO_SMALL;
        }
	g_free(authbuf);
    }

#if defined (commentout)
    // proxy auth stuff
    if (proxyurl && proxyui.username[0] && proxyui.password[0]) {
	char *authbuf = make_auth_header("Proxy-Authorization",
					 proxyui.username,
					 proxyui.password);
	if (g_strlcat(buffer, authbuf, MAX_HEADER_LEN) >= MAX_HEADER_LEN) {
            g_free(authbuf);
            return SR_ERROR_BUFFER_TOO_SMALL;
        }
	g_free(authbuf);
    }
#endif
    /* GCS Testing... Proxy authentication w/o password bug */
    if (proxyurl && proxyui.username[0]) {
	char *authbuf = make_auth_header("Proxy-Authorization",
					 proxyui.username,
					 proxyui.password);
	if (g_strlcat(buffer, authbuf, MAX_HEADER_LEN) >= MAX_HEADER_LEN) {
            g_free(authbuf);
            return SR_ERROR_BUFFER_TOO_SMALL;
        }
	g_free(authbuf);
    }

    
    if (g_strlcat(buffer, "\r\n", MAX_HEADER_LEN) >= MAX_HEADER_LEN)
        return SR_ERROR_BUFFER_TOO_SMALL;

    debug_printf ("SC REQUEST:\n%s", buffer);

    return SR_SUCCESS;
}

// Make the 'Authorization: Basic xxxxxx\r\n' or 'Proxy-Authorization...' 
// headers for the HTTP request.
static char*
make_auth_header (const char *header_name, const char *username, 
		  const char *password) 
{
    char *credentials = g_strdup_printf("%s:%s", username, password);
    char *encoded = g_base64_encode((const guchar *)credentials, strlen(credentials));
    char *header = g_strdup_printf("%s: Basic %s\r\n", header_name, encoded);
    g_free(encoded);
    g_free(credentials);
    return header;
}

// Here we pretend we're IE 5, hehe
error_code
http_construct_page_request (const char *url, BOOL proxyformat, char *buffer)
{
    int ret;
    URLINFO ui;
    char myurl[MAX_URL_LEN];
    if ((ret = http_parse_url(url, &ui)) != SR_SUCCESS)
	return ret;

    if (proxyformat)
	sprintf(myurl, "http://%s:%d%s", ui.host, ui.port, ui.path);
    else
	strcpy(myurl, ui.path);

    snprintf(buffer, MAX_HEADER_LEN,
		"GET %s HTTP/1.0\r\n"
		"Host: %s:%d\r\n"
		"Accept: */*\r\n"
		"Accept-Language: en-us\r\n"
		"Accept-Encoding: gzip, deflate\r\n"
		"User-Agent: Mozilla/4.0 (compatible; MSIE 5.01; Windows NT 5.0)\r\n"
		"Connection: Keep-Alive\r\n\r\n", 
		myurl, 
		ui.host, 
		ui.port);


    return SR_SUCCESS;
}

/* Return 1 if a match was found, 0 if not found */
static int
extract_header_value (char *header, char *dest, char *match, int maxlen)
{
    char *start = header;
    size_t len = strlen (match);
    while (*start) {
	if ((start == header || start[-1] == '\n') &&
            !g_ascii_strncasecmp(start, match, len)) {
	    subnstr_until (start + len, "\n", dest, maxlen);
	    return 1;
	}
	start ++;
    }
    return 0;
}

error_code
http_parse_sc_header (const char *url, char *header, SR_HTTP_HEADER *info)
{
    int rc;
    char *start;
    char versionbuf[64];
    char stempbr[MAX_ICY_STRING] = {0};
    URLINFO url_info;
    int url_path_len;
    int content_type_by_url;

    if (!header || !info)
	return SR_ERROR_INVALID_PARAM;

    /* Parse the url here, before doing memset, because in the 
       recursive case, it is the location referrer */
    rc = http_parse_url (url, &url_info);
    if (rc != SR_SUCCESS) return rc;

    memset(info, 0, sizeof(SR_HTTP_HEADER));

    debug_printf("http header:\n%s\n", header);

    // Get the ICY code.
    start = (char*) strstr (header, "ICY ");
    if (!start) {
	start = (char*) strstr (header, "HTTP/1.");
	if (!start) {
	    debug_printf ("Failed to find ICY or HTTP\n");
	    return SR_ERROR_NO_RESPONSE_HEADER;
	}
    }
    start = strstr (start, " ") + 1;
    sscanf (start, "%i", &info->icy_code);
    if (info->icy_code >= 400) {
	switch (info->icy_code) {
	case 400:
	    return SR_ERROR_HTTP_400_ERROR;
	case 404:
	    return SR_ERROR_HTTP_404_ERROR;
	case 401:
	    return SR_ERROR_HTTP_401_ERROR;
	case 403:
	    return SR_ERROR_HTTP_403_ERROR;
	case 407:
	    return SR_ERROR_HTTP_407_ERROR;
	case 502:
	    return SR_ERROR_HTTP_502_ERROR;
	default:
	    return SR_ERROR_NO_ICY_CODE;
	}
    }

    // read generic headers
    extract_header_value(header, info->http_location, "Location:", 
	sizeof(info->http_location));
    extract_header_value(header, info->server, "Server:", 
	sizeof(info->server));
    rc = extract_header_value(header, info->icy_name, "icy-name:", 
	sizeof(info->icy_name));
    if (rc == 0) {
	/* Icecast 2.0.1 */
	rc = extract_header_value(header, info->icy_name, "ice-name:", 
	    sizeof(info->icy_name));
    }
    info->have_icy_name = rc;
    extract_header_value(header, info->icy_url, "icy-url:", 
	sizeof(info->icy_url));
    rc = extract_header_value(header, stempbr, 
	"icy-br:", sizeof(stempbr));
    if (rc) {
	char *end;
        long bitrate = strtol(stempbr, &end, 10);
        info->icy_bitrate = end != stempbr && bitrate > 0 && bitrate <= 10000 ? (int)bitrate : 0;
    }

    /* interpret the content type from http header */
    rc = extract_header_value(header, stempbr, 
	"Content-Type:", sizeof(stempbr));
    for (char *letter = stempbr; *letter; ++letter)
        *letter = g_ascii_tolower(*letter);
    if (rc == 0) {
	info->content_type = CONTENT_TYPE_UNKNOWN;
    }
    else if (strstr(stempbr,"audio/mpeg")) {
	info->content_type = CONTENT_TYPE_MP3;
    }
    else if (strstr(stempbr,"video/nsv")) {
	info->content_type = CONTENT_TYPE_NSV;
    }
    else if (strstr(stempbr,"misc/ultravox")) {
	info->content_type = CONTENT_TYPE_ULTRAVOX;
    }
    else if ((strstr(stempbr,"application/ogg") || strstr(stempbr,"audio/ogg"))) {
	info->content_type = CONTENT_TYPE_OGG;
    }
    else if (strstr(stempbr,"audio/aac")) {
	info->content_type = CONTENT_TYPE_AAC;
    }
    else if (strstr(stempbr,"audio/x-scpls")) {
	info->content_type = CONTENT_TYPE_PLS;
    }
    else if (strstr(stempbr,"text/html")) {
	if (!info->http_location[0]) {
	    return SR_ERROR_NO_RESPONSE_HEADER;
	}
    }
    else {
	info->content_type = CONTENT_TYPE_UNKNOWN;
    }

    /* Look at url for more content type hints */
    url_path_len = strlen(url_info.path);
    content_type_by_url = CONTENT_TYPE_UNKNOWN;
    if (url_path_len >= 4) {
	if (!strcmp (&url_info.path[url_path_len-4], ".aac")) {
	    content_type_by_url = CONTENT_TYPE_AAC;
	} else if (!strcmp (&url_info.path[url_path_len-4], ".ogg")) {
	    content_type_by_url = CONTENT_TYPE_OGG;
	} else if (!strcmp (&url_info.path[url_path_len-4], ".mp3")) {
	    content_type_by_url = CONTENT_TYPE_MP3;
	} else if (!strcmp (&url_info.path[url_path_len-4], ".nsv")) {
	    content_type_by_url = CONTENT_TYPE_NSV;
	} else if (!strcmp (&url_info.path[url_path_len-4], ".pls")) {
	    content_type_by_url = CONTENT_TYPE_PLS;
	} else if (!strcmp (&url_info.path[url_path_len-4], ".m3u")) {
	    content_type_by_url = CONTENT_TYPE_M3U;
	}
    }

    // Try to guess the server

    // Check for Streamripper relay
    if (strstr(header, "[relay stream]") != NULL) {
	strcpy(info->server, "Streamripper relay server");
    }
    // Check for Shoutcast
    else if ((start = (char *)strstr(header, "SHOUTcast")) != NULL) {
	strcpy(info->server, "SHOUTcast/");
	if ((start = (char *)strstr(start, "Server/")) != NULL) {
	    sscanf(start, "Server/%63[^<]<", versionbuf);
	    strcat(info->server, versionbuf);
	}

    }
    // Check for Icecast 2
    else if (strstr(header, "Icecast 2") != NULL) {
	/* aac on icecast 2.0-2.1 declares content type of audio/mpeg */
	/* In addition, there is at least one stream with a url 
	   radioorenovscotia.ogg, but is actually audio/mpeg */
	if (info->content_type == CONTENT_TYPE_MP3 
	    && content_type_by_url != CONTENT_TYPE_UNKNOWN
	    && content_type_by_url != CONTENT_TYPE_OGG) {
	    info->content_type = content_type_by_url;
	}
    }

    // Check for Icecast 1
    else if ((start = (char *)strstr(header, "icecast")) != NULL) {
	if (!info->server[0]) {
	    strcpy(info->server, "icecast/");
	    if ((start = (char *)strstr(start, "version ")) != NULL) {
		sscanf(start, "version %63[^<]<", versionbuf);
		strcat(info->server, versionbuf);
	    }
	}

	// icecast 1.x headers.
	extract_header_value(header, info->icy_url, "x-audiocast-server-url:",
	    sizeof(info->icy_url));
	rc = extract_header_value(header, info->icy_name, "x-audiocast-name:",
	    sizeof(info->icy_name));
	info->have_icy_name |= rc;
	extract_header_value(header, info->icy_genre, "x-audiocast-genre:",
	    sizeof(info->icy_genre));
	rc = extract_header_value(header, stempbr, "x-audiocast-bitrate:",
	    sizeof(stempbr));
	if (rc) {
	    char *end;
        long bitrate = strtol(stempbr, &end, 10);
        info->icy_bitrate = end != stempbr && bitrate > 0 && bitrate <= 10000 ? (int)bitrate : 0;
	}
    }

    // Check for Apache
    else if (strstr(header, "Apache") && strstr(header, "x-audiocast-name")) {
	// Streaming straight from apache with audiocast headers
	extract_header_value(header, info->icy_url, "x-audiocast-server-url:",
	    sizeof(info->icy_url));
	rc = extract_header_value(header, info->icy_name, "x-audiocast-name:",
	    sizeof(info->icy_name));
	info->have_icy_name |= rc;
	extract_header_value(header, info->icy_genre, "x-audiocast-genre:",
	    sizeof(info->icy_genre));
	rc = extract_header_value(header, stempbr, "x-audiocast-bitrate:",
	    sizeof(stempbr));
	if (rc) {
	    char *end;
        long bitrate = strtol(stempbr, &end, 10);
        info->icy_bitrate = end != stempbr && bitrate > 0 && bitrate <= 10000 ? (int)bitrate : 0;
	}
    }

    /* Last chance to deduce content type */
    if (info->content_type == CONTENT_TYPE_UNKNOWN) {
	if (content_type_by_url == CONTENT_TYPE_UNKNOWN) {
	    info->content_type = CONTENT_TYPE_MP3;
	} else {
	    info->content_type = content_type_by_url;
	}
    }

    debug_printf ("Deduced content type: %d\n", info->content_type);

    // Make sure we don't have any CRLF's at the end of our strings
    trim(info->icy_url);
    trim(info->icy_genre);
    trim(info->icy_name);
    trim(info->http_location);
    trim(info->server);

    //get the meta interval
    if (extract_header_value(header, stempbr, "icy-metaint:", sizeof(stempbr))) {
        char *end;
        long interval = strtol(stempbr, &end, 10);
        info->meta_interval = end != stempbr && interval > 0 && interval <= G_MAXINT ?
            (int)interval : NO_META_INTERVAL;
    } else {
        info->meta_interval = NO_META_INTERVAL;
    }

    return SR_SUCCESS;
}

/* Constructs a HTTP response header from the SR_HTTP_HEADER struct, 
 * if data is null it is not added to the header
 */
error_code
http_construct_sc_response(SR_HTTP_HEADER *info, char *header, int size, int icy_meta_support)
{
    if (!info || !header || size < 1)
        return SR_ERROR_INVALID_PARAM;
    header[0] = '\0';
    GString *response = g_string_new("ICY 200 OK\r\n");
    struct { const char *name; const char *value; } fields[] = {
        {"Location", info->http_location}, {"Server", info->server},
        {"icy-name", info->have_icy_name ? info->icy_name : ""},
        {"icy-url", info->icy_url}, {"icy-genre", info->icy_genre}
    };
    for (size_t i = 0; i < G_N_ELEMENTS(fields); ++i) {
        if (fields[i].value[0]) {
            char *value = g_strdup(fields[i].value);
            g_strdelimit(value, "\r\n", ' ');
            g_string_append_printf(response, "%s:%s\r\n", fields[i].name, value);
            g_free(value);
        }
    }
    if (info->icy_bitrate > 0)
        g_string_append_printf(response, "icy-br:%d\r\n", info->icy_bitrate);
    if (info->meta_interval > 0 && icy_meta_support)
        g_string_append_printf(response, "icy-metaint:%d\r\n", info->meta_interval);
    const char *type = NULL;
    switch (info->content_type) {
    case CONTENT_TYPE_MP3: type = "audio/mpeg"; break;
    case CONTENT_TYPE_NSV: type = "video/nsv"; break;
    case CONTENT_TYPE_OGG: type = "application/ogg"; break;
    case CONTENT_TYPE_ULTRAVOX: type = "misc/ultravox"; break;
    case CONTENT_TYPE_AAC: type = "audio/aac"; break;
    default: break;
    }
    if (type)
        g_string_append_printf(response, "Content-Type: %s\r\n", type);
    g_string_append(response, "\r\n");
    error_code result = SR_ERROR_BUFFER_TOO_SMALL;
    if (response->len < (size_t)size) {
        memcpy(header, response->str, response->len + 1);
        result = SR_SUCCESS;
    }
    g_string_free(response, TRUE);
    return result;
}
