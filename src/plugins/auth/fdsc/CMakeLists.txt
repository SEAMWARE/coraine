# SPDX-License-Identifier: Apache-2.0
#
# fdsc.so - the FIWARE Data Space Connector's security inside the broker (--auth fdsc, doc/authorization.md):
# VCVerifier's access tokens (checked by authJwt, ../jwt) and the ODRL-PAP's policies.
#
ADD_LIBRARY(authFdsc SHARED fdsc.c fdscIdentity.c fdscPolicy.c fdscTokenCache.c)

TARGET_LINK_LIBRARIES(authFdsc authJwt crypto pthread)

SET_TARGET_PROPERTIES(authFdsc PROPERTIES OUTPUT_NAME "fdsc" PREFIX "")
