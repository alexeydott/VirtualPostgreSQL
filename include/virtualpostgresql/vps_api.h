#ifndef VIRTUALPOSTGRESQL_VPS_API_H
#define VIRTUALPOSTGRESQL_VPS_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sqlite3 sqlite3;

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

/** Bit mask type used by public ABI structure and credential field sets. */
typedef uint64_t VpsCredentialFields;

/* Provider callbacks return one of these stable int32 status values. */
#define VPS_CREDENTIAL_PROVIDER_OK INT32_C(0)
#define VPS_CREDENTIAL_PROVIDER_NOT_FOUND INT32_C(1)
#define VPS_CREDENTIAL_PROVIDER_UNAVAILABLE INT32_C(2)
#define VPS_CREDENTIAL_PROVIDER_INVALID_REFERENCE INT32_C(3)
#define VPS_CREDENTIAL_PROVIDER_ERROR INT32_C(4)

#define VPS_CANCEL_OK INT32_C(0)
#define VPS_CANCEL_INVALID_DATABASE INT32_C(1)
#define VPS_CANCEL_UNAVAILABLE INT32_C(2)
#define VPS_CANCEL_ERROR INT32_C(3)

/*
 * present_fields bits for VpsCredentialProvider. A zero mask is accepted as
 * the legacy 1.0 prefix; non-zero masks must advertise required callbacks.
 */
#define VPS_CREDENTIAL_PROVIDER_FIELD_RESOLVE  (UINT64_C(1) << 0)
#define VPS_CREDENTIAL_PROVIDER_FIELD_RELEASE  (UINT64_C(1) << 1)
#define VPS_CREDENTIAL_PROVIDER_FIELD_CONTEXT  (UINT64_C(1) << 2)
#define VPS_CREDENTIAL_PROVIDER_FIELDS_CURRENT \
  (VPS_CREDENTIAL_PROVIDER_FIELD_RESOLVE | \
   VPS_CREDENTIAL_PROVIDER_FIELD_RELEASE | \
   VPS_CREDENTIAL_PROVIDER_FIELD_CONTEXT)

/* present_fields bits for VpsCredentialLease. */
#define VPS_CREDENTIAL_LEASE_FIELD_CONFIG         (UINT64_C(1) << 0)
#define VPS_CREDENTIAL_LEASE_FIELD_PROVIDER_LEASE (UINT64_C(1) << 1)
#define VPS_CREDENTIAL_LEASE_FIELDS_CURRENT \
  (VPS_CREDENTIAL_LEASE_FIELD_CONFIG | \
   VPS_CREDENTIAL_LEASE_FIELD_PROVIDER_LEASE)

/* Byte limits; credential_ref is length-delimited, config values exclude NUL. */
#define VPS_CREDENTIAL_REFERENCE_MAX_LENGTH UINT32_C(1024)
#define VPS_CREDENTIAL_VALUE_MAX_LENGTH UINT32_C(4096)

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
#define VPS_CREDENTIAL_FIELDS_CURRENT \
  (VPS_CREDENTIAL_FIELD_HOSTS | VPS_CREDENTIAL_FIELD_PORTS | \
   VPS_CREDENTIAL_FIELD_USER | VPS_CREDENTIAL_FIELD_PASSWORD | \
   VPS_CREDENTIAL_FIELD_DBNAME | VPS_CREDENTIAL_FIELD_SERVICE | \
   VPS_CREDENTIAL_FIELD_SERVICE_FILE | VPS_CREDENTIAL_FIELD_SSLMODE | \
   VPS_CREDENTIAL_FIELD_SSLROOTCERT | VPS_CREDENTIAL_FIELD_SSLCERT | \
   VPS_CREDENTIAL_FIELD_SSLKEY | VPS_CREDENTIAL_FIELD_SSLCRL | \
   VPS_CREDENTIAL_FIELD_CHANNEL_BINDING | \
   VPS_CREDENTIAL_FIELD_TARGET_SESSION_ATTRS | \
   VPS_CREDENTIAL_FIELD_CONNECT_TIMEOUT | \
   VPS_CREDENTIAL_FIELD_STATEMENT_TIMEOUT | \
   VPS_CREDENTIAL_FIELD_LOCK_TIMEOUT | \
   VPS_CREDENTIAL_FIELD_APPLICATION_NAME | \
   VPS_CREDENTIAL_FIELD_SEARCH_PATH)

/**
 * Versioned prefix embedded at offset zero in every public ABI structure.
 * Callers set structure_size, api_version, and the supported field mask.
 */
typedef struct VpsAbiHeader {
    uint32_t structure_size;
    uint32_t api_version;
    uint64_t present_fields;
} VpsAbiHeader;

/**
 * Resolved PostgreSQL connection fields returned by a credential provider.
 *
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

/**
 * Provider-owned lease returned by a successful credential resolution.
 * The extension releases the lease exactly once through the provider callback.
 */
typedef struct VpsCredentialLease {
    VpsAbiHeader header;
    const VpsCredentialConfig *config;
    void *provider_lease;
    uintptr_t reserved[4];
} VpsCredentialLease;

/**
 * Resolves a length-delimited UTF-8 credential reference into a lease.
 * Returns one of the stable VPS_CREDENTIAL_PROVIDER_* status values.
 */
typedef int32_t(VPS_CALL *VpsCredentialResolveFn)(
    void *provider_context,
    const char *credential_ref,
    uint32_t credential_ref_length,
    VpsCredentialLease *lease);

/**
 * Releases a provider-owned lease after the extension has copied its fields.
 *
 * A successful resolve transfers one provider-owned lease to the caller.
 * The matching release callback is invoked exactly once after checked copy,
 * including when that copy or subsequent validation fails.
 */
typedef void(VPS_CALL *VpsCredentialReleaseFn)(
    void *provider_context,
    VpsCredentialLease *lease);

/**
 * Host-supplied credential provider callbacks and opaque provider context.
 * Implementations must remain valid for the registration lifetime.
 */
typedef struct VpsCredentialProvider {
    VpsAbiHeader header;
    VpsCredentialResolveFn resolve;
    VpsCredentialReleaseFn release;
    void *provider_context;
    uintptr_t reserved[4];
} VpsCredentialProvider;

/** Returns the encoded VirtualPostgreSQL public API version. */
VPS_API uint32_t VPS_CALL virtualpostgresql_api_version(void);

/** Returns the runtime size of VpsCredentialConfig for ABI negotiation. */
VPS_API uint32_t VPS_CALL virtualpostgresql_credential_config_structure_size(void);

/** Returns the runtime size of VpsCredentialLease for ABI negotiation. */
VPS_API uint32_t VPS_CALL virtualpostgresql_credential_lease_structure_size(void);

/** Returns the runtime size of VpsCredentialProvider for ABI negotiation. */
VPS_API uint32_t VPS_CALL virtualpostgresql_credential_provider_structure_size(void);

/**
 * Registers a credential provider for one loaded host SQLite connection.
 * Replacement is allowed only before that connection starts its first resolve.
 * Returns a SQLite result code.
 */
VPS_API int VPS_CALL virtualpostgresql_register_credential_provider(
    sqlite3 *database, const VpsCredentialProvider *provider);

#if defined(_WIN32)
/**
 * Initializes a provider backed by Windows Generic Credentials.
 * The returned callbacks remain valid while the DLL is loaded. The caller owns
 * the provider structure and may register it on one or more SQLite connections.
 * Returns a SQLite result code.
 */
VPS_API int VPS_CALL virtualpostgresql_wincred_provider(
    VpsCredentialProvider *provider);
#endif

/**
 * Cancels active VirtualPostgreSQL operations associated with database.
 * Thread-safe for a SQLite connection opened in serialized mode.
 */
VPS_API int32_t VPS_CALL virtualpostgresql_cancel(sqlite3 *database);

#ifdef __cplusplus
}
#endif

#endif
