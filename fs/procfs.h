#ifndef PROCFS_H
#define PROCFS_H

#include "vfs.h"

vfs_fs_ops_t* procfs_get_ops(void);

#endif
