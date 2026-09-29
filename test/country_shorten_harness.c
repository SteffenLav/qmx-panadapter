/* Host harness for country_shorten() - prints EVERY name in the table at the
 * widths the UI actually uses, so a shortening rule can be judged by reading
 * its output rather than by reasoning about it.
 *
 *   gcc -I main -o /tmp/cs test/country_shorten_harness.c main/util/country_shorten.c
 *   /tmp/cs 10 18 < names.txt
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "util/country_shorten.h"

int main(int argc, char **argv)
{
    int w1 = argc > 1 ? atoi(argv[1]) : 10;
    int w2 = argc > 2 ? atoi(argv[2]) : 18;
    char line[256], a[64], b[64];
    int over1 = 0, over2 = 0, n = 0;
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        n++;
        country_shorten(line, w1, a, sizeof a);
        country_shorten(line, w2, b, sizeof b);
        if ((int)strlen(a) > w1) over1++;
        if ((int)strlen(b) > w2) over2++;
        printf("%-42s | %-*s | %-*s\n", line, w1, a, w2, b);
    }
    fprintf(stderr, "\n%d names; over %d chars: %d; over %d chars: %d\n",
            n, w1, over1, w2, over2);
    return (over1 || over2) ? 1 : 0;
}
