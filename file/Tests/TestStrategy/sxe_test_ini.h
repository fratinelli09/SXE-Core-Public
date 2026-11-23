/*
 * Minimal .ini reader for the test strategies in C (C contract, sxe_strategy.h):
 * looks for "key = value" in a section. For the tests only.
 */
#ifndef SXE_TEST_INI_H
#define SXE_TEST_INI_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sxe_test_trim(char* s)
{
    size_t n = strlen(s);
    while(n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    size_t i = 0;
    while(s[i] && isspace((unsigned char)s[i]))
        ++i;
    if(i > 0)
        memmove(s, s + i, strlen(s + i) + 1);
}

/* 1 = found (value in out, at most size-1 characters), 0 = not found */
static int sxe_test_ini_get(const char* path, const char* section, const char* key, char* out, size_t size)
{
    FILE* f = fopen(path, "r");
    if(!f)
        return 0;
    char line[512];
    int in_section = 0;
    int found = 0;
    while(!found && fgets(line, sizeof line, f))
    {
        char* hash = strpbrk(line, "#;");
        if(hash)
            *hash = 0;
        sxe_test_trim(line);
        if(line[0] == '[')
        {
            char* end = strchr(line, ']');
            if(end)
                *end = 0;
            in_section = strcmp(line + 1, section) == 0;
            continue;
        }
        char* eq = strchr(line, '=');
        if(!in_section || !eq)
            continue;
        *eq = 0;
        char* value = eq + 1;
        sxe_test_trim(line);
        sxe_test_trim(value);
        if(strcmp(line, key) == 0)
        {
            strncpy(out, value, size - 1);
            out[size - 1] = 0;
            found = 1;
        }
    }
    fclose(f);
    return found;
}

static int sxe_test_ini_double(const char* path, const char* section, const char* key, double* out)
{
    char value[128];
    if(!sxe_test_ini_get(path, section, key, value, sizeof value))
        return 0;
    char* end = NULL;
    const double d = strtod(value, &end);
    if(end == value)
        return 0;
    *out = d;
    return 1;
}

#endif
