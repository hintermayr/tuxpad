#include <stdio.h>
#include <stdlib.h>

/* Demo file for tuxpad's Flow Diagram (press F8).
   Several functions with parameters, local variables and return values. */

int square(int n)
{
    int result = n * n;
    return result;
}

int sum_of_squares(int a, int b)
{
    int sa = square(a);
    int sb = square(b);
    int total = sa + sb;
    return total;
}

double average(int count, int total)
{
    double avg = (double) total / count;
    return avg;
}

void report(int a, int b)
{
    int s = sum_of_squares(a, b);
    double avg = average(2, s);
    printf("sum of squares = %d, average = %.1f\n", s, avg);
}

int main(int argc, char **argv)
{
    int x = 3;
    int y = 4;
    if (argc > 2) {
        x = atoi(argv[1]);
        y = atoi(argv[2]);
    }
    report(x, y);
    return 0;
}
