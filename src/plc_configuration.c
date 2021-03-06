/*------------------------------------------------------------------------------
 *
 * Copyright (c) 2016-Present Pivotal Software, Inc
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

#ifdef PLC_PG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "funcapi.h"
#ifdef PLC_PG
#pragma GCC diagnostic pop
#endif

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <unistd.h>
#include <sys/stat.h>

#include "common/comm_connectivity.h"
#include "common/comm_dummy.h"
#include "plc/plcontainer.h"
#include "plc/plc_backend_api.h"
#include "plc/plc_docker_api.h"
#include "plc/plc_configuration.h"

// we just want to avoid cleanup process to remove previous domain
// socket file, so int32 is sufficient
static int domain_socket_no = 0;

static void init_runtime_configurations();

static void parse_runtime_configuration(xmlNode *node);

static void get_runtime_configurations(xmlNode *node);

static void free_runtime_conf_entry(runtimeConfEntry *conf);

static void print_runtime_configurations();

PG_FUNCTION_INFO_V1(refresh_plcontainer_config);

PG_FUNCTION_INFO_V1(show_plcontainer_config);

static HTAB *rumtime_conf_table;

/*
 * init runtime conf hash table.
 */
static void init_runtime_configurations() {
    /* create the runtime conf hash table*/
	HASHCTL		hash_ctl;

	/* destroy hash table first if exists*/
	if (rumtime_conf_table != NULL) {
		HASH_SEQ_STATUS hash_status;
		runtimeConfEntry *entry;

		hash_seq_init(&hash_status, rumtime_conf_table);

		while ((entry = (runtimeConfEntry *) hash_seq_search(&hash_status)) != NULL)
		{
			free_runtime_conf_entry(entry);
		}
		hash_destroy(rumtime_conf_table);
	}
	
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = RUNTIME_ID_MAX_LENGTH;
	hash_ctl.entrysize = sizeof(runtimeConfEntry);
	hash_ctl.hash = string_hash;
	/*
	 * Key and value of hash table share the same storage of entry.
	 * the first keysize bytes of the entry is the hash key, and the
	 * value if the entry itself.
	 * For string key, we use string_hash to caculate hash key and
	 * use strlcpy to copy key(when string_hash is set as hash function).
	 */
	rumtime_conf_table = hash_create("runtime configuration hash",
								MAX_EXPECTED_RUNTIME_NUM,
								&hash_ctl,
								HASH_ELEM | HASH_FUNCTION);

	if (rumtime_conf_table == NULL) {
		plc_elog(ERROR, "Error: could not create runtime conf hash table. Check your memory usage.");
	}

	return ;
}

/* Function parses the container XML definition and fills the passed
 * plcContainerConf structure that should be already allocated */
static void parse_runtime_configuration(xmlNode *node) {
	xmlNode *cur_node = NULL;
	/* we add some cleanups after longjmp. longjmp may clobber the registers, 
	 * so we need to add volatile qualifier to pointer. If the pointee is read
	 * after longjmp, the pointee also need to include volatile qualifier.
	 * */
	xmlChar* volatile value = NULL;
	volatile char* volatile runtime_id = NULL;
	int id_num;
	int image_num;
	int command_num;
	int num_shared_dirs;


	runtimeConfEntry *conf_entry = NULL;
	bool		foundPtr;

	if (rumtime_conf_table == NULL) {
		plc_elog(ERROR, "Runtime configuration table is not initialized.");
	}

	PG_TRY();
	{
		id_num = 0;
		image_num = 0;
		command_num = 0;
		num_shared_dirs = 0;
		/* Find the hash key (runtime id) firstly.*/
		for (cur_node = node->children; cur_node; cur_node = cur_node->next) {
			if (cur_node->type == XML_ELEMENT_NODE &&
					xmlStrcmp(cur_node->name, (const xmlChar *) "id") == 0) {
				if (id_num++ > 0) {
					plc_elog(ERROR, "tag <id> must be specified only once in configuartion");
				}
				value = xmlNodeGetContent(cur_node);

				runtime_id = pstrdup((char *) value);
				if (value) {
					xmlFree((void *) value);
					value = NULL;
				}
			}
		}

		if (id_num == 0) {
			plc_elog(ERROR, "tag <id> must be specified in configuartion");
		}
		if ( strlen((char *)runtime_id) > RUNTIME_ID_MAX_LENGTH - 1) {
			plc_elog(ERROR, "runtime id should not be longer than 63 bytes.");
		}
		/* find the corresponding runtime config*/
		conf_entry = (runtimeConfEntry *) hash_search(rumtime_conf_table,  (const void *) runtime_id, HASH_ENTER, &foundPtr);

		/*check if runtime id already exists in hash table.*/
		if (foundPtr) {
			plc_elog(ERROR, "Detecting duplicated runtime id %s in configuration file", runtime_id);
		}

		/* First iteration - parse name, container_id and memory_mb and count the
		 * number of shared directories for later allocation of related structure */

		/*runtime_id will be freed with conf_entry*/
		conf_entry->memoryMb = 1024;
		conf_entry->cpuShare = 1024;
		conf_entry->useContainerLogging = false;
		conf_entry->useContainerNetwork = false;
		conf_entry->resgroupOid = InvalidOid;
		conf_entry->useUserControl = false;
		conf_entry->roles = NULL;


		for (cur_node = node->children; cur_node; cur_node = cur_node->next) {
			if (cur_node->type == XML_ELEMENT_NODE) {
				int processed = 0;
				value = NULL;

				if (xmlStrcmp(cur_node->name, (const xmlChar *) "id") == 0) {
					processed = 1;
				}

				if (xmlStrcmp(cur_node->name, (const xmlChar *) "image") == 0) {
					processed = 1;
					image_num++;
					value = xmlNodeGetContent(cur_node);
					conf_entry->image = plc_top_strdup((char *) value);
					if (value) {
						xmlFree((void *) value);
						value = NULL;
					}
				}

				if (xmlStrcmp(cur_node->name, (const xmlChar *) "command") == 0) {
					processed = 1;
					command_num++;
					value = xmlNodeGetContent(cur_node);
					conf_entry->command = plc_top_strdup((char *) value);
					if (value) {
						xmlFree((void *) value);
						value = NULL;
					}
				}

				if (xmlStrcmp(cur_node->name, (const xmlChar *) "setting") == 0) {
					bool validSetting = false;
					processed = 1;
					value = xmlGetProp(cur_node, (const xmlChar *) "use_container_logging");
					if (value != NULL) {
						validSetting = true;
						if (strcasecmp((char *) value, "yes") == 0) {
							conf_entry->useContainerLogging = true;
						} else if (strcasecmp((char *) value, "no") == 0) {
							conf_entry->useContainerLogging = false;
						} else {
							plc_elog(ERROR, "SETTING element <use_container_logging> only accepted \"yes\" or"
								"\"no\" only, current string is %s", value);
						}
						xmlFree((void *) value);
						value = NULL;
					}
					value = xmlGetProp(cur_node, (const xmlChar *) "memory_mb");
					if (value != NULL) {
						long memorySize = pg_atoi((char *) value, sizeof(int), 0);
						validSetting = true;
						
						if (memorySize <= 0) {
							plc_elog(ERROR, "container memory size couldn't be equal or less than 0, current string is %s", value);
						} else {
							conf_entry->memoryMb = memorySize;
						}
						xmlFree((void *) value);
						value = NULL;
					}
					value = xmlGetProp(cur_node, (const xmlChar *) "cpu_share");
					if (value != NULL) {
						long cpuShare = pg_atoi((char *) value, sizeof(int), 0);
						validSetting = true;
						
						if (cpuShare <= 0) {
							plc_elog(ERROR, "container cpu share couldn't be equal or less than 0, current string is %s", value);
						} else {
							conf_entry->cpuShare = cpuShare;
						}
						xmlFree((void *) value);
						value = NULL;
					}
					/* Enforce to not use network for connection. In the future
					 * this should be set by various backend implementation.
					 */
					conf_entry->useContainerNetwork = false;
					value = xmlGetProp(cur_node, (const xmlChar *) "resource_group_id");
					if (value != NULL) {
						Oid resgroupOid;
						validSetting = true;
						if (strlen((char *) value) == 0) {
							plc_elog(ERROR, "SETTING length of element <resource_group_id> is zero");
						}
						resgroupOid = (Oid) pg_atoi((char *) value, sizeof(int), 0);
#ifndef	PLC_PG							
						if (resgroupOid == InvalidOid || GetResGroupNameForId(resgroupOid) == NULL) {
							plc_elog(ERROR, "SETTING element <resource_group_id> must be a resource group id in greenplum. " "Current setting is: %s", (char * ) value);
						}
						int32 memAuditor = GetResGroupMemAuditorForId(resgroupOid, AccessShareLock);
						if (memAuditor != RESGROUP_MEMORY_AUDITOR_CGROUP) {
							plc_elog(ERROR, "SETTING element <resource_group_id> must be a resource group with memory_auditor type cgroup.");
						}
#endif
						conf_entry->resgroupOid = resgroupOid;
						xmlFree((void *) value);
						value = NULL;
					}

					value = xmlGetProp(cur_node, (const xmlChar *) "roles");
					if (value != NULL) {
						validSetting = true;
						if (strlen((char *) value) == 0) {
							plc_elog(ERROR, "SETTING length of element <roles> is zero");
						}
						conf_entry->roles = plc_top_strdup((char *) value);
						conf_entry->useUserControl = true;
						xmlFree((void *) value);
						value = NULL;
					}

					if (!validSetting) {
						plc_elog(ERROR, "Unrecognized setting options, please check the configuration file: %s", conf_entry->runtimeid);
					}

				}

				if (xmlStrcmp(cur_node->name, (const xmlChar *) "shared_directory") == 0) {
					num_shared_dirs++;
					processed = 1;
				}

				/* If the tag is not known - we raise the related error */
				if (processed == 0) {
					plc_elog(ERROR, "Unrecognized element '%s' inside of container specification",
						 cur_node->name);
				}
			}
		}

		if (image_num > 1) {
			plc_elog(ERROR, "There are more than one 'image' subelement in a runtime element %s", conf_entry->runtimeid);
		}
		else if (image_num < 1) {
			plc_elog(ERROR, "Lack of 'image' subelement in a runtime element %s", conf_entry->runtimeid);
		}

		if (command_num > 1) {
			plc_elog(ERROR, "There are more than one 'command' subelement in a runtime element %s", conf_entry->runtimeid);
		}
		else if (command_num < 1) {
			plc_elog(ERROR, "Lack of 'command' subelement in a runtime element %s", conf_entry->runtimeid);
		}

		/* Process the shared directories */
		conf_entry->nSharedDirs = num_shared_dirs;
		conf_entry->sharedDirs = NULL;
		if (num_shared_dirs > 0) {
			int i = 0;
			int j = 0;

			/* Allocate in top context as it should live between function calls */
			conf_entry->sharedDirs = palloc(num_shared_dirs * sizeof(plcSharedDir));
			for (cur_node = node->children; cur_node; cur_node = cur_node->next) {
				if (cur_node->type == XML_ELEMENT_NODE &&
					xmlStrcmp(cur_node->name, (const xmlChar *) "shared_directory") == 0) {

					value = xmlGetProp(cur_node, (const xmlChar *) "host");
					if (value == NULL) {
						plc_elog(ERROR, "Configuration tag 'shared_directory' has a mandatory element"
							" 'host' that is not found: %s", conf_entry->runtimeid);
					}
					conf_entry->sharedDirs[i].host = plc_top_strdup((char *) value);
					xmlFree((void *) value);
					value = xmlGetProp(cur_node, (const xmlChar *) "container");
					if (value == NULL) {
						plc_elog(ERROR, "Configuration tag 'shared_directory' has a mandatory element"
							" 'container' that is not found: %s", conf_entry->runtimeid);
					}
					/* Shared folders will not be created a lot, so using array to search duplicated
					 * container path is enough.
					 * */
					for (j =0; j< i; j++) {
						if (strcasecmp((char *) value, conf_entry->sharedDirs[j].container) == 0) {
							plc_elog(ERROR, "Container path cannot be the same in 'shared_directory' element "
									"in the runtime %s", conf_entry->runtimeid);
						}
					}
					conf_entry->sharedDirs[i].container = plc_top_strdup((char *) value);
					xmlFree((void *) value);
					value = xmlGetProp(cur_node, (const xmlChar *) "access");
					if (value == NULL) {
						plc_elog(ERROR, "Configuration tag 'shared_directory' has a mandatory element"
							" 'access' that is not found: %s", conf_entry->runtimeid);
					} else if (strcmp((char *) value, "ro") == 0) {
						conf_entry->sharedDirs[i].mode = PLC_ACCESS_READONLY;
					} else if (strcmp((char *) value, "rw") == 0) {
						conf_entry->sharedDirs[i].mode = PLC_ACCESS_READWRITE;
					} else {
						plc_elog(ERROR, "Directory access mode should be either 'ro' or 'rw', but passed value is '%s': %s", value, conf_entry->runtimeid);
					}
					xmlFree((void *) value);
					value = NULL;
					i += 1;
				}
			}
		}
	}
	PG_CATCH();
	{
		if (value != NULL) {
			xmlFree((void *) value);
			value = NULL;
		}

		if (rumtime_conf_table != NULL && runtime_id != NULL) {
			/* remove the broken runtime config entry in hash table*/
			hash_search(rumtime_conf_table,  (const void *) runtime_id, HASH_REMOVE, NULL);

		}

		PG_RE_THROW();
	}
	PG_END_TRY();

	return ;
}

/* Function returns an array of plcContainerConf structures based on the contents
 * of passed XML document tree. Returns NULL on failure */
static void get_runtime_configurations(xmlNode *node) {
	xmlNode *cur_node = NULL;

	/* Validation that the root node matches the expected specification */
	if (xmlStrcmp(node->name, (const xmlChar *) "configuration") != 0) {
		plc_elog(ERROR, "Wrong XML configuration provided. Expected 'configuration'"
			" as root element, got '%s' instead", node->name);
	}

	/* Iterating through the list of containers to parse them */
	for (cur_node = node->children; cur_node; cur_node = cur_node->next) {
		if (cur_node->type == XML_ELEMENT_NODE &&
		    xmlStrcmp(cur_node->name, (const xmlChar *) "runtime") == 0) {
			parse_runtime_configuration(cur_node);
		}
	}

	/* If no container definitions found - error */
	if (hash_get_num_entries(rumtime_conf_table) == 0) {
		plc_elog(ERROR, "Did not find a single 'runtime' declaration in configuration");
	}

	return ;
}

/* Safe way to deallocate container configuration list structure */
static void free_runtime_conf_entry(runtimeConfEntry *entry) {
	int i;

	if (entry->image)
		pfree(entry->image);
	if (entry->command)
		pfree(entry->command);

	for (i = 0; i < entry->nSharedDirs; i++) {
		if (entry->sharedDirs[i].container)
			pfree(entry->sharedDirs[i].container);
		if (entry->sharedDirs[i].host)
			pfree(entry->sharedDirs[i].host);
	}
	if (entry->sharedDirs)
		pfree(entry->sharedDirs);
}

static void print_runtime_configurations() {
	int j = 0;
	if (rumtime_conf_table != NULL) {
		HASH_SEQ_STATUS hash_status;
		runtimeConfEntry *conf_entry;

		hash_seq_init(&hash_status, rumtime_conf_table);

		while ((conf_entry = (runtimeConfEntry *) hash_seq_search(&hash_status)) != NULL)
		{
			plc_elog(INFO, "Container '%s' configuration", conf_entry->runtimeid);
			plc_elog(INFO, "    image = '%s'", conf_entry->image);
			plc_elog(INFO, "    memory_mb = '%d'", conf_entry->memoryMb);
			plc_elog(INFO, "    cpu_share = '%d'", conf_entry->cpuShare);
			plc_elog(INFO, "    use container logging  = '%s'", conf_entry->useContainerLogging ? "yes" : "no");
			if (conf_entry->useUserControl){
				plc_elog(INFO, "    allowed roles list  = '%s'", conf_entry->roles);
			}
			if (conf_entry->resgroupOid != InvalidOid)
			{
				plc_elog(INFO, "    resource group id  = '%u'", conf_entry->resgroupOid);
			}
			for (j = 0; j < conf_entry->nSharedDirs; j++) {
				plc_elog(INFO, "    shared directory from host '%s' to container '%s'",
					 conf_entry->sharedDirs[j].host,
					 conf_entry->sharedDirs[j].container);
				if (conf_entry->sharedDirs[j].mode == PLC_ACCESS_READONLY) {
					plc_elog(INFO, "        access = readonly");
				} else {
					plc_elog(INFO, "        access = readwrite");
				}
			}
		}
	}
}

int plc_refresh_container_config(bool verbose) {
	xmlDoc* volatile doc = NULL;
	char filename[1024];
 #ifdef PLC_PG
    char data_directory[1024];
	char *env_str;
 #endif  
	init_runtime_configurations();
	/*
	 * this initialize the library and check potential ABI mismatches
	 * between the version it was compiled for and the actual shared
	 * library used.
	 */
	LIBXML_TEST_VERSION

 #ifdef PLC_PG
    if ((env_str = getenv("PGDATA")) == NULL)
        plc_elog (ERROR, "PGDATA is not set");
	snprintf(data_directory, sizeof(data_directory), "%s", env_str );
 #endif   
	/* Parse the file and get the DOM */
	sprintf(filename, "%s/%s", data_directory, PLC_PROPERTIES_FILE);

	PG_TRY();
	{
		doc = xmlReadFile(filename, NULL, 0);
		if (doc == NULL) {
			plc_elog(ERROR, "Error: could not parse file %s, wrongly formatted XML or missing configuration file\n", filename);
			return -1;
		}

		get_runtime_configurations(xmlDocGetRootElement(doc));
	}
	PG_CATCH();
	{
		if (doc != NULL) {
			xmlFreeDoc(doc);
		}

		PG_RE_THROW();
	}
	PG_END_TRY();


	/* Free the document */
	xmlFreeDoc(doc);

	/* Free the global variables that may have been allocated by the parser */
	xmlCleanupParser();

	if (hash_get_num_entries(rumtime_conf_table) == 0) {
		return -1;
	}

	if (verbose) {
		print_runtime_configurations();
	}

	return 0;
}

static int plc_show_container_config() {
	int res = 0;

	if (rumtime_conf_table == NULL) {
		res = plc_refresh_container_config(false);
		if (res != 0)
			return -1;
	}

	if (rumtime_conf_table == NULL || hash_get_num_entries(rumtime_conf_table) == 0) {
		return -1;
	}

	print_runtime_configurations();
	return 0;
}

/* Function referenced from Postgres that can update configuration on
 * specific GPDB segment */
Datum
refresh_plcontainer_config(PG_FUNCTION_ARGS) {
	int res = plc_refresh_container_config(PG_GETARG_BOOL(0));
	if (res == 0) {
		PG_RETURN_TEXT_P(cstring_to_text("ok"));
	} else {
		PG_RETURN_TEXT_P(cstring_to_text("error"));
	}
}

/*
 * Function referenced from Postgres that can update configuration on
 * specific GPDB segment
 */
Datum
show_plcontainer_config(pg_attribute_unused() PG_FUNCTION_ARGS) {
	int res = plc_show_container_config();
	if (res == 0) {
		PG_RETURN_TEXT_P(cstring_to_text("ok"));
	} else {
		PG_RETURN_TEXT_P(cstring_to_text("error"));
	}
}

runtimeConfEntry *plc_get_runtime_configuration(char *runtime_id) {
	int res = 0;
	runtimeConfEntry *entry = NULL;

	if (rumtime_conf_table == NULL || hash_get_num_entries(rumtime_conf_table) == 0) {
		res = plc_refresh_container_config(0);
		if (res < 0) {
			return NULL;
		}
	}

	/* find the corresponding runtime config*/
	entry = (runtimeConfEntry *) hash_search(rumtime_conf_table,  (const void *) runtime_id, HASH_FIND, NULL);

	return entry;
}

char *get_sharing_options(runtimeConfEntry *conf, int container_slot, bool *has_error, char **uds_dir) {
	char *res = NULL;

	*has_error = false;

	if (conf->nSharedDirs >= 0) {
		char **volumes = NULL;
		int totallen = 0;
		char *pos;
		int i;
		int j;
		char comma = ' ';

		volumes = palloc((conf->nSharedDirs + 1) * sizeof(char *));
		for (i = 0; i < conf->nSharedDirs; i++) {
			volumes[i] = palloc(10 + strlen(conf->sharedDirs[i].host) +
			                    strlen(conf->sharedDirs[i].container));
			if (i > 0)
				comma = ',';
			if (conf->sharedDirs[i].mode == PLC_ACCESS_READONLY) {
				sprintf(volumes[i], " %c\"%s:%s:ro\"", comma, conf->sharedDirs[i].host,
				        conf->sharedDirs[i].container);
			} else if (conf->sharedDirs[i].mode == PLC_ACCESS_READWRITE) {
				sprintf(volumes[i], " %c\"%s:%s:rw\"", comma, conf->sharedDirs[i].host,
				        conf->sharedDirs[i].container);
			} else {
				plc_elog(WARNING, "Cannot determine directory sharing mode: %d",
				         conf->sharedDirs[i].mode);
				*has_error = true;
				for (j = 0; j <= i ;j++) {
					pfree(volumes[i]);
				}
				pfree(volumes);
				return NULL;
			}
			totallen += strlen(volumes[i]);
		}

		if (!conf->useContainerNetwork) {
			/* Directory for QE : IPC_GPDB_BASE_DIR + "." + PID + "." + container_slot */
			int gpdb_dir_sz;

			if (i > 0)
				comma = ',';
			
			gpdb_dir_sz = strlen(IPC_GPDB_BASE_DIR) + 1 + 16 + 1 + 16 + 1 + 4 + 1;
			*uds_dir = palloc(gpdb_dir_sz);
			sprintf(*uds_dir, "%s.%d.%d.%d", IPC_GPDB_BASE_DIR, getpid(), domain_socket_no++, container_slot);
			volumes[i] = palloc(10 + gpdb_dir_sz + strlen(IPC_CLIENT_DIR));
			sprintf(volumes[i], " %c\"%s:%s:rw\"", comma, *uds_dir, IPC_CLIENT_DIR);
			totallen += strlen(volumes[i]);

			/* Create the directory. */
			if (mkdir(*uds_dir, S_IRWXU) < 0 && errno != EEXIST) {
				plc_elog(WARNING, "Cannot create directory %s: %s",
				         *uds_dir, strerror(errno));
				*has_error = true;
				for (j = 0; j <= i ;j++) {
					pfree(volumes[i]);
				}
				pfree(volumes);
				return NULL;
			}
		}

		res = palloc(totallen + conf->nSharedDirs + 1 + 1);
		pos = res;
		for (i = 0; i < (conf->useContainerNetwork ? conf->nSharedDirs : conf->nSharedDirs + 1); i++) {
			memcpy(pos, volumes[i], strlen(volumes[i]));
			pos += strlen(volumes[i]);
			*pos = ' ';
			pos++;
			pfree(volumes[i]);
		}
		*pos = '\0';
		pfree(volumes);
	}

	return res;
}

bool plc_check_user_privilege(char *roles){

	List *elemlist;
	ListCell *l;
	Oid currentUserOid;

	if (!SplitIdentifierString(roles, ',', &elemlist))
	{
		list_free(elemlist);
		elog(ERROR, "Could not get role list from %s, please check it again", roles);
	}

	currentUserOid = GetUserId();

	if (currentUserOid == InvalidDbid){
		elog(ERROR, "Could not get current user Oid");
	}

	foreach(l, elemlist)
	{
		char *role = (char*) lfirst(l);
		Oid roleOid = get_role_oid(role, true);
		if (is_member_of_role(currentUserOid, roleOid)){
			return true;
		}
	}

	return false;

}
