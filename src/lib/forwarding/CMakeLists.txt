# SPDX-License-Identifier: Apache-2.0
IF (COR_FEATURE_COR_PROTOCOL)
  ADD_LIBRARY(forwarding STATIC forwardingHttp.c forwardingCor.c)
ELSE()
  ADD_LIBRARY(forwarding STATIC forwardingHttp.c)
ENDIF()
