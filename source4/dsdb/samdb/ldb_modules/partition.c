/* 
   Partitions ldb module

   Copyright (C) Andrew Bartlett <abartlet@samba.org> 2006
   Copyright (C) Stefan Metzmacher <metze@samba.org> 2007

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 *  Name: ldb
 *
 *  Component: ldb partitions module
 *
 *  Description: Implement LDAP partitions
 *
 *  Author: Andrew Bartlett
 *  Author: Stefan Metzmacher
 */

#include "dsdb/samdb/ldb_modules/partition.h"

struct part_request {
	struct ldb_module *module;
	struct ldb_request *req;
};

struct partition_context {
	struct ldb_module *module;
	struct ldb_request *req;

	struct part_request *part_req;
	unsigned int num_requests;
	unsigned int finished_requests;

	const char **referrals;
};

static struct partition_context *partition_init_ctx(struct ldb_module *module, struct ldb_request *req)
{
	struct partition_context *ac;

	ac = talloc_zero(req, struct partition_context);
	if (ac == NULL) {
		ldb_set_errstring(ldb_module_get_ctx(module), "Out of Memory");
		return NULL;
	}

	ac->module = module;
	ac->req = req;

	return ac;
}

/*
 * helper functions to call the next module in chain
 */
int partition_request(struct ldb_module *module, struct ldb_request *request)
{
	if ((module && ldb_module_flags(ldb_module_get_ctx(module)) & LDB_FLG_ENABLE_TRACING)) { \
		const struct dsdb_control_current_partition *partition = NULL;
		struct ldb_control *partition_ctrl = ldb_request_get_control(request, DSDB_CONTROL_CURRENT_PARTITION_OID);
		if (partition_ctrl) {
			partition = talloc_get_type(partition_ctrl->data,
						    struct dsdb_control_current_partition);
		}

		if (partition != NULL) {
			ldb_debug(ldb_module_get_ctx(module), LDB_DEBUG_TRACE, "partition_request() -> %s",
				  ldb_dn_get_linearized(partition->dn));			
		} else {
			ldb_debug(ldb_module_get_ctx(module), LDB_DEBUG_TRACE, "partition_request() -> (metadata partition)");
		}
	}

	return ldb_next_request(module, request);
}

static struct dsdb_partition *find_partition(struct partition_private_data *data,
					     struct ldb_dn *dn,
					     struct ldb_request *req)
{
	unsigned int i;
	struct ldb_control *partition_ctrl;

	/* see if the request has the partition DN specified in a
	 * control. The repl_meta_data module can specify this to
	 * ensure that replication happens to the right partition
	 */
	partition_ctrl = ldb_request_get_control(req, DSDB_CONTROL_CURRENT_PARTITION_OID);
	if (partition_ctrl) {
		const struct dsdb_control_current_partition *partition;
		partition = talloc_get_type(partition_ctrl->data,
					    struct dsdb_control_current_partition);
		if (partition != NULL) {
			dn = partition->dn;
		}
	}

	if (dn == NULL) {
		return NULL;
	}

	/* Look at base DN */
	/* Figure out which partition it is under */
	/* Skip the lot if 'data' isn't here yet (initialisation) */
	for (i=0; data && data->partitions && data->partitions[i]; i++) {
		if (ldb_dn_compare_base(data->partitions[i]->ctrl->dn, dn) == 0) {
			return data->partitions[i];
		}
	}

	return NULL;
}

/**
 * fire the caller's callback for every entry, but only send 'done' once.
 */
static int partition_req_callback(struct ldb_request *req,
				  struct ldb_reply *ares)
{
	struct partition_context *ac;
	struct ldb_module *module;
	struct ldb_request *nreq;
	int ret;
	struct ldb_control *partition_ctrl;

	ac = talloc_get_type(req->context, struct partition_context);

	if (!ares) {
		return ldb_module_done(ac->req, NULL, NULL,
					LDB_ERR_OPERATIONS_ERROR);
	}

	partition_ctrl = ldb_request_get_control(req, DSDB_CONTROL_CURRENT_PARTITION_OID);
	if (partition_ctrl && (ac->num_requests == 1 || ares->type == LDB_REPLY_ENTRY)) {
		/* If we didn't fan this request out to multiple partitions,
		 * or this is an individual search result, we can
		 * deterministically tell the caller what partition this was
		 * written to (repl_meta_data likes to know) */
		ret = ldb_reply_add_control(ares,
					    DSDB_CONTROL_CURRENT_PARTITION_OID,
					    false, partition_ctrl->data);
		if (ret != LDB_SUCCESS) {
			return ldb_module_done(ac->req, NULL, NULL,
					       ret);
		}
	}

	if (ares->error != LDB_SUCCESS) {
		return ldb_module_done(ac->req, ares->controls,
					ares->response, ares->error);
	}

	switch (ares->type) {
	case LDB_REPLY_REFERRAL:
		return ldb_module_send_referral(ac->req, ares->referral);

	case LDB_REPLY_ENTRY:
		if (ac->req->operation != LDB_SEARCH) {
			ldb_set_errstring(ldb_module_get_ctx(ac->module),
				"partition_req_callback:"
				" Unsupported reply type for this request");
			return ldb_module_done(ac->req, NULL, NULL,
						LDB_ERR_OPERATIONS_ERROR);
		}
		
		return ldb_module_send_entry(ac->req, ares->message, ares->controls);

	case LDB_REPLY_DONE:
		if (ac->req->operation == LDB_EXTENDED) {
			/* FIXME: check for ares->response, replmd does not fill it ! */
			if (ares->response) {
				if (strcmp(ares->response->oid, LDB_EXTENDED_START_TLS_OID) != 0) {
					ldb_set_errstring(ldb_module_get_ctx(ac->module),
							  "partition_req_callback:"
							  " Unknown extended reply, "
							  "only supports START_TLS");
					talloc_free(ares);
					return ldb_module_done(ac->req, NULL, NULL,
								LDB_ERR_OPERATIONS_ERROR);
				}
			}
		}

		ac->finished_requests++;
		if (ac->finished_requests == ac->num_requests) {
			/* Send back referrals if they do exist (search ops) */
			if (ac->referrals != NULL) {
				const char **ref;
				for (ref = ac->referrals; *ref != NULL; ++ref) {
					ret = ldb_module_send_referral(ac->req,
								       talloc_strdup(ac->req, *ref));
					if (ret != LDB_SUCCESS) {
						return ldb_module_done(ac->req, NULL, NULL,
								       ret);
					}
				}
			}

			/* this was the last one, call callback */
			return ldb_module_done(ac->req, ares->controls,
					       ares->response, 
					       ares->error);
		}

		/* not the last, now call the next one */
		module = ac->part_req[ac->finished_requests].module;
		nreq = ac->part_req[ac->finished_requests].req;

		ret = partition_request(module, nreq);
		if (ret != LDB_SUCCESS) {
			talloc_free(ares);
			return ldb_module_done(ac->req, NULL, NULL, ret);
		}

		break;
	}

	talloc_free(ares);
	return LDB_SUCCESS;
}

static int partition_prep_request(struct partition_context *ac,
				  struct dsdb_partition *partition)
{
	int ret;
	struct ldb_request *req;
	struct ldb_control *partition_ctrl = NULL;
	void *part_data = NULL;

	ac->part_req = talloc_realloc(ac, ac->part_req,
					struct part_request,
					ac->num_requests + 1);
	if (ac->part_req == NULL) {
		return ldb_oom(ldb_module_get_ctx(ac->module));
	}

	switch (ac->req->operation) {
	case LDB_SEARCH:
		ret = ldb_build_search_req_ex(&req, ldb_module_get_ctx(ac->module),
					ac->part_req,
					ac->req->op.search.base,
					ac->req->op.search.scope,
					ac->req->op.search.tree,
					ac->req->op.search.attrs,
					ac->req->controls,
					ac, partition_req_callback,
					ac->req);
		LDB_REQ_SET_LOCATION(req);
		break;
	case LDB_ADD:
		ret = ldb_build_add_req(&req, ldb_module_get_ctx(ac->module), ac->part_req,
					ac->req->op.add.message,
					ac->req->controls,
					ac, partition_req_callback,
					ac->req);
		LDB_REQ_SET_LOCATION(req);
		break;
	case LDB_MODIFY:
		ret = ldb_build_mod_req(&req, ldb_module_get_ctx(ac->module), ac->part_req,
					ac->req->op.mod.message,
					ac->req->controls,
					ac, partition_req_callback,
					ac->req);
		LDB_REQ_SET_LOCATION(req);
		break;
	case LDB_DELETE:
		ret = ldb_build_del_req(&req, ldb_module_get_ctx(ac->module), ac->part_req,
					ac->req->op.del.dn,
					ac->req->controls,
					ac, partition_req_callback,
					ac->req);
		LDB_REQ_SET_LOCATION(req);
		break;
	case LDB_RENAME:
		ret = ldb_build_rename_req(&req, ldb_module_get_ctx(ac->module), ac->part_req,
					ac->req->op.rename.olddn,
					ac->req->op.rename.newdn,
					ac->req->controls,
					ac, partition_req_callback,
					ac->req);
		LDB_REQ_SET_LOCATION(req);
		break;
	case LDB_EXTENDED:
		ret = ldb_build_extended_req(&req, ldb_module_get_ctx(ac->module),
					ac->part_req,
					ac->req->op.extended.oid,
					ac->req->op.extended.data,
					ac->req->controls,
					ac, partition_req_callback,
					ac->req);
		LDB_REQ_SET_LOCATION(req);
		break;
	default:
		ldb_set_errstring(ldb_module_get_ctx(ac->module),
				  "Unsupported request type!");
		ret = LDB_ERR_UNWILLING_TO_PERFORM;
	}

	if (ret != LDB_SUCCESS) {
		return ret;
	}

	ac->part_req[ac->num_requests].req = req;

	if (ac->req->controls) {
		/* Duplicate everything beside the current partition control */
		partition_ctrl = ldb_request_get_control(ac->req,
							 DSDB_CONTROL_CURRENT_PARTITION_OID);
		if (!ldb_save_controls(partition_ctrl, req, NULL)) {
			return ldb_module_oom(ac->module);
		}
	}

	part_data = partition->ctrl;

	ac->part_req[ac->num_requests].module = partition->module;

	if (partition_ctrl != NULL) {
		if (partition_ctrl->data != NULL) {
			part_data = partition_ctrl->data;
		}

		/*
		 * If the provided current partition control is without
		 * data then use the calculated one.
		 */
		ret = ldb_request_add_control(req,
					      DSDB_CONTROL_CURRENT_PARTITION_OID,
					      false, part_data);
		if (ret != LDB_SUCCESS) {
			return ret;
		}
	}

	if (req->operation == LDB_SEARCH) {
		/*
		 * If the search is for 'more' than this partition,
		 * then change the basedn, so the check of the BASE DN
		 * still passes in the ldb_key_value layer
		 */
		if (ldb_dn_compare_base(partition->ctrl->dn,
					req->op.search.base) != 0) {
			req->op.search.base = partition->ctrl->dn;
		}
	}

	ac->num_requests++;

	return LDB_SUCCESS;
}

static int partition_call_first(struct partition_context *ac)
{
	return partition_request(ac->part_req[0].module, ac->part_req[0].req);
}

/**
 * Send a request down to all the partitions (but not the sam.ldb file)
 */
static int partition_send_all(struct ldb_module *module, 
			      struct partition_context *ac, 
			      struct ldb_request *req) 
{
	unsigned int i;
	struct partition_private_data *data = talloc_get_type(ldb_module_get_private(module),
							      struct partition_private_data);
	int ret;

	for (i=0; data && data->partitions && data->partitions[i]; i++) {
		ret = partition_prep_request(ac, data->partitions[i]);
		if (ret != LDB_SUCCESS) {
			return ret;
		}
	}

	/* fire the first one */
	return partition_call_first(ac);
}

struct partition_copy_context {
	struct ldb_module *module;
	struct partition_context *partition_context;
	struct ldb_request *request;
	struct ldb_dn *dn;
};

/*
 * A special DN has been updated in the primary partition. Now propagate those
 * changes to the remaining partitions.
 *
 * Note: that the operations are asynchronous and this function is called
 *       from partition_copy_all_callback_handler in response to an async
 *       callback.
 */
static int partition_copy_all_callback_action(
	struct ldb_module *module,
	struct partition_context *ac,
	struct ldb_request *req,
	struct ldb_dn *dn)

{

	unsigned int i;
	struct partition_private_data *data =
		talloc_get_type(
			ldb_module_get_private(module),
			struct partition_private_data);
	int search_ret;
	struct ldb_result *res;
	/* now fetch the resulting object, and then copy it to all the
	 * other partitions. We need this approach to cope with the
	 * partitions getting out of sync. If for example the
	 * @ATTRIBUTES object exists on one partition but not the
	 * others then just doing each of the partitions in turn will
	 * lead to an error
	 */
	search_ret = dsdb_module_search_dn(module, ac, &res, dn, NULL, DSDB_FLAG_NEXT_MODULE, req);
	if (search_ret != LDB_SUCCESS && search_ret != LDB_ERR_NO_SUCH_OBJECT) {
		return search_ret;
	}

	/* now delete the object in the other partitions, if required
	*/
	if (search_ret == LDB_ERR_NO_SUCH_OBJECT) {
		for (i=0; data->partitions && data->partitions[i]; i++) {
			int pret;
			pret = dsdb_module_del(data->partitions[i]->module,
					       dn,
					       DSDB_FLAG_NEXT_MODULE,
					       req);
			if (pret != LDB_SUCCESS && pret != LDB_ERR_NO_SUCH_OBJECT) {
				/* we should only get success or no
				   such object from the other partitions */
				return pret;
			}
		}

		return ldb_module_done(req, NULL, NULL, LDB_SUCCESS);
	}

	/* now add/modify in the other partitions */
	for (i=0; data->partitions && data->partitions[i]; i++) {
		struct ldb_message *modify_msg = NULL;
		int pret;
		unsigned int el_idx;

		pret = dsdb_module_add(data->partitions[i]->module,
				       res->msgs[0],
				       DSDB_FLAG_NEXT_MODULE,
				       req);
		if (pret == LDB_SUCCESS) {
			continue;
		}

		if (pret != LDB_ERR_ENTRY_ALREADY_EXISTS) {
			return pret;
		}

		modify_msg = ldb_msg_copy(req, res->msgs[0]);
		if (modify_msg == NULL) {
			return ldb_module_oom(module);
		}

		/*
		 * mark all the message elements as
		 * LDB_FLAG_MOD_REPLACE
		 */
		for (el_idx=0;
		     el_idx < modify_msg->num_elements;
		     el_idx++) {
			modify_msg->elements[el_idx].flags
				= LDB_FLAG_MOD_REPLACE;
		}

		if (req->operation == LDB_MODIFY) {
			const struct ldb_message *req_msg = req->op.mod.message;
			/*
			 * mark elements to be removed, if there were
			 * deleted entirely above we need to delete
			 * them here too
			 */
			for (el_idx=0; el_idx < req_msg->num_elements; el_idx++) {
				if (LDB_FLAG_MOD_TYPE(req_msg->elements[el_idx].flags) == LDB_FLAG_MOD_DELETE
				    || ((LDB_FLAG_MOD_TYPE(req_msg->elements[el_idx].flags) == LDB_FLAG_MOD_REPLACE) &&
					req_msg->elements[el_idx].num_values == 0)) {
					if (ldb_msg_find_element(modify_msg,
								 req_msg->elements[el_idx].name) != NULL) {
						continue;
					}
					pret = ldb_msg_add_empty(
						modify_msg,
						req_msg->elements[el_idx].name,
						LDB_FLAG_MOD_REPLACE,
						NULL);
					if (pret != LDB_SUCCESS) {
						return pret;
					}
				}
			}
		}

		pret = dsdb_module_modify(data->partitions[i]->module,
					  modify_msg,
					  DSDB_FLAG_NEXT_MODULE,
					  req);

		if (pret != LDB_SUCCESS) {
			return pret;
		}
	}

	return ldb_module_done(req, NULL, NULL, LDB_SUCCESS);
}


/*
 * @brief call back function for the ldb operations on special DN's.
 *
 * As the LDB operations are async, and we wish to use the result
 * the operations, a callback needs to be registered to process the results
 * of the LDB operations.
 *
 * @param req the ldb request
 * @param res the result of the operation
 *
 * @return the LDB_STATUS
 */
static int partition_copy_all_callback_handler(
	struct ldb_request *req,
	struct ldb_reply *ares)
{
	struct partition_copy_context *ac = NULL;

	ac = talloc_get_type(
		req->context,
		struct partition_copy_context);

	if (!ares) {
		return ldb_module_done(
			ac->request,
			NULL,
			NULL,
			LDB_ERR_OPERATIONS_ERROR);
	}

	/* pass on to the callback */
	switch (ares->type) {
	case LDB_REPLY_ENTRY:
		return ldb_module_send_entry(
			ac->request,
			ares->message,
			ares->controls);

	case LDB_REPLY_REFERRAL:
		return ldb_module_send_referral(
			ac->request,
			ares->referral);

	case LDB_REPLY_DONE: {
		int error = ares->error;
		if (error == LDB_SUCCESS) {
			error = partition_copy_all_callback_action(
				ac->module,
				ac->partition_context,
				ac->request,
				ac->dn);
		}
		return ldb_module_done(
			ac->request,
			ares->controls,
			ares->response,
			error);
	}

	default:
		/* Can't happen */
		return LDB_ERR_OPERATIONS_ERROR;
	}
}

/**
 * send an operation to the top partition, then copy the resulting
 * object to all other partitions.
 */
static int partition_copy_all(
	struct ldb_module *module,
	struct partition_context *partition_context,
	struct ldb_request *req,
	struct ldb_dn *dn)
{
	struct ldb_request *new_req = NULL;
	struct ldb_context *ldb = NULL;
	struct partition_copy_context *context = NULL;

	int ret;

	ldb = ldb_module_get_ctx(module);

	context = talloc_zero(req, struct partition_copy_context);
	if (context == NULL) {
		return ldb_oom(ldb);
	}
	context->module = module;
	context->request = req;
	context->dn = dn;
	context->partition_context = partition_context;

	switch (req->operation) {
	case LDB_ADD:
		ret = ldb_build_add_req(
			&new_req,
			ldb,
			req,
			req->op.add.message,
			req->controls,
			context,
			partition_copy_all_callback_handler,
			req);
		break;
	case LDB_MODIFY:
		ret = ldb_build_mod_req(
			&new_req,
			ldb,
			req,
			req->op.mod.message,
			req->controls,
			context,
			partition_copy_all_callback_handler,
			req);
		break;
	case LDB_DELETE:
		ret = ldb_build_del_req(
			&new_req,
			ldb,
			req,
			req->op.del.dn,
			req->controls,
			context,
			partition_copy_all_callback_handler,
			req);
		break;
	case LDB_RENAME:
		ret = ldb_build_rename_req(
			&new_req,
			ldb,
			req,
			req->op.rename.olddn,
			req->op.rename.newdn,
			req->controls,
			context,
			partition_copy_all_callback_handler,
			req);
		break;
	default:
		/*
		 * Shouldn't happen.
		 */
		ldb_debug(
			ldb,
			LDB_DEBUG_ERROR,
			"Unexpected operation type (%d)\n", req->operation);
		ret = LDB_ERR_OPERATIONS_ERROR;
		break;
	}
	if (ret != LDB_SUCCESS) {
		return ret;
	}
	return ldb_next_request(module, new_req);
}
/**
 * Figure out which backend a request needs to be aimed at.  Some
 * requests must be replicated to all backends
 */
static int partition_replicate(struct ldb_module *module, struct ldb_request *req, struct ldb_dn *dn) 
{
	struct partition_context *ac;
	unsigned int i;
	int ret;
	struct dsdb_partition *partition;
	struct partition_private_data *data = talloc_get_type(ldb_module_get_private(module),
							      struct partition_private_data);

	/* if we aren't initialised yet go further */
	if (!data || !data->partitions) {
		return ldb_next_request(module, req);
	}

	if (ldb_dn_is_special(dn)) {
		/* Is this a special DN, we need to replicate to every backend? */
		for (i=0; data->replicate && data->replicate[i]; i++) {
			if (ldb_dn_compare(data->replicate[i], 
					   dn) == 0) {
				
				ac = partition_init_ctx(module, req);
				if (!ac) {
					return ldb_operr(ldb_module_get_ctx(module));
				}
				
				return partition_copy_all(module, ac, req, dn);
			}
		}
	}

	/* Otherwise, we need to find the partition to fire it to */

	/* Find partition */
	partition = find_partition(data, dn, req);
	if (!partition) {
		/*
		 * if we haven't found a matching partition
		 * pass the request to the main ldb
		 *
		 * TODO: we should maybe return an error here
		 *       if it's not a special dn
		 */

		return ldb_next_request(module, req);
	}

	ac = partition_init_ctx(module, req);
	if (!ac) {
		return ldb_operr(ldb_module_get_ctx(module));
	}

	/* we need to add a control but we never touch the original request */
	ret = partition_prep_request(ac, partition);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	/* fire the first one */
	return partition_call_first(ac);
}

/* search */
static int partition_search(struct ldb_module *module, struct ldb_request *req)
{
	/* Find backend */
	struct partition_private_data *data = talloc_get_type(ldb_module_get_private(module),
							      struct partition_private_data);
	struct partition_context *ac;
	struct ldb_context *ldb;
	struct loadparm_context *lp_ctx;

	struct ldb_control *search_control = ldb_request_get_control(req, LDB_CONTROL_SEARCH_OPTIONS_OID);
	struct ldb_control *domain_scope_control = ldb_request_get_control(req, LDB_CONTROL_DOMAIN_SCOPE_OID);
	struct ldb_control *no_gc_control = ldb_request_get_control(req, DSDB_CONTROL_NO_GLOBAL_CATALOG);
	
	struct ldb_search_options_control *search_options = NULL;
	struct dsdb_partition *p;
	unsigned int i, j;
	int ret;
	bool domain_scope = false, phantom_root = false;

	p = find_partition(data, NULL, req);
	if (p != NULL) {
		/* the caller specified what partition they want the
		 * search - just pass it on
		 */
		return ldb_next_request(p->module, req);
	}

	/* Get back the search options from the search control, and mark it as
	 * non-critical (to make backends and also dcpromo happy).
	 */
	if (search_control) {
		search_options = talloc_get_type(search_control->data, struct ldb_search_options_control);
		search_control->critical = 0;

	}

	/* if we aren't initialised yet go further */
	if (!data || !data->partitions) {
		return ldb_next_request(module, req);
	}

	/* Special DNs without specified partition should go further */
	if (ldb_dn_is_special(req->op.search.base)) {
		return ldb_next_request(module, req);
	}

	/* Locate the options */
	domain_scope = (search_options
		&& (search_options->search_options & LDB_SEARCH_OPTION_DOMAIN_SCOPE))
		|| domain_scope_control;
	phantom_root = search_options
		&& (search_options->search_options & LDB_SEARCH_OPTION_PHANTOM_ROOT);

	/* Remove handled options from the search control flag */
	if (search_options) {
		search_options->search_options = search_options->search_options
			& ~LDB_SEARCH_OPTION_DOMAIN_SCOPE
			& ~LDB_SEARCH_OPTION_PHANTOM_ROOT;
	}

	ac = partition_init_ctx(module, req);
	if (!ac) {
		return ldb_operr(ldb_module_get_ctx(module));
	}

	ldb = ldb_module_get_ctx(ac->module);
	lp_ctx = talloc_get_type(ldb_get_opaque(ldb, "loadparm"),
						struct loadparm_context);

	/* Search from the base DN */
	if (ldb_dn_is_null(req->op.search.base)) {
		if (!phantom_root) {
			return ldb_error(ldb, LDB_ERR_NO_SUCH_OBJECT, "empty base DN");
		}
		return partition_send_all(module, ac, req);
	}

	for (i=0; data->partitions[i]; i++) {
		bool match = false, stop = false;

		if (data->partitions[i]->partial_replica && no_gc_control != NULL) {
			if (ldb_dn_compare_base(data->partitions[i]->ctrl->dn,
						req->op.search.base) == 0) {
				/* base DN is in a partial replica
				   with the NO_GLOBAL_CATALOG
				   control. This partition is invisible */
				/* DEBUG(0,("DENYING NON-GC OP: %s\n", ldb_module_call_chain(req, req))); */
				continue;
			}
		}

		if (phantom_root) {
			/* Phantom root: Find all partitions under the
			 * search base. We match if:
			 *
			 * 1) the DN we are looking for exactly matches a
			 *    certain partition and always stop
			 * 2) the DN we are looking for is a parent of certain
			 *    partitions and it isn't a scope base search
			 * 3) the DN we are looking for is a child of a certain
			 *    partition and always stop
			 *    - we don't need to go any further up in the
			 *    hierarchy!
			 */
			if (ldb_dn_compare(data->partitions[i]->ctrl->dn,
					   req->op.search.base) == 0) {
				match = true;
				stop = true;
			}
			if (!match &&
			    (ldb_dn_compare_base(req->op.search.base,
						 data->partitions[i]->ctrl->dn) == 0 &&
			     req->op.search.scope != LDB_SCOPE_BASE)) {
				match = true;
			}
			if (!match &&
			    ldb_dn_compare_base(data->partitions[i]->ctrl->dn,
						req->op.search.base) == 0) {
				match = true;
				stop = true; /* note that this relies on partition ordering */
			}
		} else {
			/* Domain scope: Find all partitions under the search
			 * base.
			 *
			 * We generate referral candidates if we haven't
			 * specified the domain scope control, haven't a base
			 * search* scope and the DN we are looking for is a real
			 * predecessor of certain partitions. When a new
			 * referral candidate is nearer to the DN than an
			 * existing one delete the latter (we want to have only
			 * the closest ones). When we checked this for all
			 * candidates we have the final referrals.
			 *
			 * We match if the DN we are looking for is a child of
			 * a certain partition or the partition
			 * DN itself - we don't need to go any further
			 * up in the hierarchy!
			 */
			if ((!domain_scope) &&
			    (req->op.search.scope != LDB_SCOPE_BASE) &&
			    (ldb_dn_compare_base(req->op.search.base,
						 data->partitions[i]->ctrl->dn) == 0) &&
			    (ldb_dn_compare(req->op.search.base,
					    data->partitions[i]->ctrl->dn) != 0)) {
				const char *scheme = ldb_get_opaque(
				    ldb, LDAP_REFERRAL_SCHEME_OPAQUE);
				char *ref = talloc_asprintf(
					ac,
					"%s://%s/%s%s",
					scheme == NULL ? "ldap" : scheme,
					lpcfg_dnsdomain(lp_ctx),
					ldb_dn_get_linearized(
					    data->partitions[i]->ctrl->dn),
					req->op.search.scope ==
					    LDB_SCOPE_ONELEVEL ? "??base" : "");

				if (ref == NULL) {
					return ldb_oom(ldb);
				}

				/* Initialise the referrals list */
				if (ac->referrals == NULL) {
					char **l = str_list_make_empty(ac);
					ac->referrals = discard_const_p(const char *, l);
					if (ac->referrals == NULL) {
						return ldb_oom(ldb);
					}
				}

				/* Check if the new referral candidate is
				 * closer to the base DN than already
				 * saved ones and delete the latters */
				j = 0;
				while (ac->referrals[j] != NULL) {
					if (strstr(ac->referrals[j],
						   ldb_dn_get_linearized(data->partitions[i]->ctrl->dn)) != NULL) {
						str_list_remove(ac->referrals,
								ac->referrals[j]);
					} else {
						++j;
					}
				}

				/* Add our new candidate */
				ac->referrals = str_list_add(ac->referrals, ref);

				talloc_free(ref);

				if (ac->referrals == NULL) {
					return ldb_oom(ldb);
				}
			}
			if (ldb_dn_compare_base(data->partitions[i]->ctrl->dn, req->op.search.base) == 0) {
				match = true;
				stop = true; /* note that this relies on partition ordering */
			}
		}

		if (match) {
			ret = partition_prep_request(ac, data->partitions[i]);
			if (ret != LDB_SUCCESS) {
				return ret;
			}
		}

		if (stop) break;
	}

	/* Perhaps we didn't match any partitions. Try the main partition */
	if (ac->num_requests == 0) {
		talloc_free(ac);
		return ldb_next_request(module, req);
	}

	/* fire the first one */
	return partition_call_first(ac);
}

/* add */
static int partition_add(struct ldb_module *module, struct ldb_request *req)
{
	return partition_replicate(module, req, req->op.add.message->dn);
}

/* modify */
static int partition_modify(struct ldb_module *module, struct ldb_request *req)
{
	return partition_replicate(module, req, req->op.mod.message->dn);
}

/* delete */
static int partition_delete(struct ldb_module *module, struct ldb_request *req)
{
	return partition_replicate(module, req, req->op.del.dn);
}

/* rename */
static int partition_rename(struct ldb_module *module, struct ldb_request *req)
{
	/* Find backend */
	struct dsdb_partition *backend, *backend2;
	
	struct partition_private_data *data = talloc_get_type(ldb_module_get_private(module),
							      struct partition_private_data);

	/* Skip the lot if 'data' isn't here yet (initialisation) */
	if (!data) {
		return ldb_operr(ldb_module_get_ctx(module));
	}

	backend = find_partition(data, req->op.rename.olddn, req);
	backend2 = find_partition(data, req->op.rename.newdn, req);

	if ((backend && !backend2) || (!backend && backend2)) {
		return LDB_ERR_AFFECTS_MULTIPLE_DSAS;
	}

	if (backend != backend2) {
		ldb_asprintf_errstring(ldb_module_get_ctx(module), 
				       "Cannot rename from %s in %s to %s in %s: %s",
				       ldb_dn_get_linearized(req->op.rename.olddn),
				       ldb_dn_get_linearized(backend->ctrl->dn),
				       ldb_dn_get_linearized(req->op.rename.newdn),
				       ldb_dn_get_linearized(backend2->ctrl->dn),
				       ldb_strerror(LDB_ERR_AFFECTS_MULTIPLE_DSAS));
		return LDB_ERR_AFFECTS_MULTIPLE_DSAS;
	}

	return partition_replicate(module, req, req->op.rename.olddn);
}

/* start a transaction */
int partition_start_trans(struct ldb_module *module)
{
	int i = 0;
	int ret = 0;
	struct partition_private_data *data = talloc_get_type(ldb_module_get_private(module),
							      struct partition_private_data);
	/* Look at base DN */
	/* Figure out which partition it is under */
	/* Skip the lot if 'data' isn't here yet (initialization) */
	if (ldb_module_flags(ldb_module_get_ctx(module)) & LDB_FLG_ENABLE_TRACING) {
		ldb_debug(ldb_module_get_ctx(module), LDB_DEBUG_TRACE, "partition_start_trans() -> (metadata partition)");
	}

	/*
	 * We start a transaction on metadata.tdb first and end it last in
	 * end_trans. This makes locking semantics follow TDB rather than MDB,
	 * and effectively locks all partitions at once.
	 * Detail:
	 * Samba AD is special in that the partitions module (this file)
	 * combines multiple independently locked databases into one overall
	 * transaction. Changes across multiple partition DBs in a single
	 * transaction must ALL be either visible or invisible.
	 * The way this is achieved is by taking out a write lock on
	 * metadata.tdb at the start of prepare_commit, while unlocking it at
	 * the end of end_trans. This is matched by read_lock, ensuring it
	 * can't progress until that write lock is released.
	 *
	 * metadata.tdb needs to be a TDB file because MDB uses independent
	 * locks, which means a read lock and a write lock can be held at the
	 * same time, whereas in TDB, the two locks block each other. The TDB
	 * behaviour is required to implement the functionality described
	 * above.
	 *
	 * An important additional detail here is that if prepare_commit is
	 * called on a TDB without any changes being made, no write lock is
	 * taken. We address this by storing a sequence number in metadata.tdb
	 * which is updated every time a replicated attribute is modified.
	 * The possibility of a few unreplicated attributes being out of date
	 * turns out not to be a problem.
	 * For this reason, a lock on sam.ldb (which is a TDB) won't achieve
	 * the same end as locking metadata.tdb, unless we made a modification
	 * to the @ records found there before every prepare_commit.
	 */
	ret = partition_metadata_start_trans(module);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	ret = ldb_next_start_trans(module);
	if (ret != LDB_SUCCESS) {
		partition_metadata_del_trans(module);
		return ret;
	}

	ret = partition_reload_if_required(module, data, NULL);
	if (ret != LDB_SUCCESS) {
		ldb_next_del_trans(module);
		partition_metadata_del_trans(module);
		return ret;
	}

	/*
	 * The following per partition locks are required mostly because TDB
	 * and MDB require locks before read and write ops are permitted.
	 */
	for (i=0; data && data->partitions && data->partitions[i]; i++) {
		if ((module && ldb_module_flags(ldb_module_get_ctx(module)) & LDB_FLG_ENABLE_TRACING)) {
			ldb_debug(ldb_module_get_ctx(module), LDB_DEBUG_TRACE, "partition_start_trans() -> %s",
				  ldb_dn_get_linearized(data->partitions[i]->ctrl->dn));
		}
		ret = ldb_next_start_trans(data->partitions[i]->module);
		if (ret != LDB_SUCCESS) {
			/* Back it out, if it fails on one */
			for (i--; i >= 0; i--) {
				ldb_next_del_trans(data->partitions[i]->module);
			}
			ldb_next_del_trans(module);
			partition_metadata_del_trans(module);
			return ret;
		}
	}

	data->in_transaction++;

	return LDB_SUCCESS;
}

/* prepare for a commit */
int partition_prepare_commit(struct ldb_module *module)
{
	unsigned int i;
	struct partition_private_data *data = talloc_get_type(ldb_module_get_private(module),
							      struct partition_private_data);
	int ret;

	/*
	 * Order of prepare_commit calls must match that in
	 * partition_start_trans. See comment in that function for detail.
	 */
	ret = partition_metadata_prepare_commit(module);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	ret = ldb_next_prepare_commit(module);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	for (i=0; data && data->partitions && data->partitions[i]; i++) {
		if ((module && ldb_module_flags(ldb_module_get_ctx(module)) & LDB_FLG_ENABLE_TRACING)) {
			ldb_debug(ldb_module_get_ctx(module), LDB_DEBUG_TRACE, "partition_prepare_commit() -> %s",
				  ldb_dn_get_linearized(data->partitions[i]->ctrl->dn));
		}
		ret = ldb_next_prepare_commit(data->partitions[i]->module);
		if (ret != LDB_SUCCESS) {
			ldb_asprintf_errstring(ldb_module_get_ctx(module), "prepare_commit error on %s: %s",
					       ldb_dn_get_linearized(data->partitions[i]->ctrl->dn),
					       ldb_errstring(ldb_module_get_ctx(module)));
			return ret;
		}
	}

	if ((module && ldb_module_flags(ldb_module_get_ctx(module)) & LDB_FLG_ENABLE_TRACING)) {
		ldb_debug(ldb_module_get_ctx(module), LDB_DEBUG_TRACE, "partition_prepare_commit() -> (metadata partition)");
	}

	return LDB_SUCCESS;
}


/* end a transaction */
int partition_end_trans(struct ldb_module *module)
{
	int ret, ret2;
	int i;
	struct ldb_context *ldb = ldb_module_get_ctx(module);
	struct partition_private_data *data = talloc_get_type(ldb_module_get_private(module),
							      struct partition_private_data);
	bool trace = module && ldb_module_flags(ldb) & LDB_FLG_ENABLE_TRACING;

	ret = LDB_SUCCESS;

	if (data->in_transaction == 0) {
		DEBUG(0,("partition end transaction mismatch\n"));
		ret = LDB_ERR_OPERATIONS_ERROR;
	} else {
		data->in_transaction--;
	}

	/*
	 * Order of end_trans calls must be the reverse of that in
	 * partition_start_trans. See comment in that function for detail.
	 */
	if (data && data->partitions) {
		/* Just counting the partitions */
		for (i=0; data->partitions[i]; i++) {}

		/* now walk them backwards */
		for (i--; i>=0; i--) {
			struct dsdb_partition *p = data->partitions[i];
			if (trace) {
				ldb_debug(ldb,
					  LDB_DEBUG_TRACE,
					  "partition_end_trans() -> %s",
					  ldb_dn_get_linearized(p->ctrl->dn));
			}
			ret2 = ldb_next_end_trans(p->module);
			if (ret2 != LDB_SUCCESS) {
				ldb_asprintf_errstring(ldb,
					"end_trans error on %s: %s",
					ldb_dn_get_linearized(p->ctrl->dn),
					ldb_errstring(ldb));
				ret = ret2;
			}
		}
	}

	if (trace) {
		ldb_debug(ldb_module_get_ctx(module), LDB_DEBUG_TRACE, "partition_end_trans() -> (metadata partition)");
	}
	ret2 = ldb_next_end_trans(module);
	if (ret2 != LDB_SUCCESS) {
		ret = ret2;
	}

	ret2 = partition_metadata_end_trans(module);
	if (ret2 != LDB_SUCCESS) {
		ret = ret2;
	}

	return ret;
}

/* delete a transaction */
int partition_del_trans(struct ldb_module *module)
{
	int ret, final_ret = LDB_SUCCESS;
	int i;
	struct ldb_context *ldb = ldb_module_get_ctx(module);
	struct partition_private_data *data = talloc_get_type(ldb_module_get_private(module),
							      struct partition_private_data);
	bool trace = module && ldb_module_flags(ldb) & LDB_FLG_ENABLE_TRACING;

	if (data == NULL) {
		DEBUG(0,("partition delete transaction with no private data\n"));
		return ldb_operr(ldb);
	}

	/*
	 * Order of del_trans calls must be the reverse of that in
	 * partition_start_trans. See comment in that function for detail.
	 */
	if (data->partitions) {
		/* Just counting the partitions */
		for (i=0; data->partitions[i]; i++) {}

		/* now walk them backwards */
		for (i--; i>=0; i--) {
			struct dsdb_partition *p = data->partitions[i];
			if (trace) {
				ldb_debug(ldb,
					  LDB_DEBUG_TRACE,
					  "partition_del_trans() -> %s",
					  ldb_dn_get_linearized(p->ctrl->dn));
			}
			ret = ldb_next_del_trans(p->module);
			if (ret != LDB_SUCCESS) {
				ldb_asprintf_errstring(ldb,
					"del_trans error on %s: %s",
					ldb_dn_get_linearized(p->ctrl->dn),
					ldb_errstring(ldb));
				final_ret = ret;
			}
		}
	}

	if (trace) {
		ldb_debug(ldb_module_get_ctx(module), LDB_DEBUG_TRACE, "partition_del_trans() -> (metadata partition)");
	}
	ret = ldb_next_del_trans(module);
	if (ret != LDB_SUCCESS) {
		final_ret = ret;
	}

	ret = partition_metadata_del_trans(module);
	if (ret != LDB_SUCCESS) {
		final_ret = ret;
	}

	if (data->in_transaction == 0) {
		DEBUG(0,("partition del transaction mismatch\n"));
		return ldb_operr(ldb_module_get_ctx(module));
	}
	data->in_transaction--;

	return final_ret;
}

int partition_primary_sequence_number(struct ldb_module *module, TALLOC_CTX *mem_ctx, 
				      uint64_t *seq_number,
				      struct ldb_request *parent)
{
	int ret;
	struct ldb_result *res;
	struct ldb_seqnum_request *tseq;
	struct ldb_seqnum_result *seqr;

	tseq = talloc_zero(mem_ctx, struct ldb_seqnum_request);
	if (tseq == NULL) {
		return ldb_oom(ldb_module_get_ctx(module));
	}
	tseq->type = LDB_SEQ_HIGHEST_SEQ;
	
	ret = dsdb_module_extended(module, tseq, &res,
				   LDB_EXTENDED_SEQUENCE_NUMBER,
				   tseq,
				   DSDB_FLAG_NEXT_MODULE,
				   parent);
	if (ret != LDB_SUCCESS) {
		talloc_free(tseq);
		return ret;
	}
	
	seqr = talloc_get_type_abort(res->extended->data,
				     struct ldb_seqnum_result);
	if (seqr->flags & LDB_SEQ_TIMESTAMP_SEQUENCE) {
		talloc_free(res);
		return ldb_module_error(module, LDB_ERR_OPERATIONS_ERROR,
			"Primary backend in partition module returned a timestamp based seq");
	}

	*seq_number = seqr->seq_num;
	talloc_free(tseq);
	return LDB_SUCCESS;
}


/*
 * Older version of sequence number as sum of sequence numbers for each partition
 */
int partition_sequence_number_from_partitions(struct ldb_module *module,
					      uint64_t *seqr)
{
	int ret;
	unsigned int i;
	uint64_t seq_number = 0;
	struct partition_private_data *data = talloc_get_type(ldb_module_get_private(module),
							      struct partition_private_data);

	ret = partition_primary_sequence_number(module, data, &seq_number, NULL);
	if (ret != LDB_SUCCESS) {
		return ret;
	}
	
	/* Skip the lot if 'data' isn't here yet (initialisation) */
	for (i=0; data && data->partitions && data->partitions[i]; i++) {
		struct ldb_seqnum_request *tseq;
		struct ldb_seqnum_result *tseqr;
		struct ldb_request *treq;
		struct ldb_result *res = talloc_zero(data, struct ldb_result);
		if (res == NULL) {
			return ldb_oom(ldb_module_get_ctx(module));
		}
		tseq = talloc_zero(res, struct ldb_seqnum_request);
		if (tseq == NULL) {
			talloc_free(res);
			return ldb_oom(ldb_module_get_ctx(module));
		}
		tseq->type = LDB_SEQ_HIGHEST_SEQ;
		
		ret = ldb_build_extended_req(&treq, ldb_module_get_ctx(module), res,
					     LDB_EXTENDED_SEQUENCE_NUMBER,
					     tseq,
					     NULL,
					     res,
					     ldb_extended_default_callback,
					     NULL);
		LDB_REQ_SET_LOCATION(treq);
		if (ret != LDB_SUCCESS) {
			talloc_free(res);
			return ret;
		}
		
		ret = partition_request(data->partitions[i]->module, treq);
		if (ret != LDB_SUCCESS) {
			talloc_free(res);
			return ret;
		}
		ret = ldb_wait(treq->handle, LDB_WAIT_ALL);
		if (ret != LDB_SUCCESS) {
			talloc_free(res);
			return ret;
		}
		tseqr = talloc_get_type(res->extended->data,
					struct ldb_seqnum_result);
		seq_number += tseqr->seq_num;
		talloc_free(res);
	}

	*seqr = seq_number;
	return LDB_SUCCESS;
}


/*
 * Newer version of sequence number using metadata tdb
 */
static int partition_sequence_number(struct ldb_module *module, struct ldb_request *req)
{
	struct ldb_extended *ext;
	struct ldb_seqnum_request *seq;
	struct ldb_seqnum_result *seqr;
	uint64_t seq_number;
	int ret;

	seq = talloc_get_type_abort(req->op.extended.data, struct ldb_seqnum_request);
	switch (seq->type) {
	case LDB_SEQ_NEXT:
		ret = partition_metadata_sequence_number_increment(module, &seq_number);
		if (ret != LDB_SUCCESS) {
			return ret;
		}
		break;

	case LDB_SEQ_HIGHEST_SEQ:
		ret = partition_metadata_sequence_number(module, &seq_number);
		if (ret != LDB_SUCCESS) {
			return ret;
		}
		break;

	case LDB_SEQ_HIGHEST_TIMESTAMP:
		return ldb_module_error(module, LDB_ERR_OPERATIONS_ERROR,
					"LDB_SEQ_HIGHEST_TIMESTAMP not supported");
	}

	ext = talloc_zero(req, struct ldb_extended);
	if (!ext) {
		return ldb_module_oom(module);
	}
	seqr = talloc_zero(ext, struct ldb_seqnum_result);
	if (seqr == NULL) {
		talloc_free(ext);
		return ldb_module_oom(module);
	}
	ext->oid = LDB_EXTENDED_SEQUENCE_NUMBER;
	ext->data = seqr;

	seqr->seq_num = seq_number;
	seqr->flags |= LDB_SEQ_GLOBAL_SEQUENCE;

	/* send request done */
	return ldb_module_done(req, NULL, ext, LDB_SUCCESS);
}

/* lock all the backends */
int partition_read_lock(struct ldb_module *module)
{
	int i = 0;
	int ret = 0;
	int ret2 = 0;
	struct ldb_context *ldb = ldb_module_get_ctx(module);
	struct partition_private_data *data = \
		talloc_get_type(ldb_module_get_private(module),
				struct partition_private_data);

	if (ldb_module_flags(ldb) & LDB_FLG_ENABLE_TRACING) {
		ldb_debug(ldb, LDB_DEBUG_TRACE,
			  "partition_read_lock() -> (metadata partition)");
	}

	/*
	 * It is important to only do this for LOCK because:
	 * - we don't want to unlock what we did not lock
	 *
	 * - we don't want to make a new lock on the sam.ldb
	 *   (triggered inside this routine due to the seq num check)
	 *   during an unlock phase as that will violate the lock
	 *   ordering
	 */

	if (data == NULL) {
		TALLOC_CTX *mem_ctx = talloc_new(module);

		data = talloc_zero(mem_ctx, struct partition_private_data);
		if (data == NULL) {
			talloc_free(mem_ctx);
			return ldb_operr(ldb);
		}

		/*
		 * When used from Samba4, this message is set by the
		 * samba4 module, as a fixed value not read from the
		 * DB.  This avoids listing modules in the DB
		 */
		data->forced_module_msg = talloc_get_type(
			ldb_get_opaque(ldb,
				       DSDB_OPAQUE_PARTITION_MODULE_MSG_OPAQUE_NAME),
			struct ldb_message);

		ldb_module_set_private(module, talloc_steal(module,
							    data));
		talloc_free(mem_ctx);
	}

	/*
	 * This will lock sam.ldb and will also call event loops,
	 * so we do it before we get the whole db lock.
	 */
	ret = partition_reload_if_required(module, data, NULL);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	/*
	 * Order of read_lock calls must match that in partition_start_trans.
	 * See comment in that function for detail.
	 */
	ret = partition_metadata_read_lock(module);
	if (ret != LDB_SUCCESS) {
		goto failed;
	}

	/*
	 * The top level DB (sam.ldb) lock is not enough to block another
	 * process in prepare_commit(), because if nothing was changed in the
	 * specific backend, then prepare_commit() is a no-op. Therefore the
	 * metadata.tdb lock is taken out above, as it is the best we can do
	 * right now.
	 */
	ret = ldb_next_read_lock(module);
	if (ret != LDB_SUCCESS) {
		ldb_debug_set(ldb,
			      LDB_DEBUG_FATAL,
			      "Failed to lock db: %s / %s for metadata partition",
			      ldb_errstring(ldb),
			      ldb_strerror(ret));

		return ret;
	}

	/*
	 * The following per partition locks are required mostly because TDB
	 * and MDB require locks before reads are permitted.
	 */
	for (i=0; data && data->partitions && data->partitions[i]; i++) {
		if ((module && ldb_module_flags(ldb) & LDB_FLG_ENABLE_TRACING)) {
			ldb_debug(ldb, LDB_DEBUG_TRACE,
				  "partition_read_lock() -> %s",
				  ldb_dn_get_linearized(
					  data->partitions[i]->ctrl->dn));
		}
		ret = ldb_next_read_lock(data->partitions[i]->module);
		if (ret == LDB_SUCCESS) {
			continue;
		}

		ldb_debug_set(ldb,
			      LDB_DEBUG_FATAL,
			      "Failed to lock db: %s / %s for %s",
			      ldb_errstring(ldb),
			      ldb_strerror(ret),
			      ldb_dn_get_linearized(
				      data->partitions[i]->ctrl->dn));

		goto failed;
	}

	return LDB_SUCCESS;

failed:
	/* Back it out, if it fails on one */
	for (i--; i >= 0; i--) {
		ret2 = ldb_next_read_unlock(data->partitions[i]->module);
		if (ret2 != LDB_SUCCESS) {
			ldb_debug(ldb,
				  LDB_DEBUG_FATAL,
				  "Failed to unlock db: %s / %s",
				  ldb_errstring(ldb),
				  ldb_strerror(ret2));
		}
	}
	ret2 = ldb_next_read_unlock(module);
	if (ret2 != LDB_SUCCESS) {
		ldb_debug(ldb,
			  LDB_DEBUG_FATAL,
			  "Failed to unlock db: %s / %s",
			  ldb_errstring(ldb),
			  ldb_strerror(ret2));
	}
	return ret;
}

/* unlock all the backends */
int partition_read_unlock(struct ldb_module *module)
{
	int i;
	int ret = LDB_SUCCESS;
	int ret2;
	struct ldb_context *ldb = ldb_module_get_ctx(module);
	struct partition_private_data *data = \
		talloc_get_type(ldb_module_get_private(module),
				struct partition_private_data);
	bool trace = module && ldb_module_flags(ldb) & LDB_FLG_ENABLE_TRACING;

	/*
	 * Order of read_unlock calls must be the reverse of that in
	 * partition_start_trans. See comment in that function for detail.
	 */
	if (data && data->partitions) {
		/* Just counting the partitions */
		for (i=0; data->partitions[i]; i++) {}

		/* now walk them backwards */
		for (i--; i>=0; i--) {
			struct dsdb_partition *p = data->partitions[i];
			if (trace) {
				ldb_debug(ldb, LDB_DEBUG_TRACE,
					  "partition_read_unlock() -> %s",
					  ldb_dn_get_linearized(p->ctrl->dn));
			}
			ret2 = ldb_next_read_unlock(p->module);
			if (ret2 != LDB_SUCCESS) {
				ldb_debug_set(ldb,
					   LDB_DEBUG_FATAL,
					   "Failed to lock db: %s / %s for %s",
					   ldb_errstring(ldb),
					   ldb_strerror(ret2),
					   ldb_dn_get_linearized(p->ctrl->dn));

				/*
				 * Don't overwrite the original failure code
				 * if there was one
				 */
				if (ret == LDB_SUCCESS) {
					ret = ret2;
				}
			}
		}
	}

	if (trace) {
		ldb_debug(ldb, LDB_DEBUG_TRACE,
			  "partition_read_unlock() -> (metadata partition)");
	}

	ret2 = ldb_next_read_unlock(module);
	if (ret2 != LDB_SUCCESS) {
		ldb_debug_set(ldb,
			      LDB_DEBUG_FATAL,
			      "Failed to unlock db: %s / %s for metadata partition",
			      ldb_errstring(ldb),
			      ldb_strerror(ret2));

		/*
		 * Don't overwrite the original failure code
		 * if there was one
		 */
		if (ret == LDB_SUCCESS) {
			ret = ret2;
		}
	}

	ret2 = partition_metadata_read_unlock(module);

	/*
	 * Don't overwrite the original failure code
	 * if there was one
	 */
	if (ret == LDB_SUCCESS) {
		ret = ret2;
	}

	return ret;
}

/* extended */
static int partition_extended(struct ldb_module *module, struct ldb_request *req)
{
	struct partition_private_data *data = talloc_get_type(ldb_module_get_private(module),
							      struct partition_private_data);
	struct partition_context *ac;
	int ret;

	/* if we aren't initialised yet go further */
	if (!data) {
		return ldb_next_request(module, req);
	}

	if (strcmp(req->op.extended.oid, DSDB_EXTENDED_SCHEMA_UPDATE_NOW_OID) == 0) {
		/* Update the metadata.tdb to increment the schema version if needed*/
		DEBUG(10, ("Incrementing the sequence_number after schema_update_now\n"));
		ret = partition_metadata_inc_schema_sequence(module);
		return ldb_module_done(req, NULL, NULL, ret);
	}
	
	if (strcmp(req->op.extended.oid, LDB_EXTENDED_SEQUENCE_NUMBER) == 0) {
		return partition_sequence_number(module, req);
	}

	if (strcmp(req->op.extended.oid, DSDB_EXTENDED_CREATE_PARTITION_OID) == 0) {
		return partition_create(module, req);
	}

	/* 
	 * as the extended operation has no dn
	 * we need to send it to all partitions
	 */

	ac = partition_init_ctx(module, req);
	if (!ac) {
		return ldb_operr(ldb_module_get_ctx(module));
	}

	return partition_send_all(module, ac, req);
}

static const struct ldb_module_ops ldb_partition_module_ops = {
	.name		   = "partition",
	.init_context	   = partition_init,
	.search            = partition_search,
	.add               = partition_add,
	.modify            = partition_modify,
	.del               = partition_delete,
	.rename            = partition_rename,
	.extended          = partition_extended,
	.start_transaction = partition_start_trans,
	.prepare_commit    = partition_prepare_commit,
	.end_transaction   = partition_end_trans,
	.del_transaction   = partition_del_trans,
	.read_lock         = partition_read_lock,
	.read_unlock       = partition_read_unlock
};

int ldb_partition_module_init(const char *version)
{
	LDB_MODULE_CHECK_VERSION(version);
	return ldb_register_module(&ldb_partition_module_ops);
}
