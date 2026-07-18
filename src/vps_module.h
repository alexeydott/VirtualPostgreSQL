#ifndef VPS_MODULE_H
#define VPS_MODULE_H

#include "sqlite3ext.h"
#include "vps_cancel.h"

#include <stdint.h>

typedef struct VpsModuleContext {
    sqlite3 *database;
    VpsCancelRegistry cancel_registry;
    uint64_t next_table_id;
    uint64_t table_references;
    int initialized_cancel_registry;
    int closing;
} VpsModuleContext;

#define VPS_MODULE_CLIENTDATA_KEY "virtualpostgresql.module-context.v1"

VpsModuleContext *vps_module_context_create(sqlite3 *database);
void vps_module_context_destroy(void *context);

extern const sqlite3_module VPS_MODULE;
extern const sqlite3_module VPS_METADATA_MODULE;

#endif
