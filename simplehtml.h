#ifndef SIMPLEHTML_H
#define SIMPLEHTML_H

#include <stddef.h>
#include <string.h>

/*
 * Resolve the named character references defined by HTML 4, plus the XML
 * &apos; reference and the commonly emitted &ZeroWidthSpace; reference.
 * Keeping this lookup independent of either renderer lets terminal clients
 * decide how whitespace and invisible characters should be presented.
 */
static int simplehtml_entity_codepoint(const char *name, size_t length,
                                       unsigned long *codepoint)
{
    static const struct {
        const char *name;
        unsigned long codepoint;
    } named[] = {
        {"AElig", 0xc6},
        {"Aacute", 0xc1},
        {"Acirc", 0xc2},
        {"Agrave", 0xc0},
        {"Alpha", 0x391},
        {"Aring", 0xc5},
        {"Atilde", 0xc3},
        {"Auml", 0xc4},
        {"Beta", 0x392},
        {"Ccedil", 0xc7},
        {"Chi", 0x3a7},
        {"Dagger", 0x2021},
        {"Delta", 0x394},
        {"ETH", 0xd0},
        {"Eacute", 0xc9},
        {"Ecirc", 0xca},
        {"Egrave", 0xc8},
        {"Epsilon", 0x395},
        {"Eta", 0x397},
        {"Euml", 0xcb},
        {"Gamma", 0x393},
        {"Iacute", 0xcd},
        {"Icirc", 0xce},
        {"Igrave", 0xcc},
        {"Iota", 0x399},
        {"Iuml", 0xcf},
        {"Kappa", 0x39a},
        {"Lambda", 0x39b},
        {"Mu", 0x39c},
        {"Ntilde", 0xd1},
        {"Nu", 0x39d},
        {"OElig", 0x152},
        {"Oacute", 0xd3},
        {"Ocirc", 0xd4},
        {"Ograve", 0xd2},
        {"Omega", 0x3a9},
        {"Omicron", 0x39f},
        {"Oslash", 0xd8},
        {"Otilde", 0xd5},
        {"Ouml", 0xd6},
        {"Phi", 0x3a6},
        {"Pi", 0x3a0},
        {"Prime", 0x2033},
        {"Psi", 0x3a8},
        {"Rho", 0x3a1},
        {"Scaron", 0x160},
        {"Sigma", 0x3a3},
        {"THORN", 0xde},
        {"Tau", 0x3a4},
        {"Theta", 0x398},
        {"Uacute", 0xda},
        {"Ucirc", 0xdb},
        {"Ugrave", 0xd9},
        {"Upsilon", 0x3a5},
        {"Uuml", 0xdc},
        {"Xi", 0x39e},
        {"Yacute", 0xdd},
        {"Yuml", 0x178},
        {"ZeroWidthSpace", 0x200b},
        {"Zeta", 0x396},
        {"aacute", 0xe1},
        {"acirc", 0xe2},
        {"acute", 0xb4},
        {"aelig", 0xe6},
        {"agrave", 0xe0},
        {"alefsym", 0x2135},
        {"alpha", 0x3b1},
        {"amp", 0x26},
        {"and", 0x2227},
        {"ang", 0x2220},
        {"apos", 0x27},
        {"aring", 0xe5},
        {"asymp", 0x2248},
        {"atilde", 0xe3},
        {"auml", 0xe4},
        {"bdquo", 0x201e},
        {"beta", 0x3b2},
        {"brvbar", 0xa6},
        {"bull", 0x2022},
        {"cap", 0x2229},
        {"ccedil", 0xe7},
        {"cedil", 0xb8},
        {"cent", 0xa2},
        {"chi", 0x3c7},
        {"circ", 0x2c6},
        {"clubs", 0x2663},
        {"cong", 0x2245},
        {"copy", 0xa9},
        {"crarr", 0x21b5},
        {"cup", 0x222a},
        {"curren", 0xa4},
        {"dArr", 0x21d3},
        {"dagger", 0x2020},
        {"darr", 0x2193},
        {"deg", 0xb0},
        {"delta", 0x3b4},
        {"diams", 0x2666},
        {"divide", 0xf7},
        {"eacute", 0xe9},
        {"ecirc", 0xea},
        {"egrave", 0xe8},
        {"empty", 0x2205},
        {"emsp", 0x2003},
        {"ensp", 0x2002},
        {"epsilon", 0x3b5},
        {"equiv", 0x2261},
        {"eta", 0x3b7},
        {"eth", 0xf0},
        {"euml", 0xeb},
        {"euro", 0x20ac},
        {"exist", 0x2203},
        {"fnof", 0x192},
        {"forall", 0x2200},
        {"frac12", 0xbd},
        {"frac14", 0xbc},
        {"frac34", 0xbe},
        {"frasl", 0x2044},
        {"gamma", 0x3b3},
        {"ge", 0x2265},
        {"gt", 0x3e},
        {"hArr", 0x21d4},
        {"harr", 0x2194},
        {"hearts", 0x2665},
        {"hellip", 0x2026},
        {"iacute", 0xed},
        {"icirc", 0xee},
        {"iexcl", 0xa1},
        {"igrave", 0xec},
        {"image", 0x2111},
        {"infin", 0x221e},
        {"int", 0x222b},
        {"iota", 0x3b9},
        {"iquest", 0xbf},
        {"isin", 0x2208},
        {"iuml", 0xef},
        {"kappa", 0x3ba},
        {"lArr", 0x21d0},
        {"lambda", 0x3bb},
        {"lang", 0x2329},
        {"laquo", 0xab},
        {"larr", 0x2190},
        {"lceil", 0x2308},
        {"ldquo", 0x201c},
        {"le", 0x2264},
        {"lfloor", 0x230a},
        {"lowast", 0x2217},
        {"loz", 0x25ca},
        {"lrm", 0x200e},
        {"lsaquo", 0x2039},
        {"lsquo", 0x2018},
        {"lt", 0x3c},
        {"macr", 0xaf},
        {"mdash", 0x2014},
        {"micro", 0xb5},
        {"middot", 0xb7},
        {"minus", 0x2212},
        {"mu", 0x3bc},
        {"nabla", 0x2207},
        {"nbsp", 0xa0},
        {"ndash", 0x2013},
        {"ne", 0x2260},
        {"ni", 0x220b},
        {"not", 0xac},
        {"notin", 0x2209},
        {"nsub", 0x2284},
        {"ntilde", 0xf1},
        {"nu", 0x3bd},
        {"oacute", 0xf3},
        {"ocirc", 0xf4},
        {"oelig", 0x153},
        {"ograve", 0xf2},
        {"oline", 0x203e},
        {"omega", 0x3c9},
        {"omicron", 0x3bf},
        {"oplus", 0x2295},
        {"or", 0x2228},
        {"ordf", 0xaa},
        {"ordm", 0xba},
        {"oslash", 0xf8},
        {"otilde", 0xf5},
        {"otimes", 0x2297},
        {"ouml", 0xf6},
        {"para", 0xb6},
        {"part", 0x2202},
        {"permil", 0x2030},
        {"perp", 0x22a5},
        {"phi", 0x3c6},
        {"pi", 0x3c0},
        {"piv", 0x3d6},
        {"plusmn", 0xb1},
        {"pound", 0xa3},
        {"prime", 0x2032},
        {"prod", 0x220f},
        {"prop", 0x221d},
        {"psi", 0x3c8},
        {"quot", 0x22},
        {"rArr", 0x21d2},
        {"radic", 0x221a},
        {"rang", 0x232a},
        {"raquo", 0xbb},
        {"rarr", 0x2192},
        {"rceil", 0x2309},
        {"rdquo", 0x201d},
        {"real", 0x211c},
        {"reg", 0xae},
        {"rfloor", 0x230b},
        {"rho", 0x3c1},
        {"rlm", 0x200f},
        {"rsaquo", 0x203a},
        {"rsquo", 0x2019},
        {"sbquo", 0x201a},
        {"scaron", 0x161},
        {"sdot", 0x22c5},
        {"sect", 0xa7},
        {"shy", 0xad},
        {"sigma", 0x3c3},
        {"sigmaf", 0x3c2},
        {"sim", 0x223c},
        {"spades", 0x2660},
        {"sub", 0x2282},
        {"sube", 0x2286},
        {"sum", 0x2211},
        {"sup", 0x2283},
        {"sup1", 0xb9},
        {"sup2", 0xb2},
        {"sup3", 0xb3},
        {"supe", 0x2287},
        {"szlig", 0xdf},
        {"tau", 0x3c4},
        {"there4", 0x2234},
        {"theta", 0x3b8},
        {"thetasym", 0x3d1},
        {"thinsp", 0x2009},
        {"thorn", 0xfe},
        {"tilde", 0x2dc},
        {"times", 0xd7},
        {"trade", 0x2122},
        {"uArr", 0x21d1},
        {"uacute", 0xfa},
        {"uarr", 0x2191},
        {"ucirc", 0xfb},
        {"ugrave", 0xf9},
        {"uml", 0xa8},
        {"upsih", 0x3d2},
        {"upsilon", 0x3c5},
        {"uuml", 0xfc},
        {"weierp", 0x2118},
        {"xi", 0x3be},
        {"yacute", 0xfd},
        {"yen", 0xa5},
        {"yuml", 0xff},
        {"zeta", 0x3b6},
        {"zwj", 0x200d},
        {"zwnj", 0x200c}
    };
    size_t i;

    if (!name || !length || !codepoint)
        return 0;

    if (name[0] == '#') {
        unsigned long value = 0;
        unsigned int base = 10;
        size_t pos = 1;

        if (pos < length && (name[pos] == 'x' || name[pos] == 'X')) {
            base = 16;
            pos++;
        }
        if (pos == length)
            return 0;

        for (; pos < length; pos++) {
            unsigned int digit;
            unsigned char c = (unsigned char)name[pos];

            if (c >= '0' && c <= '9')
                digit = (unsigned int)(c - '0');
            else if (base == 16 && c >= 'a' && c <= 'f')
                digit = (unsigned int)(c - 'a' + 10);
            else if (base == 16 && c >= 'A' && c <= 'F')
                digit = (unsigned int)(c - 'A' + 10);
            else
                return 0;
            if (digit >= base ||
                value > (0x10ffffUL - digit) / (unsigned long)base)
                return 0;
            value = value * (unsigned long)base + digit;
        }

        if (!value || (value >= 0xd800 && value <= 0xdfff))
            return 0;
        *codepoint = value;
        return 1;
    }

    for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (strlen(named[i].name) == length &&
            !memcmp(named[i].name, name, length)) {
            *codepoint = named[i].codepoint;
            return 1;
        }
    }
    return 0;
}

static int simplehtml_codepoint_is_text_space(unsigned long codepoint)
{
    return codepoint == 0xa0 || codepoint == 0x2002 ||
           codepoint == 0x2003 || codepoint == 0x2009;
}

#endif
