# SPDX-License-Identifier: Apache-2.0
#
# oidc.so - an OpenID provider's access tokens (Keycloak, ...) and their roles, reading or writing, inside the
# broker (--auth oidc, doc/authorization.md). The token is checked by authJwt (../jwt).
#
ADD_LIBRARY(authOidc SHARED oidc.c)

TARGET_LINK_LIBRARIES(authOidc authJwt crypto pthread)

SET_TARGET_PROPERTIES(authOidc PROPERTIES OUTPUT_NAME "oidc" PREFIX "")
