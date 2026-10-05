//
// FILE            dbChanges.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corTree/corTreeBuilder.h"                  // corTreeArray, corTreeObject, corTreeString, corTreeChildAdd
#include "corTree/corTreeClone.h"                    // corTreeClone

#include "corRest/CorRestState.h"                    // corRest
#include "corNgsild/ldTermId.h"                      // ldNodeRename

#include "db/dbChanges.h"                            // Own interface



// -----------------------------------------------------------------------------
//
// entryOf - the entry of 'attr' in a report, NULL if none
//
static CorNode* entryOf(LdMergeReport* reportP, const char* attr)
{
  for (CorNode* eP = reportP->changes->value.head; eP != NULL; eP = eP->next)
  {
    CorNode* aP = corTreeLookup(eP, "attr");

    if ((aP != NULL) && (aP->type == CorString) && (strcmp(aP->value.s, attr) == 0))
      return eP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// dbChangesAdd -
//
void dbChangesAdd(LdMergeReport* dbReportP, LdMergeReport* fragReportP)
{
  if (dbReportP->changes == NULL)
    dbReportP->changes = corTreeArray(corRest.kallocP, "changes");

  if ((fragReportP == NULL) || (fragReportP->changes == NULL))
    return;

  for (CorNode* changeP = fragReportP->changes->value.head; changeP != NULL; changeP = changeP->next)
  {
    CorNode* attrP = corTreeLookup(changeP, "attr");

    if ((attrP == NULL) || (attrP->type != CorString) || (entryOf(dbReportP, attrP->value.s) != NULL))
      continue;                                      // the first report that touched it has its pre-write state

    CorNode* entryP = corTreeObject(corRest.kallocP, NULL);
    CorNode* preP   = corTreeLookup(changeP, "preValue");

    corTreeChildAdd(entryP, corTreeString(corRest.kallocP, "attr", attrP->value.s));
    corTreeChildAdd(entryP, corTreeString(corRest.kallocP, "reason", "attributeModified"));   // dbChangesFinish decides

    if (preP != NULL)
    {
      CorNode* cloneP = corTreeClone(corRest.kallocP, preP);

      if (cloneP != NULL)
      {
        ldNodeRename(cloneP, (char*) "preValue");
        corTreeChildAdd(entryP, cloneP);
      }
    }

    corTreeChildAdd(dbReportP->changes, entryP);
  }
}



// -----------------------------------------------------------------------------
//
// dbChangesFinish -
//
void dbChangesFinish(LdMergeReport* dbReportP, CorNode* entityP)
{
  if (dbReportP->changes == NULL)
    dbReportP->changes = corTreeArray(corRest.kallocP, "changes");

  for (CorNode* eP = dbReportP->changes->value.head; eP != NULL; eP = eP->next)
  {
    CorNode* attrP   = corTreeLookup(eP, "attr");
    CorNode* reasonP = corTreeLookup(eP, "reason");

    if ((attrP == NULL) || (reasonP == NULL) || (attrP->type != CorString) || (reasonP->type != CorString))
      continue;

    reasonP->value.s = (char*) ((corTreeLookup(entityP, attrP->value.s) == NULL) ? "attributeDeleted" : "attributeModified");
  }

  if (entryOf(dbReportP, "type") == NULL)
  {
    CorNode* entryP = corTreeObject(corRest.kallocP, NULL);

    corTreeChildAdd(entryP, corTreeString(corRest.kallocP, "attr", "type"));
    corTreeChildAdd(entryP, corTreeString(corRest.kallocP, "reason", "attributeModified"));
    corTreeChildAdd(dbReportP->changes, entryP);
  }
}
