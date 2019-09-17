#ifndef INCLUDE_UTILS_VCHECK_H_
#define INCLUDE_UTILS_VCHECK_H_


#include "utils/vtype.h"
#include "utils/tuplebatch.h"
#include "nodes/execnodes.h"

extern Plan* CheckAndReplacePlanVectorized(PlannerInfo *root, Plan *plan);
extern Oid GetVtype(Oid ntype);

#endif /* INCLUDE_UTILS_VCHECK_H_ */
