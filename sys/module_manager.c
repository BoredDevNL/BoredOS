#include "module_manager.h"
#include "memory_manager.h"

#define MAX_MODULES 32
static kernel_module_t modules[MAX_MODULES];
static int module_count = 0;

static void mod_strcpy(char *dest, const char *src) {
    while (*src) *dest++ = *src++;
    *dest = 0;
}

void module_manager_register(const char *name, uint64_t addr, uint64_t size) {
    if (module_count >= MAX_MODULES) return;
    
    kernel_module_t *m = &modules[module_count++];
    mod_strcpy(m->name, name);
    m->address = addr;
    m->size = size;
}

int module_manager_get_count(void) {
    return module_count;
}

kernel_module_t* module_manager_get_index(int index) {
    if (index < 0 || index >= module_count) return NULL;
    return &modules[index];
}
