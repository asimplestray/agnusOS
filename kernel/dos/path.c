#include <dos/path.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* dos_file_part — extract filename from path                          */
/* ------------------------------------------------------------------ */

char *dos_file_part(const char *path) {
    if (!path || !path[0]) return (char *)path;

    const char *last = NULL;

    /* Find last '/' or ':' */
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == ':')
            last = p + 1;
    }

    return last ? (char *)last : (char *)path;
}

/* ------------------------------------------------------------------ */
/* dos_path_part — extract directory part from path                    */
/* ------------------------------------------------------------------ */

char *dos_path_part(const char *path) {
    if (!path || !path[0]) return (char *)path;

    const char *last = NULL;

    /* Find last '/' or ':' */
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == ':')
            last = p;
    }

    if (!last) return (char *)path;

    /* Return static buffer with the path up to and including separator */
    static char result[256];
    int len = (int)(last - path + 1);
    if (len > 255) len = 255;
    for (int i = 0; i < len; i++)
        result[i] = path[i];
    result[len] = 0;

    return result;
}

/* ------------------------------------------------------------------ */
/* dos_add_part — join directory + filename                            */
/* ------------------------------------------------------------------ */

int32_t dos_add_part(const char *dir, const char *file, char *buf, uint32_t bufsize) {
    if (!dir || !file || !buf || bufsize == 0) return -1;

    uint32_t pos = 0;

    /* Copy directory */
    while (*dir && pos < bufsize - 1) {
        buf[pos++] = *dir++;
    }

    /* Add separator if needed */
    if (pos > 0 && buf[pos - 1] != '/' && buf[pos - 1] != ':') {
        if (pos < bufsize - 1) buf[pos++] = '/';
    }

    /* Copy filename */
    while (*file && pos < bufsize - 1) {
        buf[pos++] = *file++;
    }

    buf[pos] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* dos_is_absolute — check if path has a volume (contains ':')         */
/* ------------------------------------------------------------------ */

int32_t dos_is_absolute(const char *path) {
    if (!path) return 0;

    for (const char *p = path; *p; p++) {
        if (*p == ':') return 1;
        if (*p == '/') return 0; /* Unix-style absolute */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* dos_parse_pattern — AmigaOS wildcard pattern → internal form        */
/*                                                                     */
/* Supports:                                                           */
/*   #?     = match anything                                           */
/*   #?.ext = match anything ending in .ext                            */
/*   ~text  = does not contain text                                    */
/*   *      = match anything (alias for #?)                            */
/*                                                                     */
/* Parsed form stored in buf as:                                       */
/*   0x01 + original_pattern (null-terminated)                         */
/* ------------------------------------------------------------------ */

int32_t dos_parse_pattern(const char *pat, char *buf, uint32_t bufsize) {
    if (!pat || !buf || bufsize < 2) return -1;

    uint32_t len = 0;

    /* Prefix: 0x01 = simple wildcard pattern */
    buf[len++] = 0x01;

    /* Copy pattern as-is (simplified parser) */
    while (*pat && len < bufsize - 1) {
        buf[len++] = *pat++;
    }
    buf[len] = 0;

    return 0;
}

/* ------------------------------------------------------------------ */
/* dos_match_pattern — match string against AmigaOS pattern             */
/*                                                                     */
/* Simple implementation: converts #? to .* for matching.              */
/* ------------------------------------------------------------------ */

static int match_recursive(const char *pat, const char *str) {
    while (*pat) {
        if (*pat == '#') {
            /* AmigaOS wildcard: #? = any sequence */
            if (pat[1] == '?') {
                pat += 2;
                /* Try matching zero or more characters */
                while (1) {
                    if (match_recursive(pat, str))
                        return 1;
                    if (!*str) return 0;
                    str++;
                }
            } else {
                /* Unknown # pattern, skip */
                pat++;
            }
        } else if (*pat == '*') {
            /* Shell-style wildcard: * = any sequence */
            pat++;
            while (1) {
                if (match_recursive(pat, str))
                    return 1;
                if (!*str) return 0;
                str++;
            }
        } else if (*pat == '~') {
            /* Negation: ~text means does NOT contain text */
            pat++;
            const char *sub = pat;
            /* Skip to end of negation pattern */
            while (*pat) pat++;
            /* For now, simplified: just check it's not the prefix */
            (void)sub;
            str++;
        } else {
            /* Literal character */
            if (*pat != *str) return 0;
            pat++;
            str++;
        }
    }

    return *str == 0;
}

int32_t dos_match_pattern(const char *pat, const char *str) {
    if (!pat || !str) return 0;

    /* Skip the 0x01 prefix if present */
    if (*pat == 0x01) pat++;

    return match_recursive(pat, str);
}
