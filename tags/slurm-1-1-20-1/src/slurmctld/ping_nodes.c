/*****************************************************************************\
 *  ping_nodes.c - ping the slurmd daemons to test if they respond
 *	Note: there is a global node table (node_record_table_ptr)
 *****************************************************************************
 *  Copyright (C) 2003-2006 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov> et. al.
 *  UCRL-CODE-217948.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
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
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifdef WITH_PTHREADS
#  include <pthread.h>
#endif

#include <time.h>
#include <string.h>

#include "src/common/hostlist.h"
#include "src/common/read_config.h"
#include "src/slurmctld/agent.h"
#include "src/slurmctld/ping_nodes.h"
#include "src/slurmctld/slurmctld.h"

/* Attempt to fork a thread at most MAX_RETRIES times before aborting */
#define MAX_RETRIES 10

/* Request that nodes re-register at most every MAX_REG_FREQUENCY pings */
#define MAX_REG_FREQUENCY 20

/* Spawn no more than MAX_REG_THREADS for node re-registration */
#define MAX_REG_THREADS   DEFAULT_TREE_WIDTH

static pthread_mutex_t lock_mutex = PTHREAD_MUTEX_INITIALIZER;
static int ping_count = 0;

/* struct timeval start_time, end_time; */

/*
 * is_ping_done - test if the last node ping cycle has completed.
 *	Use this to avoid starting a new set of ping requests before the 
 *	previous one completes
 * RET true if ping process is done, false otherwise
 */
bool is_ping_done (void)
{
	bool is_done = true;

	slurm_mutex_lock(&lock_mutex);
	if (ping_count)
		is_done = false;
	slurm_mutex_unlock(&lock_mutex);

	return is_done;
}

/*
 * ping_begin - record that a ping cycle has begin. This can be called more 
 *	than once (for REQUEST_PING and simultaneous REQUEST_NODE_REGISTRATION 
 *	for selected nodes). Matching ping_end calls must be made for each 
 *	before is_ping_done returns true.
 */
void ping_begin (void)
{
	slurm_mutex_lock(&lock_mutex);
	ping_count++;
	slurm_mutex_unlock(&lock_mutex);
}

/*
 * ping_end - record that a ping cycle has ended. This can be called more 
 *	than once (for REQUEST_PING and simultaneous REQUEST_NODE_REGISTRATION 
 *	for selected nodes). Matching ping_end calls must be made for each 
 *	before is_ping_done returns true.
 */
void ping_end (void)
{
	slurm_mutex_lock(&lock_mutex);
	if (ping_count > 0)
		ping_count--;
	else
		fatal ("ping_count < 0");
	slurm_mutex_unlock(&lock_mutex);
	
	/* gettimeofday(&end_time, NULL); */
/* 	start = start_time.tv_sec; */
/* 	start *= 1000000; */
/* 	start += start_time.tv_usec; */
/* 	end = end_time.tv_sec; */
/* 	end *= 1000000; */
/* 	end += end_time.tv_usec; */
/* 	info("done with ping took %ld",(end-start)); */
}

/*
 * ping_nodes - check that all nodes and daemons are alive,  
 *	get nodes in UNKNOWN state to register
 */
void ping_nodes (void)
{
	static int offset = 0;	/* mutex via node table write lock on entry */
	int i, pos;
	time_t now, still_live_time, node_dead_time;
	static time_t last_ping_time = (time_t) 0;
	uint16_t base_state, no_resp_flag;
	bool restart_flag;
	hostlist_t ping_hostlist = hostlist_create("");
	hostlist_t reg_hostlist  = hostlist_create("");
	hostlist_t down_hostlist = NULL;
	char host_str[MAX_SLURM_NAME];

	int ping_buf_rec_size = 0;
	agent_arg_t *ping_agent_args;

	int reg_buf_rec_size = 0;
	agent_arg_t *reg_agent_args;
	
	ping_agent_args = xmalloc (sizeof (agent_arg_t));
	ping_agent_args->msg_type = REQUEST_PING;
	ping_agent_args->retry = 0;
	reg_agent_args = xmalloc (sizeof (agent_arg_t));
	reg_agent_args->msg_type = REQUEST_NODE_REGISTRATION_STATUS;
	reg_agent_args->retry = 0;
	/* gettimeofday(&start_time, NULL); */
		
	/*
	 * If there are a large number of down nodes, the node ping
	 * can take a long time to complete: 
	 *  ping_time = down_nodes * agent_timeout / agent_parallelism
	 *  ping_time = down_nodes * 10_seconds / 10
	 *  ping_time = down_nodes (seconds)
	 * Because of this, we extend the SlurmdTimeout by the 
	 * time needed to complete a ping of all nodes.
	 */
	now = time (NULL);
	if ((slurmctld_conf.slurmd_timeout == 0) 
	||  (last_ping_time == (time_t) 0)) {
		node_dead_time = (time_t) 0;
	} else {
		node_dead_time = last_ping_time -
				slurmctld_conf.slurmd_timeout;
	}
	still_live_time = now - (slurmctld_conf.slurmd_timeout / 2);
	last_ping_time  = now;

	offset += MAX_REG_THREADS;
	if ((offset > node_record_count) && 
	    (offset >= (MAX_REG_THREADS * MAX_REG_FREQUENCY)))
		offset = 0;

	for (i = 0; i < node_record_count; i++) {
		struct node_record *node_ptr;
		
		node_ptr = &node_record_table_ptr[i];
		base_state   = node_ptr->node_state & NODE_STATE_BASE;
		no_resp_flag = node_ptr->node_state & NODE_STATE_NO_RESPOND;
		
		if ((slurmctld_conf.slurmd_timeout == 0)
		&&  (base_state != NODE_STATE_UNKNOWN))
			continue;

		if ((node_ptr->last_response != (time_t) 0)
		    &&  (node_ptr->last_response <= node_dead_time)
		    &&  (base_state != NODE_STATE_DOWN)) {
			if (down_hostlist)
				(void) hostlist_push_host(down_hostlist,
					node_ptr->name);
			else
				down_hostlist = hostlist_create(node_ptr->name);
			set_node_down(node_ptr->name, "Not responding");
			continue;
		}

		if (node_ptr->last_response == (time_t) 0) {
			restart_flag = true;	/* system just restarted */
			node_ptr->last_response = slurmctld_conf.last_update;
		} else
			restart_flag = false;

#ifdef HAVE_FRONT_END		/* Operate only on front-end */
		if (i > 0)
			continue;
#endif

		/* Request a node registration if its state is UNKNOWN or 
		 * on a periodic basis (about every MAX_REG_FREQUENCY ping, 
		 * this mechanism avoids an additional (per node) timer or 
		 * counter and gets updated configuration information 
		 * once in a while). We limit these requests since they 
		 * can generate a flood of incomming RPCs. */
		if ((base_state == NODE_STATE_UNKNOWN) || restart_flag ||
		    ((i >= offset) && (i < (offset + MAX_REG_THREADS)))) {
			(void) hostlist_push_host(reg_hostlist, node_ptr->name);
			if ((reg_agent_args->node_count+1) > 
						reg_buf_rec_size) {
				reg_buf_rec_size += 32;
				xrealloc ((reg_agent_args->slurm_addr), 
				          (sizeof (struct sockaddr_in) * 
					  reg_buf_rec_size));
				xrealloc ((reg_agent_args->node_names), 
				          (MAX_SLURM_NAME * reg_buf_rec_size));
			}
			reg_agent_args->slurm_addr[reg_agent_args->node_count] 
				= node_ptr->slurm_addr;
			pos = MAX_SLURM_NAME * reg_agent_args->node_count;
			strncpy (&reg_agent_args->node_names[pos],
			         node_ptr->name, MAX_SLURM_NAME);
			reg_agent_args->node_count++;
			continue;
		}

		if (node_ptr->last_response >= still_live_time)
			continue;

		/* Do not keep pinging down nodes since this can induce
		 * huge delays in hierarchical communication fail-over */
		if ((no_resp_flag) && (base_state == NODE_STATE_DOWN))
			continue;

		(void) hostlist_push_host(ping_hostlist, node_ptr->name);
		if ((ping_agent_args->node_count+1) > ping_buf_rec_size) {
			ping_buf_rec_size += 32;
			xrealloc ((ping_agent_args->slurm_addr), 
			          (sizeof (struct sockaddr_in) * 
				  ping_buf_rec_size));
			xrealloc ((ping_agent_args->node_names), 
			          (MAX_SLURM_NAME * ping_buf_rec_size));
		}
		ping_agent_args->slurm_addr[ping_agent_args->node_count] = 
					node_ptr->slurm_addr;
		pos = MAX_SLURM_NAME * ping_agent_args->node_count;
		strncpy (&ping_agent_args->node_names[pos],
		         node_ptr->name, MAX_SLURM_NAME);
		ping_agent_args->node_count++;
	}

	if (ping_agent_args->node_count == 0)
		xfree (ping_agent_args);
	else {
		hostlist_uniq(ping_hostlist);
		hostlist_ranged_string(ping_hostlist, 
			sizeof(host_str), host_str);
		verbose("Spawning ping agent for %s", host_str);
		ping_begin();
		agent_queue_request(ping_agent_args);
	}

	if (reg_agent_args->node_count == 0)
		xfree (reg_agent_args);
	else {
		hostlist_uniq(reg_hostlist);
		hostlist_ranged_string(reg_hostlist, 
			sizeof(host_str), host_str);
		verbose("Spawning registration agent for %s %d hosts", 
			host_str, reg_agent_args->node_count);
		ping_begin();
		agent_queue_request(reg_agent_args);
	}

	if (down_hostlist) {
		hostlist_uniq(down_hostlist);
		hostlist_ranged_string(down_hostlist,
			sizeof(host_str), host_str);
		error("Nodes %s not responding, setting DOWN", host_str);
		hostlist_destroy(down_hostlist);
	}
	hostlist_destroy(ping_hostlist);
	hostlist_destroy(reg_hostlist);
}
