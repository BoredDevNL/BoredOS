#ifndef MODULE_MANAGER_H
#define MODULE_MANAGER_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    char name[64];
    uint64_t address;
    uint64_t size;
} kernel_module_t;

void module_manager_register(const char *name, uint64_t addr, uint64_t size);
int module_manager_get_count(void);
kernel_module_t* module_manager_get_index(int index);

#endif
