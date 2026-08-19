# SPDX-License-Identifier: Apache-2.0
ADD_LIBRARY(db STATIC dbInit.c dbClose.c tenant.c snapshotTenant.c dbExpiredEntities.c contextCache.c)
