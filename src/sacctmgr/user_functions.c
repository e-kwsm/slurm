/*****************************************************************************\
 *  user_functions.c - functions dealing with users in the accounting system.
 *****************************************************************************
 *  Copyright (C) 2008 Lawrence Livermore National Security.
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <da@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "src/sacctmgr/sacctmgr.h"
#include "src/common/assoc_mgr.h"
#include "src/common/uid.h"
#include "src/common/xstring.h"
#include "src/interfaces/data_parser.h"

typedef struct {
	char *cluster;
	char *user;
} regret_t;

static int _set_add_cond(int *start, int argc, char **argv,
			 slurmdb_add_assoc_cond_t *add_assoc,
			 slurmdb_user_rec_t *user)
{
	int i, end, command_len, option = 0, set = 0;

	xassert(add_assoc);
	xassert(user);

	for (i = (*start); i < argc; i++) {
		end = parse_option_end(argv[i]);
		if (!end)
			command_len = strlen(argv[i]);
		else {
			command_len = end - 1;
			if (argv[i][end] == '=') {
				option = (int)argv[i][end - 1];
				end++;
			}
		}

		if (!end ||
		    !xstrncasecmp(argv[i], "Names", MAX(command_len, 1)) ||
		    !xstrncasecmp(argv[i], "Users", MAX(command_len, 1))) {
			if (!add_assoc->user_list)
				add_assoc->user_list = list_create(xfree_ptr);
			if (!slurm_addto_char_list_with_case(
				    add_assoc->user_list,
				    argv[i] + end,
				    user_case_norm))
				exit_code=1;
		} else if (!xstrncasecmp(argv[i], "Accounts",
					   MAX(command_len, 3)) ||
			   !xstrncasecmp(argv[i], "Acct",
					 MAX(command_len, 4))) {
			if (!add_assoc->acct_list)
				add_assoc->acct_list = list_create(xfree_ptr);
			slurm_addto_char_list(add_assoc->acct_list,
					      argv[i] + end);
		} else if (!xstrncasecmp(argv[i], "AdminLevel",
					 MAX(command_len, 2))) {
			user->admin_level =
				str_2_slurmdb_admin_level(argv[i] + end);
		} else if (!xstrncasecmp(argv[i], "Clusters",
					 MAX(command_len, 1))) {
			if (!add_assoc->cluster_list)
				add_assoc->cluster_list =
					list_create(xfree_ptr);
			slurm_addto_char_list(add_assoc->cluster_list,
					      argv[i] + end);
		} else if (!xstrncasecmp(argv[i], "DefaultAccount",
					 MAX(command_len, 8))) {
			if (user->default_acct) {
				fprintf(stderr,
					" Already listed DefaultAccount %s\n",
					user->default_acct);
				exit_code = 1;
				continue;
			}
			user->default_acct =
				strip_quotes(argv[i]+end, NULL, 1);
			if (!add_assoc->acct_list)
				add_assoc->acct_list = list_create(xfree_ptr);
			slurm_addto_char_list(add_assoc->acct_list,
					      user->default_acct);
		} else if (!xstrncasecmp(argv[i], "DefaultWCKey",
					 MAX(command_len, 8))) {
			if (user->default_wckey) {
				fprintf(stderr,
					" Already listed DefaultWCKey %s\n",
					user->default_wckey);
				exit_code = 1;
				continue;
			}
			user->default_wckey =
				strip_quotes(argv[i]+end, NULL, 1);
			if (!add_assoc->wckey_list)
				add_assoc->wckey_list = list_create(xfree_ptr);
			slurm_addto_char_list(add_assoc->wckey_list,
					      user->default_wckey);
		} else if (!xstrncasecmp(argv[i], "Partitions",
					 MAX(command_len, 1))) {
			if (!add_assoc->partition_list)
				add_assoc->partition_list =
					list_create(xfree_ptr);
			slurm_addto_char_list(add_assoc->partition_list,
					      argv[i] + end);
		} else if (!xstrncasecmp(argv[i], "WCKeys",
					 MAX(command_len, 1))) {
			if (!add_assoc->wckey_list)
				add_assoc->wckey_list = list_create(xfree_ptr);
			slurm_addto_char_list(add_assoc->wckey_list, argv[i] + end);
		} else {
			set = sacctmgr_set_assoc_rec(&add_assoc->assoc,
						     argv[i], argv[i] + end,
						     command_len, option);
			if (!set) {
				exit_code=1;
				fprintf(stderr, " Unknown option: %s\n",
					argv[i]);
			}
		}
	}

	(*start) = i;
	return set;
}

static int _set_cond(int *start, int argc, char **argv,
		     slurmdb_user_cond_t *user_cond,
		     slurmdb_wckey_cond_t *wckey_cond,
		     list_t *format_list)
{
	int i;
	int cond_set = 0;
	int end = 0;
	slurmdb_assoc_cond_t *assoc_cond = NULL;
	int command_len = 0;
	int option = 0;

	if (!user_cond) {
		error("No user_cond given");
		return -1;
	}

	if (!user_cond->assoc_cond)
		user_cond->assoc_cond =
			xmalloc(sizeof(slurmdb_assoc_cond_t));

	assoc_cond = user_cond->assoc_cond;

	/* we need this to make sure we only change users, not
	 * accounts if this list didn't exist it would change
	 * accounts. Having it blank is fine, it just needs to
	 * exist.
	 */
	if (!assoc_cond->user_list)
		assoc_cond->user_list = list_create(xfree_ptr);

	for (i=(*start); i<argc; i++) {
		end = parse_option_end(argv[i]);
		if (!end)
			command_len=strlen(argv[i]);
		else {
			command_len=end-1;
			if (argv[i][end] == '=') {
				option = (int)argv[i][end-1];
				end++;
			}
		}

		if (!xstrncasecmp(argv[i], "Set", MAX(command_len, 3))) {
			i--;
			break;
		} else if (!end && !xstrncasecmp(argv[i], "WithAssoc",
						 MAX(command_len, 5))) {
			user_cond->with_assocs = 1;
		} else if (!end &&
			   !xstrncasecmp(argv[i], "WithCoordinators",
					 MAX(command_len, 5))) {
			user_cond->with_coords = 1;
		} else if (!end &&
			   !xstrncasecmp(argv[i], "WithDeleted",
					 MAX(command_len, 5))) {
			user_cond->with_deleted = 1;
			assoc_cond->flags |= ASSOC_COND_FLAG_WITH_DELETED;
		} else if (!end &&
			   !xstrncasecmp(argv[i], "WithRawQOSLevel",
					 MAX(command_len, 5))) {
			assoc_cond->flags |= ASSOC_COND_FLAG_RAW_QOS;
		} else if (!end && !xstrncasecmp(argv[i], "WOPLimits",
						 MAX(command_len, 4))) {
			assoc_cond->flags |= ASSOC_COND_FLAG_WOPL;
		} else if (!end && !xstrncasecmp(argv[i], "where",
						 MAX(command_len, 5))) {
			continue;
		} else if (!end
			   || !xstrncasecmp(argv[i], "Names",
					    MAX(command_len, 1))
			   || !xstrncasecmp(argv[i], "Users",
					    MAX(command_len, 1))) {
			if (slurm_addto_char_list_with_case(assoc_cond->user_list,
							    argv[i]+end,
							    user_case_norm))
				cond_set |= SA_SET_USER;
			else
				exit_code=1;
		} else if (!xstrncasecmp(argv[i], "AdminLevel",
					 MAX(command_len, 2))) {
			user_cond->admin_level =
				str_2_slurmdb_admin_level(argv[i]+end);
			cond_set |= SA_SET_USER;
		} else if (!xstrncasecmp(argv[i], "Clusters",
					 MAX(command_len, 1))) {
			if (!assoc_cond->cluster_list)
				assoc_cond->cluster_list =
					list_create(xfree_ptr);
			if (slurm_addto_char_list(assoc_cond->cluster_list,
						  argv[i]+end)) {
				/*
				 *  Don't set SA_SET_ASSOC here, it is only
				 *  needed for deleting user and it is
				 *  handled there later.
				 */
				cond_set |= SA_SET_USER;
			}
		} else if (!xstrncasecmp(argv[i], "DefaultAccount",
					 MAX(command_len, 8))) {
			if (!user_cond->def_acct_list) {
				user_cond->def_acct_list =
					list_create(xfree_ptr);
			}
			if (slurm_addto_char_list(user_cond->def_acct_list,
						 argv[i]+end))
				cond_set |= SA_SET_USER;
			else
				exit_code=1;
		} else if (!xstrncasecmp(argv[i], "DefaultWCKey",
					 MAX(command_len, 8))) {
			if (!user_cond->def_wckey_list) {
				user_cond->def_wckey_list =
					list_create(xfree_ptr);
			}
			if (slurm_addto_char_list(user_cond->def_wckey_list,
						 argv[i]+end))
				cond_set |= SA_SET_USER;
			else
				exit_code=1;
		} else if (!xstrncasecmp(argv[i], "Format",
					 MAX(command_len, 1))) {
			if (format_list) {
				/* We need this to get the defaults. (Usually
				 * only for the calling cluster) */
				if (xstrcasestr(argv[i]+end, "default"))
					assoc_cond->flags |=
						ASSOC_COND_FLAG_ONLY_DEFS;

				slurm_addto_char_list(format_list, argv[i]+end);
			}
		} else if (!xstrncasecmp(argv[i], "WCKeys",
					 MAX(command_len, 1))) {
			if (!wckey_cond) {
				exit_code = 1;
				break;
			}

			if (!wckey_cond->name_list)
				wckey_cond->name_list = list_create(xfree_ptr);

			if (slurm_addto_char_list(wckey_cond->name_list,
						  argv[i]+end))
				cond_set |= SA_SET_WCKEY;
			else
				exit_code = 1;
		} else if (sacctmgr_set_assoc_cond(
				   assoc_cond, argv[i], argv[i]+end,
				   command_len, option)) {
			cond_set |= SA_SET_ASSOC;
		} else {
			exit_code=1;
			fprintf(stderr, " Unknown condition: %s\n"
				" Use keyword 'set' to modify value\n",
				argv[i]);
		}
	}

	(*start) = i;

	if ((cond_set & SA_SET_ASSOC) && (cond_set & SA_SET_WCKEY)) {
		fprintf(stderr," Mixing Account and WCKeys is not allowed\n");
		exit_code = 1;
		return -1;
	}

	return cond_set;
}

static int _set_rec(int *start, int argc, char **argv,
		    slurmdb_user_rec_t *user,
		    slurmdb_assoc_rec_t *assoc)
{
	int i;
	int rec_set = 0;
	int end = 0;
	int command_len = 0;
	int option = 0;

	xassert(user);
	xassert(assoc);

	for (i=(*start); i<argc; i++) {
		end = parse_option_end(argv[i]);
		if (!end)
			command_len=strlen(argv[i]);
		else {
			command_len=end-1;
			if (argv[i][end] == '=') {
				option = (int)argv[i][end-1];
				end++;
			}
		}

		if (!xstrncasecmp(argv[i], "Where", MAX(command_len, 5))) {
			i--;
			break;
		} else if (!end && !xstrncasecmp(argv[i], "set",
						 MAX(command_len, 3))) {
			continue;
		} else if (!end) {
			exit_code=1;
			fprintf(stderr,
				" Bad format on %s: End your option with "
				"an '=' sign\n", argv[i]);
		} else if (!xstrncasecmp(argv[i], "AdminLevel",
					 MAX(command_len, 2))) {
			user->admin_level =
				str_2_slurmdb_admin_level(argv[i]+end);
			rec_set |= SA_SET_USER;
		} else if (!xstrncasecmp(argv[i], "DefaultAccount",
					 MAX(command_len, 8))) {
			if (user->default_acct)
				xfree(user->default_acct);
			user->default_acct = strip_quotes(argv[i]+end, NULL, 1);
			rec_set |= SA_SET_USER;
		} else if (!xstrncasecmp(argv[i], "DefaultWCKey",
					 MAX(command_len, 8))) {
			if (user->default_wckey)
				xfree(user->default_wckey);
			user->default_wckey =
				strip_quotes(argv[i]+end, NULL, 1);
			rec_set |= SA_SET_USER;
		} else if (!xstrncasecmp(argv[i], "NewName",
					 MAX(command_len, 1))) {
			if (user->name)
				xfree(user->name);
			user->name = strip_quotes(argv[i]+end, NULL,
						  user_case_norm);
			rec_set |= SA_SET_USER;
		} else if (!xstrncasecmp(argv[i], "RawUsage",
					 MAX(command_len, 7))) {
			uint32_t usage;
			if (!assoc->usage)
				assoc->usage =
					xmalloc(sizeof(slurmdb_assoc_usage_t));
			if (get_uint(argv[i]+end, &usage,
				     "RawUsage") == SLURM_SUCCESS) {
				assoc->usage->usage_raw = usage;
				rec_set |= SA_SET_ASSOC;
			}
		} else if (sacctmgr_set_assoc_rec(
				   assoc, argv[i], argv[i]+end,
				   command_len, option)) {
			rec_set |= SA_SET_ASSOC;
		} else {
			exit_code=1;
			fprintf(stderr, " Unknown option: %s\n"
				" Use keyword 'where' to modify condition\n",
				argv[i]);
		}
	}

	(*start) = i;

	return rec_set;
}

static int _check_and_set_cluster_list(list_t *cluster_list)
{
	int rc = SLURM_SUCCESS;
	list_t *tmp_list = NULL;
	list_itr_t *itr_c;
	slurmdb_cluster_rec_t *cluster_rec = NULL;

	xassert(cluster_list);

	if (list_count(cluster_list))
		return rc;

	tmp_list = slurmdb_clusters_get(db_conn, NULL);
	if (!tmp_list) {
		exit_code=1;
		fprintf(stderr,
			" Problem getting clusters from database.  "
			"Contact your admin.\n");
		return SLURM_ERROR;
	}

	itr_c = list_iterator_create(tmp_list);
	while ((cluster_rec = list_next(itr_c))) {
		if (cluster_rec->flags & CLUSTER_FLAG_EXT)
			continue;
		list_append(cluster_list, cluster_rec->name);
		cluster_rec->name = NULL;
	}
	list_iterator_destroy(itr_c);
	FREE_NULL_LIST(tmp_list);

	if (!list_count(cluster_list)) {
		exit_code=1;
		fprintf(stderr,
			"  Can't add/modify users, no cluster defined yet.\n"
			" Please contact your administrator.\n");
		return SLURM_ERROR;
	}

	return rc;
}

static int _check_default_assocs(char *def_acct, list_t *user_list,
				 list_t *cluster_list)
{
	char *user = NULL, *cluster = NULL;
	list_t *regret_list = NULL;
	list_t *local_assoc_list = NULL;
	list_itr_t *itr = NULL;
	list_itr_t *itr_c = NULL;
	regret_t *regret = NULL;
	slurmdb_assoc_cond_t assoc_cond;
	int rc = SLURM_SUCCESS;

	if (!def_acct)
		return rc;

	xassert(user_list);
	xassert(cluster_list);

	if (!list_count(user_list) || !list_count(cluster_list))
		return SLURM_ERROR;

	memset(&assoc_cond, 0, sizeof(slurmdb_assoc_cond_t));
	assoc_cond.user_list = user_list;
	assoc_cond.cluster_list = cluster_list;
	assoc_cond.acct_list = list_create(NULL);
	list_append(assoc_cond.acct_list, def_acct);
	local_assoc_list = slurmdb_associations_get(
		db_conn, &assoc_cond);
	FREE_NULL_LIST(assoc_cond.acct_list);

	itr = list_iterator_create(user_list);
	itr_c = list_iterator_create(cluster_list);
	/* We have to check here for the user names to
	 * make sure the user has an association with
	 * the new default account.  We have to wait
	 * until we get the ret_list of names since
	 * names are not required to change a user since
	 * you can specify a user by something else
	 * like default_account or something.  If the
	 * user doesn't have the account make
	 * note of it.
	 */
	while((user = list_next(itr))) {
		while((cluster = list_next(itr_c))) {
			if (!sacctmgr_find_assoc_from_list(
				local_assoc_list,
				user, def_acct, cluster, "*")) {
				regret = xmalloc(sizeof(regret_t));
				regret->user = user;
				regret->cluster = cluster;
				/* xfree_ptr just does an
				   xfree so we can override it here
				   since we aren't allocating any
				   extra memory */
				if (!regret_list)
					regret_list = list_create(xfree_ptr);
				list_append(regret_list, regret);
				continue;
			}
		}
		list_iterator_reset(itr_c);
	}
	list_iterator_destroy(itr);
	list_iterator_destroy(itr_c);
	FREE_NULL_LIST(local_assoc_list);

	if (regret_list) {
		itr = list_iterator_create(regret_list);
		printf(" Can't modify because these users "
		       "aren't associated with new "
		       "default account '%s'...\n",
		       def_acct);
		while((regret = list_next(itr))) {
			printf("  U = %s C = %s\n",
			       regret->user, regret->cluster);
		}
		list_iterator_destroy(itr);
		exit_code=1;
		rc = SLURM_ERROR;
		FREE_NULL_LIST(regret_list);
	}

	return rc;
}

static int _check_default_wckeys(char *def_wckey, list_t *user_list,
				 list_t *cluster_list)
{
	char *user = NULL, *cluster = NULL;
	list_t *regret_list = NULL;
	list_t *local_wckey_list = NULL;
	list_itr_t *itr = NULL;
	list_itr_t *itr_c = NULL;
	regret_t *regret = NULL;
	slurmdb_wckey_cond_t wckey_cond;
	int rc = SLURM_SUCCESS;

	if (!def_wckey)
		return rc;

	xassert(user_list);
	xassert(cluster_list);

	if (!list_count(user_list) || !list_count(cluster_list))
		return SLURM_ERROR;

	memset(&wckey_cond, 0, sizeof(slurmdb_wckey_cond_t));
	wckey_cond.user_list = user_list;
	wckey_cond.cluster_list = cluster_list;
	wckey_cond.name_list = list_create(NULL);
	list_append(wckey_cond.name_list, def_wckey);
	local_wckey_list = slurmdb_wckeys_get(
		db_conn, &wckey_cond);
	FREE_NULL_LIST(wckey_cond.name_list);

	itr = list_iterator_create(user_list);
	itr_c = list_iterator_create(cluster_list);
	/* We have to check here for the user names to
	 * make sure the user has an wckey with
	 * the new default wckey.  We have to wait
	 * until we get the ret_list of names since
	 * names are not required to change a user since
	 * you can specify a user by something else
	 * like default_account or something.  If the
	 * user doesn't have the account make
	 * note of it.
	 */
	while((user = list_next(itr))) {
		while((cluster = list_next(itr_c))) {
			if (!sacctmgr_find_wckey_from_list(
				local_wckey_list,
				user, def_wckey, cluster)) {
				regret = xmalloc(sizeof(regret_t));
				regret->user = user;
				regret->cluster = cluster;
				/* xfree_ptr just does an
				   xfree so we can override it here
				   since we aren't allocating any
				   extra memory */
				if (!regret_list)
					regret_list = list_create(xfree_ptr);
				list_append(regret_list, regret);
				continue;
			}
		}
		list_iterator_reset(itr_c);
	}
	list_iterator_destroy(itr);
	list_iterator_destroy(itr_c);
	FREE_NULL_LIST(local_wckey_list);

	if (regret_list) {
		itr = list_iterator_create(regret_list);
		printf(" Can't modify because these users "
		       "aren't associated with new "
		       "default wckey '%s'...\n",
		       def_wckey);
		while((regret = list_next(itr))) {
			printf("  U = %s C = %s\n",
			       regret->user, regret->cluster);
		}
		list_iterator_destroy(itr);
		exit_code=1;
		rc = SLURM_ERROR;
		FREE_NULL_LIST(regret_list);
	}

	return rc;
}

/*
 * IN: user_cond - used for the assoc_cond pointing to the user and
 *     account list
 * IN: check - whether or not to check if the existence of the above lists
 */
static int _check_coord_request(slurmdb_user_cond_t *user_cond, bool check)
{
	list_itr_t *itr = NULL, *itr2 = NULL;
	char *name = NULL;
	slurmdb_user_rec_t *user_rec = NULL;
	slurmdb_account_rec_t *acct_rec = NULL;
	slurmdb_account_cond_t account_cond;
	list_t *local_acct_list = NULL;
	list_t *local_user_list = NULL;
	int rc = SLURM_SUCCESS;

	if (!user_cond) {
		exit_code=1;
		fprintf(stderr, " You need to specify the user_cond here.\n");
		return SLURM_ERROR;
	}

	if (check && (!user_cond->assoc_cond->user_list
		     || !list_count(user_cond->assoc_cond->user_list))) {
		exit_code=1;
		fprintf(stderr, " You need to specify a user list here.\n");
		return SLURM_ERROR;
	}

	if (check && (!user_cond->assoc_cond->acct_list
		     || !list_count(user_cond->assoc_cond->acct_list))) {
		exit_code=1;
		fprintf(stderr, " You need to specify an account list here.\n");
		return SLURM_ERROR;
	}

	memset(&account_cond, 0, sizeof(slurmdb_account_cond_t));
	account_cond.assoc_cond = user_cond->assoc_cond;
	local_acct_list =
		slurmdb_accounts_get(db_conn, &account_cond);
	if (!local_acct_list) {
		exit_code=1;
		fprintf(stderr, " Problem getting accounts from database.  "
			"Contact your admin.\n");
		return SLURM_ERROR;
	}

	if (user_cond->assoc_cond->acct_list &&
	   (list_count(local_acct_list) !=
	    list_count(user_cond->assoc_cond->acct_list))) {

		itr = list_iterator_create(user_cond->assoc_cond->acct_list);
		itr2 = list_iterator_create(local_acct_list);

		while((name = list_next(itr))) {
			while((acct_rec = list_next(itr2))) {
				if (!xstrcmp(name, acct_rec->name))
					break;
			}
			list_iterator_reset(itr2);
			if (!acct_rec) {
				fprintf(stderr,
					" You specified a non-existent "
					"account '%s'.\n", name);
				exit_code=1;
				rc = SLURM_ERROR;
			}
		}
		list_iterator_destroy(itr);
		list_iterator_destroy(itr2);
	}

	local_user_list = slurmdb_users_get(db_conn, user_cond);
	if (!local_user_list) {
		exit_code=1;
		fprintf(stderr, " Problem getting users from database.  "
			"Contact your admin.\n");
		FREE_NULL_LIST(local_acct_list);
		return SLURM_ERROR;
	}

	if (user_cond->assoc_cond->user_list &&
	   (list_count(local_user_list) !=
	    list_count(user_cond->assoc_cond->user_list))) {

		itr = list_iterator_create(user_cond->assoc_cond->user_list);
		itr2 = list_iterator_create(local_user_list);

		while((name = list_next(itr))) {
			while((user_rec = list_next(itr2))) {
				if (!xstrcmp(name, user_rec->name))
					break;
			}
			list_iterator_reset(itr2);
			if (!user_rec) {
				fprintf(stderr,
					" You specified a non-existent "
					"user '%s'.\n", name);
				exit_code=1;
				rc = SLURM_ERROR;
			}
		}
		list_iterator_destroy(itr);
		list_iterator_destroy(itr2);
	}

	FREE_NULL_LIST(local_acct_list);
	FREE_NULL_LIST(local_user_list);

	return rc;
}

static int _check_uid(void *x, void *arg)
{
	char *name = x;
	uid_t pw_uid;

	if (uid_from_string(name, &pw_uid) != SLURM_SUCCESS) {
		char *warning = xstrdup_printf(
			"There is no uid for user '%s'\nAre you sure you want to continue?",
			name);

		if (!commit_check(warning)) {
			xfree(warning);
			exit_code = 1;
			return -1;
		}
		xfree(warning);
	}

	return 0;
}

extern int sacctmgr_add_user(int argc, char **argv)
{
	int rc = SLURM_SUCCESS;
	char *ret_str = NULL;
	int limit_set = 0;
	slurmdb_add_assoc_cond_t add_assoc;
	slurmdb_user_rec_t user;
	slurmdb_assoc_rec_t *start_assoc;

	slurmdb_init_add_assoc_cond(&add_assoc, 0);
	start_assoc = &add_assoc.assoc;

	memset(&user, 0, sizeof(user));
	user.admin_level = SLURMDB_ADMIN_NOTSET;

	for (int i = 0; i < argc; i++) {
		int command_len = strlen(argv[i]);
		if (!xstrncasecmp(argv[i], "Where", MAX(command_len, 5)) ||
		    !xstrncasecmp(argv[i], "Set", MAX(command_len, 3)))
			i++;
		limit_set = _set_add_cond(&i, argc, argv, &add_assoc, &user);
		if (exit_code) {
			rc = SLURM_ERROR;
			goto end_it;
		}
	}

	if (!list_count(add_assoc.user_list)) {
		fprintf(stderr, " Need name of user to add.\n");
		exit_code = 1;
		rc = SLURM_ERROR;
		goto end_it;
	}

	if (!list_count(add_assoc.acct_list) &&
	    !list_count(add_assoc.wckey_list)) {
		fprintf(stderr, " Need name of account or wckey to add.\n");
		exit_code = 1;
		rc = SLURM_ERROR;
		goto end_it;
	}

	list_for_each_ro(add_assoc.user_list, _check_uid, NULL);

	if (exit_code) {
		rc = SLURM_ERROR;
		goto end_it;
	}

	notice_thread_init();
	ret_str = slurmdb_users_add_cond(db_conn, &add_assoc, &user);
	rc = errno;
	notice_thread_fini();

	if (rc == SLURM_SUCCESS) {
		if (ret_str) {
			printf("%s", ret_str);
			xfree(ret_str);
			if (limit_set) {
				printf(" Non Default Settings\n");
				sacctmgr_print_assoc_limits(start_assoc);
			}
		}
		if (commit_check("Would you like to commit changes?")) {
			rc = slurmdb_connection_commit(db_conn, 1);
			if (rc != SLURM_SUCCESS)
				fprintf(stderr, " Error committing changes: %s\n",
					slurm_strerror(rc));
		} else {
			printf(" Changes Discarded\n");
			rc = slurmdb_connection_commit(db_conn, 0);
			if (rc != SLURM_SUCCESS)
				fprintf(stderr, " Error rolling back changes: %s\n",
					slurm_strerror(rc));
		}
	} else if (rc == SLURM_NO_CHANGE_IN_DATA) {
		if (ret_str) {
			printf("%sNothing added.\nMake sure the accounts requested here have been added to the system/cluster. (sacctmgr add account...)\n", ret_str);
		} else
			printf("%s\n", slurm_strerror(rc));
	} else {
		exit_code=1;
		fprintf(stderr,
			" error: Problem adding user associations: %s (%s)\n",
			slurm_strerror(rc), ret_str);
		rc = SLURM_ERROR;
	}

end_it:
	xfree(ret_str);
	slurmdb_free_add_assoc_cond_members(&add_assoc);
	slurmdb_free_user_rec_members(&user);

	return rc;
}

extern int sacctmgr_add_coord(int argc, char **argv)
{
	int rc = SLURM_SUCCESS;
	int i = 0;
	int cond_set = 0, prev_set = 0;
	slurmdb_user_cond_t *user_cond = xmalloc(sizeof(slurmdb_user_cond_t));
	char *name = NULL;
	char *user_str = NULL;
	char *acct_str = NULL;
	list_itr_t *itr = NULL;

	for (i=0; i<argc; i++) {
		int command_len = strlen(argv[i]);
		if (!xstrncasecmp(argv[i], "Where", MAX(command_len, 5))
		    || !xstrncasecmp(argv[i], "Set", MAX(command_len, 3)))
			i++;
		prev_set = _set_cond(&i, argc, argv, user_cond, NULL, NULL);
		cond_set |= prev_set;
	}

	if (exit_code) {
		slurmdb_destroy_user_cond(user_cond);
		return SLURM_ERROR;
	} else if (!cond_set) {
		exit_code=1;
		fprintf(stderr, " You need to specify conditions to "
			"to add the coordinator.\n");
		slurmdb_destroy_user_cond(user_cond);
		return SLURM_ERROR;
	}

	if ((_check_coord_request(user_cond, true) == SLURM_ERROR)
	   || exit_code) {
		slurmdb_destroy_user_cond(user_cond);
		return SLURM_ERROR;
	}

	itr = list_iterator_create(user_cond->assoc_cond->user_list);
	while((name = list_next(itr))) {
		xstrfmtcat(user_str, "  %s\n", name);
	}
	list_iterator_destroy(itr);

	itr = list_iterator_create(user_cond->assoc_cond->acct_list);
	while((name = list_next(itr))) {
		xstrfmtcat(acct_str, "  %s\n", name);
	}
	list_iterator_destroy(itr);

	printf(" Adding Coordinator User(s)\n%s", user_str);
	printf(" To Account(s) and all sub-accounts\n%s", acct_str);

	notice_thread_init();
	rc = slurmdb_coord_add(db_conn,
			       user_cond->assoc_cond->acct_list,
			       user_cond);
	notice_thread_fini();
	slurmdb_destroy_user_cond(user_cond);

	if (rc == SLURM_SUCCESS) {
		if (commit_check("Would you like to commit changes?")) {
			rc = slurmdb_connection_commit(db_conn, 1);
			if (rc != SLURM_SUCCESS)
				fprintf(stderr, " Error committing changes: %s\n",
					slurm_strerror(rc));
		} else {
			printf(" Changes Discarded\n");
			rc = slurmdb_connection_commit(db_conn, 0);
			if (rc != SLURM_SUCCESS)
				fprintf(stderr, " Error rolling back changes: %s\n",
					slurm_strerror(rc));
		}
	} else {
		exit_code = 1;
		fprintf(stderr, " Problem adding coordinator: %s\n",
			slurm_strerror(rc));
		rc = SLURM_ERROR;
	}

	return rc;
}

extern int sacctmgr_list_user(int argc, char **argv)
{
	int rc = SLURM_SUCCESS;
	slurmdb_user_cond_t *user_cond = xmalloc(sizeof(slurmdb_user_cond_t));
	list_t *user_list;
	int i=0, cond_set=0, prev_set=0;
	list_itr_t *itr = NULL;
	list_itr_t *itr2 = NULL;
	slurmdb_user_rec_t *user = NULL;
	slurmdb_assoc_rec_t *assoc = NULL;

	print_field_t *field = NULL;
	int field_count = 0;

	list_t *format_list = list_create(xfree_ptr);
	list_t *print_fields_list; /* types are of print_field_t */

	user_cond->with_assocs = with_assoc_flag;
	user_cond->assoc_cond = xmalloc(sizeof(slurmdb_assoc_cond_t));

	for (i=0; i<argc; i++) {
		int command_len = strlen(argv[i]);
		if (!xstrncasecmp(argv[i], "Where", MAX(command_len, 5))
		    || !xstrncasecmp(argv[i], "Set", MAX(command_len, 3)))
			i++;
		prev_set = _set_cond(&i, argc, argv, user_cond,
				     NULL, format_list);
		cond_set |= prev_set;
	}

	if (exit_code) {
		slurmdb_destroy_user_cond(user_cond);
		FREE_NULL_LIST(format_list);
		return SLURM_ERROR;
	}

	if (!list_count(format_list)) {
		if (slurm_get_track_wckey())
			slurm_addto_char_list(format_list,
					      "U,DefaultA,DefaultW,Ad");
		else
			slurm_addto_char_list(format_list, "U,DefaultA,Ad");
		if (user_cond->with_coords)
			slurm_addto_char_list(format_list, "Coord");
		if (user_cond->with_assocs)
			slurm_addto_char_list(format_list,
					      "Cl,Acc,Part,Share,Priority,"
					      "MaxJ,MaxN,MaxCPUs,MaxS,MaxW,"
					      "MaxCPUMins,QOS,DefaultQOS");
		else
			user_cond->assoc_cond->flags |=
				ASSOC_COND_FLAG_ONLY_DEFS;
	}
	/* If we are getting associations we want to disable only defs */
	if (user_cond->with_assocs) {
		user_cond->assoc_cond->flags &= ~ASSOC_COND_FLAG_ONLY_DEFS;
		user_cond->with_wckeys = 1;
	}

	if (!user_cond->with_assocs && (cond_set & SA_SET_ASSOC)) {
		if (!commit_check("You requested options that are only valid "
				 "when querying with the withassoc option.\n"
				 "Are you sure you want to continue?")) {
			printf("Aborted\n");
			FREE_NULL_LIST(format_list);
			slurmdb_destroy_user_cond(user_cond);
			return SLURM_SUCCESS;
		}
	}

	print_fields_list = sacctmgr_process_format_list(format_list);
	FREE_NULL_LIST(format_list);

	if (exit_code) {
		slurmdb_destroy_user_cond(user_cond);
		FREE_NULL_LIST(print_fields_list);
		return SLURM_ERROR;
	}

	user_list = slurmdb_users_get(db_conn, user_cond);
	slurmdb_destroy_user_cond(user_cond);

	if (mime_type) {
		DATA_DUMP_CLI_SINGLE(OPENAPI_USERS_RESP, user_list, argc, argv,
				     db_conn, mime_type, data_parser, rc);
		FREE_NULL_LIST(print_fields_list);
		FREE_NULL_LIST(user_list);
		return rc;
	}

	if (!user_list) {
		exit_code=1;
		fprintf(stderr, " Problem with query.\n");
		FREE_NULL_LIST(print_fields_list);
		return SLURM_ERROR;
	}

	itr = list_iterator_create(user_list);
	itr2 = list_iterator_create(print_fields_list);
	print_fields_header(print_fields_list);

	field_count = list_count(print_fields_list);

	while((user = list_next(itr))) {
		char *tmp_char = NULL;
		uint32_t tmp_uint32;
		if (user->assoc_list) {
			char *curr_cluster = NULL;
			list_itr_t *itr3 =
				list_iterator_create(user->assoc_list);
			list_itr_t *itr4 =
				list_iterator_create(user->assoc_list);
			list_itr_t *wckey_itr = NULL;
			if (user->wckey_list)
				wckey_itr =
					list_iterator_create(user->wckey_list);
			while((assoc = list_next(itr3))) {
				int curr_inx = 1;

				/* get the defaults */
				if (!curr_cluster
				    || xstrcmp(curr_cluster, assoc->cluster)) {
					slurmdb_assoc_rec_t *assoc2;
					/* We shouldn't have to reset this
					 * unless no default is on the
					 * cluster. */
					while ((assoc2 = list_next(itr4))) {
						if (!assoc2->is_def ||
						    xstrcmp(assoc->cluster,
							    assoc2->cluster))
							continue;
						curr_cluster = assoc2->cluster;
						xfree(user->default_acct);
						user->default_acct =
							xstrdup(assoc2->acct);
						break;
					}

					/* This means there wasn't a
					   default on the current cluster.
					*/
					if (!assoc2)
						list_iterator_reset(itr4);
					if (curr_cluster && wckey_itr) {
						slurmdb_wckey_rec_t *wckey;
						/* We shouldn't have
						 * to reset this
						 * unless no default
						 * is on the cluster. */
						while ((wckey =
							list_next(wckey_itr))) {
							if (!wckey->is_def ||
							    xstrcmp(curr_cluster,
								    wckey->
								    cluster))
								continue;

							xfree(user->
							      default_wckey);
							user->default_wckey =
								xstrdup(wckey->
									name);
							break;
						}

						/* This means there wasn't a
						   default on the
						   current cluster.
						*/
						if (!wckey)
							list_iterator_reset(
								wckey_itr);
					}
				}

				while((field = list_next(itr2))) {
					switch(field->type) {
					case PRINT_ADMIN:
						tmp_char =
							slurmdb_admin_level_str(
								user->
								admin_level);
						field->print_routine(
							field,
							tmp_char,
							(curr_inx ==
							 field_count));
						break;
					case PRINT_COORDS:
						field->print_routine(
							field,
							&user->coord_accts,
							(curr_inx ==
							 field_count));
						break;
					case PRINT_DACCT:
						field->print_routine(
							field,
							user->default_acct,
							(curr_inx ==
							 field_count));
						break;
					case PRINT_DWCKEY:
						field->print_routine(
							field,
							user->default_wckey,
							(curr_inx ==
							 field_count));
						break;
					default:
						sacctmgr_print_assoc_rec(
							assoc, field, NULL,
							(curr_inx ==
							 field_count));
						break;
					}
					curr_inx++;
				}
				list_iterator_reset(itr2);
				printf("\n");
			}
			list_iterator_destroy(itr3);
		} else {
			int curr_inx = 1;
			while((field = list_next(itr2))) {
				switch(field->type) {
				case PRINT_QOS:
					tmp_char = get_qos_complete_str(NULL,
									NULL);
					field->print_routine(
						field,
						tmp_char,
						(curr_inx == field_count));
					xfree(tmp_char);
					break;
				case PRINT_ADMIN:
					tmp_char = slurmdb_admin_level_str(
							user->admin_level);
					field->print_routine(
						field,
						tmp_char,
						(curr_inx == field_count));
					break;
				case PRINT_COORDS:
					field->print_routine(
						field,
						&user->coord_accts,
						(curr_inx == field_count));
					break;
				case PRINT_DACCT:
					field->print_routine(
						field,
						user->default_acct,
						(curr_inx == field_count));
					break;
				case PRINT_DWCKEY:
					field->print_routine(
						field,
						user->default_wckey,
						(curr_inx == field_count));
					break;
				case PRINT_USER:
					field->print_routine(
						field,
						user->name,
						(curr_inx == field_count));
					break;
				case PRINT_PRIO:
					tmp_uint32 = INFINITE;
					field->print_routine(
						field,
						&tmp_uint32,
						(curr_inx == field_count));
					break;
				default:
					field->print_routine(
						field, NULL,
						(curr_inx == field_count));
					break;
				}
				curr_inx++;
			}
			list_iterator_reset(itr2);
			printf("\n");
		}
	}

	list_iterator_destroy(itr2);
	list_iterator_destroy(itr);
	FREE_NULL_LIST(user_list);
	FREE_NULL_LIST(print_fields_list);

	return rc;
}

extern int sacctmgr_modify_user(int argc, char **argv)
{
	int rc = SLURM_SUCCESS;
	slurmdb_user_cond_t *user_cond = xmalloc(sizeof(slurmdb_user_cond_t));
	slurmdb_user_rec_t *user = xmalloc(sizeof(slurmdb_user_rec_t));
	slurmdb_assoc_rec_t *assoc =
		xmalloc(sizeof(slurmdb_assoc_rec_t));
	int i=0;
	int cond_set = 0, prev_set = 0, rec_set = 0, set = 0;
	list_t *ret_list = NULL;

	slurmdb_init_assoc_rec(assoc, 0);

	user_cond->assoc_cond = xmalloc(sizeof(slurmdb_assoc_cond_t));
	user_cond->assoc_cond->cluster_list = list_create(xfree_ptr);
	/*
	 * We need this to make sure we only change users, not
	 * accounts if this list didn't exist it would change
	 * accounts. Having it blank is fine, it just needs to
	 * exist.  This also happens in _set_cond, but that doesn't
	 * always happen.
	 */
	user_cond->assoc_cond->user_list = list_create(xfree_ptr);

	for (i = 0; i < argc; i++) {
		int command_len = strlen(argv[i]);
		if (!xstrncasecmp(argv[i], "Where", MAX(command_len, 5))) {
			i++;
			prev_set = _set_cond(&i, argc, argv, user_cond,
					     NULL, NULL);
			cond_set |= prev_set;
		} else if (!xstrncasecmp(argv[i], "Set", MAX(command_len, 3))) {
			i++;
			prev_set = _set_rec(&i, argc, argv, user, assoc);
			rec_set |= prev_set;
		} else {
			prev_set = _set_cond(&i, argc, argv, user_cond,
					     NULL, NULL);
			cond_set |= prev_set;
		}
	}

	if (exit_code) {
		slurmdb_destroy_user_cond(user_cond);
		slurmdb_destroy_user_rec(user);
		slurmdb_destroy_assoc_rec(assoc);
		return SLURM_ERROR;
	} else if (!rec_set) {
		exit_code=1;
		fprintf(stderr, " You didn't give me anything to set\n");
		slurmdb_destroy_user_cond(user_cond);
		slurmdb_destroy_user_rec(user);
		slurmdb_destroy_assoc_rec(assoc);
		return SLURM_ERROR;
	} else if (!cond_set) {
		if (!commit_check("You didn't set any conditions with 'WHERE'.\n"
				 "Are you sure you want to continue?")) {
			printf("Aborted\n");
			slurmdb_destroy_user_cond(user_cond);
			slurmdb_destroy_user_rec(user);
			slurmdb_destroy_assoc_rec(assoc);
			return SLURM_SUCCESS;
		}
	}

	// Special case:  reset raw usage only
	if (assoc->usage) {
		rc = SLURM_ERROR;
		if (user_cond->assoc_cond->acct_list) {
			if (assoc->usage->usage_raw == 0.0)
				rc = sacctmgr_remove_assoc_usage(
					user_cond->assoc_cond);
			else
				error("Raw usage can only be set to 0 (zero)");
		} else {
			error("An account must be specified");
		}

		slurmdb_destroy_user_cond(user_cond);
		slurmdb_destroy_user_rec(user);
		slurmdb_destroy_assoc_rec(assoc);
		return rc;
	}

	_check_and_set_cluster_list(user_cond->assoc_cond->cluster_list);

	if (rec_set & SA_SET_USER) { // process the account changes
		if (cond_set == SA_SET_ASSOC) {
			rc = SLURM_ERROR;
			exit_code=1;
			fprintf(stderr,
				" There was a problem with your "
				"'where' options.\n");
			goto assoc_start;
		}
		notice_thread_init();
		if (user_cond->assoc_cond->acct_list
		    && list_count(user_cond->assoc_cond->acct_list)) {
			notice_thread_fini();
			if (commit_check(
				   " You specified Accounts in your "
				   "request.  Did you mean "
				   "DefaultAccounts?\n")) {
				if (!user_cond->def_acct_list)
					user_cond->def_acct_list =
						list_create(xfree_ptr);
				list_transfer(user_cond->def_acct_list,
					      user_cond->assoc_cond->acct_list);
			}
			notice_thread_init();
		}

		ret_list = slurmdb_users_modify(
			db_conn, user_cond, user);
		printf(" Modified users...\n");
		if (ret_list && list_count(ret_list)) {
			set = 1;
			if (user->default_acct && user->default_acct[0]
			    && _check_default_assocs(
				    user->default_acct, ret_list,
				    user_cond->assoc_cond->cluster_list)
			    != SLURM_SUCCESS) {
				set = 0;
			}

			if (user->default_wckey
			    && _check_default_wckeys(
				    user->default_wckey, ret_list,
				    user_cond->assoc_cond->cluster_list)
			    != SLURM_SUCCESS) {
				set = 0;
			}

			if (set) {
				char *object;
				list_itr_t *itr =
					list_iterator_create(ret_list);
				while ((object = list_next(itr))) {
					printf("  %s\n", object);
				}
				list_iterator_destroy(itr);
			}
		} else if (ret_list || errno == SLURM_NO_CHANGE_IN_DATA) {
			printf("  Nothing modified\n");
			rc = SLURM_ERROR;
		} else {
			exit_code=1;
			fprintf(stderr, " Error with request: %s\n",
				slurm_strerror(errno));
			if (errno == ESLURM_ONE_CHANGE)
				fprintf(stderr, " If you are changing a users "
					"name you can only specify 1 user "
					"at a time.\n");
			rc = SLURM_ERROR;
		}
		FREE_NULL_LIST(ret_list);
		notice_thread_fini();
		if (set) {
			if (commit_check("Would you like to commit changes?")) {
				rc = slurmdb_connection_commit(db_conn, 1);
				if (rc != SLURM_SUCCESS)
					fprintf(stderr, " Error committing changes: %s\n",
						slurm_strerror(rc));
			} else {
				printf(" Changes Discarded\n");
				rc = slurmdb_connection_commit(db_conn, 0);
				if (rc != SLURM_SUCCESS)
					fprintf(stderr, " Error rolling back changes: %s\n",
						slurm_strerror(rc));
			}
			set = 0;
		}
	}

assoc_start:
	if (rec_set & SA_SET_ASSOC) { // process the assoc changes
		if ((cond_set == SA_SET_USER) &&
		    !list_count(user_cond->assoc_cond->user_list)) {
			rc = SLURM_ERROR;
			exit_code=1;
			fprintf(stderr,
				" There was a problem with your "
				"'where' options.\n");
			goto assoc_end;
		}

		notice_thread_init();
		ret_list = slurmdb_associations_modify(
			db_conn, user_cond->assoc_cond, assoc);

		printf(" Modified user associations...\n");
		if (ret_list && list_count(ret_list)) {
			char *object = NULL;
			list_itr_t *itr = list_iterator_create(ret_list);
			while((object = list_next(itr))) {
				printf("  %s\n", object);
			}
			list_iterator_destroy(itr);
			set = 1;
		} else if (ret_list || errno == SLURM_NO_CHANGE_IN_DATA) {
			printf("  Nothing modified\n");
			rc = SLURM_ERROR;
		} else {
			exit_code=1;
			fprintf(stderr, " Error with request: %s\n",
				slurm_strerror(errno));
			rc = SLURM_ERROR;
		}

		FREE_NULL_LIST(ret_list);
		notice_thread_fini();
		if (set) {
			if (commit_check("Would you like to commit changes?")) {
				rc = slurmdb_connection_commit(db_conn, 1);
				if (rc != SLURM_SUCCESS)
					fprintf(stderr, " Error committing changes: %s\n",
						slurm_strerror(rc));
			} else {
				printf(" Changes Discarded\n");
				rc = slurmdb_connection_commit(db_conn, 0);
				if (rc != SLURM_SUCCESS)
					fprintf(stderr, " Error rolling back changes: %s\n",
						slurm_strerror(rc));
			}
		}
	}
assoc_end:

	slurmdb_destroy_user_cond(user_cond);
	slurmdb_destroy_user_rec(user);
	slurmdb_destroy_assoc_rec(assoc);

	return rc;
}

extern int sacctmgr_delete_user(int argc, char **argv)
{
	int rc = SLURM_SUCCESS;
	slurmdb_user_cond_t *user_cond = xmalloc(sizeof(slurmdb_user_cond_t));
	slurmdb_wckey_cond_t * wckey_cond =
		xmalloc(sizeof(slurmdb_wckey_cond_t));
	int i=0;
	list_t *ret_list = NULL;
	int cond_set = 0, prev_set = 0;

	for (i=0; i<argc; i++) {
		int command_len = strlen(argv[i]);
		if (!xstrncasecmp(argv[i], "Where", MAX(command_len, 5))
		    || !xstrncasecmp(argv[i], "Set", MAX(command_len, 3)))
			i++;
		prev_set = _set_cond(&i, argc, argv, user_cond,
				     wckey_cond, NULL);
		cond_set |= prev_set;
	}

	/* Since the association flag isn't set we need to change
	   things to handle things correctly.
	*/
	if (user_cond->assoc_cond) {
		if (cond_set & SA_SET_WCKEY) {
			/*
			 * You can no delete associations and wckeys at the same
			 * time, so if SA_SET_WCKEY is set we need to grab some
			 * lists that are only set up in the assoc_cond and use
			 * them in the wckey_cond.
			 */
			if (user_cond->assoc_cond->cluster_list &&
			    list_count(user_cond->assoc_cond->cluster_list)) {
				wckey_cond->cluster_list =
					user_cond->assoc_cond->cluster_list;
				user_cond->assoc_cond->cluster_list = NULL;
			}
			if (user_cond->assoc_cond->user_list &&
			    list_count(user_cond->assoc_cond->user_list)) {
				wckey_cond->user_list =
					user_cond->assoc_cond->user_list;
				user_cond->assoc_cond->user_list = NULL;
			}
		} else if (user_cond->assoc_cond->cluster_list &&
			   list_count(user_cond->assoc_cond->cluster_list)) {
			/*
			 * If not deleting wckeys specifically we need to check
			 * if we have a cluster list.  If we do we are only
			 * deleting a user from a set of clusters and not really
			 * from the whole system. If this is the case then we
			 * need to set SA_SET_ASSOC so we don't remove the user
			 * from the whole system.
			 */
			cond_set |= SA_SET_ASSOC;
		}
	}

	if (!cond_set) {
		exit_code=1;
		fprintf(stderr,
			" No conditions given to remove, not executing.\n");
		slurmdb_destroy_user_cond(user_cond);
		slurmdb_destroy_wckey_cond(wckey_cond);
		return SLURM_ERROR;
	}

	if (exit_code) {
		slurmdb_destroy_user_cond(user_cond);
		slurmdb_destroy_wckey_cond(wckey_cond);
		return SLURM_ERROR;
	}

	notice_thread_init();
	if (cond_set == SA_SET_USER) {
		ret_list = slurmdb_users_remove(
			db_conn, user_cond);
	} else if (cond_set & SA_SET_ASSOC) {
		ret_list = slurmdb_associations_remove(
			db_conn, user_cond->assoc_cond);
	} else if (cond_set & SA_SET_WCKEY) {
		ret_list = slurmdb_wckeys_remove(db_conn, wckey_cond);
	}

	rc = errno;
	notice_thread_fini();

	slurmdb_destroy_user_cond(user_cond);
	slurmdb_destroy_wckey_cond(wckey_cond);

	if (ret_list && list_count(ret_list)) {
		char *object = NULL;
		list_t *del_user_list = NULL;
		list_itr_t *itr = list_iterator_create(ret_list);
		/* If there were jobs running with an association to
		   be deleted, don't.
		*/
		if (rc == ESLURM_JOBS_RUNNING_ON_ASSOC) {
			fprintf(stderr, " Error with request: %s\n",
				slurm_strerror(rc));
			while((object = list_next(itr))) {
				fprintf(stderr,"  %s\n", object);
			}
			list_iterator_destroy(itr);
			FREE_NULL_LIST(ret_list);
			slurmdb_connection_commit(db_conn, 0);
			return rc;
		}

		if (rc == ESLURM_NO_REMOVE_DEFAULT_ACCOUNT) {
			fprintf(stderr, " Error with request: %s\n",
				slurm_strerror(rc));

			while((object = list_next(itr))) {
				fprintf(stderr,"  %s\n", object);
			}
			fprintf(stderr,
				" You must change the default "
				"account of these users or "
				"remove the users completely "
				"from the affected clusters "
				"to allow these changes.\n"
				" Changes Discarded\n");
			list_iterator_destroy(itr);
			FREE_NULL_LIST(ret_list);
			slurmdb_connection_commit(db_conn, 0);
			return rc;
		}

		if (cond_set == SA_SET_USER) {
			printf(" Deleting users...\n");
		} else if (cond_set & SA_SET_ASSOC) {
			printf(" Deleting user associations...\n");
		} else if (cond_set & SA_SET_WCKEY) {
			printf(" Deleting user WCKeys...\n");
		}
		while((object = list_next(itr))) {
			printf("  %s\n", object);
			if (cond_set & SA_SET_ASSOC) {
				static const char *needle = "U = ";
				int i = 0;
				char *tmp;

				if (!(tmp = strstr(object, needle))) {
					error("Missing \"%s\" from \"%s\". Database is possibly corrupted.",
					      needle, object);
					list_iterator_destroy(itr);
					FREE_NULL_LIST(del_user_list);
					rc = SLURM_ERROR;
					goto end_it;
				}
				tmp += 4; /* strlen(needle) */

				/* If the association has a partition on
				 * it we need to get only the name portion, so
				 * break on the first non alphanum char.
				 */
				while (tmp[i]) {
					if (!(tmp[i] >= '0' && tmp[i] <= '9') &&
					    !(tmp[i] >= 'a' && tmp[i] <= 'z') &&
					    !(tmp[i] >= 'A' && tmp[i] <= 'Z') &&
					    tmp[i] != '_' && tmp[i] != '.' &&
					    tmp[i] != '-' && tmp[i] != '@') {
						tmp[i] = '\0';
						break;
					}
					i++;
				}

				if (!del_user_list)
					del_user_list = list_create(xfree_ptr);
				slurm_addto_char_list_with_case(del_user_list,
								tmp,
								user_case_norm);
			}
		}
		list_iterator_destroy(itr);

		/* Remove user if no associations left. */
		if ((cond_set & SA_SET_ASSOC) && del_user_list) {
			list_t *user_list = NULL;
			slurmdb_user_cond_t del_user_cond;
			slurmdb_assoc_cond_t del_user_assoc_cond;
			slurmdb_user_rec_t *user = NULL;
			/* Use a fresh cond here so we check all
			   clusters and such to make sure there are no
			   associations.
			*/
			memset(&del_user_cond, 0, sizeof(slurmdb_user_cond_t));
			memset(&del_user_assoc_cond, 0,
			       sizeof(slurmdb_assoc_cond_t));
			del_user_cond.with_assocs = 1;
			del_user_assoc_cond.user_list = del_user_list;
			/* No need to get all the extra info about the
			   association, just want to know if it
			   exists.
			*/
			del_user_assoc_cond.flags = ASSOC_COND_FLAG_WOPI;
			del_user_cond.assoc_cond = &del_user_assoc_cond;
			user_list = slurmdb_users_get(db_conn, &del_user_cond);
			FREE_NULL_LIST(del_user_list);
			del_user_list = NULL;

			if (user_list) {
				itr = list_iterator_create(user_list);
				while ((user = list_next(itr))) {
					if (user->assoc_list) {
						continue;
					}
					if (!del_user_list) {
						del_user_list = list_create(
							xfree_ptr);
						printf(" Deleting users "
						       "(No Associations)"
						       "...\n");
					}
					printf("  %s\n", user->name);
					slurm_addto_char_list_with_case(del_user_list,
									user->name,
									user_case_norm);
				}
				list_iterator_destroy(itr);
				FREE_NULL_LIST(user_list);


			}

			if (del_user_list) {
				list_t *del_user_ret_list = NULL;

				memset(&del_user_cond, 0,
				       sizeof(slurmdb_user_cond_t));
				memset(&del_user_assoc_cond, 0,
				       sizeof(slurmdb_assoc_cond_t));

				del_user_assoc_cond.user_list = del_user_list;
				del_user_cond.assoc_cond = &del_user_assoc_cond;

				del_user_ret_list = slurmdb_users_remove(
					db_conn, &del_user_cond);
				rc = errno;
				FREE_NULL_LIST(del_user_ret_list);
				FREE_NULL_LIST(del_user_list);
				if (rc) {
					exit_code = 1;
					fprintf(stderr,
						" Error with request: %s.\n"
						" Changes Discarded\n",
						slurm_strerror(rc));
					rc = SLURM_ERROR;
					goto end_it;
				}
			}
		}

		if (commit_check("Would you like to commit changes?")) {
			rc = slurmdb_connection_commit(db_conn, 1);
			if (rc != SLURM_SUCCESS)
				fprintf(stderr, " Error committing changes: %s\n",
					slurm_strerror(rc));
		} else {
			printf(" Changes Discarded\n");
			rc = slurmdb_connection_commit(db_conn, 0);
			if (rc != SLURM_SUCCESS)
				fprintf(stderr, " Error rolling back changes: %s\n",
					slurm_strerror(rc));
		}
	} else if (ret_list) {
		printf(" Nothing deleted\n");
		rc = SLURM_ERROR;
	} else {
		exit_code=1;
		fprintf(stderr, " Error with request: %s\n",
			slurm_strerror(rc));
		rc = SLURM_ERROR;
	}
end_it:
	FREE_NULL_LIST(ret_list);

	return rc;
}

extern int sacctmgr_delete_coord(int argc, char **argv)
{
	int rc = SLURM_SUCCESS;
	int i=0, set=0;
	int cond_set = 0, prev_set = 0;
	slurmdb_user_cond_t *user_cond = xmalloc(sizeof(slurmdb_user_cond_t));
	char *name = NULL;
	char *user_str = NULL;
	char *acct_str = NULL;
	list_itr_t *itr = NULL;
	list_t *ret_list = NULL;

	for (i=0; i<argc; i++) {
		int command_len = strlen(argv[i]);
		if (!xstrncasecmp(argv[i], "Where", MAX(command_len, 5))
		    || !xstrncasecmp(argv[i], "Set", MAX(command_len, 3)))
			i++;
		prev_set = _set_cond(&i, argc, argv, user_cond, NULL, NULL);
		cond_set |= prev_set;
	}

	if (exit_code) {
		slurmdb_destroy_user_cond(user_cond);
		return SLURM_ERROR;
	} else if (!cond_set) {
		exit_code=1;
		fprintf(stderr, " You need to specify a user list "
			"or account list here.\n");
		slurmdb_destroy_user_cond(user_cond);
		return SLURM_ERROR;
	}
	if ((_check_coord_request(user_cond, false) == SLURM_ERROR)
	   || exit_code) {
		slurmdb_destroy_user_cond(user_cond);
		return SLURM_ERROR;
	}

	if (user_cond->assoc_cond->user_list) {
		itr = list_iterator_create(user_cond->assoc_cond->user_list);
		while((name = list_next(itr))) {
			xstrfmtcat(user_str, "  %s\n", name);

		}
		list_iterator_destroy(itr);
	}

	if (user_cond->assoc_cond->acct_list) {
		itr = list_iterator_create(user_cond->assoc_cond->acct_list);
		while((name = list_next(itr))) {
			xstrfmtcat(acct_str, "  %s\n", name);

		}
		list_iterator_destroy(itr);
	}

	if (!user_str && !acct_str) {
		exit_code=1;
		fprintf(stderr, " You need to specify a user list "
			"or an account list here.\n");
		slurmdb_destroy_user_cond(user_cond);
		return SLURM_ERROR;
	}
	/* FIX ME: This list should be received from the slurmdbd not
	 * just assumed.  Right now it doesn't do it correctly though.
	 * This is why we are doing it this way.
	 */
	if (user_str) {
		printf(" Removing Coordinators with user name\n%s", user_str);
		if (acct_str)
			printf(" From Account(s)\n%s", acct_str);
		else
			printf(" From all accounts\n");
	} else
		printf(" Removing all users from Accounts\n%s", acct_str);

	notice_thread_init();
	ret_list = slurmdb_coord_remove(db_conn,
					user_cond->assoc_cond->acct_list,
					user_cond);
	slurmdb_destroy_user_cond(user_cond);


	if (ret_list && list_count(ret_list)) {
		char *object = NULL;
		list_itr_t *itr = list_iterator_create(ret_list);
		printf(" Removed Coordinators (sub accounts not listed)...\n");
		while((object = list_next(itr))) {
			printf("  %s\n", object);
		}
		list_iterator_destroy(itr);
		set = 1;
	} else if (ret_list) {
		printf(" Nothing removed\n");
		rc = SLURM_ERROR;
	} else {
		exit_code=1;
		fprintf(stderr, " Error with request: %s\n",
			slurm_strerror(errno));
		rc = SLURM_ERROR;
	}

	FREE_NULL_LIST(ret_list);
	notice_thread_fini();
	if (set) {
		if (commit_check("Would you like to commit changes?")) {
			rc = slurmdb_connection_commit(db_conn, 1);
			if (rc != SLURM_SUCCESS)
				fprintf(stderr, " Error committing changes: %s\n",
					slurm_strerror(rc));

		} else {
			printf(" Changes Discarded\n");
			rc = slurmdb_connection_commit(db_conn, 0);
			if (rc != SLURM_SUCCESS)
				fprintf(stderr, " Error rolling back changes: %s\n",
					slurm_strerror(rc));
		}
	}

	return rc;
}
