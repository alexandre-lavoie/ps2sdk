#ifndef _MASS_DEBUG_H
#define _MASS_DEBUG_H 1

#ifdef DEBUG
#define XPRINTF(args...) printf(args)
#else
#define XPRINTF(args...) \
    do {                 \
    } while (0)
#endif

#endif /* _MASS_DEBUG_H */
