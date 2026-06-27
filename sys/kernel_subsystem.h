#ifndef KERNEL_SUBSYSTEM_H
#define KERNEL_SUBSYSTEM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_SUBSYSTEMS 16
#define MAX_SUBSYSTEM_FILES 32

typedef struct {
    char name[64];
    int (*read)(char *buffer, int size, int offset);
    int (*write)(const char *buffer, int size, int offset);
} subsystem_file_t;

typedef struct {
    char name[64];
    subsystem_file_t files[MAX_SUBSYSTEM_FILES];
    int file_count;
} kernel_subsystem_t;

void subsystem_register(const char *name, kernel_subsystem_t **out_sub);
void subsystem_add_file(kernel_subsystem_t *sub, const char *name, 
                        int (*read)(char*, int, int), 
                        int (*write)(const char*, int, int));

kernel_subsystem_t* subsystem_get_by_name(const char *name);
int subsystem_get_count(void);
kernel_subsystem_t* subsystem_get_by_index(int index);

#endif
