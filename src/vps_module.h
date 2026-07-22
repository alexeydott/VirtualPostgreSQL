#ifndef VPS_MODULE_H
#define VPS_MODULE_H

#include "sqlite3ext.h"
#include "vps_cancel.h"
#include "vps_client.h"
#include "vps_connection_pool.h"
#include "vps_query_profile.h"
#include "vps_transaction.h"
#if defined(_WIN32)
#include "vps_credential_provider.h"
#include "vps_wincred_provider.h"
#endif

#include <stdint.h>

typedef struct VpsModuleContext {
    sqlite3 *database;
    VpsCancelRegistry cancel_registry;
    VpsQueryProfileRegistry query_profile_registry;
    VpsAllocator transaction_allocator;
    VpsTransactionCoordinator transaction;
    VpsConnectionLease transaction_lease;
    VpsClientConnection *transaction_connection;
#if defined(_WIN32)
    VpsAllocator credential_allocator;
    VpsCredentialRegistry credential_registry;
    VpsWinCredProviderContext wincred;
#endif
    uint64_t next_table_id;
    uint64_t table_references;
    int initialized_cancel_registry;
    int initialized_query_profile_registry;
    int initialized_transaction;
#if defined(_WIN32)
    int initialized_credential_registry;
    int initialized_wincred;
#endif
    int closing;
} VpsModuleContext;

#define VPS_MODULE_CLIENTDATA_KEY "virtualpostgresql.module-context.v1"

VpsModuleContext *vps_module_context_create(sqlite3 *database);
void vps_module_context_destroy(void *context);

extern const sqlite3_module VPS_MODULE;
extern const sqlite3_module VPS_METADATA_MODULE;

#endif
