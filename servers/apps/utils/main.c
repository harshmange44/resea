#include <resea/printf.h>
#include <resea/cmdline.h>
#include <string.h>
#include "http.h"
#include "fs.h"

void main(const char *cmdline) {
    char *cmd, *url, *path, *data;
    cmdline_cmd("http-get");
    cmdline_arg(&url, "http-get", "url", false);

    cmdline_cmd("fs-read");
    cmdline_arg(&path, "fs-read", "path", false);

    cmdline_cmd("fs-write");
    cmdline_arg(&path, "fs-write", "path", false);
    cmdline_arg(&data, "fs-write", "data", false);

    cmdline_parse(cmdline, &cmd);

    if (!strcmp(cmd, "http-get")) {
        http_get(url);
    } else if (!strcmp(cmd, "fs-read")) {
        fs_read(path);
    } else if (!strcmp(cmd, "fs-write")) {
        fs_write(path, (uint8_t *) data, strlen(data));
    }
}
