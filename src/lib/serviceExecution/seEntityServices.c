//
// FILE            seEntityServices.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeString, corTreeBoolean, corTreeChildAdd
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corRest/CorRestState.h"                     // corRest
#include "corNgsild/CorNgsild.h"                      // corNgsild

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant
#include "serviceExecution/seRegistrationMatch.h"     // seRegistrationMatches
#include "serviceExecution/seRequest.h"               // seRequestParam
#include "corNgsild/ldServiceDescription.h"           // ldServiceDescriptionIs
#include "serviceExecution/seEntityServices.h"        // Own interface



// -----------------------------------------------------------------------------
//
// isTrue - a URL parameter that is "true"
//
static bool isTrue(const char* name)
{
  const char* value = seRequestParam(name);

  return (value != NULL) && (strcmp(value, "true") == 0);
}



// -----------------------------------------------------------------------------
//
// description - a registration's service, as the entity shows it
//
static CorNode* description(CorNode* regP, bool details)
{
  CorNode*    siP  = corTreeLookup(regP, "serviceInformation");
  CorNode*    nameP = corTreeLookup(siP, "serviceName");
  CorNode*    descP = corTreeObject(corRest.kallocP, nameP->value.s);
  const char* copied[] = { "title", "description", NULL };

  corTreeChildAdd(descP, corTreeString(corRest.kallocP, "type", "ServiceDescription"));

  for (int ix = 0; copied[ix] != NULL; ix++)
  {
    CorNode* mP = corTreeLookup(siP, copied[ix]);

    if (mP != NULL)
      corTreeChildAdd(descP, corTreeClone(corRest.kallocP, mP));
  }

  CorNode* modeP = corTreeLookup(siP, "mode");

  corTreeChildAdd(descP, corTreeString(corRest.kallocP, "mode", ((modeP != NULL) && (modeP->type == CorString)) ? modeP->value.s : "synchronous"));

  if (details == true)
  {
    CorNode* inP  = corTreeLookup(siP, "inputSchema");
    CorNode* outP = corTreeLookup(siP, "outputSchema");

    if (inP != NULL)  corTreeChildAdd(descP, corTreeClone(corRest.kallocP, inP));
    if (outP != NULL) corTreeChildAdd(descP, corTreeClone(corRest.kallocP, outP));
  }

  corTreeChildAdd(descP, corTreeBoolean(corRest.kallocP, "serviceDescriptionInEntity", false));

  return descP;
}



// -----------------------------------------------------------------------------
//
// inEntity - the entity's own Service Descriptions (GR CIM-055 § 6.3.3): hidden without
// ?includeServices; with it, serviceDescriptionInEntity true, and their schemas only with ?serviceDetails
//
static void inEntity(CorNode* entityP, bool include, bool details)
{
  CorNode* attrP = entityP->value.head;

  while (attrP != NULL)
  {
    CorNode* nextP = attrP->next;
    CorNode* instP = (attrP->type == CorObject) ? corTreeLookup(attrP, "@none") : NULL;

    if (ldServiceDescriptionIs(instP))
    {
      if (include == false)
        corTreeChildRemove(entityP, attrP);
      else
      {
        CorNode* flagP = corTreeLookup(instP, "serviceDescriptionInEntity");

        if (flagP != NULL)
          corTreeChildRemove(instP, flagP);
        corTreeChildAdd(instP, corTreeBoolean(corRest.kallocP, "serviceDescriptionInEntity", true));

        if (details == false)
        {
          CorNode* inP  = corTreeLookup(instP, "inputSchema");
          CorNode* outP = corTreeLookup(instP, "outputSchema");

          if (inP  != NULL) corTreeChildRemove(instP, inP);
          if (outP != NULL) corTreeChildRemove(instP, outP);
        }
      }
    }

    attrP = nextP;
  }
}



// -----------------------------------------------------------------------------
//
// entityAdd - the services of one entity
//
static void entityAdd(CorNode* entityP, CorNode* regsP, bool details)
{
  if ((entityP == NULL) || (entityP->type != CorObject) || (regsP == NULL))
    return;

  CorNode* idP   = corTreeLookup(entityP, "id");
  CorNode* typeP = corTreeLookup(entityP, "type");
  char*    typeV[16];
  int      typeN = 0;

  if ((idP == NULL) || (idP->type != CorString))
    return;

  if ((typeP != NULL) && (typeP->type == CorString))
    typeV[typeN++] = typeP->value.s;
  else if ((typeP != NULL) && (typeP->type == CorArray))
  {
    for (CorNode* tP = typeP->value.head; (tP != NULL) && (typeN < 15); tP = tP->next)
    {
      if (tP->type == CorString)
        typeV[typeN++] = tP->value.s;
    }
  }
  typeV[typeN] = NULL;

  for (CorNode* regP = regsP->value.head; regP != NULL; regP = regP->next)
  {
    CorNode* siP   = corTreeLookup(regP, "serviceInformation");
    CorNode* nameP = (siP != NULL) ? corTreeLookup(siP, "serviceName") : NULL;

    if ((nameP == NULL) || (nameP->type != CorString) || (corTreeLookup(entityP, nameP->value.s) != NULL))
      continue;                                       // an attribute of that name wins - it is the entity's

    if (seRegistrationMatches(regP, idP->value.s, typeV, entityP))
    {
      //
      // In the stored form, as every attribute of the entity at this point: its instances keyed by
      // datasetId, the one instance "@none" - the render (ldEntityToApi) unwraps them all alike
      //
      CorNode* attrP = corTreeObject(corRest.kallocP, nameP->value.s);
      CorNode* instP = description(regP, details);

      instP->name = (char*) "@none";
      corTreeChildAdd(attrP, instP);
      corTreeChildAdd(entityP, attrP);
    }
  }
}



// -----------------------------------------------------------------------------
//
// seEntityServicesAdd -
//
void seEntityServicesAdd(CorNode* treeP)
{
  CorNode* regsP = NULL;

  if (treeP == NULL)
    return;

  bool include = isTrue("includeServices");
  bool details = isTrue("serviceDetails");

  if ((include == true) && (db.docQuery != NULL) &&
      ((db.docQuery((Tenant*) corNgsild.tenantP, "serviceRegistrations", &regsP) != DB_OK) || (regsP->value.head == NULL)))
    regsP = NULL;

  if (treeP->type == CorArray)
  {
    for (CorNode* eP = treeP->value.head; eP != NULL; eP = eP->next)
    {
      inEntity(eP, include, details);
      if (include == true)
        entityAdd(eP, regsP, details);
    }
  }
  else if (treeP->type == CorObject)
  {
    inEntity(treeP, include, details);
    if (include == true)
      entityAdd(treeP, regsP, details);
  }
}
