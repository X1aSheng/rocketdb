/* Minimal performance test to verify build system */
#include <stdio.h>
#include <windows.h>

int main(void) {
    printf("RocketDB Performance Test\n");

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);

    /* Measure timing */
    QueryPerformanceCounter(&start);

    /* Dummy work */
    for (int i = 0; i < 1000; i++) {
        volatile int x = i * 2;
        (void)x;
    }

    QueryPerformanceCounter(&end);

    double elapsed_us = (double)(end.QuadPart - start.QuadPart) * 1000000.0 / freq.QuadPart;
    printf("Elapsed: %.2f microseconds\n", elapsed_us);

    return 0;
}
