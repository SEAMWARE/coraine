//
// FILE            ngsildServices.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdlib.h>                              // malloc
#include <string.h>                              // memcpy

#include "swRest/SwRestService.h"                // SwRestServiceSimplified, SwRestVerb
#include "swRest/SwRestVerb.h"                   // SwVerbGet, SwVerbPost, SwVerbDelete, SwVerbPatch
#include "swNgsild/ldParams.h"                   // LD_PARAMS_GET_ENTITIES, LD_PARAMS_GET_ENTITY, LD_PARAMS_POST_ENTITIES, LD_PARAMS_DELETE_ENTITY, LD_PARAMS_PATCH_ENTITY
#include "swNgsild/LdOp.h"                       // LdOp*

#include "serviceRoutines/getEntities.h"         // getEntities
#include "serviceRoutines/getEntity.h"           // getEntity
#include "serviceRoutines/postEntities.h"        // postEntities
#include "serviceRoutines/deleteEntity.h"        // deleteEntity
#include "serviceRoutines/patchEntity.h"         // patchEntity
#include "serviceRoutines/replaceEntity.h"       // replaceEntity
#include "serviceRoutines/postEntityAttrs.h"     // postEntityAttrs
#include "serviceRoutines/patchEntityAttrs.h"    // patchEntityAttrs
#include "serviceRoutines/purgeEntities.h"       // purgeEntities
#include "serviceRoutines/getEntityAttr.h"       // getEntityAttr
#include "serviceRoutines/patchEntityAttr.h"     // patchEntityAttr
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
#include "serviceRoutines/getEntityTemporal.h"         // getEntityTemporal
#include "serviceRoutines/getEntitiesTemporal.h"       // getEntitiesTemporal
#include "serviceRoutines/postTemporalEntityBatchQuery.h" // postTemporalEntityBatchQuery
#include "serviceRoutines/deleteEntityTemporal.h"      // deleteEntityTemporal
#include "serviceRoutines/deleteEntityTemporalAttr.h"  // deleteEntityTemporalAttr
#include "serviceRoutines/postEntitiesTemporal.h"      // postEntitiesTemporal
#include "serviceRoutines/postEntityTemporalAttrs.h"   // postEntityTemporalAttrs

#include "plugin/ApiPlugin.h"                    // ApiPlugin, apiPlugins, apiPluginCount

#include "ngsildServices.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// ngsildCoreServices - flat array of core NGSI-LD services
//
SwRestServiceSimplified ngsildCoreServices[] =
{
  { SwVerbGet,    "/ngsi-ld/v1/entities",   getEntities,  LD_PARAMS_GET_ENTITIES,   LdOpQueryEntities  },
  { SwVerbGet,    "/ngsi-ld/v1/entities/*", getEntity,    LD_PARAMS_GET_ENTITY,     LdOpRetrieveEntity },
  { SwVerbPost,   "/ngsi-ld/v1/entities",   postEntities, LD_PARAMS_POST_ENTITIES,  LdOpCreateEntity   },
  { SwVerbDelete, "/ngsi-ld/v1/entities/*", deleteEntity, LD_PARAMS_DELETE_ENTITY,  LdOpDeleteEntity   },
  { SwVerbPatch,  "/ngsi-ld/v1/entities/*", patchEntity,  LD_PARAMS_PATCH_ENTITY,   LdOpMergeEntity    },
  { SwVerbPut,    "/ngsi-ld/v1/entities/*", replaceEntity, LD_PARAMS_REPLACE_ENTITY, LdOpReplaceEntity },
  { SwVerbPost,   "/ngsi-ld/v1/entities/*/attrs", postEntityAttrs,  LD_PARAMS_POST_ENTITY_ATTRS,  LdOpAppendAttrs },
  { SwVerbPatch,  "/ngsi-ld/v1/entities/*/attrs", patchEntityAttrs, LD_PARAMS_PATCH_ENTITY_ATTRS, LdOpUpdateAttrs },
  { SwVerbGet,    "/ngsi-ld/v1/entities/*/attrs/*", getEntityAttr,    LD_PARAMS_GET_ENTITY_ATTR,    LdOpRetrieveEntity },
  { SwVerbPatch,  "/ngsi-ld/v1/entities/*/attrs/*", patchEntityAttr,  LD_PARAMS_PATCH_ENTITY_ATTR,  LdOpUpdateAttrs    },
  { SwVerbPut,    "/ngsi-ld/v1/entities/*/attrs/*", putEntityAttr,    LD_PARAMS_PUT_ENTITY_ATTR,    LdOpReplaceAttr    },
  { SwVerbDelete, "/ngsi-ld/v1/entities/*/attrs/*", deleteEntityAttr, LD_PARAMS_DELETE_ENTITY_ATTR, LdOpDeleteAttr     },
  { SwVerbDelete, "/ngsi-ld/v1/entities",         purgeEntities,    LD_PARAMS_PURGE_ENTITIES,     LdOpPurgeEntity    },

  // Discovery (§ 5.7.5 – § 5.7.10)
  { SwVerbGet,    "/ngsi-ld/v1/types",            getTypes,         LD_PARAMS_GET_TYPES,          LdOpRetrieveEntityTypes       },
  { SwVerbGet,    "/ngsi-ld/v1/types/*",          getType,          LD_PARAMS_GET_TYPE,           LdOpRetrieveEntityTypeInfo    },
  { SwVerbGet,    "/ngsi-ld/v1/attributes",       getAttributes,    LD_PARAMS_GET_ATTRIBUTES,     LdOpRetrieveAttrTypes         },
  { SwVerbGet,    "/ngsi-ld/v1/attributes/*",     getAttribute,     LD_PARAMS_GET_ATTRIBUTE,      LdOpRetrieveAttrTypeInfo      },

  { SwVerbPost,   "/ngsi-ld/v1/subscriptions",   postSubscriptions,   LD_PARAMS_POST_SUBSCRIPTIONS,   LdOpCreateSubscription   },
  { SwVerbGet,    "/ngsi-ld/v1/subscriptions",   getSubscriptions,    LD_PARAMS_GET_SUBSCRIPTIONS,    LdOpQuerySubscription    },
  { SwVerbGet,    "/ngsi-ld/v1/subscriptions/*", getSubscription,     LD_PARAMS_GET_SUBSCRIPTION,     LdOpRetrieveSubscription },
  { SwVerbPatch,  "/ngsi-ld/v1/subscriptions/*", patchSubscription,   LD_PARAMS_PATCH_SUBSCRIPTION,   LdOpUpdateSubscription   },
  { SwVerbDelete, "/ngsi-ld/v1/subscriptions/*", deleteSubscription,  LD_PARAMS_DELETE_SUBSCRIPTION,  LdOpDeleteSubscription   },

  // Distributed-subscription notification receiver (§ 5.8.1.4) — invoked
  // by remote Context Sources when a derived sub fires; the body is
  // re-dispatched to the original local sub's notification endpoint.
  { SwVerbPost,   "/ngsi-ld/ex/v1/notifications/*", postExNotification, 0, LdOpNone },

  { SwVerbGet,    "/ngsi-ld/v1/jsonldContexts",   getJsonldContexts, LD_PARAMS_GET_JSONLD_CONTEXTS, LdOpNone },
  { SwVerbGet,    "/ngsi-ld/v1/jsonldContexts/**", getJsonldContext, LD_PARAMS_GET_JSONLD_CONTEXT,  LdOpNone },
  { SwVerbPost,   "/ngsi-ld/v1/jsonldContexts",   postJsonldContexts,  LD_PARAMS_POST_JSONLD_CONTEXTS,  LdOpNone },
  { SwVerbDelete, "/ngsi-ld/v1/jsonldContexts/**", deleteJsonldContext, LD_PARAMS_DELETE_JSONLD_CONTEXT, LdOpNone },

  { SwVerbPost,   "/ngsi-ld/v1/csourceRegistrations",   postCsourceRegistration,   LD_PARAMS_POST_CSOURCE_REGISTRATIONS, LdOpCreateRegistration   },
  { SwVerbGet,    "/ngsi-ld/v1/csourceRegistrations",   getCsourceRegistrations,   LD_PARAMS_GET_CSOURCE_REGISTRATIONS,  LdOpQueryRegistration    },
  { SwVerbGet,    "/ngsi-ld/v1/csourceRegistrations/*", getCsourceRegistration,    LD_PARAMS_GET_CSOURCE_REGISTRATION,   LdOpRetrieveRegistration },
  { SwVerbPatch,  "/ngsi-ld/v1/csourceRegistrations/*", patchCsourceRegistration,  LD_PARAMS_PATCH_CSOURCE_REGISTRATION, LdOpUpdateRegistration   },
  { SwVerbDelete, "/ngsi-ld/v1/csourceRegistrations/*", deleteCsourceRegistration, LD_PARAMS_DELETE_CSOURCE_REGISTRATION, LdOpDeleteRegistration  },

  // Context Source Registration Subscriptions (§ 5.11)
  { SwVerbPost,   "/ngsi-ld/v1/csourceSubscriptions",   postCsourceSubscriptions,   LD_PARAMS_POST_CSOURCE_SUBSCRIPTIONS,   LdOpCreateCsourceSubscription   },
  { SwVerbGet,    "/ngsi-ld/v1/csourceSubscriptions",   getCsourceSubscriptions,    LD_PARAMS_GET_CSOURCE_SUBSCRIPTIONS,    LdOpQueryCsourceSubscription    },
  { SwVerbGet,    "/ngsi-ld/v1/csourceSubscriptions/*", getCsourceSubscription,     LD_PARAMS_GET_CSOURCE_SUBSCRIPTION,     LdOpRetrieveCsourceSubscription },
  { SwVerbPatch,  "/ngsi-ld/v1/csourceSubscriptions/*", patchCsourceSubscription,   LD_PARAMS_PATCH_CSOURCE_SUBSCRIPTION,   LdOpUpdateCsourceSubscription   },
  { SwVerbDelete, "/ngsi-ld/v1/csourceSubscriptions/*", deleteCsourceSubscription,  LD_PARAMS_DELETE_CSOURCE_SUBSCRIPTION,  LdOpDeleteCsourceSubscription   },

  // EntityMap CRUD (§ 5.14)
  { SwVerbGet,    "/ngsi-ld/v1/entityMaps",    createEntityMap, LD_PARAMS_GET_ENTITIES, LdOpNone },
  { SwVerbPost,   "/ngsi-ld/v1/entityMaps",    postEntityMap,   0,                      LdOpNone },
  { SwVerbGet,    "/ngsi-ld/v1/entityMaps/*",  getEntityMap,    0,                      LdOpNone },
  { SwVerbDelete, "/ngsi-ld/v1/entityMaps/*",  deleteEntityMap, 0,                      LdOpNone },
  { SwVerbPatch,  "/ngsi-ld/v1/entityMaps/*",  patchEntityMap,  0,                      LdOpNone },

  // Batch Operations (§ 5.6.7 – § 5.6.10, § 5.6.20, § 6.14 – § 6.17, § 6.31)
  { SwVerbPost,   "/ngsi-ld/v1/entityOperations/create", postEntityBatchCreate, 0,                      LdOpBatchCreate },
  { SwVerbPost,   "/ngsi-ld/v1/entityOperations/update", postEntityBatchUpdate, 0,                      LdOpBatchUpdate },
  { SwVerbPost,   "/ngsi-ld/v1/entityOperations/upsert", postEntityBatchUpsert, LD_PARAM_OPTIONS,       LdOpBatchUpsert },
  { SwVerbPost,   "/ngsi-ld/v1/entityOperations/merge",  postEntityBatchMerge,  0,                      LdOpBatchMerge  },
  { SwVerbPost,   "/ngsi-ld/v1/entityOperations/delete", postEntityBatchDelete, 0,                      LdOpBatchDelete },
  { SwVerbPost,   "/ngsi-ld/v1/entityOperations/query",  postEntityBatchQuery,  LD_PARAMS_GET_ENTITIES, LdOpBatchQuery  },

  // Context Source Identity (§ 5.15 / § 6.33)
  { SwVerbGet,    "/info/sourceIdentity",      getSourceIdentity, 0, LdOpNone },

  // Temporal Operations (§ 5.6.11-§ 5.6.16, § 5.7.3, § 5.7.4).
  // More-specific routes first — the router matches in registration order.
  { SwVerbDelete, "/ngsi-ld/v1/temporal/entities/*/attrs/*",        deleteEntityTemporalAttr,      LD_PARAMS_DELETE_TEMPORAL_ATTR,    LdOpNone },
  { SwVerbPost,   "/ngsi-ld/v1/temporal/entities/*/attrs",          postEntityTemporalAttrs,       0,                                 LdOpNone },
  { SwVerbGet,    "/ngsi-ld/v1/temporal/entities/*",                getEntityTemporal,             LD_PARAMS_GET_TEMPORAL_ENTITY,     LdOpNone },
  { SwVerbDelete, "/ngsi-ld/v1/temporal/entities/*",                deleteEntityTemporal,          0,                                 LdOpNone },
  { SwVerbGet,    "/ngsi-ld/v1/temporal/entities",                  getEntitiesTemporal,           LD_PARAMS_QUERY_TEMPORAL_ENTITIES, LdOpNone },
  { SwVerbPost,   "/ngsi-ld/v1/temporal/entities",                  postEntitiesTemporal,          0,                                 LdOpNone },
  { SwVerbPost,   "/ngsi-ld/v1/temporal/entityOperations/query",    postTemporalEntityBatchQuery,  LD_PARAMS_QUERY_TEMPORAL_ENTITIES, LdOpNone }
};

int ngsildCoreServiceCount = sizeof(ngsildCoreServices) / sizeof(ngsildCoreServices[0]);



// -----------------------------------------------------------------------------
//
// serviceBuild - build combined flat service array from core + all API plugins
//
// Returns a malloc'd array.  *totalCountP is set to the total number of entries.
//
SwRestServiceSimplified* serviceBuild(int* totalCountP)
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
  SwRestServiceSimplified* allServices = (SwRestServiceSimplified*) malloc(total * sizeof(SwRestServiceSimplified));
  if (allServices == NULL)
    return NULL;

  //
  // Copy core services
  //
  int offset = 0;
  memcpy(&allServices[offset], ngsildCoreServices, ngsildCoreServiceCount * sizeof(SwRestServiceSimplified));
  offset += ngsildCoreServiceCount;

  //
  // Copy plugin services
  //
  for (int i = 0; i < apiPluginCount; i++)
  {
    ApiPlugin* p = &apiPlugins[i];
    if (p->serviceCount > 0 && p->services != NULL)
    {
      memcpy(&allServices[offset], p->services, p->serviceCount * sizeof(SwRestServiceSimplified));
      offset += p->serviceCount;
    }
  }

  *totalCountP = total;
  return allServices;
}
