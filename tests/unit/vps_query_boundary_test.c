#include "vps_query_boundary.h"

#include <stdio.h>
#include <string.h>

typedef struct FakeExecutor {
    size_t begin_count;
    size_t configure_count;
    size_t commit_count;
    size_t rollback_count;
    VpsQueryBoundaryResult configure_result;
} FakeExecutor;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    (void)fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #condition); \
    ++failures; } } while (0)

static VpsQueryBoundaryResult fake_begin(void *context, VpsError *error)
{ FakeExecutor *fake=(FakeExecutor *)context; (void)error;
  ++fake->begin_count; return VPS_QUERY_BOUNDARY_OK; }
static VpsQueryBoundaryResult fake_configure(
    void *context, const VpsQueryBoundaryPolicy *policy, VpsError *error)
{ FakeExecutor *fake=(FakeExecutor *)context; (void)error;
  CHECK(policy->search_path_length==10U &&
        memcmp(policy->search_path,"pg_catalog",10U)==0 &&
        policy->statement_timeout_ms==1000U &&
        policy->lock_timeout_ms==500U); ++fake->configure_count;
  return fake->configure_result; }
static VpsQueryBoundaryResult fake_commit(void *context, VpsError *error)
{ FakeExecutor *fake=(FakeExecutor *)context; (void)error;
  ++fake->commit_count; return VPS_QUERY_BOUNDARY_OK; }
static VpsQueryBoundaryResult fake_rollback(void *context, VpsError *error)
{ FakeExecutor *fake=(FakeExecutor *)context; (void)error;
  ++fake->rollback_count; return VPS_QUERY_BOUNDARY_OK; }

static void make_executor(FakeExecutor *fake,
                          VpsQueryBoundaryExecutor *executor)
{
    (void)memset(executor,0,sizeof(*executor));
    executor->structure_size=(uint32_t)sizeof(*executor);
    executor->format_version=VPS_QUERY_BOUNDARY_FORMAT_VERSION;
    executor->context=fake; executor->begin_read_only=fake_begin;
    executor->configure_local=fake_configure; executor->commit=fake_commit;
    executor->rollback=fake_rollback;
}

int main(void)
{
    VpsQueryBoundary boundary;
    VpsQueryBoundaryExecutor executor;
    VpsQueryBoundaryPolicy policy={"pg_catalog",10U,1000U,500U,2U,10U,100U};
    FakeExecutor fake;
    (void)memset(&fake,0,sizeof(fake));
    make_executor(&fake,&executor);
    CHECK(vps_query_boundary_init(&boundary,&executor,&policy,NULL)==
          VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_open(&boundary,1000U,NULL)==VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_observe(&boundary,4U,1001U,NULL)==
          VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_observe(&boundary,4U,1002U,NULL)==
          VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_finish(&boundary,NULL)==VPS_QUERY_BOUNDARY_OK);
    CHECK(fake.begin_count==1U && fake.configure_count==1U &&
          fake.commit_count==1U && fake.rollback_count==0U);
    CHECK(vps_query_boundary_cleanup(&boundary,NULL)==VPS_QUERY_BOUNDARY_OK);

    (void)memset(&fake,0,sizeof(fake)); make_executor(&fake,&executor);
    CHECK(vps_query_boundary_init(&boundary,&executor,&policy,NULL)==
          VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_open(&boundary,10U,NULL)==VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_observe(&boundary,1U,11U,NULL)==
          VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_observe(&boundary,1U,12U,NULL)==
          VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_observe(&boundary,1U,13U,NULL)==
          VPS_QUERY_BOUNDARY_ROW_LIMIT);
    CHECK(fake.rollback_count==1U);
    CHECK(vps_query_boundary_cleanup(&boundary,NULL)==VPS_QUERY_BOUNDARY_OK);

    (void)memset(&fake,0,sizeof(fake)); make_executor(&fake,&executor);
    CHECK(vps_query_boundary_init(&boundary,&executor,&policy,NULL)==
          VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_open(&boundary,100U,NULL)==VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_observe(&boundary,1U,201U,NULL)==
          VPS_QUERY_BOUNDARY_DEADLINE);
    CHECK(fake.rollback_count==1U);
    CHECK(vps_query_boundary_cleanup(&boundary,NULL)==VPS_QUERY_BOUNDARY_OK);

    (void)memset(&fake,0,sizeof(fake)); make_executor(&fake,&executor);
    CHECK(vps_query_boundary_init(&boundary,&executor,&policy,NULL)==
          VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_open(&boundary,0U,NULL)==VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_observe(&boundary,11U,1U,NULL)==
          VPS_QUERY_BOUNDARY_BYTE_LIMIT);
    CHECK(boundary.state==VPS_QUERY_BOUNDARY_ROLLED_BACK &&
          fake.rollback_count==1U);
    CHECK(vps_query_boundary_cleanup(&boundary,NULL)==VPS_QUERY_BOUNDARY_OK);

    (void)memset(&fake,0,sizeof(fake)); fake.configure_result=
        VPS_QUERY_BOUNDARY_CLIENT_ERROR; make_executor(&fake,&executor);
    CHECK(vps_query_boundary_init(&boundary,&executor,&policy,NULL)==
          VPS_QUERY_BOUNDARY_OK);
    CHECK(vps_query_boundary_open(&boundary,0U,NULL)==
          VPS_QUERY_BOUNDARY_CLIENT_ERROR);
    CHECK(fake.rollback_count==1U);
    CHECK(vps_query_boundary_cleanup(&boundary,NULL)==VPS_QUERY_BOUNDARY_OK);

    if(failures!=0)return 1;
    (void)puts("vps_query_boundary_test: passed"); return 0;
}
