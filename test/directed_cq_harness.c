/* Host harness for ft8_cq_direction() - prints a verdict for every case so the
 * classifier is judged by reading its output rather than by reasoning about it.
 *
 *   gcc -I main -o /tmp/dcq test/directed_cq_harness.c \
 *       main/ft8_directed_cq.c main/util/geo_coords.c
 *   /tmp/dcq
 *
 * It is the measurement behind the header's claim that the thing is
 * under-confident: every DECLINE printed here has to be one a human would
 * also make, and every MAYBE has to be a case where declining would have cost
 * a contact.
 */
#include <stdio.h>
#include <string.h>

#include "ft8_directed_cq.h"

static const char *verdict(ft8_cq_direction_t d)
{
    switch (d) {
        case FT8_CQ_NOT_DIRECTED: return "plain/unjudged";
        case FT8_CQ_MAYBE_FOR_US: return "answer";
        case FT8_CQ_NOT_FOR_US:   return "DECLINE";
    }
    return "?";
}

struct tc { const char *text; const char *their; const char *mine;
            const char *state; const char *note; };

int main(void)
{
    static const struct tc cases[] = {
      /* --- plain CQs must never be touched ------------------------------- */
      {"CQ OZ1LAV JO65",   "OZ1LAV", "N4OPI",  "", "plain CQ, token is their call"},
      {"CQ K1ABC FN42",    "K1ABC",  "OZ1LAV", "", "plain CQ"},
      {"CQ",               "K1ABC",  "OZ1LAV", "", "bare CQ"},

      /* --- Randy's own two cases, from his email ------------------------- */
      {"CQ DX K1ABC FN42", "K1ABC",  "N4OPI",  "", "Randy: CQ DX from a US call, he is US"},
      {"CQ FL K1ABC FN42", "K1ABC",  "OZ1LAV", "", "Randy: a state he does not live in"},

      /* --- CQ DX: declined ONLY inside our own entity -------------------- */
      {"CQ DX K1ABC FN42", "K1ABC",  "OZ1LAV", "", "DX from USA, we are DNK - we ARE their DX"},
      {"CQ DX OZ7XYZ JO65","OZ7XYZ", "OZ1LAV", "", "DX from our own entity - decline"},
      {"CQ DX JA1XYZ PM95","JA1XYZ", "OZ1LAV", "", "DX from Japan - we are their DX"},
      {"CQ DX K1ABC FN42", NULL,     "OZ1LAV", "", "DX with no caller known - answer"},

      /* --- US state calls ----------------------------------------------- */
      {"CQ TX K1ABC EM12", "K1ABC",  "OZ1LAV", "", "Texas, we are DNK - decline"},
      {"CQ TX K1ABC EM12", "K1ABC",  "N4OPI",  "", "Texas, we are USA - must NOT decline"},
      {"CQ WY K1ABC DN71", "K1ABC",  "N4OPI",  "", "a state he is not in, but he is US"},

      /* --- THE AMBIGUOUS CODES. These are why the guard exists. ---------- */
      {"CQ OK K1ABC EM15", "K1ABC",  "OK1DAB", "", "Oklahoma vs Czechia - Czech op"},
      {"CQ OK K1ABC EM15", "K1ABC",  "OZ1LAV", "", "Oklahoma, Danish op - decline"},
      {"CQ CA K1ABC CM87", "K1ABC",  "CE3ABC", "", "California vs Chile - Chilean op"},
      {"CQ CA K1ABC CM87", "K1ABC",  "OZ1LAV", "", "California, Danish op - decline"},
      /* ⛔ DE IS NEVER JUDGED - it means "from". These two USED to decline for
         the Danish op; Randy N4OPI's log proves that was wrong. */
      {"CQ DE K1ABC FM29", "K1ABC",  "DL1ABC", "", "DE: German op, unjudged"},
      {"CQ DE K1ABC FM29", "K1ABC",  "OZ1LAV", "", "DE: was a decline, now answered"},
      {"CQ DE WB4FIS EM70","WB4FIS", "N4OPI",  "TN", "Randy's actual decode - EM70 is FLORIDA"},
      {"CQ DE KD2UUC FN20","KD2UUC", "N4OPI",  "TN", "from his log - New York"},
      {"CQ DE AJ6CG DM14", "AJ6CG",  "N4OPI",  "TN", "from his log - California"},

      /* --- the new state field ------------------------------------------- */
      {"CQ TX K1ABC EM12", "K1ABC",  "N4OPI",  "TN", "state set, not ours - decline"},
      {"CQ TN K1ABC EM66", "K1ABC",  "N4OPI",  "TN", "our own state - answer"},
      {"CQ TX K1ABC EM12", "K1ABC",  "N4OPI",  "",   "state unset - still answer"},

      /* --- not judged, on purpose --------------------------------------- */
      {"CQ EU OZ1LAV JO65","OZ1LAV", "N4OPI",  "", "continent: no continent table"},
      {"CQ NA K1ABC FN42", "K1ABC",  "OZ1LAV", "", "continent: would be a right decline, but unjudged"},
      {"CQ POTA K1ABC FN42","K1ABC", "OZ1LAV", "", "activity - anyone may answer"},
      {"CQ TEST K1ABC FN42","K1ABC", "OZ1LAV", "", "contest"},
      {"CQ WW K1ABC FN42", "K1ABC",  "OZ1LAV", "", "contest"},
      {"CQ JA JA1XYZ PM95","JA1XYZ", "OZ1LAV", "", "country token - unjudged today"},

      /* --- no callsign of our own: today's behaviour, never a guess ------ */
      {"CQ TX K1ABC EM12", "K1ABC",  "",       "", "our call unset"},
      {"CQ DX K1ABC FN42", "K1ABC",  "ZZ9ZZZ", "", "our prefix unknown to the table"},
    };

    int n = 0, dec = 0;
    printf("%-22s %-8s %-8s %-15s %s\n", "message", "caller", "us", "verdict", "note");
    printf("%-22s %-8s %-8s %-15s %s\n", "----------------------", "--------",
           "--------", "---------------", "----");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char why[96];
        ft8_cq_direction_t d = ft8_cq_direction(cases[i].text, cases[i].their,
                                                cases[i].mine, cases[i].state,
                                                why, sizeof why);
        n++;
        if (d == FT8_CQ_NOT_FOR_US) dec++;
        printf("%-22s %-8s %-8s %-15s %s\n",
               cases[i].text,
               cases[i].their ? cases[i].their : "-",
               cases[i].mine[0] ? cases[i].mine : "(unset)",
               verdict(d), cases[i].note);
        if (why[0]) printf("%-22s %-8s %-8s %-15s -> %s\n", "", "", "", "", why);
    }
    printf("\n%d cases, %d declined\n", n, dec);

    /* The state table itself, against the codes that collide. */
    const char *probe[] = {"OK","CA","DE","FL","TX","DX","EU","NA","ZZ","POTA"};
    printf("\nus_state():");
    for (size_t i = 0; i < sizeof(probe)/sizeof(probe[0]); i++)
        printf(" %s=%d", probe[i], ft8_cq_token_is_us_state(probe[i]) ? 1 : 0);
    printf("\n");
    return 0;
}
