/*****************************************************************************\
 *  update_node.c - node update function for scontrol.
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
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

#include "slurm/slurm.h"
#include "src/common/proc_args.h"
#include "src/common/slurm_resource_info.h"
#include "src/common/uid.h"

#include "src/scontrol/scontrol.h"

extern int scontrol_create_node(int argc, char **argv)
{
	update_node_msg_t node_msg;
	char *node_line = NULL;

	/* Reconstruct NodeName= line from cmd line */
	for (int i = 0; i < argc; i++)
		xstrfmtcat(node_line, "%s%s", i > 0 ? " " : "", argv[i]);

	slurm_init_update_node_msg(&node_msg);
	node_msg.extra = node_line;
	if (slurm_create_node(&node_msg)) {
		exit_code = 1;
		slurm_perror("Error creating node(s)");
		return errno;
	}
	xfree(node_line);

	return SLURM_SUCCESS;
}

/*
 * scontrol_update_node - update the slurm node configuration per the supplied
 *	arguments
 * IN argc - count of arguments
 * IN argv - list of arguments
 * RET 0 if no slurm error, errno otherwise. parsing error prints
 *			error message and returns 0
 */
extern int
scontrol_update_node (int argc, char **argv)
{
	int i, j, rc = 0, update_cnt = 0;
	update_node_msg_t node_msg;
	char *reason_str = NULL;
	char *tag, *val;
	int tag_len, val_len;

	slurm_init_update_node_msg(&node_msg);
	for (i = 0; i < argc; i++) {
		tag = argv[i];
		val = strchr(argv[i], '=');
		if (val) {
			tag_len = val - argv[i];
			val++;
			val_len = strlen(val);
		} else {
			exit_code = 1;
			error("Invalid input: %s  Request aborted", argv[i]);
			return -1;
		}

		if (xstrncasecmp(tag, "NodeAddr", MAX(tag_len, 5)) == 0) {
			node_msg.node_addr = val;
			update_cnt++;
		} else if (xstrncasecmp(tag, "NodeHostName", MAX(tag_len, 5))
			   == 0) {
			node_msg.node_hostname = val;
			update_cnt++;
		} else if (xstrncasecmp(tag, "NodeName", MAX(tag_len, 1)) == 0) {
			node_msg.node_names = val;
		} else if (!xstrncasecmp(tag, "ActiveFeatures",
					 MAX(tag_len,3))) {
			node_msg.features_act = val;
			update_cnt++;
		} else if (!xstrncasecmp(tag, "CertToken", MAX(tag_len, 1))) {
			node_msg.cert_token = val;
			update_cnt++;
		} else if (xstrncasecmp(tag, "CpuBind", MAX(tag_len, 7)) == 0) {
			if (xlate_cpu_bind_str(val, &node_msg.cpu_bind) !=
			    SLURM_SUCCESS) {
				exit_code = 1;
				error("Invalid input %s", argv[i]);
				return -1;
			}
			update_cnt++;
		} else if (!xstrncasecmp(tag, "Extra", MAX(tag_len, 1))) {
			node_msg.extra = val;
			update_cnt++;
		} else if (!xstrncasecmp(tag, "Features", MAX(tag_len, 1)) ||
			   !xstrncasecmp(tag, "AvailableFeatures",
					 MAX(tag_len,3))) {
			node_msg.features = val;
			update_cnt++;
		} else if (xstrncasecmp(tag, "Gres", MAX(tag_len, 1)) == 0) {
			node_msg.gres = val;
			update_cnt++;
		} else if (!xstrncasecmp(tag, "InstanceId", MAX(tag_len, 9))) {
			node_msg.instance_id = val;
			update_cnt++;
		} else if (!xstrncasecmp(tag, "InstanceType",
					 MAX(tag_len, 9))) {
			node_msg.instance_type = val;
			update_cnt++;
		} else if (xstrncasecmp(tag, "Weight", MAX(tag_len,1)) == 0) {
			if (parse_uint32(val, &node_msg.weight)) {
				exit_code = 1;
				error("Invalid value %s for Weight", argv[i]);
				return -1;
			}
			update_cnt++;
		} else if (!xstrncasecmp(tag, "Comment", MAX(tag_len, 2))) {
			node_msg.comment = val;
			update_cnt++;
		} else if (xstrncasecmp(tag, "Reason", MAX(tag_len, 1)) == 0) {
			int len;

			if (*val == '"')
				reason_str = xstrdup(val + 1);
			else
				reason_str = xstrdup(val);

			len = strlen(reason_str) - 1;
			if ((len >= 0) && (reason_str[len] == '"'))
				reason_str[len] = '\0';

			node_msg.reason = reason_str;
			update_cnt++;
		} else if (!xstrncasecmp(tag, "ResumeAfter", MAX(tag_len, 1))) {
			if (!xstrcmp(val, "-1")) {
			    node_msg.resume_after = INFINITE;
			} else if (parse_uint32(val, &node_msg.resume_after)) {
				exit_code = 1;
				error("Invalid value %s for ResumeAfter",
				      argv[i]);
				return -1;
			}
			update_cnt++;
		} else if (xstrncasecmp(tag, "State", MAX(tag_len, 1)) == 0) {
			if (xstrncasecmp(val, "NoResp",
				        MAX(val_len, 3)) == 0) {
				node_msg.node_state = NODE_STATE_NO_RESPOND;
				update_cnt++;
			} else if (xstrncasecmp(val, "CANCEL_REBOOT",
				   MAX(val_len, 3)) == 0) {
				node_msg.node_state = NODE_STATE_REBOOT_CANCEL;
				update_cnt++;
			} else if (xstrncasecmp(val, "DRAIN",
				   MAX(val_len, 3)) == 0) {
				node_msg.node_state = NODE_STATE_DRAIN;
				update_cnt++;
			} else if (xstrncasecmp(val, "FAIL",
				   MAX(val_len, 3)) == 0) {
				node_msg.node_state = NODE_STATE_FAIL;
				update_cnt++;
			} else if (xstrncasecmp(val, "FUTURE",
				   MAX(val_len, 3)) == 0) {
				node_msg.node_state = NODE_STATE_FUTURE;
				update_cnt++;
			} else if (xstrncasecmp(val, "RESUME",
				   MAX(val_len, 3)) == 0) {
				node_msg.node_state = NODE_RESUME;
				update_cnt++;
			} else if (xstrncasecmp(val, "POWER_DOWN_ASAP",
				   MAX(val_len, 12)) == 0) {
				node_msg.node_state =
					NODE_STATE_POWER_DOWN |
					NODE_STATE_POWER_DRAIN;
				update_cnt++;
			} else if (xstrncasecmp(val, "POWER_DOWN_FORCE",
				   MAX(val_len, 12)) == 0) {
				node_msg.node_state =
					NODE_STATE_POWER_DOWN |
					NODE_STATE_POWERED_DOWN;
				update_cnt++;
			} else if (xstrncasecmp(val, "POWER_DOWN",
				   MAX(val_len, 7)) == 0) {
				node_msg.node_state = NODE_STATE_POWER_DOWN;
				update_cnt++;
			} else if (xstrncasecmp(val, "POWER_UP",
				   MAX(val_len, 7)) == 0) {
				node_msg.node_state = NODE_STATE_POWER_UP;
				update_cnt++;
			} else if (xstrncasecmp(val, "UNDRAIN",
				   MAX(val_len, 3)) == 0) {
				node_msg.node_state = NODE_STATE_UNDRAIN;
				update_cnt++;
			} else {
				uint32_t state_val = NO_VAL;
				for (j = 0; j < NODE_STATE_END; j++) {
					if (xstrncasecmp(node_state_string(j),
							 val,
							 MAX(val_len, 3)) == 0){
						state_val = (uint32_t) j;
						break;
					}
				}
				if (j == NODE_STATE_END) {
					exit_code = 1;
					fprintf(stderr, "Invalid input: %s\n",
						argv[i]);
					fprintf (stderr, "Request aborted\n");
					fprintf (stderr, "Valid states are: ");
					fprintf (stderr,
						 "NoResp DRAIN FAIL FUTURE RESUME "
						 "POWER_DOWN POWER_UP UNDRAIN");
					fprintf (stderr, "\n");
					fprintf (stderr,
						 "Not all states are valid "
						 "given a node's prior "
						 "state\n");
					goto done;
				}
				node_msg.node_state = state_val;
				update_cnt++;
			}
		} else if (!xstrncasecmp(tag, "Topology", MAX(tag_len, 4))) {
			node_msg.topology_str = val;
			update_cnt++;
		} else {
			exit_code = 1;
			fprintf (stderr, "Update of this parameter is not "
				 "supported: %s\n", argv[i]);
			fprintf (stderr, "Request aborted\n");
			goto done;
		}
	}

	if (((node_msg.node_state == NODE_STATE_DOWN)  ||
	     (node_msg.node_state == NODE_STATE_DRAIN) ||
	     (node_msg.node_state == NODE_STATE_FAIL)) &&
	    ((node_msg.reason == NULL) || (strlen(node_msg.reason) == 0))) {
		exit_code = 1;
		fprintf(stderr, "You must specify a reason when DOWNING or "
			"DRAINING a node. Request denied\n");
		goto done;
	}

	if ((node_msg.resume_after != NO_VAL) &&
	    (node_msg.node_state != NODE_STATE_DOWN) &&
	    (node_msg.node_state != NODE_STATE_DRAIN)) {
		exit_code = 1;
		fprintf(stderr, "You can only specify a resume time when DOWNING or DRAINING a node. Request denied\n");
		goto done;
	}

	if (update_cnt == 0) {
		exit_code = 1;
		fprintf (stderr, "No changes specified\n");
		return 0;
	}

	rc = slurm_update_node(&node_msg);

done:	xfree(reason_str);
	if (rc) {
		exit_code = 1;
		return errno;
	} else
		return 0;
}
