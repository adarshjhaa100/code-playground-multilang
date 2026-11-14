/*
    storage util poc with test page size:

    file_size ~ page_size
    fallocate - pre-allocate file
    mmap + fallocate - prevent ENOSPC, map storage to memory - Write to memory = write to disk
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

int main(void) {
    const char* file_path = "temp/temp_test.dat";
    const off_t file_size = 1 * 1024 * 1024;
    const int file_offset = 0;

    //create and open file
    int fd = open(file_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        perror("failure");
        return -1;
    }

    // pre-allocate space. To expand pages upon the same file, change offset. If offset = file_size, file starts expanding
    int ret = posix_fallocate(fd, 0, file_size);
    if (ret != 0) {
        fprintf(stderr, "posix_fallocate failed: %s\n", strerror(ret));
        close(fd);
        return EXIT_FAILURE;
    }
    printf("Successfully preallocated %jd bytes.\n", (intmax_t)file_size);

    // map memory to file - prevent ENOSPC since storage is pre allocated
    // get the address to write to
    void *addr = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return EXIT_FAILURE;
    }

    // cast the address to char* and write byte level data
    char *mem = (char*) addr;
    const char *msg = "Hello, posix_fallocate + mmap!";
    strcpy(mem, msg);
    mem[file_size-2] = 'B';
    printf("Wrote to mapped memory: %s\n", mem);

    // unmap
    if(munmap(addr, file_size) == -1) {
        perror("munmap");
    }
    close(fd);

    return EXIT_SUCCESS;

}
