
#ifndef __BUILD_TIME_H
#define __BUILD_TIME_H


#define BUILD_YEAR   ((__DATE__[9]  - '0') * 10 + (__DATE__[10] - '0'))

#define BUILD_MONTH  ( \
    (__DATE__[0] == 'J') ? ((__DATE__[1] == 'a') ? 1 : ((__DATE__[2] == 'n') ? 6 : 7)) : \
    (__DATE__[0] == 'F') ? 2  : \
    (__DATE__[0] == 'M') ? ((__DATE__[2] == 'r') ? 3 : 5) : \
    (__DATE__[0] == 'A') ? ((__DATE__[1] == 'p') ? 4 : 8) : \
    (__DATE__[0] == 'S') ? 9  : \
    (__DATE__[0] == 'O') ? 10 : \
    (__DATE__[0] == 'N') ? 11 : 12)

#define BUILD_DAY    (((__DATE__[4] == ' ') ? 0 : (__DATE__[4] - '0')) * 10 + (__DATE__[5] - '0'))

#define BUILD_HOUR   ((__TIME__[0] - '0') * 10 + (__TIME__[1] - '0'))
#define BUILD_MINUTE ((__TIME__[3] - '0') * 10 + (__TIME__[4] - '0'))
#define BUILD_SECOND ((__TIME__[6] - '0') * 10 + (__TIME__[7] - '0'))

#define BUILD_TIME_STAMP  ((uint32_t)( \
    ((((((uint32_t)BUILD_YEAR * 12u + (uint32_t)(BUILD_MONTH - 1)) * 31u \
        + (uint32_t)(BUILD_DAY - 1)) * 24u + (uint32_t)BUILD_HOUR) * 3600u) \
     + (uint32_t)BUILD_MINUTE * 60u + (uint32_t)BUILD_SECOND)))

#endif
