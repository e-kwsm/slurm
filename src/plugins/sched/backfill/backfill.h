/*****************************************************************************\
 *  backfill.h - header for simple backfill scheduler plugin.
 *****************************************************************************
 *  Copyright (C) 2003-2007 The Regents of the University of California.
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


#ifndef _SLURM_BACKFILL_H
#define _SLURM_BACKFILL_H

#include "src/slurmctld/licenses.h"

typedef struct {
	time_t begin_time;
	time_t end_time;
	bitstr_t *avail_bitmap;
	bf_licenses_t *licenses;
	uint32_t fragmentation;
	int next; /* next record, by time, zero termination */
} node_space_map_t;

/* backfill_agent - detached thread periodically attempts to backfill jobs */
extern void *backfill_agent(void *args);

/* Terminate backfill_agent */
extern void stop_backfill_agent(void);

/* Note that slurm.conf has changed */
extern void backfill_reconfig(void);

/* Used for testsuite to call backfill */
extern void __attempt_backfill(void);

#endif	/* _SLURM_BACKFILL_H */
