#ifndef ADCSHIM_H
#define ADCSHIM_H
#include <sys/types.h>
// shm_open is VARIADIC; Swift cannot call it with a mode argument (arm64 puts
// variadic args on the stack, so a fixed-signature pointer cast silently drops
// the mode -> the object is created with garbage permissions and the host's
// attach gets EACCES). This C wrapper makes the call with the correct ABI.
int adshm_open_create(const char *name, mode_t mode);
#endif
