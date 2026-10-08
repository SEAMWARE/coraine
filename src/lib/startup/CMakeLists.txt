# SPDX-License-Identifier: Apache-2.0
#
# What every program built on the broker's libraries sets up the same way - the broker and coraine-import
#
ADD_LIBRARY(startup STATIC startupContext.c startupCoreContext.c startupLog.c startupArena.c)
