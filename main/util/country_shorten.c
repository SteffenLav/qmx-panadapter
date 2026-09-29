// Shorten a country name to fit a column, WITHOUT falling back to a code.
//
// ⛔ THIS REVERSES A RULE THIS PROJECT HELD UNTIL v1.15.0, DELIBERATELY.
// country_display() used to spell the name out where it fitted and return the
// 3-letter ISO code where it did not - "Netherlands" or "NLD", never
// "Netherlan" - on the reasoning that a clipped name reads as a bug while a
// code is simply the shorter true answer. Operator, 2026-09-19, looking at a
// list of them: "I hate to see those 3 char countries in the list - for me its
// apples and pears - please cut them off intelligently".
//
// He is right that MIXING the two is the real fault: a column reading
// "Sweden / Ireland / NLD / Italy" asks the reader to switch alphabets
// mid-column. One consistent kind of answer beats one that is occasionally
// more precise.
//
// ⭐ "Intelligently" is four steps, in order, each one cheaper in meaning than
// the last. Every rule below was chosen against the ACTUAL 340 names in Uwe
// DL8UG's table, counted by how many prefixes carry them - not from
// imagination. The data is already half-abbreviated by him ("Dem. Rep. of
// Congo", "Balearic Is."), so the work left is mostly parentheticals and a
// handful of very common long ones.
//
// Portable - no ESP dependencies - so test/country_shorten_harness.c can print
// the whole table at every width the UI actually uses.

#include "country_shorten.h"
#include <string.h>
#include <stddef.h>

/* Step 2. Names whose natural short form is a WORD, not an abbreviation. Kept
 * deliberately short: every entry here is a judgement someone has to agree
 * with, and the generic rules below handle the rest without one. Ordered by
 * how many prefixes carry the long form (measured), so the common cases are
 * the ones that got the attention. */
static const struct { const char *full; const char *shrt; } k_short[] = {
    { "United States of America",   "USA"          },  /* 1178 prefixes */
    { "European Russia",            "Russia EU"    },  /* 3221 - the region is Uwe's whole point, and it
                                                          fits 10 without brackets; "Russia" alone threw
                                                          away the only thing the split is FOR */
    { "Asiatic Russia",             "Russia AS"    },  /* 1450 */
    { "Dem. Rep. of Congo",         "DR Congo"     },
    { "Fed. Rep. of Germany",       "Germany"      },
    { "Republic of Korea",          "South Korea"  },
    { "Dem. People's Rep. of Korea","North Korea"  },
    { "Slovak Republic",            "Slovakia"     },
    { "United Arab Emirates",       "UAE"          },
    { "Papua New Guinea",           "Papua N.G."   },
    { "Bosnia-Herzegovina",         "Bosnia"       },
    { "Dominican Republic",         "Dominican Rep." },
    { "Central African Republic",   "C. Afr. Rep." },
    { "Equatorial Guinea",          "Eq. Guinea"   },
    { "Trinidad & Tobago",          "Trinidad"     },
    { "Antigua & Barbuda",          "Antigua"      },
    { "St. Kitts & Nevis",          "St. Kitts"    },
    /* Measured: these are the names a 10-character column would otherwise cut
     * mid-word, in the order the harness reports them. A real short form beats
     * a period every time - "N. Zealand" is a name, "New Zeala." is a stump. */
    { "New Zealand",                "N. Zealand"   },
    { "Saudi Arabia",               "S. Arabia"    },
    { "El Salvador",                "Salvador"     },
    { "Timor-Leste",                "Timor"        },
    { "Sint Maarten",               "S. Maarten"   },
    { "New Caledonia",              "N. Caledonia" },
    { "East Malaysia",              "E. Malaysia"  },
    { "West Malaysia",              "W. Malaysia"  },
    { "Cote d'Ivoire",              "Ivory Coast"  },
    { "Franz Josef Land",           "Franz Josef"  },
    { "Mount Athos",                "Mt. Athos"    },
    { "Juan Fernandez Is.",         "Juan Fdez."   },
    { "New Zealand Subantarctic Islands", "NZ Subant." },
    { "Isle of Man",                "I. of Man"    },
    { "DPR of Korea",               "N. Korea"     },
    { "Juan de Nova, Europa",       "Juan de Nova" },
};

/* Step 3. Generic word abbreviations, longest first so "Islands" is tried
 * before "Island". These only ever SHORTEN, and each keeps the word readable -
 * "Is." for Islands is the form Uwe's own data already uses. */
static const struct { const char *from; const char *to; } k_abbrev[] = {
    { " Islands",    " Is."   },
    { " Island",     " I."    },
    { "Republic of ", ""      },
    { " Republic",   " Rep."  },
    { "Democratic ", "Dem. "  },
    { "Federation",  "Fed."   },
    { "Territory",   "Terr."  },
    { "Southern ",   "S. "    },
    { "Northern ",   "N. "    },
    { "Western ",    "W. "    },
    { "Eastern ",    "E. "    },
    { "Central ",    "C. "    },
    { "South ",      "S. "    },
    { "North ",      "N. "    },
    { "Saint ",      "St. "   },
    { " and ",       " & "    },
};

static void cut_at_word(char *s, int max_chars)
{
    int n = (int)strlen(s);
    if (n <= max_chars) return;
    /* Prefer a space at or before the limit, so a name breaks between words
     * rather than mid-syllable. Only if that leaves something worth reading -
     * otherwise a hard cut is better than one surviving letter. */
    int cut = -1;
    for (int i = max_chars; i > max_chars / 2; i--) {
        if (s[i] == ' ' || s[i] == '-') { cut = i; break; }
    }
    if (cut >= 0) {
        /* Broke between words - that already reads as a name, no marker. */
        while (cut > 0 && (s[cut - 1] == ' ' || s[cut - 1] == ',' ||
                           s[cut - 1] == '-' || s[cut - 1] == '&')) cut--;
        s[cut] = '\0';
        /* ⛔ NEVER END ON A CONNECTIVE. The cut above is purely positional, so
         * it happily produced "Isle of", "DPR of" and "Juan de" - each of which
         * reads as a sentence someone forgot to finish, and none of which is a
         * place. Dropping a trailing joining word is one rule that fixes all of
         * them, and it can only ever make the answer shorter and cleaner. */
        static const char *k_tail[] = { " of", " de", " du", " da", " la",
                                        " and", " &", " the", " le" };
        for (;;) {
            int len = (int)strlen(s), hit = 0;
            for (size_t i = 0; i < sizeof(k_tail) / sizeof(k_tail[0]); i++) {
                int tl = (int)strlen(k_tail[i]);
                if (len > tl && strcmp(s + len - tl, k_tail[i]) == 0) {
                    s[len - tl] = '\0';
                    hit = 1;
                    break;
                }
            }
            if (!hit) break;
        }
        return;
    }
    /* ⭐ A MID-WORD CUT GETS A TRAILING PERIOD, and that full stop is the whole
     * difference between "Netherl." and "Netherlan". The first is plainly an
     * abbreviation; the second reads as a bug - which is exactly the objection
     * that made this project print 3-letter codes in the first place. The
     * operator wants names rather than codes in a narrow column, so the cut has
     * to announce itself instead of pretending to be a whole word. */
    cut = max_chars - 1;
    while (cut > 0 && (s[cut - 1] == ' ' || s[cut - 1] == ',' ||
                       s[cut - 1] == '-' || s[cut - 1] == '&' ||
                       s[cut - 1] == '.')) cut--;
    s[cut] = '.';
    s[cut + 1] = '\0';
}

void country_shorten(const char *full, int max_chars, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!full || !full[0] || max_chars <= 0) return;
    if (max_chars > (int)out_sz - 1) max_chars = (int)out_sz - 1;

    /* Step 1. Drop a parenthetical. Every name over 27 characters in the table
     * is one of these - "Clipperton I. (no distinct ISO code; French
     * sovereignty)" - and what precedes the bracket is already the answer. */
    size_t n = strlen(full);
    const char *par = strstr(full, " (");
    if (par) n = (size_t)(par - full);
    if (n > out_sz - 1) n = out_sz - 1;
    memcpy(out, full, n);
    out[n] = '\0';
    if ((int)strlen(out) <= max_chars) return;

    /* Step 2. A known short form for the whole name. */
    for (size_t i = 0; i < sizeof(k_short) / sizeof(k_short[0]); i++) {
        if (strcmp(out, k_short[i].full) == 0) {


            size_t l = strlen(k_short[i].shrt);
            if (l > out_sz - 1) l = out_sz - 1;
            memcpy(out, k_short[i].shrt, l);
            out[l] = '\0';
            break;
        }
    }
    if ((int)strlen(out) <= max_chars) return;

    /* Step 3. Generic abbreviations, applied until it fits or they run out. */
    for (size_t i = 0; i < sizeof(k_abbrev) / sizeof(k_abbrev[0]); i++) {
        char *hit = strstr(out, k_abbrev[i].from);
        if (!hit) continue;
        size_t flen = strlen(k_abbrev[i].from), tlen = strlen(k_abbrev[i].to);
        memmove(hit + tlen, hit + flen, strlen(hit + flen) + 1);
        memcpy(hit, k_abbrev[i].to, tlen);
        if ((int)strlen(out) <= max_chars) return;
    }

    /* Step 4. Cut, on a word boundary where there is one. */
    cut_at_word(out, max_chars);
}
