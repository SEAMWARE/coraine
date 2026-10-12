# SPDX-License-Identifier: Apache-2.0
#
# fdsc.so - the FIWARE Data Space Connector's security inside the broker (--auth fdsc, doc/authorization.md):
# VCVerifier's access tokens (RS256 / ES256 against its JWK Set, libcrypto) and the ODRL-PAP's policies.
#
ADD_LIBRARY(authFdsc SHARED fdsc.c fdscBase64.c fdscFetch.c fdscIdentity.c fdscJwt.c fdscKeys.c fdscPolicy.c fdscTokenCache.c)

TARGET_LINK_LIBRARIES(authFdsc crypto pthread)

SET_TARGET_PROPERTIES(authFdsc PROPERTIES OUTPUT_NAME "fdsc" PREFIX "")
