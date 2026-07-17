#ifndef VIRTUALPOSTGRESQL_VPS_API_H
#define VIRTUALPOSTGRESQL_VPS_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define VPS_CALL __cdecl
#  if defined(VPS_BUILD_DLL)
#    define VPS_API __declspec(dllexport)
#  elif defined(VPS_USE_DLL)
#    define VPS_API __declspec(dllimport)
#  else
#    define VPS_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define VPS_CALL
#  if defined(VPS_BUILD_SHARED)
#    define VPS_API __attribute__((visibility("default")))
#  else
#    define VPS_API
#  endif
#else
#  define VPS_CALL
#  define VPS_API
#endif

#define VPS_API_VERSION_MAJOR UINT32_C(1)
#define VPS_API_VERSION_MINOR UINT32_C(0)
#define VPS_API_VERSION_PATCH UINT32_C(0)
#define VPS_API_VERSION_ENCODE(major, minor, patch) \
  ((((uint32_t)(major) & UINT32_C(0xff)) << 24) | \
   (((uint32_t)(minor) & UINT32_C(0xff)) << 16) | \
   ((uint32_t)(patch) & UINT32_C(0xffff)))
#define VPS_API_VERSION \
  VPS_API_VERSION_ENCODE(VPS_API_VERSION_MAJOR, VPS_API_VERSION_MINOR, \
                         VPS_API_VERSION_PATCH)

typedef uint64_t VpsCredentialFields;

#define VPS_CREDENTIAL_FIELD_HOSTS                (UINT64_C(1) << 0)
#define VPS_CREDENTIAL_FIELD_PORTS                (UINT64_C(1) << 1)
#define VPS_CREDENTIAL_FIELD_USER                 (UINT64_C(1) << 2)
#define VPS_CREDENTIAL_FIELD_PASSWORD             (UINT64_C(1) << 3)
#define VPS_CREDENTIAL_FIELD_DBNAME               (UINT64_C(1) << 4)
#define VPS_CREDENTIAL_FIELD_SERVICE              (UINT64_C(1) << 5)
#define VPS_CREDENTIAL_FIELD_SERVICE_FILE         (UINT64_C(1) << 6)
#define VPS_CREDENTIAL_FIELD_SSLMODE              (UINT64_C(1) << 7)
#define VPS_CREDENTIAL_FIELD_SSLROOTCERT          (UINT64_C(1) << 8)
#define VPS_CREDENTIAL_FIELD_SSLCERT              (UINT64_C(1) << 9)
#define VPS_CREDENTIAL_FIELD_SSLKEY               (UINT64_C(1) << 10)
#define VPS_CREDENTIAL_FIELD_SSLCRL               (UINT64_C(1) << 11)
#define VPS_CREDENTIAL_FIELD_CHANNEL_BINDING      (UINT64_C(1) << 12)
#define VPS_CREDENTIAL_FIELD_TARGET_SESSION_ATTRS (UINT64_C(1) << 13)
#define VPS_CREDENTIAL_FIELD_CONNECT_TIMEOUT      (UINT64_C(1) << 14)
#define VPS_CREDENTIAL_FIELD_STATEMENT_TIMEOUT    (UINT64_C(1) << 15)
#define VPS_CREDENTIAL_FIELD_LOCK_TIMEOUT         (UINT64_C(1) << 16)
#define VPS_CREDENTIAL_FIELD_APPLICATION_NAME     (UINT64_C(1) << 17)
#define VPS_CREDENTIAL_FIELD_SEARCH_PATH          (UINT64_C(1) << 18)

typedef struct VpsAbiHeader {
    uint32_t structure_size;
    uint32_t api_version;
    uint64_t present_fields;
} VpsAbiHeader;

/*
 * All string pointers below are provider-owned UTF-8, valid only until the
 * matching release callback. The extension must copy checked lengths before
 * release and must securely erase copied credential material.
 */
typedef struct VpsCredentialConfig {
    VpsAbiHeader header;
    const char *hosts;
    const char *ports;
    const char *user;
    const char *password;
    const char *dbname;
    const char *service;
    const char *service_file;
    const char *sslmode;
    const char *sslrootcert;
    const char *sslcert;
    const char *sslkey;
    const char *sslcrl;
    const char *channel_binding;
    const char *target_session_attrs;
    const char *connect_timeout;
    const char *statement_timeout;
    const char *lock_timeout;
    const char *application_name;
    const char *search_path;
    uintptr_t reserved[4];
} VpsCredentialConfig;

typedef struct VpsCredentialLease {
    VpsAbiHeader header;
    const VpsCredentialConfig *config;
    void *provider_lease;
    uintptr_t reserved[4];
} VpsCredentialLease;

typedef int32_t(VPS_CALL *VpsCredentialResolveFn)(
    void *provider_context,
    const char *credential_ref,
    uint32_t credential_ref_length,
    VpsCredentialLease *lease);

typedef void(VPS_CALL *VpsCredentialReleaseFn)(
    void *provider_context,
    VpsCredentialLease *lease);

typedef struct VpsCredentialProvider {
    VpsAbiHeader header;
    VpsCredentialResolveFn resolve;
    VpsCredentialReleaseFn release;
    void *provider_context;
    uintptr_t reserved[4];
} VpsCredentialProvider;

VPS_API uint32_t VPS_CALL virtualpostgresql_api_version(void);
VPS_API uint32_t VPS_CALL virtualpostgresql_credential_config_structure_size(void);
VPS_API uint32_t VPS_CALL virtualpostgresql_credential_lease_structure_size(void);
VPS_API uint32_t VPS_CALL virtualpostgresql_credential_provider_structure_size(void);

#ifdef __cplusplus
}
#endif

#endif
