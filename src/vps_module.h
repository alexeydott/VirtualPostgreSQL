#ifndef VPS_MODULE_H
#define VPS_MODULE_H

#include "sqlite3ext.h"

#include <stdint.h>

typedef struct VpsModuleContext {
    uint64_t next_table_id;
    uint64_t table_references;
    int closing;
} VpsModuleContext;

VpsModuleContext *vps_module_context_create(void);
void vps_module_context_destroy(void *context);

extern const sqlite3_module VPS_MODULE;
extern const sqlite3_module VPS_METADATA_MODULE;

#endif
