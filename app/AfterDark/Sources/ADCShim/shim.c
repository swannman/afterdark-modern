#include "include/adcshim.h"
#include <sys/mman.h>
#include <fcntl.h>
int adshm_open_create(const char *name, mode_t mode) {
    return shm_open(name, O_CREAT | O_RDWR, mode);
}
