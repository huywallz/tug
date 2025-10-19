#include <stdio.h>
#include <stdlib.h>
#include "tug.h"
#include "tuglib.h"

static char* read_file(const char* path) {
    FILE* file = fopen(path, "r");
    if (!file) return NULL;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char* buf = malloc(size + 1);
    fread(buf, size, 1, file);
    buf[size] = '\0';

    fclose(file);
    return buf;
}

int main() {
    tug_init();

    char errmsg[2048];
    char* code = read_file("main.tug");
    tug_Task* task = tug_task("main.tug", code, errmsg);
    free(code);
    if (!task) {
        printf("%s\n", errmsg);
        return 1;
    }
    tuglib_loadlibs(task);
    tug_resume(task);
    
    if (tug_getstate(task) == TUG_ERROR) {
        const char* msg = tug_geterr(task);
        printf("%s\n", msg);
    }

    tug_close();
    return 0;
}