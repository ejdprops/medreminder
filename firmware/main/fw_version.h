#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* "v0.2.0" when built exactly at tag fw-v0.2.0 with a clean tree, else "v0.2.0-dev". */
const char *fw_version_short(void);
/* git describe of the build, e.g. "fw-v0.2.0-3-g8b65f67-dirty". */
const char *fw_version_git(void);
/* Matter SoftwareVersion (PROJECT_VER_NUMBER). */
unsigned fw_version_number(void);

#ifdef __cplusplus
}
#endif
