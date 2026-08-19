//
// FILE            ngsildServices.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdlib.h>                              // malloc
#include <string.h>                              // memcpy

#include "corRest/CorRestService.h"                // CorRestServiceSimplified, CorRestVerb
#include "corRest/CorRestVerb.h"                   // CorVerbGet, CorVerbPost, CorVerbDelete, CorVerbPatch
#include "corNgsild/ldParams.h"                   // LD_PARAMS_GET_ENTITIES, LD_PARAMS_GET_ENTITY, LD_PARAMS_POST_ENTITIES, LD_PARAMS_DELETE_ENTITY, LD_PARAMS_PATCH_ENTITY
#include "corNgsild/LdOp.h"                       // LdOp*
#include "corNgsild/ldPCheckQuery.h"              // pCheckQuery

#include "serviceRoutines/getEntities.h"         // getEntities
#include "serviceRoutines/getEntity.h"           // getEntity
#include "serviceRoutines/postEntities.h"        // postEntities
#include "serviceRoutines/deleteEntity.h"        // deleteEntity
#include "serviceRoutines/patchEntity.h"         // patchEntity
#include "serviceRoutines/replaceEntity.h"       // replaceEntity
#include "serviceRoutines/postEntityAttrs.h"     // postEntityAttrs
#include "serviceRoutines/patchEntityAttrs.h"    // patchEntityAttrs
#include "serviceRoutines/purgeEntities.h"       // purgeEntities
#include "serviceRoutines/postSnapshot.h"        // postSnapshot
#include "serviceRoutines/getSnapshot.h"         // getSnapshot
#include "serviceRoutines/deleteSnapshot.h"      // deleteSnapshot
#include "serviceRoutines/patchSnapshot.h"       // patchSnapshot
#include "serviceRoutines/cloneSnapshot.h"       // cloneSnapshot
#include "serviceRoutines/purgeSnapshots.h"      // purgeSnapshots
#include "serviceRoutines/getEntityAttr.h"       // getEntityAttr
#include "serviceRoutines/patchEntityAttr.h"     // patchEntityAttr
#include "serviceRoutines/getEntityAttrValue.h"  // getEntityAttrValue
#include "serviceRoutines/putEntityAttrValue.h"  // putEntityAttrValue
#include "serviceRoutines/putEntityAttr.h"       // putEntityAttr
#include "serviceRoutines/deleteEntityAttr.h"    // deleteEntityAttr
#include "serviceRoutines/getTypes.h"            // getTypes
#include "serviceRoutines/getType.h"             // getType
#include "serviceRoutines/getAttributes.h"       // getAttributes
#include "serviceRoutines/getAttribute.h"        // getAttribute
#include "serviceRoutines/postSubscriptions.h"   // postSubscriptions
#include "serviceRoutines/getSubscriptions.h"    // getSubscriptions
#include "serviceRoutines/getSubscription.h"     // getSubscription
#include "serviceRoutines/patchSubscription.h"   // patchSubscription
#include "serviceRoutines/deleteSubscription.h"  // deleteSubscription
#include "serviceRoutines/postExNotification.h"  // postExNotification
#include "serviceRoutines/getJsonldContexts.h"   // getJsonldContexts
#include "serviceRoutines/getJsonldContext.h"    // getJsonldContext
#include "serviceRoutines/postJsonldContexts.h"  // postJsonldContexts
#include "serviceRoutines/deleteJsonldContext.h" // deleteJsonldContext
#include "serviceRoutines/postCsourceRegistration.h"   // postCsourceRegistration
#include "serviceRoutines/getCsourceRegistrations.h"   // getCsourceRegistrations
#include "serviceRoutines/getCsourceRegistration.h"    // getCsourceRegistration
#include "serviceRoutines/patchCsourceRegistration.h"  // patchCsourceRegistration
#include "serviceRoutines/deleteCsourceRegistration.h" // deleteCsourceRegistration
#include "serviceRoutines/postCsourceSubscriptions.h"  // postCsourceSubscriptions
#include "serviceRoutines/getCsourceSubscriptions.h"   // getCsourceSubscriptions
#include "serviceRoutines/getCsourceSubscription.h"    // getCsourceSubscription
#include "serviceRoutines/patchCsourceSubscription.h"  // patchCsourceSubscription
#include "serviceRoutines/deleteCsourceSubscription.h" // deleteCsourceSubscription
#include "serviceRoutines/getEntityMap.h"              // getEntityMap
#include "serviceRoutines/deleteEntityMap.h"           // deleteEntityMap
#include "serviceRoutines/patchEntityMap.h"            // patchEntityMap
#include "serviceRoutines/createEntityMap.h"           // createEntityMap
#include "serviceRoutines/postEntityMap.h"             // postEntityMap
#include "serviceRoutines/postEntityBatchCreate.h"     // postEntityBatchCreate
#include "serviceRoutines/postEntityBatchUpdate.h"     // postEntityBatchUpdate
#include "serviceRoutines/postEntityBatchUpsert.h"     // postEntityBatchUpsert
#include "serviceRoutines/postEntityBatchMerge.h"      // postEntityBatchMerge
#include "serviceRoutines/postEntityBatchDelete.h"     // postEntityBatchDelete
#include "serviceRoutines/postEntityBatchQuery.h"      // postEntityBatchQuery
#include "serviceRoutines/getSourceIdentity.h"         // getSourceIdentity
#include "serviceRoutines/getVersion.h"                // getVersion
#include "serviceRoutines/getEntityTemporal.h"         // getEntityTemporal
#include "serviceRoutines/getEntitiesTemporal.h"       // getEntitiesTemporal
#include "serviceRoutines/postTemporalEntityBatchQuery.h" // postTemporalEntityBatchQuery
#include "serviceRoutines/deleteEntityTemporal.h"      // deleteEntityTemporal
#include "serviceRoutines/deleteEntityTemporalAttr.h"  // deleteEntityTemporalAttr
#include "serviceRoutines/postEntitiesTemporal.h"      // postEntitiesTemporal
#include "serviceRoutines/postEntityTemporalAttrs.h"   // postEntityTemporalAttrs
#include "serviceRoutines/patchEntityTemporalInstance.h"  // patchEntityTemporalInstance
#include "serviceRoutines/deleteEntityTemporalInstance.h" // deleteEntityTemporalInstance

#include "plugin/ApiPlugin.h"                    // ApiPlugin, apiPlugins, apiPluginCount

#include "ngsildServices.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// ngsildCoreServices - flat array of core NGSI-LD services
//
CorRestServiceSimplified ngsildCoreServices[] =
{
  { CorVerbGet,    "/ngsi-ld/v1/entities",   getEntities,  LD_PARAMS_GET_ENTITIES,   LdOpQueryEntities  },
  { CorVerbGet,    "/ngsi-ld/v1/entities/*", getEntity,    LD_PARAMS_GET_ENTITY,     LdOpRetrieveEntity },
  { CorVerbPost,   "/ngsi-ld/v1/entities",   postEntities, LD_PARAMS_POST_ENTITIES,  LdOpCreateEntity   },
  { CorVerbDelete, "/ngsi-ld/v1/entities/*", deleteEntity, LD_PARAMS_DELETE_ENTITY,  LdOpDeleteEntity   },
  { CorVerbPatch,  "/ngsi-ld/v1/entities/*", patchEntity,  LD_PARAMS_PATCH_ENTITY,   LdOpMergeEntity    },
  { CorVerbPut,    "/ngsi-ld/v1/entities/*", replaceEntity, LD_PARAMS_REPLACE_ENTITY, LdOpReplaceEntity },
  { CorVerbPost,   "/ngsi-ld/v1/entities/*/attrs", postEntityAttrs,  LD_PARAMS_POST_ENTITY_ATTRS,  LdOpAppendAttrs },
  { CorVerbPatch,  "/ngsi-ld/v1/entities/*/attrs", patchEntityAttrs, LD_PARAMS_PATCH_ENTITY_ATTRS, LdOpUpdateAttrs },
  // value-only sub-resource (TS 104-176 § 7.6) — MUST precede /attrs/* so the
  // trailing-literal route wins over the single-attribute route.
  { CorVerbGet,    "/ngsi-ld/v1/entities/*/attrs/*/value", getEntityAttrValue, LD_PARAMS_GET_ENTITY_ATTR,    LdOpRetrieveEntity },
  { CorVerbPut,    "/ngsi-ld/v1/entities/*/attrs/*/value", putEntityAttrValue, LD_PARAMS_PATCH_ENTITY_ATTR,  LdOpUpdateAttrs    },
  { CorVerbGet,    "/ngsi-ld/v1/entities/*/attrs/*", getEntityAttr,    LD_PARAMS_GET_ENTITY_ATTR,    LdOpRetrieveEntity },
  { CorVerbPatch,  "/ngsi-ld/v1/entities/*/attrs/*", patchEntityAttr,  LD_PARAMS_PATCH_ENTITY_ATTR,  LdOpUpdateAttrs    },
  { CorVerbPut,    "/ngsi-ld/v1/entities/*/attrs/*", putEntityAttr,    LD_PARAMS_PUT_ENTITY_ATTR,    LdOpReplaceAttr    },
  { CorVerbDelete, "/ngsi-ld/v1/entities/*/attrs/*", deleteEntityAttr, LD_PARAMS_DELETE_ENTITY_ATTR, LdOpDeleteAttr     },
  { CorVerbDelete, "/ngsi-ld/v1/entities",         purgeEntities,    LD_PARAMS_PURGE_ENTITIES,     LdOpPurgeEntity    },

  // Discovery (§ 5.7.5 – § 5.7.10)
  { CorVerbGet,    "/ngsi-ld/v1/types",            getTypes,         LD_PARAMS_GET_TYPES,          LdOpRetrieveEntityTypes       },
  { CorVerbGet,    "/ngsi-ld/v1/types/*",          getType,          LD_PARAMS_GET_TYPE,           LdOpRetrieveEntityTypeInfo    },
  { CorVerbGet,    "/ngsi-ld/v1/attributes",       getAttributes,    LD_PARAMS_GET_ATTRIBUTES,     LdOpRetrieveAttrTypes         },
  { CorVerbGet,    "/ngsi-ld/v1/attributes/*",     getAttribute,     LD_PARAMS_GET_ATTRIBUTE,      LdOpRetrieveAttrTypeInfo      },

  { CorVerbPost,   "/ngsi-ld/v1/subscriptions",   postSubscriptions,   LD_PARAMS_POST_SUBSCRIPTIONS,   LdOpCreateSubscription   },
  { CorVerbGet,    "/ngsi-ld/v1/subscriptions",   getSubscriptions,    LD_PARAMS_GET_SUBSCRIPTIONS,    LdOpQuerySubscription    },
  { CorVerbGet,    "/ngsi-ld/v1/subscriptions/*", getSubscription,     LD_PARAMS_GET_SUBSCRIPTION,     LdOpRetrieveSubscription },
  { CorVerbPatch,  "/ngsi-ld/v1/subscriptions/*", patchSubscription,   LD_PARAMS_PATCH_SUBSCRIPTION,   LdOpUpdateSubscription   },
  { CorVerbDelete, "/ngsi-ld/v1/subscriptions/*", deleteSubscription,  LD_PARAMS_DELETE_SUBSCRIPTION,  LdOpDeleteSubscription   },

  // Distributed-subscription notification receiver (§ 5.8.1.4) — invoked
  // by remote Context Sources when a derived sub fires; the body is
  // re-dispatched to the original local sub's notification endpoint.
  { CorVerbPost,   "/ngsi-ld/ex/v1/notifications/*", postExNotification, 0, LdOpNone },

  { CorVerbGet,    "/ngsi-ld/v1/jsonldContexts",   getJsonldContexts, LD_PARAMS_GET_JSONLD_CONTEXTS, LdOpNone },
  { CorVerbGet,    "/ngsi-ld/v1/jsonldContexts/**", getJsonldContext, LD_PARAMS_GET_JSONLD_CONTEXT,  LdOpNone },
  { CorVerbPost,   "/ngsi-ld/v1/jsonldContexts",   postJsonldContexts,  LD_PARAMS_POST_JSONLD_CONTEXTS,  LdOpNone },
  { CorVerbDelete, "/ngsi-ld/v1/jsonldContexts/**", deleteJsonldContext, LD_PARAMS_DELETE_JSONLD_CONTEXT, LdOpNone },

  { CorVerbPost,   "/ngsi-ld/v1/csourceRegistrations",   postCsourceRegistration,   LD_PARAMS_POST_CSOURCE_REGISTRATIONS, LdOpCreateRegistration   },
  { CorVerbGet,    "/ngsi-ld/v1/csourceRegistrations",   getCsourceRegistrations,   LD_PARAMS_GET_CSOURCE_REGISTRATIONS,  LdOpQueryRegistration    },
  { CorVerbGet,    "/ngsi-ld/v1/csourceRegistrations/*", getCsourceRegistration,    LD_PARAMS_GET_CSOURCE_REGISTRATION,   LdOpRetrieveRegistration },
  { CorVerbPatch,  "/ngsi-ld/v1/csourceRegistrations/*", patchCsourceRegistration,  LD_PARAMS_PATCH_CSOURCE_REGISTRATION, LdOpUpdateRegistration   },
  { CorVerbDelete, "/ngsi-ld/v1/csourceRegistrations/*", deleteCsourceRegistration, LD_PARAMS_DELETE_CSOURCE_REGISTRATION, LdOpDeleteRegistration  },

  // Context Source Registration Subscriptions (§ 5.11)
  { CorVerbPost,   "/ngsi-ld/v1/csourceSubscriptions",   postCsourceSubscriptions,   LD_PARAMS_POST_CSOURCE_SUBSCRIPTIONS,   LdOpCreateCsourceSubscription   },
  { CorVerbGet,    "/ngsi-ld/v1/csourceSubscriptions",   getCsourceSubscriptions,    LD_PARAMS_GET_CSOURCE_SUBSCRIPTIONS,    LdOpQueryCsourceSubscription    },
  { CorVerbGet,    "/ngsi-ld/v1/csourceSubscriptions/*", getCsourceSubscription,     LD_PARAMS_GET_CSOURCE_SUBSCRIPTION,     LdOpRetrieveCsourceSubscription },
  { CorVerbPatch,  "/ngsi-ld/v1/csourceSubscriptions/*", patchCsourceSubscription,   LD_PARAMS_PATCH_CSOURCE_SUBSCRIPTION,   LdOpUpdateCsourceSubscription   },
  { CorVerbDelete, "/ngsi-ld/v1/csourceSubscriptions/*", deleteCsourceSubscription,  LD_PARAMS_DELETE_CSOURCE_SUBSCRIPTION,  LdOpDeleteCsourceSubscription   },

  // EntityMap CRUD (§ 5.14)
  { CorVerbGet,    "/ngsi-ld/v1/entityMaps",    createEntityMap, LD_PARAMS_GET_ENTITIES, LdOpNone },
  { CorVerbPost,   "/ngsi-ld/v1/entityMaps",    postEntityMap,   0,                      LdOpNone },
  { CorVerbGet,    "/ngsi-ld/v1/entityMaps/*",  getEntityMap,    0,                      LdOpNone },
  { CorVerbDelete, "/ngsi-ld/v1/entityMaps/*",  deleteEntityMap, 0,                      LdOpNone },
  { CorVerbPatch,  "/ngsi-ld/v1/entityMaps/*",  patchEntityMap,  0,                      LdOpNone },

  // Batch Operations (§ 5.6.7 – § 5.6.10, § 5.6.20, § 6.14 – § 6.17, § 6.31)
  { CorVerbPost,   "/ngsi-ld/v1/entityOperations/create", postEntityBatchCreate, LD_PARAM_LOCAL,         LdOpBatchCreate, NULL, { .features.entityArrayBody = 1 } },
  { CorVerbPost,   "/ngsi-ld/v1/entityOperations/update", postEntityBatchUpdate, LD_PARAM_OPTIONS | LD_PARAM_LOCAL, LdOpBatchUpdate, NULL, { .features.entityArrayBody = 1 } },
  { CorVerbPost,   "/ngsi-ld/v1/entityOperations/upsert", postEntityBatchUpsert, LD_PARAM_OPTIONS | LD_PARAM_LOCAL, LdOpBatchUpsert, NULL, { .features.entityArrayBody = 1 } },
  { CorVerbPost,   "/ngsi-ld/v1/entityOperations/merge",  postEntityBatchMerge,  LD_PARAM_LOCAL,         LdOpBatchMerge, NULL, { .features.entityArrayBody = 1 } },
  { CorVerbPost,   "/ngsi-ld/v1/entityOperations/delete", postEntityBatchDelete, LD_PARAM_LOCAL,         LdOpBatchDelete },
  { CorVerbPost,   "/ngsi-ld/v1/entityOperations/query",  postEntityBatchQuery,  LD_PARAMS_GET_ENTITIES, LdOpBatchQuery, pCheckQuery },

  // Context Source Identity (§ 5.15 / § 6.33)
  { CorVerbGet,    "/ngsi-ld/v1/info/sourceIdentity", getSourceIdentity, 0, LdOpNone },

  // Broker product / version handshake (non-NGSI-LD).
  { CorVerbGet,    "/version",                  getVersion,        0, LdOpNone },

  // Temporal Operations (§ 5.6.11-§ 5.6.16, § 5.7.3, § 5.7.4).
  // More-specific routes first — the router matches in registration order.
  { CorVerbPatch,  "/ngsi-ld/v1/temporal/entities/*/attrs/*/*",      patchEntityTemporalInstance,   0,                                 LdOpUpdateAttrInstanceTemporal },
  { CorVerbDelete, "/ngsi-ld/v1/temporal/entities/*/attrs/*/*",      deleteEntityTemporalInstance,  0,                                 LdOpDeleteAttrInstanceTemporal },
  { CorVerbDelete, "/ngsi-ld/v1/temporal/entities/*/attrs/*",        deleteEntityTemporalAttr,      LD_PARAMS_DELETE_TEMPORAL_ATTR,    LdOpDeleteAttrsTemporal },
  { CorVerbPost,   "/ngsi-ld/v1/temporal/entities/*/attrs",          postEntityTemporalAttrs,       0,                                 LdOpAppendAttrsTemporal },
  { CorVerbGet,    "/ngsi-ld/v1/temporal/entities/*",                getEntityTemporal,             LD_PARAMS_GET_TEMPORAL_ENTITY,     LdOpRetrieveTemporal },
  { CorVerbDelete, "/ngsi-ld/v1/temporal/entities/*",                deleteEntityTemporal,          0,                                 LdOpDeleteTemporal },
  { CorVerbGet,    "/ngsi-ld/v1/temporal/entities",                  getEntitiesTemporal,           LD_PARAMS_QUERY_TEMPORAL_ENTITIES, LdOpQueryTemporal },
  { CorVerbPost,   "/ngsi-ld/v1/temporal/entities",                  postEntitiesTemporal,          0,                                 LdOpUpsertTemporal },
  { CorVerbPost,   "/ngsi-ld/v1/temporal/entityOperations/query",    postTemporalEntityBatchQuery,  LD_PARAMS_QUERY_TEMPORAL_ENTITIES, LdOpQueryTemporal },

  // Snapshots — § 5.16
  { CorVerbPost,   "/ngsi-ld/v1/snapshots",                          postSnapshot,                  LD_PARAMS_POST_SNAPSHOT,           LdOpCreateSnapshot   },
  { CorVerbGet,    "/ngsi-ld/v1/snapshots/*",                        getSnapshot,                   0,                                 LdOpRetrieveSnapshot },
  { CorVerbDelete, "/ngsi-ld/v1/snapshots/*",                        deleteSnapshot,                0,                                 LdOpDeleteSnapshot   },
  { CorVerbPatch,  "/ngsi-ld/v1/snapshots/*",                        patchSnapshot,                 0,                                 LdOpUpdateSnapshot   },
  { CorVerbPost,   "/ngsi-ld/v1/snapshots/*/clone",                  cloneSnapshot,                 0,                                 LdOpCloneSnapshot    },
  { CorVerbDelete, "/ngsi-ld/v1/snapshots",                          purgeSnapshots,                LD_PARAMS_PURGE_SNAPSHOTS,         LdOpPurgeSnapshots   }
};

int ngsildCoreServiceCount = sizeof(ngsildCoreServices) / sizeof(ngsildCoreServices[0]);



// -----------------------------------------------------------------------------
//
// serviceBuild - build combined flat service array from core + all API plugins
//
// Returns a malloc'd array.  *totalCountP is set to the total number of entries.
//
CorRestServiceSimplified* serviceBuild(int* totalCountP)
{
  //
  // Count total services: core + all API plugins
  //
  int total = ngsildCoreServiceCount;

  for (int i = 0; i < apiPluginCount; i++)
    total += apiPlugins[i].serviceCount;

  //
  // Allocate the combined array
  //
  CorRestServiceSimplified* allServices = (CorRestServiceSimplified*) malloc(total * sizeof(CorRestServiceSimplified));
  if (allServices == NULL)
    return NULL;

  //
  // Copy core services
  //
  int offset = 0;
  memcpy(&allServices[offset], ngsildCoreServices, ngsildCoreServiceCount * sizeof(CorRestServiceSimplified));
  offset += ngsildCoreServiceCount;

  //
  // Copy plugin services
  //
  for (int i = 0; i < apiPluginCount; i++)
  {
    ApiPlugin* p = &apiPlugins[i];
    if (p->serviceCount > 0 && p->services != NULL)
    {
      memcpy(&allServices[offset], p->services, p->serviceCount * sizeof(CorRestServiceSimplified));
      offset += p->serviceCount;
    }
  }

  *totalCountP = total;
  return allServices;
}
