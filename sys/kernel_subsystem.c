#include "kernel_subsystem.h"
#include "memory_manager.h"
#include "spinlock.h"
#include "kutils.h"


static kernel_subsystem_t subsystems[MAX_SUBSYSTEMS];
static int subsystem_count = 0;
static spinlock_t sub_lock = SPINLOCK_INIT;


void subsystem_register(const char *name, kernel_subsystem_t **out_sub) {
    uint64_t flags = spinlock_acquire_irqsave(&sub_lock);
    
    if (subsystem_count >= MAX_SUBSYSTEMS) {
        spinlock_release_irqrestore(&sub_lock, flags);
        if (out_sub) *out_sub = NULL;
        return;
    }

    // Check if already exists
    for (int i = 0; i < subsystem_count; i++) {
        if (strcmp(subsystems[i].name, name) == 0) {
            spinlock_release_irqrestore(&sub_lock, flags);
            if (out_sub) *out_sub = &subsystems[i];
            return;
        }
    }

    kernel_subsystem_t *s = &subsystems[subsystem_count++];
    memset(s, 0, sizeof(kernel_subsystem_t));
    strcpy(s->name, name);
    
    spinlock_release_irqrestore(&sub_lock, flags);
    if (out_sub) *out_sub = s;
}

void subsystem_add_file(kernel_subsystem_t *sub, const char *name, 
                        int (*read)(char*, size_t, size_t), 
                        int (*write)(const char*, size_t, size_t)) {
    if (!sub || sub->file_count >= MAX_SUBSYSTEM_FILES) return;
    
    subsystem_file_t *f = &sub->files[sub->file_count++];
    strcpy(f->name, name);
    f->read = read;
    f->write = write;
}

kernel_subsystem_t* subsystem_get_by_name(const char *name) {
    for (int i = 0; i < subsystem_count; i++) {
        if (strcmp(subsystems[i].name, name) == 0) return &subsystems[i];
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
