#ifndef SRC_LIB_SERVICEROUTINES_DISCOVERYIRI_H_
#define SRC_LIB_SERVICEROUTINES_DISCOVERYIRI_H_

//
// FILE            discoveryIri.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//



// -----------------------------------------------------------------------------
//
// discoveryIri - the full URI of an Attribute or Entity Type name, as the broker holds it
//
// The `id` of an Attribute (§ 5.2.6.10.1) and of an EntityType / EntityTypeInfo
// (§ 5.2.6.10.3, § 5.2.6.10.5) is the "Full URI" of the name. The broker's internal
// form of a CORE term is its short name ("location", "status" - corLdExpand returns
// it, and that is what the stores hold), every other name is held expanded. A core
// short name gives the IRI of the term in the core @context as published (the pristine
// copy - the working core has every id rewritten to its short name); anything else is
// returned as it is.
//
// The name is not copied: the result is either the argument or a core item's id.
//
extern const char* discoveryIri(const char* name);

#endif  // SRC_LIB_SERVICEROUTINES_DISCOVERYIRI_H_
