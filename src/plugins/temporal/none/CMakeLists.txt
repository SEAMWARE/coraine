# SPDX-License-Identifier: Apache-2.0
ADD_LIBRARY(troeNone SHARED noneRegister.c ${CMAKE_SOURCE_DIR}/src/plugins/shared/dbPluginAbi.c)
ADD_DEPENDENCIES(troeNone dbAbiStamp)               # dbAbiStamp.h - dbPluginAbi
SET_TARGET_PROPERTIES(troeNone PROPERTIES OUTPUT_NAME "none" PREFIX "")
