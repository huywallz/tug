#include <stdio.h>

void a(int i) {
	char str[i];
	
	printf("%zu\n", sizeof(str));
}

int main(int argc, char *argv[]) {
	a(5);
}