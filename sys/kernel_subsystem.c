#include "kernel_subsystem.h"
#include "memory_manager.h"
#include "spinlock.h"
#include "kutils.h"


static kernel_subsystem_t subsystems[MAX_SUBSYSTEMS];
static int subsystem_count = 0;
static spinlock_t sub_lock = SPINLOCK_INIT;


static void sub_strcpy(char *dest, const char *src) {
    while (*src) *dest++ = *src++;
    *dest = 0;
}

static int sub_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

void subsystem_register(const char *name, kernel_subsystem_t **out_sub) {
    uint64_t flags = spinlock_acquire_irqsave(&sub_lock);
    
    if (subsystem_count >= MAX_SUBSYSTEMS) {
        spinlock_release_irqrestore(&sub_lock, flags);
        if (out_sub) *out_sub = NULL;
        return;
    }

    // Check if already exists
    for (int i = 0; i < subsystem_count; i++) {
        if (sub_strcmp(subsystems[i].name, name) == 0) {
            spinlock_release_irqrestore(&sub_lock, flags);
            if (out_sub) *out_sub = &subsystems[i];
            return;
        }
    }

    kernel_subsystem_t *s = &subsystems[subsystem_count++];
    memset(s, 0, sizeof(kernel_subsystem_t));
    sub_strcpy(s->name, name);
    
    spinlock_release_irqrestore(&sub_lock, flags);
    if (out_sub) *out_sub = s;
}

void subsystem_add_file(kernel_subsystem_t *sub, const char *name, 
                        int (*read)(char*, int, int), 
                        int (*write)(const char*, int, int)) {
    if (!sub || sub->file_count >= MAX_SUBSYSTEM_FILES) return;
    
    subsystem_file_t *f = &sub->files[sub->file_count++];
    sub_strcpy(f->name, name);
    f->read = read;
    f->write = write;
}

kernel_subsystem_t* subsystem_get_by_name(const char *name) {
    for (int i = 0; i < subsystem_count; i++) {
        if (sub_strcmp(subsystems[i].name, name) == 0) return &subsystems[i];
    }
    return NULL;
}

int subsystem_get_count(void) {
    return subsystem_count;
}

kernel_subsystem_t* subsystem_get_by_index(int index) {
    if (index < 0 || index >= subsystem_count) return NULL;
    return &subsystems[index];
}
