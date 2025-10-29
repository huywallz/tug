#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
	const char* text = "hello";
	size_t len = strlen(text) + 1;
	char* clone = malloc(len);
	char* res = memcpy(clone, text, len);
	printf("%p %p\n", clone, res);
	free(clone);

	return 0;
}