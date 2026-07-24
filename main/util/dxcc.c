// DXCC prefix lookup. Compact built-in table, longest-prefix match.
//
// Trade-off: this is a maintenance-light alternative to cty.dat.
// ~190 entries, no date ranges, no portable-suffix DXCC rules
// beyond simple slash splitting. Good enough for at-a-glance
// country tagging on the FT8 decode list.

#include "dxcc.h"
#include <string.h>
#include <ctype.h>

typedef struct { const char *p; const char *name; } dxcc_row_t;

// Order matters only between same-length entries (first match wins
// for equal-length prefixes). Longer prefixes are matched first by
// the lookup loop, so the order across length classes is flexible.
static const dxcc_row_t TBL[] = {
    // ---- USA, Canada, Mexico ----
    { "K",   "USA"   }, { "N",   "USA"   },
    { "W",   "USA"   }, { "AA",  "USA"   },
    { "AB",  "USA"   }, { "AC",  "USA"   },
    { "AD",  "USA"   }, { "AE",  "USA"   },
    { "AF",  "USA"   }, { "AG",  "USA"   },
    { "AH",  "Hawaii"          }, { "AI",  "USA"   },
    { "AJ",  "USA"   }, { "AK",  "USA"   },
    { "AL",  "Alaska"          }, { "NH",  "Hawaii"          },
    { "KH",  "Hawaii"          }, { "WH",  "Hawaii"          },
    { "NL",  "Alaska"          }, { "KL",  "Alaska"          },
    { "WL",  "Alaska"          }, { "KP4", "Puerto Rico"     },
    { "NP",  "Puerto Rico"     }, { "WP",  "Puerto Rico"     },
    { "VE",  "Canada"          }, { "VA",  "Canada"          },
    { "VO",  "Canada"          }, { "VY",  "Canada"          },
    { "XE",  "Mexico"          }, { "XF",  "Mexico"          },
    { "4A",  "Mexico"          }, { "4B",  "Mexico"          },
    { "4C",  "Mexico"          }, { "6D",  "Mexico"          },
    { "6E",  "Mexico"          }, { "6F",  "Mexico"          },
    { "6G",  "Mexico"          }, { "6H",  "Mexico"          },
    { "6I",  "Mexico"          }, { "6J",  "Mexico"          },

    // ---- Caribbean / Central America ----
    { "C6",  "Bahamas"         }, { "CO",  "Cuba"            },
    { "CM",  "Cuba"            }, { "CL",  "Cuba"            },
    { "HH",  "Haiti"           }, { "HI",  "Dom. Rep."  },
    { "J3",  "Grenada"         }, { "J6",  "St. Lucia"       },
    { "J7",  "Dominica"        }, { "J8",  "St. Vincent"     },
    { "FG",  "Guadeloupe"      }, { "FM",  "Martinique"      },
    { "V2",  "Antigua"         }, { "V3",  "Belize"          },
    { "V4",  "St. Kitts"       }, { "VP2", "Br. Caribbean"   },
    { "VP5", "Turks&Caic."  }, { "VP9", "Bermuda"         },
    { "ZF",  "Cayman Is."      }, { "8P",  "Barbados"        },
    { "9Y",  "Trinidad"        }, { "HK",  "Colombia"        },
    { "HJ",  "Colombia"        }, { "YV",  "Venezuela"       },
    { "YW",  "Venezuela"       }, { "YY",  "Venezuela"       },
    { "TI",  "Costa Rica"      }, { "TG",  "Guatemala"       },
    { "YN",  "Nicaragua"       }, { "HP",  "Panama"          },
    { "HR",  "Honduras"        }, { "YS",  "El Salvador"     },

    // ---- South America ----
    { "LU",  "Argentina"       }, { "LO",  "Argentina"       },
    { "LP",  "Argentina"       }, { "LQ",  "Argentina"       },
    { "L2",  "Argentina"       }, { "AY",  "Argentina"       },
    { "AZ",  "Argentina"       }, { "PY",  "Brazil"          },
    { "PP",  "Brazil"          }, { "PQ",  "Brazil"          },
    { "PR",  "Brazil"          }, { "PS",  "Brazil"          },
    { "PT",  "Brazil"          }, { "PU",  "Brazil"          },
    { "PV",  "Brazil"          }, { "PW",  "Brazil"          },
    { "PX",  "Brazil"          }, { "ZZ",  "Brazil"          },
    { "CE",  "Chile"           }, { "CA",  "Chile"           },
    { "CB",  "Chile"           }, { "XQ",  "Chile"           },
    { "XR",  "Chile"           }, { "OA",  "Peru"            },
    { "OB",  "Peru"            }, { "OC",  "Peru"            },
    { "HC",  "Ecuador"         }, { "HD",  "Ecuador"         },
    { "CP",  "Bolivia"         }, { "ZP",  "Paraguay"        },
    { "CX",  "Uruguay"         }, { "PJ",  "Curacao"         },

    // ---- Europe ----
    { "G",   "England"         }, { "M",   "England"         },
    { "2E",  "England"         }, { "GM",  "Scotland"        },
    { "MM",  "Scotland"        }, { "2M",  "Scotland"        },
    { "GW",  "Wales"           }, { "MW",  "Wales"           },
    { "2W",  "Wales"           }, { "GI",  "N. Ireland"      },
    { "MI",  "N. Ireland"      }, { "GJ",  "Jersey"          },
    { "GU",  "Guernsey"        }, { "GD",  "Isle of Man"     },
    { "EI",  "Ireland"         }, { "EJ",  "Ireland"         },
    { "DA",  "Germany"         }, { "DB",  "Germany"         },
    { "DC",  "Germany"         }, { "DD",  "Germany"         },
    { "DE",  "Germany"         }, { "DF",  "Germany"         },
    { "DG",  "Germany"         }, { "DH",  "Germany"         },
    { "DJ",  "Germany"         }, { "DK",  "Germany"         },
    { "DL",  "Germany"         }, { "DM",  "Germany"         },
    { "DN",  "Germany"         }, { "DO",  "Germany"         },
    { "DP",  "Germany"         }, { "DQ",  "Germany"         },
    { "DR",  "Germany"         }, { "F",   "France"          },
    { "TM",  "France"          }, { "TH",  "France"          },
    { "TK",  "Corsica"         }, { "I",   "Italy"           },
    { "IA",  "Italy"           }, { "IB",  "Italy"           },
    { "IC",  "Italy"           }, { "ID",  "Italy"           },
    { "IE",  "Italy"           }, { "IF",  "Italy"           },
    { "IG",  "Italy"           }, { "IH",  "Italy"           },
    { "II",  "Italy"           }, { "IJ",  "Italy"           },
    { "IK",  "Italy"           }, { "IL",  "Italy"           },
    { "IM",  "Sardinia"        }, { "IN",  "Italy"           },
    { "IO",  "Italy"           }, { "IP",  "Italy"           },
    { "IQ",  "Italy"           }, { "IR",  "Italy"           },
    { "IS",  "Sardinia"        }, { "IT",  "Sicily"          },
    { "IU",  "Italy"           }, { "IV",  "Italy"           },
    { "IW",  "Italy"           }, { "IX",  "Italy"           },
    { "IY",  "Italy"           }, { "IZ",  "Italy"           },
    { "EA",  "Spain"           }, { "EB",  "Spain"           },
    { "EC",  "Spain"           }, { "ED",  "Spain"           },
    { "EE",  "Spain"           }, { "EF",  "Spain"           },
    { "EG",  "Spain"           }, { "EH",  "Spain"           },
    { "AM",  "Spain"           }, { "AN",  "Spain"           },
    { "AO",  "Spain"           }, { "EA6", "Balearic Is."    },
    { "EA8", "Canary Is."      }, { "EA9", "Ceuta&Mel." },
    { "CT",  "Portugal"        }, { "CR",  "Portugal"        },
    { "CQ",  "Portugal"        }, { "CS",  "Portugal"        },
    { "CT3", "Madeira"         }, { "CT9", "Madeira"         },
    { "CU",  "Azores"          }, { "PA",  "Netherlands"     },
    { "PB",  "Netherlands"     }, { "PC",  "Netherlands"     },
    { "PD",  "Netherlands"     }, { "PE",  "Netherlands"     },
    { "PF",  "Netherlands"     }, { "PG",  "Netherlands"     },
    { "PH",  "Netherlands"     }, { "PI",  "Netherlands"     },
    { "ON",  "Belgium"         }, { "OO",  "Belgium"         },
    { "OP",  "Belgium"         }, { "OQ",  "Belgium"         },
    { "OR",  "Belgium"         }, { "OS",  "Belgium"         },
    { "OT",  "Belgium"         }, { "LX",  "Luxembourg"      },
    { "HB",  "Switzerland"     }, { "HB0", "Liechtenstein"   },
    { "HB9", "Switzerland"     }, { "OE",  "Austria"         },
    { "OK",  "Czech Rep."  }, { "OL",  "Czech Rep."  },
    { "OM",  "Slovakia"        }, { "HA",  "Hungary"         },
    { "HG",  "Hungary"         }, { "SP",  "Poland"          },
    { "SO",  "Poland"          }, { "SN",  "Poland"          },
    { "SQ",  "Poland"          }, { "SR",  "Poland"          },
    { "3Z",  "Poland"          }, { "HF",  "Poland"          },
    { "YO",  "Romania"         }, { "YP",  "Romania"         },
    { "YQ",  "Romania"         }, { "YR",  "Romania"         },
    { "LZ",  "Bulgaria"        }, { "SV",  "Greece"          },
    { "SW",  "Greece"          }, { "SX",  "Greece"          },
    { "SY",  "Greece"          }, { "SZ",  "Greece"          },
    { "SV9", "Crete"           }, { "SV5", "Dodecanese"      },
    { "SY9", "Mt. Athos"       }, { "5B",  "Cyprus"          },
    { "C4",  "Cyprus"          }, { "H2",  "Cyprus"          },
    { "P3",  "Cyprus"          }, { "TA",  "Turkey"          },
    { "TC",  "Turkey"          }, { "YM",  "Turkey"          },
    { "4J",  "Azerbaijan"      }, { "4K",  "Azerbaijan"      },
    { "EK",  "Armenia"         }, { "4L",  "Georgia"         },
    { "S5",  "Slovenia"        }, { "9A",  "Croatia"         },
    { "E7",  "Bosnia-H."    }, { "YU",  "Serbia"          },
    { "YT",  "Serbia"          }, { "4O",  "Montenegro"      },
    { "Z3",  "N. Macedon."    }, { "ZA",  "Albania"         },
    { "OZ",  "Denmark"         }, { "OY",  "Faroe Is."       },
    { "OX",  "Greenland"       }, { "5P",  "Denmark"         },
    { "5Q",  "Denmark"         }, { "OH",  "Finland"         },
    { "OF",  "Finland"         }, { "OG",  "Finland"         },
    { "OI",  "Finland"         }, { "OJ",  "Market Reef"     },
    { "OH0", "Aland Is."       }, { "LA",  "Norway"          },
    { "LB",  "Norway"          }, { "LC",  "Norway"          },
    { "LF",  "Norway"          }, { "LG",  "Norway"          },
    { "LH",  "Norway"          }, { "LI",  "Norway"          },
    { "LJ",  "Norway"          }, { "LK",  "Norway"          },
    { "LM",  "Norway"          }, { "LN",  "Norway"          },
    { "JW",  "Svalbard"        }, { "JX",  "Jan Mayen"       },
    { "SM",  "Sweden"          }, { "SA",  "Sweden"          },
    { "SB",  "Sweden"          }, { "SC",  "Sweden"          },
    { "SD",  "Sweden"          }, { "SE",  "Sweden"          },
    { "SF",  "Sweden"          }, { "SG",  "Sweden"          },
    { "SH",  "Sweden"          }, { "SI",  "Sweden"          },
    { "SJ",  "Sweden"          }, { "SK",  "Sweden"          },
    { "SL",  "Sweden"          }, { "TF",  "Iceland"         },
    { "R",   "Russia"          }, { "U",   "Russia"          },
    { "RA",  "Russia"          }, { "RZ",  "Russia"          },
    { "UA",  "Russia"          }, { "UB",  "Russia"          },
    { "UC",  "Russia"          }, { "UE",  "Russia"          },
    { "UI",  "Russia"          }, { "RA1", "Russia EU"     },
    { "RA9", "Russia AS"     }, { "RA0", "Russia AS"     },
    { "UR",  "Ukraine"         }, { "US",  "Ukraine"         },
    { "UT",  "Ukraine"         }, { "UU",  "Ukraine"         },
    { "UV",  "Ukraine"         }, { "UW",  "Ukraine"         },
    { "UX",  "Ukraine"         }, { "UY",  "Ukraine"         },
    { "UZ",  "Ukraine"         }, { "EM",  "Ukraine"         },
    { "EN",  "Ukraine"         }, { "EO",  "Ukraine"         },
    { "EU",  "Belarus"         }, { "EV",  "Belarus"         },
    { "EW",  "Belarus"         }, { "ER",  "Moldova"         },
    { "LY",  "Lithuania"       }, { "YL",  "Latvia"          },
    { "ES",  "Estonia"         },

    // ---- Asia ----
    { "JA",  "Japan"           }, { "JE",  "Japan"           },
    { "JF",  "Japan"           }, { "JG",  "Japan"           },
    { "JH",  "Japan"           }, { "JI",  "Japan"           },
    { "JJ",  "Japan"           }, { "JK",  "Japan"           },
    { "JL",  "Japan"           }, { "JM",  "Japan"           },
    { "JN",  "Japan"           }, { "JO",  "Japan"           },
    { "JP",  "Japan"           }, { "JQ",  "Japan"           },
    { "JR",  "Japan"           }, { "JS",  "Japan"           },
    { "7J",  "Japan"           }, { "7K",  "Japan"           },
    { "7L",  "Japan"           }, { "7M",  "Japan"           },
    { "7N",  "Japan"           }, { "8J",  "Japan"           },
    { "8N",  "Japan"           }, { "HL",  "South Korea"     },
    { "DS",  "South Korea"     }, { "DT",  "South Korea"     },
    { "6K",  "South Korea"     }, { "6L",  "South Korea"     },
    { "6M",  "South Korea"     }, { "6N",  "South Korea"     },
    { "P5",  "North Korea"     }, { "BV",  "Taiwan"          },
    { "BU",  "Taiwan"          }, { "BW",  "Taiwan"          },
    { "BX",  "Taiwan"          }, { "B",   "China"           },
    { "BA",  "China"           }, { "BD",  "China"           },
    { "BG",  "China"           }, { "BH",  "China"           },
    { "BI",  "China"           }, { "BJ",  "China"           },
    { "BR",  "China"           }, { "BS",  "China"           },
    { "BT",  "China"           }, { "BY",  "China"           },
    { "BZ",  "China"           }, { "VR",  "Hong Kong"       },
    { "XX9", "Macao"           }, { "VU",  "India"           },
    { "AT",  "India"           }, { "4S",  "Sri Lanka"       },
    { "S2",  "Bangladesh"      }, { "AP",  "Pakistan"        },
    { "6S",  "Pakistan"        }, { "YA",  "Afghanistan"     },
    { "T6",  "Afghanistan"     }, { "EP",  "Iran"            },
    { "EQ",  "Iran"            }, { "YI",  "Iraq"            },
    { "HZ",  "Saudi Arab."    }, { "7Z",  "Saudi Arab."    },
    { "8Z",  "Saudi Arab."    }, { "A4",  "Oman"            },
    { "A6",  "UAE"             }, { "A7",  "Qatar"           },
    { "A9",  "Bahrain"         }, { "4X",  "Israel"          },
    { "4Z",  "Israel"          }, { "OD",  "Lebanon"         },
    { "YK",  "Syria"           }, { "JY",  "Jordan"          },
    { "9K",  "Kuwait"          }, { "YE",  "Indonesia"       },
    { "YB",  "Indonesia"       }, { "YC",  "Indonesia"       },
    { "YD",  "Indonesia"       }, { "YF",  "Indonesia"       },
    { "YG",  "Indonesia"       }, { "YH",  "Indonesia"       },
    { "9M",  "Malaysia"        }, { "9V",  "Singapore"       },
    { "HS",  "Thailand"        }, { "E2",  "Thailand"        },
    { "DU",  "Philippines"     }, { "DV",  "Philippines"     },
    { "DW",  "Philippines"     }, { "DX",  "Philippines"     },
    { "DY",  "Philippines"     }, { "DZ",  "Philippines"     },
    { "3W",  "Vietnam"         }, { "XV",  "Vietnam"         },
    { "XU",  "Cambodia"        }, { "XW",  "Laos"            },
    { "XZ",  "Myanmar"         }, { "UN",  "Kazakhstan"      },
    { "UO",  "Kazakhstan"      }, { "UP",  "Kazakhstan"      },
    { "UQ",  "Kazakhstan"      }, { "EZ",  "Turkmenistan"    },
    { "EX",  "Kyrgyzstan"      }, { "EY",  "Tajikistan"      },
    { "UJ",  "Uzbekistan"      }, { "UK",  "Uzbekistan"      },
    { "UL",  "Uzbekistan"      }, { "JT",  "Mongolia"        },
    { "JU",  "Mongolia"        }, { "JV",  "Mongolia"        },

    // ---- Africa ----
    { "CN",  "Morocco"         }, { "CN0", "Morocco"         },
    { "7X",  "Algeria"         }, { "3V",  "Tunisia"         },
    { "5A",  "Libya"           }, { "SU",  "Egypt"           },
    { "ST",  "Sudan"           }, { "5Z",  "Kenya"           },
    { "5Y",  "Kenya"           }, { "5H",  "Tanzania"        },
    { "5I",  "Tanzania"        }, { "3B8", "Mauritius"       },
    { "3B9", "Rodrigues"       }, { "3B6", "Agalega"         },
    { "FR",  "Reunion"         }, { "5R",  "Madagascar"      },
    { "5T",  "Mauritania"      }, { "6W",  "Senegal"         },
    { "TT",  "Chad"            }, { "TU",  "Ivory Coast"     },
    { "TY",  "Benin"           }, { "TZ",  "Mali"            },
    { "TR",  "Gabon"           }, { "TJ",  "Cameroon"        },
    { "TN",  "Congo"           }, { "9Q",  "DR Congo"        },
    { "9X",  "Rwanda"          }, { "9U",  "Burundi"         },
    { "5N",  "Nigeria"         }, { "5V",  "Togo"            },
    { "5X",  "Uganda"          }, { "7P",  "Lesotho"         },
    { "7Q",  "Malawi"          }, { "A2",  "Botswana"        },
    { "V5",  "Namibia"         }, { "ZS",  "South Africa"    },
    { "ZR",  "South Africa"    }, { "ZT",  "South Africa"    },
    { "ZU",  "South Africa"    }, { "3DA", "Eswatini"        },
    { "C9",  "Mozambique"      }, { "D2",  "Angola"          },
    { "Z2",  "Zimbabwe"        }, { "9J",  "Zambia"          },
    { "V8",  "Brunei"          }, { "EL",  "Liberia"         },
    { "6Y",  "Jamaica"         },

    // ---- Oceania ----
    { "VK",  "Australia"       }, { "AX",  "Australia"       },
    { "VI",  "Australia"       }, { "VJ",  "Australia"       },
    { "VL",  "Australia"       }, { "VM",  "Australia"       },
    { "VN",  "Australia"       }, { "ZL",  "New Zealand"     },
    { "ZK",  "Cook Is."        }, { "ZM",  "New Zealand"     },
    { "FK",  "New Caledonia"   }, { "FO",  "Fr. Polynesia"},
    { "FW",  "Wallis & F." }, { "E5",  "Cook Is."        },
    { "3D2", "Fiji"            }, { "YJ",  "Vanuatu"         },
    { "H4",  "Solomon I."     }, { "T2",  "Tuvalu"          },
    { "T3",  "Kiribati"        }, { "T8",  "Palau"           },
    { "V6",  "Micronesia"      }, { "V7",  "Marshall I."    },
    { "KH8", "Am. Samoa"       }, { "5W",  "Samoa"           },
    { "A3",  "Tonga"           }, { "P2",  "Papua N.G."      },
    { "E6",  "Niue"            },
};
static const int TBL_N = (int)(sizeof(TBL) / sizeof(TBL[0]));

// Extract the lookup head from a callsign:
//   - If "PREFIX/CALL" form, use the part before the slash if it is
//     short (1-3 chars - DXCC convention for portable prefix) else
//     use the part after.
//   - Otherwise use the full call.
//   - Strip trailing /P /M /MM /AM (portable indicators don't change DXCC).
static void normalise_call(const char *in, char *out, size_t cap)
{
    out[0] = '\0';
    if (!in) return;
    // Find slash positions
    const char *slash = strchr(in, '/');
    const char *head = in;
    size_t head_len = (slash ? (size_t)(slash - in) : strlen(in));
    if (slash) {
        const char *tail = slash + 1;
        size_t tail_len = strlen(tail);
        // Strip trailing portable suffix on tail (P/M/MM/AM)
        if (tail_len <= 2) {
            // It's a portable suffix - keep head
        } else if (head_len <= 3) {
            // Head is a DXCC prefix ("W3/G0XYZ") - use head
            // out = head only
            size_t n = head_len;
            if (n >= cap) n = cap - 1;
            memcpy(out, head, n);
            out[n] = '\0';
            return;
        } else {
            // Head is a call ("G0XYZ/W3") - use tail
            size_t n = tail_len;
            if (n >= cap) n = cap - 1;
            memcpy(out, tail, n);
            out[n] = '\0';
            return;
        }
    }
    // No slash, or slash was a portable suffix - use head
    size_t n = head_len;
    if (n >= cap) n = cap - 1;
    memcpy(out, head, n);
    out[n] = '\0';
}

// Entity name -> 3-letter code for the decode list's compact CTY column
// (Roy KI0ER: full names ate the width needed for DT + Hz columns).
// ISO 3166-1 alpha-3 wherever the entity maps to a country; ham entities
// without an ISO code get a recognizable 3-letter tag (HAW, SAR, MKR, ...).
// Keyed on the exact name strings in TBL - keep the two tables in step.
typedef struct { const char *name; const char *a3; } dxcc_a3_row_t;
static const dxcc_a3_row_t A3[] = {
    { "USA", "USA" }, { "Hawaii", "HAW" }, { "Alaska", "ALK" },
    { "Puerto Rico", "PRI" }, { "Canada", "CAN" }, { "Mexico", "MEX" },
    { "Bahamas", "BHS" }, { "Cuba", "CUB" }, { "Haiti", "HTI" },
    { "Dom. Rep.", "DOM" }, { "Grenada", "GRD" }, { "St. Lucia", "LCA" },
    { "Dominica", "DMA" }, { "St. Vincent", "VCT" }, { "Guadeloupe", "GLP" },
    { "Martinique", "MTQ" }, { "Antigua", "ATG" }, { "Belize", "BLZ" },
    { "St. Kitts", "KNA" }, { "Br. Caribbean", "VP2" }, { "Turks&Caic.", "TCA" },
    { "Bermuda", "BMU" }, { "Cayman Is.", "CYM" }, { "Barbados", "BRB" },
    { "Trinidad", "TTO" }, { "Colombia", "COL" }, { "Venezuela", "VEN" },
    { "Costa Rica", "CRI" }, { "Guatemala", "GTM" }, { "Nicaragua", "NIC" },
    { "Panama", "PAN" }, { "Honduras", "HND" }, { "El Salvador", "SLV" },
    { "Argentina", "ARG" }, { "Brazil", "BRA" }, { "Chile", "CHL" },
    { "Peru", "PER" }, { "Ecuador", "ECU" }, { "Bolivia", "BOL" },
    { "Paraguay", "PRY" }, { "Uruguay", "URY" }, { "Curacao", "CUW" },
    { "England", "ENG" }, { "Scotland", "SCT" }, { "Wales", "WLS" },
    { "N. Ireland", "NIR" }, { "Jersey", "JEY" }, { "Guernsey", "GGY" },
    { "Isle of Man", "IMN" }, { "Ireland", "IRL" }, { "Germany", "DEU" },
    { "France", "FRA" }, { "Corsica", "COR" }, { "Italy", "ITA" },
    { "Sardinia", "SAR" }, { "Sicily", "SIC" }, { "Spain", "ESP" },
    { "Balearic Is.", "BAL" }, { "Canary Is.", "CNY" }, { "Ceuta&Mel.", "CEU" },
    { "Portugal", "PRT" }, { "Madeira", "MAD" }, { "Azores", "AZO" },
    { "Netherlands", "NLD" }, { "Belgium", "BEL" }, { "Luxembourg", "LUX" },
    { "Switzerland", "CHE" }, { "Liechtenstein", "LIE" }, { "Austria", "AUT" },
    { "Czech Rep.", "CZE" }, { "Slovakia", "SVK" }, { "Hungary", "HUN" },
    { "Poland", "POL" }, { "Romania", "ROU" }, { "Bulgaria", "BGR" },
    { "Greece", "GRC" }, { "Crete", "CRT" }, { "Dodecanese", "DOD" },
    { "Mt. Athos", "ATH" }, { "Cyprus", "CYP" }, { "Turkey", "TUR" },
    { "Azerbaijan", "AZE" }, { "Armenia", "ARM" }, { "Georgia", "GEO" },
    { "Slovenia", "SVN" }, { "Croatia", "HRV" }, { "Bosnia-H.", "BIH" },
    { "Serbia", "SRB" }, { "Montenegro", "MNE" }, { "N. Macedon.", "MKD" },
    { "Albania", "ALB" }, { "Denmark", "DNK" }, { "Faroe Is.", "FRO" },
    { "Greenland", "GRL" }, { "Finland", "FIN" }, { "Market Reef", "MKR" },
    { "Aland Is.", "ALA" }, { "Norway", "NOR" }, { "Svalbard", "SJM" },
    { "Jan Mayen", "JMY" }, { "Sweden", "SWE" }, { "Iceland", "ISL" },
    { "Russia", "RUS" }, { "Russia EU", "RUS" }, { "Russia AS", "RUS" },
    { "Ukraine", "UKR" }, { "Belarus", "BLR" }, { "Moldova", "MDA" },
    { "Lithuania", "LTU" }, { "Latvia", "LVA" }, { "Estonia", "EST" },
    { "Japan", "JPN" }, { "South Korea", "KOR" }, { "North Korea", "PRK" },
    { "Taiwan", "TWN" }, { "China", "CHN" }, { "Hong Kong", "HKG" },
    { "Macao", "MAC" }, { "India", "IND" }, { "Sri Lanka", "LKA" },
    { "Bangladesh", "BGD" }, { "Pakistan", "PAK" }, { "Afghanistan", "AFG" },
    { "Iran", "IRN" }, { "Iraq", "IRQ" }, { "Saudi Arab.", "SAU" },
    { "Oman", "OMN" }, { "UAE", "ARE" }, { "Qatar", "QAT" },
    { "Bahrain", "BHR" }, { "Israel", "ISR" }, { "Lebanon", "LBN" },
    { "Syria", "SYR" }, { "Jordan", "JOR" }, { "Kuwait", "KWT" },
    { "Indonesia", "IDN" }, { "Malaysia", "MYS" }, { "Singapore", "SGP" },
    { "Thailand", "THA" }, { "Philippines", "PHL" }, { "Vietnam", "VNM" },
    { "Cambodia", "KHM" }, { "Laos", "LAO" }, { "Myanmar", "MMR" },
    { "Kazakhstan", "KAZ" }, { "Turkmenistan", "TKM" }, { "Kyrgyzstan", "KGZ" },
    { "Tajikistan", "TJK" }, { "Uzbekistan", "UZB" }, { "Mongolia", "MNG" },
    { "Morocco", "MAR" }, { "Algeria", "DZA" }, { "Tunisia", "TUN" },
    { "Libya", "LBY" }, { "Egypt", "EGY" }, { "Sudan", "SDN" },
    { "Kenya", "KEN" }, { "Tanzania", "TZA" }, { "Mauritius", "MUS" },
    { "Rodrigues", "ROD" }, { "Agalega", "AGA" }, { "Reunion", "REU" },
    { "Madagascar", "MDG" }, { "Mauritania", "MRT" }, { "Senegal", "SEN" },
    { "Chad", "TCD" }, { "Ivory Coast", "CIV" }, { "Benin", "BEN" },
    { "Mali", "MLI" }, { "Gabon", "GAB" }, { "Cameroon", "CMR" },
    { "Congo", "COG" }, { "DR Congo", "COD" }, { "Rwanda", "RWA" },
    { "Burundi", "BDI" }, { "Nigeria", "NGA" }, { "Togo", "TGO" },
    { "Uganda", "UGA" }, { "Lesotho", "LSO" }, { "Malawi", "MWI" },
    { "Botswana", "BWA" }, { "Namibia", "NAM" }, { "South Africa", "ZAF" },
    { "Eswatini", "SWZ" }, { "Mozambique", "MOZ" }, { "Angola", "AGO" },
    { "Zimbabwe", "ZWE" }, { "Zambia", "ZMB" }, { "Brunei", "BRN" },
    { "Liberia", "LBR" }, { "Jamaica", "JAM" },
    { "Australia", "AUS" }, { "New Zealand", "NZL" }, { "Cook Is.", "COK" },
    { "New Caledonia", "NCL" }, { "Fr. Polynesia", "PYF" }, { "Wallis & F.", "WLF" },
    { "Fiji", "FJI" }, { "Vanuatu", "VUT" }, { "Solomon I.", "SLB" },
    { "Tuvalu", "TUV" }, { "Kiribati", "KIR" }, { "Palau", "PLW" },
    { "Micronesia", "FSM" }, { "Marshall I.", "MHL" }, { "Am. Samoa", "ASM" },
    { "Samoa", "WSM" }, { "Tonga", "TON" }, { "Papua N.G.", "PNG" },
    { "Niue", "NIU" },
};
static const int A3_N = (int)(sizeof(A3) / sizeof(A3[0]));

const char *dxcc_lookup_alpha3(const char *callsign)
{
    const char *name = dxcc_lookup(callsign);
    if (!name) return NULL;
    for (int i = 0; i < A3_N; i++) {
        if (strcmp(A3[i].name, name) == 0) return A3[i].a3;
    }
    return NULL;   // TBL entry with no A3 mapping - keep the tables in step
}

const char *dxcc_lookup(const char *callsign)
{
    if (!callsign) return NULL;
    char head[16];
    normalise_call(callsign, head, sizeof(head));
    // Uppercase
    for (char *p = head; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    }
    size_t hl = strlen(head);
    if (hl == 0) return NULL;

    // Longest-prefix match: try 4..1 char windows
    for (int plen = 4; plen >= 1; plen--) {
        if ((size_t)plen > hl) continue;
        for (int i = 0; i < TBL_N; i++) {
            const char *p = TBL[i].p;
            if ((int)strlen(p) != plen) continue;
            if (strncmp(head, p, plen) == 0) {
                return TBL[i].name;
            }
        }
    }
    return NULL;
}
