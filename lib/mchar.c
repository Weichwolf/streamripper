/* mchar.c
 * Codeset and wide character processing
 *
 * This program is free software; you can redistribute it and/or modify
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
 *
 * REFS:
 *   http://mail.nl.linux.org/linux-utf8/2001-04/msg00083.html
 *   http://www.cl.cam.ac.uk/~mgk25/unicode.html
 *   http://mail.nl.linux.org/linux-utf8/2001-06/msg00020.html
 *   http://mail.nl.linux.org/linux-utf8/2001-04/msg00254.html
 */
#include "sr_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#if defined HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>
#if defined HAVE_LOCALE_CHARSET
#include <localcharset.h>
#elif defined HAVE_LANGINFO_CODESET
#include <langinfo.h>
#endif
#include <locale.h>
#include <time.h>
#include <errno.h>
#include "debug.h"
#include "srtypes.h"
#include "mchar.h"

#if WIN32
    #define ICONV_WCHAR "UCS-2-INTERNAL"
    #define vsnprintf _vsnprintf
//    #define vswprintf _vsnwprintf
#else
    #define ICONV_WCHAR "WCHAR_T"
    /* This prototype is missing in some systems */
//    int vswprintf (wchar_t * ws, size_t n, const wchar_t * format, va_list arg);
#endif


/*****************************************************************************
 * Public functions
 *****************************************************************************/
char	*left_str(char *str, int len);
char	*subnstr_until(const char *str, char *until, char *newstr, int maxlen);
char	*format_byte_size(char *str, long size);
void	trim(char *str);


/*****************************************************************************
 * These functions are NOT mchar related
 *****************************************************************************/
char*
subnstr_until(const char *str, char *until, char *newstr, int maxlen)
{
    const char *p = str;
    int len = 0;

    for (len = 0; strncmp(p, until, strlen(until)) != 0 && len < maxlen-1; p++)
    {
	newstr[len] = *p;
	len++;
    }
    newstr[len] = '\0';

    return newstr;
}

char *left_str(char *str, int len)
{
    int slen = strlen(str);

    if (slen <= len)
	return str;

    str[len] = '\0';
    return str;
}

char *format_byte_size(char *str, long size)
{
    const long ONE_K = 1024;
    const long ONE_M = ONE_K*ONE_K;

    if (size < ONE_K)
	sprintf(str, "%ldb", size);
    else if (size < ONE_M)
	sprintf(str, "%ldkb", size/ONE_K);
    else 
	sprintf(str, "%.2fM", (float)size/(ONE_M));
	
    return str;
}

void trim(char *str)
{
    char *start = str;
    char *end;
    char *original_end;
    char *test;
    
    /* skip over initial whitespace */
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }
    
    /* locate end of string */
    end = start;
    while (*end) {
        ++end;
    }
    original_end = end;
    
    /* backtrack over final whitespace */
    while (end > start) {
        test = end-1;
        if (isspace((unsigned char)*test)) {
            end = test;
        } else {
            break;
        }
    }
    
    /* move non-whitespace text if initial whitespace was found above. */
    /* move is unnecessary if resulting string is empty */
    if (start > str && start != end) {
        memmove(str, start, end-start);
    }
    
    /* null-terminate resulting string. */
    /* this is necessary in all cases except when the string was not modified */
    if (start > str || end < original_end) {
        str[end-start] = '\0';
    }
}

/* This is a little different from standard strncpy, because:
   1) behavior is known when dst & src overlap
   2) only copy n-1 characters max
   3) then add the null char
*/
void
sr_strncpy (char* dst, char* src, int n)
{
    int i = 0;
    for (i = 0; i < n-1; i++) {
	if (!(dst[i] = src[i])) {
	    return;
	}
    }
    dst[i] = 0;
}

/*****************************************************************************
 * Lstring functions
 *****************************************************************************/



/*****************************************************************************
 * These functions ARE mchar related
 *****************************************************************************/
/* Decode legacy encodings first, then encode from valid UTF-8. GLib owns
 * conversion state and appends the required binary terminators. */
static void
convert_string_with_replacement(gchar **output_string, gsize *output_bytes,
    char *input_string, gsize input_bytes, char *from_codeset,
    char *to_codeset, char *repl)
{
    GString *utf8 = g_string_new(NULL);
    GError *error = NULL;
    *output_string = NULL;
    *output_bytes = 0;
    if (!g_ascii_strcasecmp(from_codeset, "UTF-8")) {
        char *valid = g_utf8_make_valid(input_string, (gssize)input_bytes);
        g_string_append(utf8, valid);
        g_free(valid);
    } else {
        gsize offset = 0;
        while (offset < input_bytes) {
            gsize read = 0, written = 0;
            char *part = g_convert(input_string + offset,
                (gssize)(input_bytes - offset), "UTF-8", from_codeset,
                &read, &written, &error);
            if (part) {
                g_string_append_len(utf8, part, (gssize)written);
                g_free(part);
            } else if (read) {
                /* g_convert does not return partial output on an illegal byte. */
                char *prefix = g_convert(input_string + offset, (gssize)read,
                    "UTF-8", from_codeset, NULL, &written, NULL);
                if (prefix) {
                    g_string_append_len(utf8, prefix, (gssize)written);
                    g_free(prefix);
                }
            }
            offset += read;
            if (error && error->code == G_CONVERT_ERROR_NO_CONVERSION) {
                g_clear_error(&error);
                g_string_free(utf8, TRUE);
                return;
            }
            g_clear_error(&error);
            if (offset < input_bytes) {
                g_string_append(utf8, "\357\277\275");
                ++offset;
            }
        }
    }
    *output_string = g_convert_with_fallback(utf8->str, (gssize)utf8->len,
        to_codeset, "UTF-8", repl, NULL, output_bytes, &error);
    g_clear_error(&error);
    g_string_free(utf8, TRUE);
}

char *
sr_filesystem_path(RIP_MANAGER_INFO *rmi, char *filename)
{
    char *path = NULL;
    gsize length = 0;
    convert_string_with_replacement(&path, &length, filename, strlen(filename),
        "UTF-8", rmi->mchar_cs.codeset_filesys, "?");
    return path;
}

/* Assumes src is valid utf8 */
static int
utf8cpy(gchar *dst, const gchar *src, int capacity)
{
    if (capacity <= 0)
        return 0;
    size_t length = MIN(strlen(src), (size_t)capacity - 1);
    const char *end;
    if (!g_utf8_validate(src, (gssize)length, &end))
        length = (size_t)(end - src);
    memmove(dst, src, length);
    dst[length] = '\0';
    return (int)length;
}

/* Input value mlen is measured in mchar, not bytes.
   Return value is the number of mchar occupied by the converted string, 
   not including the null character. 
   
   For GLIB UTF8, it makes more sense to return the dynamically allocated 
   string, and pass in the codeset string itself rather than 
   rmi & codeset type. 
   
   GLIB UTF8 returns number of bytes.
*/
int
gstring_from_string (RIP_MANAGER_INFO* rmi, mchar* m, int mlen, 
		     char* c, int codeset_type)
{
    CODESET_OPTIONS* mchar_cs = &rmi->mchar_cs;
    if (mlen <= 0) return 0;
    *m = 0;
    if (!c) return 0;

    {
	gchar* gstring;
	gsize gstring_len;
	char* src_codeset;
	int rc;
	
	switch (codeset_type) {
	case CODESET_UTF8:
	    src_codeset = "UTF-8";
	    break;
	case CODESET_LOCALE:
	    src_codeset = mchar_cs->codeset_locale;
	    break;
	case CODESET_FILESYS:
	    src_codeset = mchar_cs->codeset_filesys;
	    break;
	case CODESET_ID3:
	    src_codeset = mchar_cs->codeset_id3;
	    break;
	case CODESET_METADATA:
	    src_codeset = mchar_cs->codeset_metadata;
            if (!g_ascii_strcasecmp(src_codeset, "auto") || !*src_codeset)
                src_codeset = g_utf8_validate(c, -1, NULL) ? "UTF-8" : "WINDOWS-1252";
	    break;
	case CODESET_RELAY:
	    src_codeset = mchar_cs->codeset_relay;
	    break;
	default:
	    printf ("Program error.  Bad codeset m->c (%d)\n", codeset_type);
	    exit (-1);
	}
	/* GCS FIX: This is not correct, as strlen(c) won't work 
	   for UTF-16. */
	convert_string_with_replacement (&gstring, &gstring_len,
					 c, strlen(c),
					 src_codeset, "UTF-8", 
					 "?");
	if (!gstring) {
	    debug_printf ("Error converting gstring_from_string\n");
	    return 0;
	}
	rc = utf8cpy (m, gstring, mlen);
	g_free (gstring);
	return rc;
    }
}

/* Return value is the number of char occupied by the converted string, 
   not including the null character. */
int
string_from_gstring (RIP_MANAGER_INFO* rmi, char* c, int clen, mchar* m, int codeset_type)
{
    CODESET_OPTIONS* mchar_cs = &rmi->mchar_cs;
    if (clen <= 0) return 0;
    *c = 0;
    if (!m) return 0;
    {
	gchar* cstring;
	gsize cstring_len;
	char* tgt_codeset;
	
	switch (codeset_type) {
	case CODESET_UTF8:
	    tgt_codeset = "UTF-8";
	    break;
	case CODESET_LOCALE:
	    tgt_codeset = mchar_cs->codeset_locale;
	    break;
	case CODESET_FILESYS:
	    tgt_codeset = mchar_cs->codeset_filesys;
	    break;
	case CODESET_ID3:
	    tgt_codeset = mchar_cs->codeset_id3;
	    break;
	case CODESET_METADATA:
	    tgt_codeset = mchar_cs->codeset_metadata;
	    break;
	case CODESET_RELAY:
	    tgt_codeset = mchar_cs->codeset_relay;
	    break;
	default:
	    printf ("Program error.  Bad codeset m->c (%d)\n", codeset_type);
	    exit (-1);
	}
    if (!g_ascii_strcasecmp(tgt_codeset, "auto") || !*tgt_codeset)
            tgt_codeset = "UTF-8";
	/* This is the new method */
	convert_string_with_replacement (&cstring, &cstring_len,
					 m, strlen(m),
					 "UTF-8", 
					 tgt_codeset, 
					 "?");
	if (!cstring) {
	    debug_printf ("Error converting string_from_gstring\n");
	    return 0;
	}
        int terminator = g_str_has_prefix(tgt_codeset, "UTF-16") ? 2 : 1;
        if (clen < terminator) {
            g_free(cstring);
            return 0;
        }
        /* Re-encode a shorter source prefix instead of splitting an encoded
         * code point or a UTF-16 surrogate pair. Truncation is uncommon. */
        char *prefix = NULL;
        if (cstring_len > (gsize)(clen - terminator)) {
            prefix = g_utf8_make_valid(m, -1);
            size_t length = strlen(prefix);
            while (cstring_len > (gsize)(clen - terminator) && length) {
                length = (size_t)(g_utf8_prev_char(prefix + length) - prefix);
                prefix[length] = '\0';
                g_free(cstring);
                convert_string_with_replacement(&cstring, &cstring_len,
                    prefix, length, "UTF-8", tgt_codeset, "?");
                if (!cstring) {
                    g_free(prefix);
                    return 0;
                }
            }
        }
        g_free(prefix);
        if (cstring_len > (gsize)(clen - terminator)) {
            g_free(cstring);
            return 0;
        }
        memcpy(c, cstring, cstring_len);
        memset(c + cstring_len, 0, (size_t)terminator);
        g_free(cstring);
        return (int)cstring_len;
    }
}

static const char*
default_codeset (void)
{
    const char* fromcode = 0;

#if defined HAVE_LOCALE_CHARSET
    debug_printf ("Using locale_charset() to get system codeset.\n");
    fromcode = locale_charset ();
#elif defined HAVE_LANGINFO_CODESET
    debug_printf ("Using nl_langinfo() to get system codeset.\n");
    fromcode = nl_langinfo (CODESET);
#else
    debug_printf ("No way to get system codeset.\n");
#endif
    if (!fromcode || !fromcode[0]) {
	debug_printf ("No default codeset, using ISO-8859-1.\n");
	fromcode = "ISO-8859-1";
    } else {
	debug_printf ("Found default codeset %s\n", fromcode);
    }

#if defined (WIN32)
    {
	/* This is just for debugging */
	LCID lcid;
	lcid = GetSystemDefaultLCID ();
	debug_printf ("SystemDefaultLCID: %04x\n", lcid);
	lcid = GetUserDefaultLCID ();
	debug_printf ("UserDefaultLCID: %04x\n", lcid);
    }
#endif

    // #if defined HAVE_ICONV
    //     debug_printf ("Have iconv.\n");
    // #else
    //     debug_printf ("No iconv.\n");
    // #endif

    return fromcode;
}

void
sr_set_locale (void)
{
    setlocale (LC_ALL, "");
    setlocale (LC_CTYPE, "");
    debug_printf ("LOCALE is %s\n",setlocale(LC_ALL,NULL));
}

void
set_codesets_default (CODESET_OPTIONS* cs_opt)
{
    const char* fromcode = 0;

    fromcode = default_codeset();
    g_strlcpy(cs_opt->codeset_locale, fromcode, MAX_CODESET_STRING);
    g_strlcpy(cs_opt->codeset_filesys, "UTF-8", MAX_CODESET_STRING);
    g_strlcpy(cs_opt->codeset_metadata, "auto", MAX_CODESET_STRING);
    g_strlcpy(cs_opt->codeset_relay, "UTF-8", MAX_CODESET_STRING);
    g_strlcpy(cs_opt->codeset_id3, "UTF-16", MAX_CODESET_STRING);
}

void
register_codesets (RIP_MANAGER_INFO* rmi, CODESET_OPTIONS* cs_opt)
{
    CODESET_OPTIONS* mchar_cs = &rmi->mchar_cs;

    /* ID3v2.3 has exactly two text encodings: Latin-1 and UTF-16 with BOM. */
    if (g_ascii_strcasecmp(cs_opt->codeset_id3, "ISO-8859-1"))
        g_strlcpy(cs_opt->codeset_id3, "UTF-16", MAX_CODESET_STRING);

    strcpy (mchar_cs->codeset_locale, cs_opt->codeset_locale);
    strcpy (mchar_cs->codeset_filesys, cs_opt->codeset_filesys);
    strcpy (mchar_cs->codeset_id3, cs_opt->codeset_id3);
    strcpy (mchar_cs->codeset_metadata, cs_opt->codeset_metadata);
    strcpy (mchar_cs->codeset_relay, cs_opt->codeset_relay);
    debug_printf ("Locale codeset: %s\n", mchar_cs->codeset_locale);
    debug_printf ("Filesys codeset: %s\n", mchar_cs->codeset_filesys);
    debug_printf ("ID3 codeset: %s\n", mchar_cs->codeset_id3);
    debug_printf ("Metadata codeset: %s\n", mchar_cs->codeset_metadata);
    debug_printf ("Relay codeset: %s\n", mchar_cs->codeset_relay);
}

/* This is used to set the codeset byte for id3v2 frames */
int
is_id3_unicode (RIP_MANAGER_INFO* rmi)
{
    CODESET_OPTIONS* mchar_cs = &rmi->mchar_cs;
    if (!strcmp ("UTF-16", mchar_cs->codeset_id3)) {
	return 1;
    }
    return 0;
}

void
mstrncpy (mchar* dst, mchar* src, int n)
{
    utf8cpy(dst, src, n);
}

mchar* 
mstrdup (mchar* src)
{
    return strdup (src);
}

mchar* 
mstrcpy (mchar* dest, const mchar* src)
{
    return strcpy (dest, src);
}

size_t
mstrlen (mchar* s)
{
    return strlen (s);
}

/* GCS FIX: gcc can give a warning about vswprintf.  This may require 
   setting gcc -std=c99, or gcc -lang-c99 */
int
msnprintf (mchar* dest, size_t n, const mchar* fmt, ...)
{
    int rc;
    va_list ap;
    va_start (ap, fmt);
    rc = vsnprintf (dest, n, fmt, ap);
    va_end (ap);
    if (n) {
        const char *end;
        if (!g_utf8_validate(dest, -1, &end))
            dest[end - dest] = '\0';
    }
    return rc;
}

mchar*
mstrchr (const mchar* ws, mchar wc)
{
    return strchr(ws, (unsigned char)wc);
}

mchar*
mstrrchr (const mchar* ws, mchar wc)
{
    return strrchr(ws, (unsigned char)wc);
}

mchar*
mstrncat (mchar* ws1, const mchar* ws2, size_t n)
{
    size_t current = strlen(ws1);
    if (n > G_MAXINT - 1)
        n = G_MAXINT - 1;
    utf8cpy(ws1 + current, ws2, (int)n + 1);
    return ws1;
}

int
mstrcmp (const mchar* ws1, const mchar* ws2)
{
    return strcmp (ws1, ws2);
}

long int
mtol (const mchar* string)
{
    return strtol (string, 0, 0);
}
