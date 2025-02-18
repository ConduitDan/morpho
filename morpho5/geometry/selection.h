/** @file selection.c
 *  @author T J Atherton
 *
 *  @brief Selections
 */

#ifndef selection_h
#define selection_h

#include "mesh.h"

#define SELECTION_CLASSNAME "Selection"
#define SELECTION_ISSELECTEDMETHOD "isselected"
#define SELECTION_IDLISTFORGRADEMETHOD "idlistforgrade"
#define SELECTION_ADDGRADEMETHOD "addgrade"
#define SELECTION_REMOVEGRADEMETHOD "removegrade"

#define SELECTION_COMPLEMENTMETHOD "complement"

#define SELECTION_BOUNDARYOPTION "boundary"
#define SELECTION_PARTIALSOPTION "partials"

#define SELECTION_NOMESH                     "SlNoMsh"
#define SELECTION_NOMESH_MSG                 "Selection requires a Mesh object as an argument."

#define SELECTION_ISSLCTDARG                 "SlIsSlArg"
#define SELECTION_ISSLCTDARG_MSG             "Selection.isselected requires a grade and element id as arguments."

#define SELECTION_GRADEARG                   "SlGrdArg"
#define SELECTION_GRADEARG_MSG               "Method requires a grade as the argument."

#define SELECTION_STARG                      "SlStArg"
#define SELECTION_STARG_MSG                  "Selection set methods require a selection as the argument."

#define SELECTION_BND                        "SlBnd"
#define SELECTION_BND_MSG                    "Mesh has no boundary elements."

void selection_clear(objectselection *s);

bool selection_isselected(objectselection *sel, grade g, elementid id);
void selection_initialize(void);

#endif /* selection_h */
