/*------------------------------------------------------------------------------
 *
 * Copyright (c) 2019-Present Pivotal Software, Inc
 *
 *------------------------------------------------------------------------------
 */

#include "postgres.h"
#ifndef PLC_PG
  #include "commands/resgroupcmds.h"
  #include "catalog/gp_segment_config.h"
#else
  #include "catalog/pg_type.h"
  #include "access/sysattr.h"
  #include "miscadmin.h"

  #define InvalidDbid 0
#endif
#include "utils/builtins.h"
#include "utils/guc.h"
#include "libpq/libpq-be.h"
#include "utils/acl.h"

#include "funcapi.h"
#include <json-c/json.h>
#include "common/comm_dummy.h"
#include "plc/plc_docker_api.h"

