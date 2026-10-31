#include<stdio.h>
#include <unistd.h>

int main() {
    // printf("\nHello, world!\n");
    // printf("\nPID, %d, PPID %d\n", getpid(), getppid());
    int a[1000000] = {0};
    int sum1 = 0, sum2 = 0;
    for(int i = 0; i < 1000000; i++) {
        a[i] = (i+29)%97;
    }
    for(int i = 0; i < 1000000; i++) {
        sum1 += a[i];
    }
    for(int i = 0; i < 1000000; i += 16) {
        sum2 += a[i];
    }

    printf("\nSum1=%d, Sum2=%d\n", sum1, sum2);

    return 0;
}
