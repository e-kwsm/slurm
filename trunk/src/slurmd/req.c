/*****************************************************************************\
 * src/slurmd/req.c - slurmd request handling
 * $Id$
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <mgrondona@llnl.gov>.
 *  UCRL-CODE-2002-040.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/
#if HAVE_CONFIG_H
#  include "config.h"
#endif

#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/param.h>

#include "src/common/credential_utils.h"
#include "src/common/log.h"
#include "src/common/slurm_auth.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"

#include "src/slurmd/slurmd.h"
#include "src/slurmd/shm.h"
#include "src/slurmd/mgr.h"

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN	64
#endif

static void _rpc_launch_tasks(slurm_msg_t *, slurm_addr *);
static void _rpc_batch_job(slurm_msg_t *, slurm_addr *);
static void _rpc_kill_tasks(slurm_msg_t *, slurm_addr *);
static void _rpc_reattach_tasks(slurm_msg_t *, slurm_addr *);
static void _rpc_revoke_credential(slurm_msg_t *, slurm_addr *);
static void _rpc_shutdown(slurm_msg_t *msg, slurm_addr *cli_addr);
static int  _rpc_ping(slurm_msg_t *, slurm_addr *);
static int  _launch_tasks(launch_tasks_request_msg_t *, slurm_addr *);

void
slurmd_req(slurm_msg_t *msg, slurm_addr *cli)
{
	switch(msg->msg_type) {
	case REQUEST_BATCH_JOB_LAUNCH:
		_rpc_batch_job(msg, cli);
		slurm_free_job_launch_msg(msg->data);
		break;
	case REQUEST_LAUNCH_TASKS:
		_rpc_launch_tasks(msg, cli);
		slurm_free_launch_tasks_request_msg(msg->data);
		break;
	case REQUEST_KILL_TASKS:
		_rpc_kill_tasks(msg, cli);
		slurm_free_kill_tasks_msg(msg->data);
		break;
	case REQUEST_REATTACH_TASKS:
		_rpc_reattach_tasks(msg, cli);
		slurm_free_reattach_tasks_request_msg(msg->data);
		break;
	case REQUEST_REVOKE_JOB_CREDENTIAL:
		_rpc_revoke_credential(msg, cli);
		slurm_free_revoke_credential_msg(msg->data);
		break;
	case REQUEST_SHUTDOWN:
	case REQUEST_SHUTDOWN_IMMEDIATE:
		_rpc_shutdown(msg, cli);
		slurm_free_shutdown_msg(msg->data);
		break;
	case REQUEST_NODE_REGISTRATION_STATUS:
		/* Treat as ping (for slurmctld agent) */
		if (_rpc_ping(msg, cli) == SLURM_SUCCESS) {
			/* Then initiate a separate node registration */
			slurm_free_node_registration_status_msg(msg->data);
			send_registration_msg();
		}
		break;
	case REQUEST_PING:
		_rpc_ping(msg, cli);
		break;
	default:
		error("slurmd_req: invalid request msg type %d\n",
				msg->msg_type);
		slurm_send_rc_msg(msg, EINVAL);
		break;
	}
	slurm_free_msg(msg);
	return;
}

static int
_launch_batch_job(batch_job_launch_msg_t *req, slurm_addr *cli)
{	
	pid_t pid;
	int rc;

	
	switch ((pid = fork())) {
		case -1:
			error("launch_tasks: fork: %m");
			return SLURM_ERROR;
			break;
		case 0: /* child runs job */
			slurm_shutdown_msg_engine(conf->lfd);
			list_destroy(conf->threads);
			destroy_credential_state_list(conf->cred_state_list);
			slurm_destroy_ssl_key_ctx(&conf->vctx);
			slurm_ssl_destroy();
			rc = mgr_launch_batch_job(req, cli);
			exit(rc);
			/* NOTREACHED */
			break;
		default:
			debug("created process %ld for job %d",
					pid, req->job_id);
			break;
	}

	return SLURM_SUCCESS;

}

static int
_launch_tasks(launch_tasks_request_msg_t *req, slurm_addr *cli)
{
	pid_t pid;
	int rc;

	switch ((pid = fork())) {
		case -1:
			error("launch_tasks: fork: %m");
			return SLURM_ERROR;
			break;
		case 0: /* child runs job */
			slurm_shutdown_msg_engine(conf->lfd);
			list_destroy(conf->threads);
			destroy_credential_state_list(conf->cred_state_list);
			slurm_destroy_ssl_key_ctx(&conf->vctx);
			slurm_ssl_destroy();
			rc = mgr_launch_tasks(req, cli);
			exit(rc);
			/* NOTREACHED */
			break;
		default:
			debug("created process %ld for job %d.%d",
					pid, req->job_id, req->job_step_id);
			break;
	}

	return SLURM_SUCCESS;
}
				                                            

static void 
_rpc_launch_tasks(slurm_msg_t *msg, slurm_addr *cli)
{
	int      rc;
	uint16_t port;
	char     host[MAXHOSTNAMELEN];
	uid_t    req_uid;
	bool     super_user = false;
	launch_tasks_request_msg_t *req = msg->data;

	req_uid = slurm_auth_uid(msg->cred);
	if ((req_uid == conf->slurm_user_id) || (req_uid == 0))
		super_user = true;
	if ((super_user == false) && (req_uid != req->uid)) {
		error("Security violation, launch task RCP from uid %u",
		      (unsigned int) req_uid);
		rc = ESLURM_USER_ID_MISSING;	/* or invalid user */
		slurm_send_rc_msg(msg, rc);
		return;
	}

	slurm_get_addr(cli, &port, host, sizeof(host));
	info("launch task %u.%u request from %ld@%s", req->job_id, 
	     req->job_step_id, req->uid, host);

	rc = verify_credential(&conf->vctx, 
			       req->credential, 
			       conf->cred_state_list);

	if ((rc != SLURM_SUCCESS) && (super_user == false)) {
		error("Invalid credential from %ld@%s", req_uid, host);
		slurm_send_rc_msg(msg, rc);
		return;
	}

	if ((rc = _launch_tasks(req, cli)))
		slurm_send_rc_msg(msg, rc);
}


static void
_rpc_batch_job(slurm_msg_t *msg, slurm_addr *cli)
{
	batch_job_launch_msg_t *req = (batch_job_launch_msg_t *)msg->data;
	int      rc = SLURM_SUCCESS;
	uid_t    req_uid = slurm_auth_uid(msg->cred);

	if ((req_uid != conf->slurm_user_id) && (req_uid != 0)) {
		error("Security violation, batch launch RPC from uid %u",
		      (unsigned int) req_uid);
		rc = ESLURM_USER_ID_MISSING;	/* or bad in this case */
	} else {
		info("Launching batch job %u for UID %d",
			req->job_id, req->uid);
		if (_launch_batch_job(req, cli) < 0)
			rc = SLURM_FAILURE;
	}

	slurm_send_rc_msg(msg, rc);
}

static void
_rpc_shutdown(slurm_msg_t *msg, slurm_addr *cli_addr)
{
	uid_t req_uid = slurm_auth_uid(msg->cred);

	if ((req_uid != conf->slurm_user_id) && (req_uid != 0)) {
		error("Security violation, shutdown RPC from uid %u",
		      (unsigned int) req_uid);
		slurm_send_rc_msg(msg, ESLURM_USER_ID_MISSING);	/* uid bad */
	} else
		kill(conf->pid, SIGTERM);
}

static int
_rpc_ping(slurm_msg_t *msg, slurm_addr *cli_addr)
{
	int               rc = SLURM_SUCCESS;
	uid_t req_uid = slurm_auth_uid(msg->cred);

	if ((req_uid != conf->slurm_user_id) && (req_uid != 0)) {
		error("Security violation, ping RPC from uid %u",
		      (unsigned int) req_uid);
		rc = ESLURM_USER_ID_MISSING;	/* or bad in this case */
	}

	/* return result */
	slurm_send_rc_msg(msg, rc);
	return rc;
}

static void
_rpc_kill_tasks(slurm_msg_t *msg, slurm_addr *cli_addr)
{
	int               rc = SLURM_SUCCESS;
	uid_t             req_uid;
	job_step_t       *step;
	kill_tasks_msg_t *req = (kill_tasks_msg_t *) msg->data;

	if (!(step = shm_get_step(req->job_id, req->job_step_id))) {
		debug("kill for nonexistent job %d.%d requested",
				req->job_id, req->job_step_id);
		rc = EEXIST;
		goto done;
	} 

	req_uid = slurm_auth_uid(msg->cred);
	if ((req_uid != step->uid) && (req_uid != 0)) {
	       debug("kill req from uid %ld for job %d.%d owned by uid %ld",
		     req_uid, step->jobid, step->stepid, step->uid);	       
	       rc = ESLURM_USER_ID_MISSING;	/* or bad in this case */
	       goto done;
	}

	shm_free_step(step);

	rc = shm_signal_step(req->job_id, req->job_step_id, req->signal);

  done:
	slurm_send_rc_msg(msg, rc);
}

static void 
_rpc_reattach_tasks(slurm_msg_t *msg, slurm_addr *cli)
{
	int         rc   = SLURM_SUCCESS;
	uint16_t    port = 0;
	char        host[MAXHOSTNAMELEN];
	uid_t       req_uid, owner;
	gid_t       req_gid;
	slurm_addr  ioaddr;
	slurm_msg_t                    resp_msg;
	reattach_tasks_request_msg_t  *req = msg->data;
	reattach_tasks_response_msg_t  resp;

	slurm_get_addr(cli, &port, host, sizeof(host));
	req_uid = slurm_auth_uid(msg->cred);
	req_gid = slurm_auth_gid(msg->cred);

	info("reattach request from %ld@%s for %d.%d", 
	     req_uid, host, req->job_id, req->job_step_id);

	/* 
	 * Set response addr by resp_port and client address
	 */
	memcpy(&resp_msg.address, cli, sizeof(slurm_addr));
	slurm_set_addr(&resp_msg.address, req->resp_port, NULL); 

	owner = shm_get_step_owner(req->job_id, req->job_step_id);
	if (owner == (uid_t) -1) {
		rc = ESRCH;
		goto done;
	}

	if ((owner != req_uid) && (req_uid != 0)) {
		error("uid %ld attempt to attach to job %d.%d owned by %ld",
				(long) req_uid, req->job_id, req->job_step_id,
				(long) owner);
		rc = EPERM;
		goto done;
	}

	/* 
	 * Set IO and response addresses in shared memory
	 */
	memcpy(&ioaddr, cli, sizeof(slurm_addr));
	slurm_set_addr(&ioaddr, req->io_port, NULL);
	slurm_get_addr(&ioaddr, &port, host, sizeof(host));

	debug3("reattach: srun ioaddr: %s:%d", host, port);

	do {
		rc = shm_update_step_addrs( req->job_id, req->job_step_id,
				            &ioaddr, &resp_msg.address,
				            req->key ); 
	} while ((rc < 0) && (errno == EAGAIN));


    done:
	debug2("update step addrs rc = %d", rc);
	resp_msg.data         = &resp;
	resp_msg.msg_type     = RESPONSE_REATTACH_TASKS;
	resp.srun_node_id     = req->srun_node_id;
	resp.return_code      = rc;

	slurm_send_only_node_msg(&resp_msg);
}


static void
_kill_all_active_steps(uint32_t jobid)
{
	List         steps = shm_get_steps();
	ListIterator i     = list_iterator_create(steps);
	job_step_t  *s     = NULL;   

	while ((s = list_next(i))) {
		if (s->jobid == jobid) {
			/* Kill entire process group 
			 * (slurmd manager will clean up any stragglers)
			 */
			debug2("sending SIGKILL to process group %d", s->sid);
			killpg(s->sid, SIGKILL);
		}
	}
	list_iterator_destroy(i);
	list_destroy(steps);
}

static void 
_rpc_revoke_credential(slurm_msg_t *msg, slurm_addr *cli)
{
	int   rc      = SLURM_SUCCESS;
	uid_t req_uid = slurm_auth_uid(msg->cred);
	revoke_credential_msg_t *req = (revoke_credential_msg_t *) msg->data;

	if ((req_uid != conf->slurm_user_id) && (req_uid != 0)) {
		rc = ESLURM_USER_ID_MISSING;
		error("Security violation, uid %u can't revoke credentials",
		      (unsigned int) req_uid);
	} else {
		rc = revoke_credential(req, conf->cred_state_list);

		/*
		 * Now kill all steps associated with this job, they are
		 * no longer allowed to be running
		 */
		_kill_all_active_steps(req->job_id);

		if (rc < 0)
			error("revoking credential for job %d: %m", 
			      req->job_id);
		else
			debug("credential for job %d revoked", req->job_id);
	}

	slurm_send_rc_msg(msg, rc);
}
