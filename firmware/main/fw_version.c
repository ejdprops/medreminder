#include "fw_version.h"

#include <stdio.h>
#include <string.h>

#include <esp_app_desc.h>

#include "build_info.h"

const char *fw_version_short(void)
{
    static char buf[40];
    if (!buf[0]) {
        const char *ver = esp_app_get_description()->version; /* PROJECT_VER */
        char tag[40];
        snprintf(tag, sizeof(tag), "fw-v%s", ver);
        snprintf(buf, sizeof(buf), "v%s%s", ver, strcmp(BUILD_GIT_DESC, tag) == 0 ? "" : "-dev");
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
