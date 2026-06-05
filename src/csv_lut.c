#include "../include/csv_lut.h"

const unsigned char csv_default_class_lut[CSV_LUT_SIZE] =
    CSV_LUT_INIT(CSV_DEFAULT_DELIMITER, CSV_DEFAULT_QUALIFIER,
                 CSV_DEFAULT_NEWLINE);

void csv_lut_build(unsigned char lut[CSV_LUT_SIZE], unsigned char d,
                   unsigned char q, unsigned char nl)
{
    memset(lut, CSV_LUT_TOKEN, CSV_LUT_SIZE);
    lut[csv_lut_index(d)] = CSV_LUT_DELIMITER;
    lut[csv_lut_index(q)] = CSV_LUT_QUALIFIER;
    lut[csv_lut_index(nl)] = CSV_LUT_NEWLINE;
}
