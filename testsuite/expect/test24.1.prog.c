/*****************************************************************************\
 *  test24.1.prog.c - link and test algo of the multifactor plugin.
 *
 *  Usage: test24.1.prog
 *****************************************************************************
 *  Copyright (C) 2009 Lawrence Livermore National Security.
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
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <inttypes.h>
#include <strings.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <slurm/slurm.h>
#include <slurm/slurm_errno.h>

#include "src/interfaces/priority.h"
#include "src/common/assoc_mgr.h"
#include "src/common/xstring.h"
#include "src/common/log.h"
#include "src/sshare/sshare.h"

/* set up some fake system */
void *acct_db_conn = NULL;
uint32_t cluster_cpus = 50;
int exit_code = 0;
sshare_time_format_t time_format = SSHARE_TIME_MINS;
char *time_format_string = "Minutes";
time_t last_job_update = (time_t) 0;
uint16_t running_cache = RUNNING_CACHE_STATE_NOTRUNNING;

list_t *job_list = NULL;		/* job_record list */
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

/* this will leak memory, but we don't care really */
static void _list_delete_job(void *job_entry)
{
	job_record_t *job_ptr = (job_record_t *) job_entry;

	xfree(job_ptr);
}

int _setup_assoc_list(void)
{
	slurmdb_update_object_t update;
	slurmdb_assoc_rec_t *assoc = NULL;
	slurmdb_tres_rec_t *tres = NULL;
	assoc_init_args_t assoc_init_arg;

	/* make the main list */
	assoc_mgr_assoc_list =
		list_create(slurmdb_destroy_assoc_rec);
	assoc_mgr_user_list =
		list_create(slurmdb_destroy_user_rec);
	assoc_mgr_qos_list =
		list_create(slurmdb_destroy_qos_rec);

	/* we just want make it so we setup_children so just pretend
	 * we are running off cache */
	memset(&assoc_init_arg, 0, sizeof(assoc_init_args_t));
	assoc_init_arg.running_cache = &running_cache;
	running_cache = RUNNING_CACHE_STATE_RUNNING;
	assoc_mgr_init(NULL, &assoc_init_arg, SLURM_SUCCESS);

	/* Here we make the tres we want to add to the system.
	 * We do this as an update to avoid having to do setup. */
	memset(&update, 0, sizeof(slurmdb_update_object_t));
	update.type = SLURMDB_ADD_TRES;
	update.objects = list_create(slurmdb_destroy_tres_rec);

	tres = xmalloc(sizeof(slurmdb_tres_rec_t));
	tres->id = 1;
	tres->type = xstrdup("cpu");
	list_append(update.objects, tres);

	if (assoc_mgr_update_tres(&update, false))
		error("assoc_mgr_update_tres: %m");
	FREE_NULL_LIST(update.objects);

	/* Here we make the associations we want to add to the system.
	 * We do this as an update to avoid having to do setup. */
	update.type = SLURMDB_ADD_ASSOC;
	update.objects = list_create(slurmdb_destroy_assoc_rec);

	/* First only add the accounts */
	/* root association */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 1;
	assoc->acct = xstrdup("root");
	list_append(update.objects, assoc);

	/* sub of root id 1 */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 2;
	assoc->parent_id = 1;
	assoc->shares_raw = 40;
	assoc->acct = xstrdup("AccountA");
	list_append(update.objects, assoc);

	/* sub of AccountA id 2 */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 21;
	assoc->parent_id = 2;
	assoc->shares_raw = 30;
	assoc->acct = xstrdup("AccountB");
	list_append(update.objects, assoc);

	/* sub of AccountB id 21 */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 211;
	assoc->parent_id = 21;
	assoc->shares_raw = 1;
	assoc->usage->usage_raw = 20;
	assoc->acct = xstrdup("AccountB");
	assoc->user = xstrdup("User1");
	list_append(update.objects, assoc);

	/* sub of AccountA id 2 */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 22;
	assoc->parent_id = 2;
	assoc->shares_raw = 10;
	assoc->acct = xstrdup("AccountC");
	list_append(update.objects, assoc);

	/* sub of AccountC id 22 */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 221;
	assoc->parent_id = 22;
	assoc->shares_raw = 1;
	assoc->usage->usage_raw = 25;
	assoc->acct = xstrdup("AccountC");
	assoc->user = xstrdup("User2");
	list_append(update.objects, assoc);

	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 222;
	assoc->parent_id = 22;
	assoc->shares_raw = 1;
	assoc->usage->usage_raw = 0;
	assoc->acct = xstrdup("AccountC");
	assoc->user = xstrdup("User3");
	list_append(update.objects, assoc);

	/* sub of root id 1 */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 3;
	assoc->parent_id = 1;
	assoc->shares_raw = 60;
	assoc->acct = xstrdup("AccountD");
	list_append(update.objects, assoc);

	/* sub of AccountD id 3 */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 31;
	assoc->parent_id = 3;
	assoc->shares_raw = 25;
	assoc->acct = xstrdup("AccountE");
	list_append(update.objects, assoc);

	/* sub of AccountE id 31 */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 311;
	assoc->parent_id = 31;
	assoc->shares_raw = 1;
	assoc->usage->usage_raw = 25;
	assoc->acct = xstrdup("AccountE");
	assoc->user = xstrdup("User4");
	list_append(update.objects, assoc);

	/* sub of AccountD id 3 */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 32;
	assoc->parent_id = 3;
	assoc->shares_raw = 35;
	assoc->acct = xstrdup("AccountF");
	list_append(update.objects, assoc);

	/* sub of AccountF id 32 */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 321;
	assoc->parent_id = 32;
	assoc->shares_raw = 1;
	assoc->usage->usage_raw = 0;
	assoc->acct = xstrdup("AccountF");
	assoc->user = xstrdup("User5");
	list_append(update.objects, assoc);

	/* sub of root id 1 */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 4;
	assoc->parent_id = 1;
	assoc->shares_raw = 0;
	assoc->acct = xstrdup("AccountG");
	list_append(update.objects, assoc);

	/* sub of AccountG id 4 */
	assoc = xmalloc(sizeof(slurmdb_assoc_rec_t));
	assoc->cluster = xstrdup(slurm_conf.cluster_name);
	assoc->usage = slurmdb_create_assoc_usage(g_tres_count);
	assoc->id = 41;
	assoc->parent_id = 4;
	assoc->shares_raw = 0;
	assoc->usage->usage_raw = 30;
	assoc->acct = xstrdup("AccountG");
	assoc->user = xstrdup("User6");
	list_append(update.objects, assoc);

	if (assoc_mgr_update_assocs(&update, false))
		error("assoc_mgr_update_assocs: %m");
	list_destroy(update.objects);

	return SLURM_SUCCESS;
}

int main (int argc, char **argv)
{
	log_options_t logopt = LOG_OPTS_STDERR_ONLY;
	slurm_conf_t *conf = NULL;
	shares_response_msg_t resp;

	log_init(xbasename(argv[0]), logopt, 0, NULL);
	xfree(slurm_conf.priority_type);
	//logopt.stderr_level += 5;
	logopt.prefix_level = 1;
	log_alter(logopt, 0, NULL);
	print_fields_have_header = 0;
	print_fields_parsable_print = PRINT_FIELDS_PARSABLE_ENDING;

	conf = slurm_conf_lock();
	/* force priority type to be multifactor */
	xfree(conf->priority_type);
	conf->priority_type = xstrdup("priority/multifactor");
	conf->priority_flags = 0;
	/* force accounting type to be slurmdbd (It doesn't really talk
	 * to any database, but needs this to work with fairshare
	 * calculation). */
	xfree(conf->accounting_storage_type);
	conf->accounting_storage_type = xstrdup("accounting_storage/slurmdbd");
	/* set up a known environment to test against.  Since we are
	 * only concerned about the fairshare we won't look at the other
	 * factors here. */
	conf->priority_decay_hl = 1;
	conf->priority_favor_small = 0;
	conf->priority_max_age = conf->priority_decay_hl;
	conf->priority_reset_period = 0;
	conf->priority_weight_age = 0;
	conf->priority_weight_fs = 10000;
	conf->priority_weight_js = 0;
	conf->priority_weight_part = 0;
	conf->priority_weight_qos = 0;
	xfree(conf->priority_weight_tres);
	slurm_conf_unlock();

	/* we don't want to do any decay here so make the save state
	 * to /dev/null */
	xfree(slurm_conf.state_save_location);
	slurm_conf.state_save_location = "/dev/null";
	/* now set up the association tree */
	_setup_assoc_list();
	/* now set up the job list */
	job_list = list_create(_list_delete_job);

	/* now init the priorities of the associations */
	if (priority_g_init() != SLURM_SUCCESS)
		fatal("failed to initialize priority plugin");
	priority_g_thread_start();
	/* on some systems that don't have multiple cores we need to
	 * sleep to make sure the thread gets started. */
	sleep(1);
	memset(&resp, 0, sizeof(shares_response_msg_t));
	assoc_mgr_get_shares(NULL, 0, NULL, &resp);

	/*
	 * This is the global var from sshare.h to tell we want the long format
	 */
	long_flag = 1;

	process(&resp, 0);

	/* free memory */
	if (priority_g_fini() != SLURM_SUCCESS)
		fatal("failed to finalize priority plugin");
	if (job_list)
		list_destroy(job_list);
	if (resp.assoc_shares_list)
		list_destroy(resp.assoc_shares_list);
	if (assoc_mgr_assoc_list)
		list_destroy(assoc_mgr_assoc_list);
	if (assoc_mgr_qos_list)
		list_destroy(assoc_mgr_qos_list);
	return 0;
}
