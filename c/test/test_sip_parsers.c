#include <stdlib.h>
#include <stdio.h>
#include "../src/sip_parsers.h"

char * test_read_unsigned() {
    ui8 example[] = "0123hi";
    ui8 * start = example, * end = example + 6;
    if (read_unsigned(&start, end) != 123) {
        return "Did not extract correct number";
    }
    if (start != example + 4) {
        return "Start not advanced as expected";
    }
    start = example;
    end = start + 2;
    if (read_unsigned(&start, end) != 1 || start != end) {
        return "Did not respect end bound";
    }
    return NULL;
}

int main() {
    char * err = test_read_unsigned();
    if (err) {
        printf("%s\n", err);
        return 1;
    }
}
