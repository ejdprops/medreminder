#include "fw_version.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_app_desc.h>

#include "build_info.h"

/* "v0.2.0" at a clean release tag; past it, the commit count since the tag and "*" for uncommitted
 * changes ("v0.2.0-2*"); "v0.2.0-dev" if the latest tag isn't this PROJECT_VER. */
const char *fw_version_short(void)
{
    static char buf[40];
    if (!buf[0]) {
        const char *ver = esp_app_get_description()->version; /* PROJECT_VER */
        char tag[40];
        snprintf(tag, sizeof(tag), "fw-v%s", ver);
        size_t n = strlen(tag);
        if (strncmp(BUILD_GIT_DESC, tag, n) != 0 || (BUILD_GIT_DESC[n] != '\0' && BUILD_GIT_DESC[n] != '-')) {
            snprintf(buf, sizeof(buf), "v%s-dev", ver);
            return buf;
        }
        /* git describe after the tag: "", "-dirty", "-<commits>-g<hash>" or "-<commits>-g<hash>-dirty" */
        const char *rest = BUILD_GIT_DESC + n;
        int commits = strstr(rest, "-g") ? atoi(rest + 1) : 0;
        bool dirty = strstr(rest, "-dirty") != NULL;
        if (commits == 0 && !dirty) {
            snprintf(buf, sizeof(buf), "v%s", ver);
        } else {
            snprintf(buf, sizeof(buf), "v%s-%d%s", ver, commits, dirty ? "*" : "");
        }
    }
    return buf;
}

const char *fw_version_git(void)
{
    return BUILD_GIT_DESC;
}

unsigned fw_version_number(void)
{
    return FW_VERSION_NUMBER;
}
