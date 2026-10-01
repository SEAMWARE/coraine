# SPDX-License-Identifier: Apache-2.0
ADD_LIBRARY(bridgeModbus SHARED modbusRegister.c)

TARGET_LINK_LIBRARIES(bridgeModbus pthread m)

SET_TARGET_PROPERTIES(bridgeModbus PROPERTIES OUTPUT_NAME "modbus" PREFIX "")
