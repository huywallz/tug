#include <stdio.h>
#include <libunwind.h>

void print_stacktrace() {
    unw_context_t ctx;
    unw_cursor_t cursor;
    unw_word_t ip;
    char func[256];

    unw_getcontext(&ctx);
    unw_init_local(&cursor, &ctx);

    printf("Stack trace:\n");
    while (unw_step(&cursor) > 0) {
        unw_get_reg(&cursor, UNW_REG_IP, &ip);
        if (unw_get_proc_name(&cursor, func, sizeof(func), &ip) == 0) {
            printf("0x%lx : %s + 0x%lx\n", (long)ip, func, (long)ip);
        } else {
            printf("0x%lx : -- unknown --\n", (long)ip);
        }
    }
}

void c() { print_stacktrace(); }
void b() { c(); }
void a() { b(); }

int main() {
    a();
    return 0;
}
