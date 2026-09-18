# SPDX-License-Identifier: Apache-2.0
ADD_LIBRARY(troeCorDb SHARED corDbRegister.c)
SET_TARGET_PROPERTIES(troeCorDb PROPERTIES OUTPUT_NAME "corDB" PREFIX "")
