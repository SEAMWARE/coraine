# SPDX-License-Identifier: Apache-2.0
#
# fdsc.so - the FIWARE Data Space Connector's security inside the broker (--auth fdsc, doc/authorization.md):
# VCVerifier's access tokens (checked by corRest's corRestJwt, in the broker) and the ODRL-PAP's policies.
#
ADD_LIBRARY(authFdsc SHARED fdsc.c fdscIdentity.c fdscPolicy.c)

TARGET_LINK_LIBRARIES(authFdsc pthread)

SET_TARGET_PROPERTIES(authFdsc PROPERTIES OUTPUT_NAME "fdsc" PREFIX "")
