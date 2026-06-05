#ifndef __CSV_LUT_H__
#define __CSV_LUT_H__

#include <string.h>

#define CSV_LUT_SIZE 255

#define CSV_DEFAULT_DELIMITER ','
#define CSV_DEFAULT_QUALIFIER '"'
#define CSV_DEFAULT_NEWLINE '\n'

#define CSV_LUT_TOKEN 0u
#define CSV_LUT_DELIMITER (1u << 0)
#define CSV_LUT_QUALIFIER (1u << 1)
#define CSV_LUT_NEWLINE (1u << 2)

#define CSV_LUT_IDX_DELIMITER(ch) [(unsigned char)(ch)]
#define CSV_LUT_IDX_QUALIFIER(ch) [(unsigned char)(ch)]
#define CSV_LUT_IDX_NEWLINE(ch) [(unsigned char)(ch)]

#define CSV_LUT_INIT(delim, qual, newline) \
    {                                        \
        CSV_LUT_IDX_DELIMITER(delim) = CSV_LUT_DELIMITER, \
        CSV_LUT_IDX_QUALIFIER(qual) = CSV_LUT_QUALIFIER,   \
        CSV_LUT_IDX_NEWLINE(newline) = CSV_LUT_NEWLINE     \
    }

extern const unsigned char csv_default_class_lut[CSV_LUT_SIZE];

void csv_lut_build(unsigned char lut[CSV_LUT_SIZE], unsigned char d,
                   unsigned char q, unsigned char nl);

static inline unsigned char csv_lut_index(unsigned char c)
{
    return (unsigned char)(c == 255 ? 0 : c);
}

static inline const unsigned char *csv_lut_resolve(
    unsigned char scratch[CSV_LUT_SIZE], unsigned char d, unsigned char q,
    unsigned char nl)
{
    if (d == (unsigned char)CSV_DEFAULT_DELIMITER &&
        q == (unsigned char)CSV_DEFAULT_QUALIFIER &&
        nl == (unsigned char)CSV_DEFAULT_NEWLINE) {
        return csv_default_class_lut;
    }

    csv_lut_build(scratch, d, q, nl);
    return scratch;
}

#endif