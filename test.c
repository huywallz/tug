#include <stdio.h>
#include <stdlib.h>
#include <execinfo.h>

void print_trace(void) {
    void *buffer[4096];
    int nptrs = backtrace(buffer, 4096);
    char **symbols = backtrace_symbols(buffer, nptrs);

    printf("Stack trace:\n");
    for (int i = 0; i < nptrs; i++) {
        printf("  %s\n", symbols[i]);
    }

    free(symbols);
}

int main(int argc, char *argv[]) {
	print_trace();
}