/*
* Portions Copyright 1994-2004 The PL-J Project. All rights reserved.
* Portions Copyright © 2016-Present Pivotal Software, Inc.
*/


/* Postgres Headers */
#include "postgres.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "catalog/pg_proc.h"
#ifdef PLC_PG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "commands/trigger.h"
#include "executor/spi.h"
#ifdef PLC_PG
#pragma GCC diagnostic pop
#endif

#include "storage/ipc.h"
#include "funcapi.h"
#include "miscadmin.h"
#ifndef PLC_PG
  #include "utils/faultinjector.h"
#endif
#include "utils/memutils.h"
#include "utils/guc.h"
/* PLContainer Headers */
#include "common/comm_channel.h"
#include "common/comm_dummy.h"
#include "common/messages/messages.h"
#include "plc/containers.h"
#include "plc/message_fns.h"
#include "plc/plcontainer.h"
#include "plc/plc_configuration.h"
#include "plc/plc_typeio.h"
#include "plc/sqlhandler.h"
#include "plc/subtransaction_handler.h"

PG_MODULE_MAGIC;

/* exported functions */
Datum plcontainer_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plcontainer_validator);

PG_FUNCTION_INFO_V1(plcontainer_call_handler);

static Datum plcontainer_function_handler(FunctionCallInfo fcinfo, plcProcInfo *proc);

static plcProcResult *plcontainer_get_result(FunctionCallInfo fcinfo,
                                             plcProcInfo *proc);

static Datum plcontainer_process_result(FunctionCallInfo fcinfo,
                                        plcProcInfo *proc,
                                        plcProcResult *presult);

static void plcontainer_process_exception(plcMsgError *msg);

static void plcontainer_process_sql(plcMsgSQL *msg, plcContext *ctx, plcProcInfo *proc);

static void plcontainer_process_log(plcMsgLog *log);

static void plcontainer_process_quote(plcMsgQuote *quote, plcConn *conn);

static void plpython_error_callback(void *arg);

static char * PLy_procedure_name(plcProcInfo *proc);

/*
 * Currently active plpython function
 */
static plcProcInfo *PLy_curr_procedure = NULL;

/* this is saved and restored by plcontainer_call_handler */
MemoryContext pl_container_caller_context = NULL;

void _PG_init(void);

/*
 * _PG_init() - library load-time initialization
 *
 * DO NOT make this static nor change its name!
 */
void
_PG_init(void) {
	/* Be sure we do initialization only once (should be redundant now) */
	static bool inited = false;
	if (inited)
		return;

	explicit_subtransactions = NIL;
	inited = true;
}

static bool
PLy_procedure_is_trigger(Form_pg_proc procStruct)
{
	return (procStruct->prorettype == TRIGGEROID ||
			(procStruct->prorettype == OPAQUEOID &&
			 procStruct->pronargs == 0));
}

Datum
plcontainer_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc procStruct;
	bool		is_trigger;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();

	if (!check_function_bodies)
	{
		PG_RETURN_VOID();
	}

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	procStruct = (Form_pg_proc) GETSTRUCT(tuple);

	is_trigger = PLy_procedure_is_trigger(procStruct);

	ReleaseSysCache(tuple);

	/* We can't validate triggers against any particular table ... */
	plcontainer_procedure_get(fcinfo);

	PG_RETURN_VOID();
}

/*
 * Get the name of the last procedure called by the backend (the
 * innermost, if a plpython procedure call calls the backend and the
 * backend calls another plpython procedure).
 *
 * NB: this returns the SQL name, not the internal Python procedure name
 */
static char *
PLy_procedure_name(plcProcInfo *proc)
{
	if (proc == NULL)
		return "<unknown procedure>";
	return proc->proname;
}

static void
plpython_error_callback(void __attribute__((__unused__)) *arg)
{
	if (PLy_curr_procedure)
		errcontext("PLContainer function \"%s\"",
				   PLy_procedure_name(PLy_curr_procedure));
}

Datum plcontainer_call_handler(PG_FUNCTION_ARGS) {
	Datum datumreturn = (Datum) 0;
	int ret;
	plcProcInfo *save_curr_proc;
	ErrorContextCallback plerrcontext;

	/* TODO: handle trigger requests as well */
	if (CALLED_AS_TRIGGER(fcinfo)) {
		plc_elog(ERROR, "PL/Container does not support triggers");
		return datumreturn;
	}

	/* pl_container_caller_context refer to the CurrentMemoryContext(e.g. ExprContext)
	 * since SPI_connect() will switch memory context to SPI_PROC, we need
	 * to switch back to the pl_container_caller_context at plcontainer_get_result*/
	pl_container_caller_context = CurrentMemoryContext;

	ret = SPI_connect();
	if (ret != SPI_OK_CONNECT)
		plc_elog(ERROR, "[plcontainer] SPI connect error: %d (%s)", ret,
		     SPI_result_code_string(ret));


	plc_elog(DEBUG1, "Entering call handler with  PLy_curr_procedure");

	save_curr_proc = PLy_curr_procedure;
	/*
	 * Setup error traceback support for ereport()
	 */
	plerrcontext.callback = plpython_error_callback;
	plerrcontext.previous = error_context_stack;
	error_context_stack = &plerrcontext;

	/* We need to cover this in try-catch block to catch the even of user
	 * requesting the query termination. In this case we should forcefully
	 * kill the container and reset its information
	 */
	PG_TRY();
	{


		plcProcInfo *proc;
		/*TODO By default we return NULL */
		fcinfo->isnull = true;

		/* Get procedure info from cache or compose it based on catalog */
		proc = plcontainer_procedure_get(fcinfo);

		PLy_curr_procedure = proc;

		plc_elog(DEBUG1, "Calling python proc @ address: %p", proc);

		datumreturn = plcontainer_function_handler(fcinfo, proc);
	}
	PG_CATCH();
	{
		reset_containers();
		/* If the reason is Cancel or Termination or Backend error. */
		if (InterruptPending || QueryCancelPending || QueryFinishPending) {
			plc_elog(DEBUG1, "Terminating containers due to user request reason("
				"Flags for debugging: %d %d %d", InterruptPending,
			     QueryCancelPending, QueryFinishPending);
		}
		error_context_stack = plerrcontext.previous;
		PLy_curr_procedure = save_curr_proc;
		PG_RE_THROW();
	}
	PG_END_TRY();

	/**
	 *  SPI_finish() will clear the old memory context. Upstream code place it at earlier
	 *  part of code, but we need to place it here.
	 */
	ret = SPI_finish();
	if (ret != SPI_OK_FINISH)
		plc_elog(ERROR, "[plcontainer] SPI finish error: %d (%s)", ret,
		     SPI_result_code_string(ret));

	/* Pop the error context stack */
	error_context_stack = plerrcontext.previous;

	PLy_curr_procedure = save_curr_proc;


	return datumreturn;
}

// This function is only called by plcontainer, not by server
// TODO: post handler of plcContext: free buffer? close socket?
static plcProcResult *plcontainer_get_result(FunctionCallInfo fcinfo,
                                             plcProcInfo *proc) {
	char *runtime_id;
	plcContext *ctx;
	plcConn *conn;
	int message_type;
	plcMsgCallreq *req = NULL;
	plcProcResult *result;
	int volatile save_subxact_level = list_length(explicit_subtransactions);

	PG_TRY();
	{
		int res;
		result = NULL;

		req = plcontainer_generate_call_request(fcinfo, proc);
		runtime_id = parse_container_meta(req->proc.src);
		
		ctx = get_container_context(runtime_id);
		conn = (plcConn *)ctx;
		pfree(runtime_id);

		if (conn == NULL) {
			plc_elog(ERROR, "Could not create or connect to container.");
		}
		res = plcontainer_channel_send(conn, (plcMessage *) req);
#ifndef PLC_PG				
		SIMPLE_FAULT_INJECTOR("plcontainer_after_send_request");
#endif

		if (res < 0) {
			plc_elog(ERROR, "Error sending data to the client. "
						"Maybe retry later.");
			return NULL;
		}
		free_callreq(req, true, true);

		while (1) {
			plcMessage *answer;

			res = plcontainer_channel_receive(conn, &answer, MT_ALL_BITS);
#ifndef PLC_PG					
			SIMPLE_FAULT_INJECTOR("plcontainer_after_recv_request");
#endif				
			if (res < 0) {
				plc_elog(ERROR, "Error receiving data from the client. "
							"Maybe retry later.");
				break;
			}

			message_type = answer->msgtype;
			switch (message_type) {
				case MT_RESULT:
					result = (plcProcResult *) palloc(sizeof(plcProcResult));
					result->resmsg = (plcMsgResult *) answer;
					result->resrow = 0;
					break;
				case MT_EXCEPTION:
					/* For exception, no need to delete containers. */
					plcontainer_process_exception((plcMsgError *) answer);
					break;
				case MT_SQL:
					plcontainer_process_sql((plcMsgSQL *) answer, ctx, proc);
					break;
				case MT_LOG:
					plcontainer_process_log((plcMsgLog *) answer);
					break;
				case MT_QUOTE:
					plcontainer_process_quote((plcMsgQuote *)answer, conn);
					break;
				case MT_SUBTRANSACTION:
					plcontainer_process_subtransaction(
							(plcMsgSubtransaction *) answer, conn);
					break;
				default:
					plc_elog(ERROR, "Received unhandled message with type id %d "
							"from client", message_type);
					break;
			}

			if (message_type != MT_SQL && message_type != MT_LOG
				&& message_type != MT_SUBTRANSACTION && message_type != MT_QUOTE)
				break;
		}
		/*
		 * Since plpy will only let you close subtransactions that you
		 * started, you cannot *unnest* subtransactions, only *nest* them
		 * without closing.
		 */
		Assert(list_length(explicit_subtransactions) >= save_subxact_level);
	}
	PG_CATCH();
	{
		plcontainer_abort_open_subtransactions(save_subxact_level);
		PG_RE_THROW();
	}
	PG_END_TRY();

	plcontainer_abort_open_subtransactions(save_subxact_level);

	return result;
}

/*
 * Processing client results message
 */
static Datum plcontainer_process_result(FunctionCallInfo fcinfo,
                                        plcProcInfo *proc,
                                        plcProcResult *presult) {
	Datum result = (Datum) 0;
	plcMsgResult *resmsg = presult->resmsg;

	if (resmsg->cols > 1) {
		plc_elog(ERROR, "Functions returning multiple columns are not supported yet");
		return result;
	}

	if (resmsg->rows == 0) {
		return result;
	}

	if (presult->resrow >= resmsg->rows) {
		ereport(ERROR,
		        (errcode(ERRCODE_CARDINALITY_VIOLATION),
			        errmsg("Trying to access result row %d of the %d-rows result set",
			               presult->resrow, resmsg->rows)));
		return result;
	}

	if (resmsg->data[presult->resrow][0].isnull == 0) {
		fcinfo->isnull = false;
		result = proc->result.infunc(resmsg->data[presult->resrow][0].value, &proc->result);
	}

	return result;
}

/*
 * Processing client log message
 */
static void plcontainer_process_log(plcMsgLog *log) {
	ereport(log->level,
	        (errcode(ERRCODE_NO_DATA),
		        errmsg("%s", log->message)));
	if (log->message != NULL)
		pfree(log->message);
}

static void plcontainer_process_quote(plcMsgQuote *msg, plcConn *conn) {
	int16 res = 0;
	plcMsgQuoteResult *result;
	result = palloc(sizeof(plcMsgQuoteResult));
	result->msgtype = MT_QUOTE_RESULT;
	result->quote_type = msg->quote_type;

	if (msg->quote_type == QUOTE_TYPE_LITERAL || msg->quote_type == QUOTE_TYPE_NULLABLE) {
		char *str;
		str = quote_literal_cstr(msg->msg);
		result->result = plc_top_strdup(str);
		pfree(str);
	} else if (msg->quote_type == QUOTE_TYPE_IDENT) {
		result->result = plc_top_strdup(quote_identifier(msg->msg));
	}

	res = plcontainer_channel_send(conn, (plcMessage *) result);
	if (res < 0) {
		plc_elog(ERROR, "Error sending data to the client, with errno %d. ", res);
	}
	if (msg->msg != NULL)
		pfree(msg->msg);
}


/*
 * Processing client SQL query message
 */
static void plcontainer_process_sql(plcMsgSQL *msg, plcContext *ctx, plcProcInfo *proc) {
	plcMessage *res;
	volatile MemoryContext oldcontext;
	volatile ResourceOwner oldowner;
	int retval;

	oldcontext = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;

	res = handle_sql_message(msg, ctx, proc);
	if (res != NULL) {
		retval = plcontainer_channel_send((plcConn *)ctx, res);
		if (retval < 0) {
			plc_elog(ERROR, "Error sending data to the client. "
				"Maybe retry later.");
			return;
		}
		switch (res->msgtype) {
			case MT_RESULT:
				free_result((plcMsgResult *) res, true);
				break;
			case MT_CALLREQ:
				free_callreq((plcMsgCallreq *) res, true, true);
				break;
			case MT_RAW:
				free_rawmsg((plcMsgRaw *) res);
				break;
			default:
				ereport(ERROR,
				        (errcode(ERRCODE_RAISE_EXCEPTION),
					        errmsg("Returning message type '%c' from SPI call is not implemented", res->msgtype)));
		}
	}

	MemoryContextSwitchTo(oldcontext);
	CurrentResourceOwner = oldowner;

	/*
	 * AtEOSubXact_SPI() should not have popped any SPI context, but just
	 * in case it did, make sure we remain connected.
	 */
	SPI_restore_connection();
}

/*
 * Processing client exception message
 */
static void plcontainer_process_exception(plcMsgError *msg) {
	if (msg->stacktrace != NULL) {
		ereport(ERROR,
		        (errcode(ERRCODE_RAISE_EXCEPTION),
			        errmsg("PL/Container client exception occurred: \n %s \n %s", msg->message, msg->stacktrace)));
	} else {
		ereport(ERROR,
		        (errcode(ERRCODE_RAISE_EXCEPTION),
			        errmsg("PL/Container client exception occurred: \n %s", msg->message)));
	}
	free_error(msg);
}

/* function handler and friends */
static Datum
plcontainer_function_handler(FunctionCallInfo fcinfo, plcProcInfo *proc)
{
	Datum						datumreturn;
	plcProcResult * volatile 	presult = 		NULL;
	MemoryContext volatile 		oldcontext = 	CurrentMemoryContext;
	FuncCallContext	* volatile	funcctx =		NULL;
	bool	 volatile 				bFirstTimeCall = false;

	PG_TRY();
	{
		plc_elog(DEBUG1, "fcinfo->flinfo->fn_retset: %d", fcinfo->flinfo->fn_retset);

		if (fcinfo->flinfo->fn_retset)
		{
			/* First Call setup */
			if (SRF_IS_FIRSTCALL())
			{
				funcctx = SRF_FIRSTCALL_INIT();
				bFirstTimeCall = true;

				plc_elog(DEBUG1, "The funcctx pointer returned by SRF_FIRSTCALL_INIT() is: %p", funcctx);
			}

			/* Every call setup */
			funcctx = SRF_PERCALL_SETUP();
			plc_elog(DEBUG1, "The funcctx pointer returned by SRF_PERCALL_SETUP() is: %p", funcctx);

			Assert(funcctx != NULL);
			/* SRF uses multi_call_memory_ctx context shared between function calls,
			 * since EXPR etc. context will be cleared after one of the SRF calls.
			 * Note that plpython doesn't need it, because it doesn't use palloc to store
			 * the SRF result.
			 */
			oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		} else {
			oldcontext = MemoryContextSwitchTo(pl_container_caller_context);
		}

		/* First time call for SRF or just a call of scalar function */
		if (!fcinfo->flinfo->fn_retset || bFirstTimeCall) {
			presult = plcontainer_get_result(fcinfo, proc);
			if (!fcinfo->flinfo->fn_retset) {
				/*
				 * SETOF function parameters will be deleted when last row is
				 * returned
				 */
				//TODO: delete proc->global when support it.
				//PLy_function_delete_args(proc);
			}
		}

		if (fcinfo->flinfo->fn_retset) {
			ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

			if (funcctx->user_fctx == NULL) {
				plc_elog(DEBUG1, "first time call, preparing the result set...");

				/* first time -- do checks and setup */
				if (!rsi || !IsA(rsi, ReturnSetInfo)
						|| (rsi->allowedModes & SFRM_ValuePerCall) == 0) {
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg(
									"unsupported set function return mode"), errdetail(
									"PL/Python set-returning functions only support returning only value per call.")));
				}
				rsi->returnMode = SFRM_ValuePerCall;

				funcctx->user_fctx = (void *) presult;

				if (funcctx->user_fctx == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH), errmsg(
									"returned object cannot be iterated"), errdetail(
									"PL/Python set-returning functions must return an iterable object.")));
			}

			presult = (plcProcResult *) funcctx->user_fctx;

			if (presult->resrow < presult->resmsg->rows)
				rsi->isDone = ExprMultipleResult;
			else {
				rsi->isDone = ExprEndResult;
			}

			if (rsi->isDone == ExprEndResult) {
				free_result(presult->resmsg, false);
				pfree(presult);
				MemoryContextSwitchTo(oldcontext);


				funcctx->user_fctx = NULL;

				//TODO: delete proc->global when support it.
				//PLy_function_delete_args(proc);

				SRF_RETURN_DONE(funcctx);
			}
		}

		/* Process the result message from client */
		datumreturn = plcontainer_process_result(fcinfo, proc, presult);
		presult->resrow += 1;
		MemoryContextSwitchTo(oldcontext);

	}
	PG_CATCH();
	{

		/*
		 * If there was an error the iterator might have not been exhausted
		 * yet. Set it to NULL so the next invocation of the function will
		 * start the iteration again.
		 */
		if (fcinfo->flinfo->fn_retset && funcctx->user_fctx != NULL) {
			funcctx->user_fctx = NULL;
		}
		if(presult && presult->resmsg) {
			free_result(presult->resmsg, false);
			pfree(presult);
		}
		MemoryContextSwitchTo(oldcontext);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (fcinfo->flinfo->fn_retset) {
		SRF_RETURN_NEXT(funcctx, datumreturn);
	} else {
		free_result(presult->resmsg, false);
		pfree(presult);
	}

#ifndef PLC_PG
		SIMPLE_FAULT_INJECTOR("plcontainer_before_udf_finish");
#endif

	return datumreturn;
}
