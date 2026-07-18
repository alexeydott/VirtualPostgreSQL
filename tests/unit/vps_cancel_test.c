#include "vps_cancel.h"

#include <stdio.h>

#define CHECK(expression, label)                                             \
    do {                                                                     \
        if (!(expression)) {                                                 \
            (void)fprintf(stderr, "cancel_case=%s status=failed\n", label); \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int main(void)
{
    VpsCancelRegistry registry;
    VpsCancelToken first = {0};
    VpsCancelToken second = {0};
    size_t count = 0U;
    CHECK(vps_cancel_registry_init(&registry,
                                   vps_platform_current_operations(), NULL) ==
              VPS_CANCEL_REGISTRY_OK,
          "init");
    CHECK(vps_cancel_token_register(&registry, &first, 1U) ==
              VPS_CANCEL_REGISTRY_OK &&
              vps_cancel_token_register(&registry, &second, 2U) ==
                  VPS_CANCEL_REGISTRY_OK,
          "register");
    CHECK(vps_cancel_token_probe(&first) == VPS_INTERRUPT_CONTINUE,
          "continue");
    CHECK(vps_cancel_registry_request_all(&registry, &count) ==
              VPS_CANCEL_REGISTRY_OK && count == 2U,
          "request_all");
    CHECK(vps_cancel_token_probe(&first) == VPS_INTERRUPT_REQUESTED &&
              vps_cancel_token_probe(&second) == VPS_INTERRUPT_REQUESTED,
          "requested");
    CHECK(vps_cancel_token_unregister(&first) == VPS_CANCEL_REGISTRY_OK &&
              vps_cancel_token_unregister(&second) == VPS_CANCEL_REGISTRY_OK,
          "unregister");
    CHECK(vps_cancel_token_unregister(&first) == VPS_CANCEL_REGISTRY_OK,
          "unregister_idempotent");
    CHECK(vps_cancel_registry_cleanup(&registry) == VPS_CANCEL_REGISTRY_OK,
          "cleanup");
    CHECK(vps_cancel_registry_cleanup(&registry) == VPS_CANCEL_REGISTRY_OK,
          "cleanup_idempotent");
    (void)fprintf(stdout, "cancel_suite status=passed\n");
    return 0;
}
