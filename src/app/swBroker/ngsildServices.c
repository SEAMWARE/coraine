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

#include "serviceRoutines/getEntities.h"         // getEntities
#include "serviceRoutines/getEntity.h"           // getEntity
#include "serviceRoutines/postEntities.h"        // postEntities
#include "serviceRoutines/deleteEntity.h"        // deleteEntity
#include "serviceRoutines/patchEntity.h"         // patchEntity

#include "plugin/ApiPlugin.h"                    // ApiPlugin, apiPlugins, apiPluginCount

#include "ngsildServices.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// ngsildCoreServices - flat array of core NGSI-LD services
//
SwRestServiceSimplified ngsildCoreServices[] =
{
  { SwVerbGet,    "/ngsi-ld/v1/entities",   getEntities,  LD_PARAMS_GET_ENTITIES   },
  { SwVerbGet,    "/ngsi-ld/v1/entities/*", getEntity,    LD_PARAMS_GET_ENTITY     },
  { SwVerbPost,   "/ngsi-ld/v1/entities",   postEntities, LD_PARAMS_POST_ENTITIES  },
  { SwVerbDelete, "/ngsi-ld/v1/entities/*", deleteEntity, LD_PARAMS_DELETE_ENTITY  },
  { SwVerbPatch,  "/ngsi-ld/v1/entities/*", patchEntity,  LD_PARAMS_PATCH_ENTITY   }
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
