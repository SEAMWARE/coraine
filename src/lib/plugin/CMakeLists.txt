# SPDX-License-Identifier: Apache-2.0
ADD_LIBRARY(plugin STATIC pluginLoader.c)
ADD_DEPENDENCIES(plugin dbAbiStamp)                 # dbAbiStamp.h - coraineDbAbi
