# SPDX-License-Identifier: Apache-2.0
#
# authJwt - what the auth plugins share (fdsc.so, oidc.so): an access token checked - a JWT, RS256 / ES256
# against a JWK Set (libcrypto) - the tokens already checked, and a JWK Set or a document fetched. Static,
# position-independent: each plugin links its own copy.
#
ADD_LIBRARY(authJwt STATIC authBase64.c authFetch.c authJwt.c authKeys.c authTokenCache.c)

SET_TARGET_PROPERTIES(authJwt PROPERTIES POSITION_INDEPENDENT_CODE ON)

TARGET_INCLUDE_DIRECTORIES(authJwt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

TARGET_LINK_LIBRARIES(authJwt crypto pthread)
