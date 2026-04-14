#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: ./hog <MB>\n");
        return 1;
    }
    int mb = atoi(argv[1]);
    size_t size = (size_t)mb * 1024 * 1024;
    char *buffer = malloc(size);
    if (!buffer) {
        perror("malloc failed");
        return 1;
    }
    memset(buffer, 'A', size); // This line is mandatory for RSS
    printf("Allocated and touched %d MB. Check dmesg now.\n", mb);
    sleep(20);
    return 0;
}
