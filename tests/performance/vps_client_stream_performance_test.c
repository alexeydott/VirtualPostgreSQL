#include "vps_client.h"

#include <stdio.h>
#include <string.h>

#define VPS_STREAM_TEST_ROWS UINT64_C(1000000)

typedef struct StreamBackend {
    int connection_token;
    int statement_token;
    uint64_t next_row;
    uint64_t release_count;
    VpsClientOperation operation;
} StreamBackend;

static VpsClientStatus stream_connection_create(void *context, void **handle,
                                                 VpsError *error)
{ StreamBackend *backend=(StreamBackend *)context; (void)error;
  *handle=&backend->connection_token; return VPS_CLIENT_OK; }
static VpsClientStatus stream_connection_start(void *context, void *handle,
                                                VpsClientOperation operation,
                                                VpsError *error)
{ (void)context; (void)handle; (void)operation; (void)error;
  return VPS_CLIENT_OK; }
static VpsClientStatus stream_connection_poll(void *context, void *handle,
                                               VpsClientPollResult *result,
                                               VpsError *error)
{ (void)context; (void)handle; (void)error; (void)memset(result,0,sizeof(*result));
  result->outcome=VPS_CLIENT_POLL_COMPLETE; return VPS_CLIENT_OK; }
static VpsClientStatus stream_connection_wait(void *context, void *handle,
                                               const VpsClientWaitRequest *wait,
                                               VpsError *error)
{ (void)context; (void)handle; (void)wait; (void)error;
  return VPS_CLIENT_INVALID_STATE; }
static void stream_connection_destroy(void *context, void *handle)
{ (void)context; (void)handle; }

static VpsClientStatus stream_statement_create(
    void *context, void *connection, const VpsClientStatementSpec *spec,
    void **handle, VpsError *error)
{ StreamBackend *backend=(StreamBackend *)context; (void)connection;
  (void)spec; (void)error; *handle=&backend->statement_token;
  return VPS_CLIENT_OK; }
static VpsClientStatus stream_statement_start(void *context, void *handle,
                                               VpsClientOperation operation,
                                               VpsError *error)
{ StreamBackend *backend=(StreamBackend *)context; (void)handle; (void)error;
  backend->operation=operation; return VPS_CLIENT_OK; }
static VpsClientStatus stream_statement_poll(void *context, void *handle,
                                              VpsClientPollResult *result,
                                              VpsError *error)
{ StreamBackend *backend=(StreamBackend *)context; (void)handle; (void)error;
  (void)memset(result,0,sizeof(*result));
  if (backend->operation == VPS_CLIENT_OPERATION_EXECUTE) {
      result->outcome=VPS_CLIENT_POLL_COMPLETE;
  } else if (backend->next_row < VPS_STREAM_TEST_ROWS) {
      backend->next_row += 1U; result->outcome=VPS_CLIENT_POLL_ROW_READY;
  } else { result->outcome=VPS_CLIENT_POLL_COMPLETE; }
  return VPS_CLIENT_OK; }
static VpsClientStatus stream_statement_wait(void *context, void *handle,
                                              const VpsClientWaitRequest *wait,
                                              VpsError *error)
{ (void)context; (void)handle; (void)wait; (void)error;
  return VPS_CLIENT_INVALID_STATE; }
static VpsClientStatus stream_statement_metadata(
    void *context, const void *handle, VpsClientStatementMetadata *metadata,
    VpsError *error)
{ (void)context; (void)handle; (void)error;
  (void)memset(metadata,0,sizeof(*metadata)); metadata->result_field_count=1U;
  metadata->described=1; return VPS_CLIENT_OK; }
static VpsClientStatus stream_statement_result_field(
    void *context, const void *handle, size_t index,
    VpsClientResultFieldMetadata *field, VpsError *error)
{ static const char name[]="value"; (void)context; (void)handle; (void)error;
  if(index!=0U||field==NULL)return VPS_CLIENT_INVALID_ARGUMENT;
  (void)memset(field,0,sizeof(*field)); field->name=name;
  field->name_length=sizeof(name)-1U; field->type_oid=23U;
  field->type_modifier=-1; field->format=VPS_CLIENT_VALUE_TEXT;
  return VPS_CLIENT_OK; }
static VpsClientStatus stream_statement_row(void *context, const void *handle,
                                             size_t *count, VpsError *error)
{ (void)context; (void)handle; (void)error; *count=1U; return VPS_CLIENT_OK; }
static VpsClientStatus stream_statement_column(
    void *context, const void *handle, size_t index, VpsClientColumnView *column,
    VpsError *error)
{ static const char value[]="1"; (void)context; (void)handle; (void)error;
  if(index!=0U)return VPS_CLIENT_INVALID_ARGUMENT;
  (void)memset(column,0,sizeof(*column)); column->data=value;
  column->length=1U; column->type_oid=23U;
  column->format=VPS_CLIENT_VALUE_TEXT; return VPS_CLIENT_OK; }
static void stream_statement_row_release(void *context, void *handle)
{ StreamBackend *backend=(StreamBackend *)context; (void)handle;
  backend->release_count += 1U; }
static void stream_statement_destroy(void *context, void *handle)
{ (void)context; (void)handle; }

int main(void)
{
    static const char query[]="SELECT 1";
    VpsAllocator system_allocator;
    VpsAllocator allocator;
    VpsFaultAllocator fault;
    VpsClientOperations operations;
    VpsClient client;
    StreamBackend backend;
    VpsClientConnection *connection=NULL;
    VpsClientStatement *statement=NULL;
    VpsClientStatementSpec spec;
    VpsClientPollResult poll;
    VpsClientResultFieldExpectation field={23U,VPS_CLIENT_VALUE_TEXT};
    uint64_t index;
    size_t stable_allocations;
    (void)memset(&backend,0,sizeof(backend));
    (void)memset(&operations,0,sizeof(operations));
    operations.structure_size=sizeof(operations);
    operations.contract_version=VPS_CLIENT_CONTRACT_VERSION;
    operations.capabilities=VPS_CLIENT_CAP_CONNECT|VPS_CLIENT_CAP_EXECUTE|
                            VPS_CLIENT_CAP_FETCH;
    operations.connection_create=stream_connection_create;
    operations.connection_start=stream_connection_start;
    operations.connection_poll=stream_connection_poll;
    operations.connection_wait=stream_connection_wait;
    operations.connection_destroy=stream_connection_destroy;
    operations.statement_create=stream_statement_create;
    operations.statement_start=stream_statement_start;
    operations.statement_poll=stream_statement_poll;
    operations.statement_wait=stream_statement_wait;
    operations.statement_metadata=stream_statement_metadata;
    operations.statement_result_field=stream_statement_result_field;
    operations.statement_row=stream_statement_row;
    operations.statement_column=stream_statement_column;
    operations.statement_row_release=stream_statement_row_release;
    operations.statement_destroy=stream_statement_destroy;
    (void)memset(&spec,0,sizeof(spec)); spec.query=query;
    spec.query_length=sizeof(query)-1U; spec.result_fields=&field;
    spec.result_field_count=1U; spec.timeout_ms=1000U; spec.single_row=1;
    if (vps_allocator_system(&system_allocator)!=VPS_MEMORY_OK ||
        vps_fault_allocator_init(&fault,&system_allocator,0U)!=VPS_MEMORY_OK ||
        vps_fault_allocator_make(&fault,&allocator)!=VPS_MEMORY_OK ||
        vps_client_init(&client,&allocator,&operations,&backend,NULL)!=VPS_CLIENT_OK ||
        vps_client_connection_open(&client,&connection,NULL)!=VPS_CLIENT_OK ||
        vps_client_connection_start(connection,VPS_CLIENT_OPERATION_CONNECT,NULL)!=VPS_CLIENT_OK ||
        vps_client_connection_poll(connection,&poll,NULL)!=VPS_CLIENT_OK ||
        vps_client_statement_open(connection,&spec,&statement,NULL)!=VPS_CLIENT_OK ||
        vps_client_statement_start(statement,VPS_CLIENT_OPERATION_EXECUTE,NULL)!=VPS_CLIENT_OK ||
        vps_client_statement_poll(statement,&poll,NULL)!=VPS_CLIENT_OK) return 1;
    stable_allocations=fault.active_allocations;
    for(index=0U;index<VPS_STREAM_TEST_ROWS;++index){
        const VpsClientRowView *row=NULL; VpsClientColumnView column;
        if(vps_client_statement_start(statement,VPS_CLIENT_OPERATION_FETCH,NULL)!=VPS_CLIENT_OK ||
           vps_client_statement_poll(statement,&poll,NULL)!=VPS_CLIENT_OK ||
           poll.outcome!=VPS_CLIENT_POLL_ROW_READY ||
           vps_client_statement_current_row(statement,&row,NULL)!=VPS_CLIENT_OK ||
           vps_client_row_column(row,0U,&column,NULL)!=VPS_CLIENT_OK ||
           column.length!=1U || fault.active_allocations!=stable_allocations ||
           vps_client_statement_row_consumed(statement,NULL)!=VPS_CLIENT_OK) return 1;
    }
    if(vps_client_statement_start(statement,VPS_CLIENT_OPERATION_FETCH,NULL)!=VPS_CLIENT_OK ||
       vps_client_statement_poll(statement,&poll,NULL)!=VPS_CLIENT_OK ||
       poll.outcome!=VPS_CLIENT_POLL_COMPLETE ||
       backend.release_count!=VPS_STREAM_TEST_ROWS ||
       vps_client_statement_close(&statement)!=VPS_CLIENT_OK ||
       vps_client_connection_close(&connection)!=VPS_CLIENT_OK ||
       vps_client_cleanup(&client)!=VPS_CLIENT_OK || fault.active_allocations!=0U) return 1;
    (void)printf("client_stream rows=%u stable_allocations=%u status=passed\n",
                 (unsigned int)VPS_STREAM_TEST_ROWS,(unsigned int)stable_allocations);
    return 0;
}
