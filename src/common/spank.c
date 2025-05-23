/*****************************************************************************\
 *  spank.c -- stackable plugin architecture for node job kontrol (SPANK)
 *****************************************************************************
 *  Copyright (C) 2005-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
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

#define _GNU_SOURCE

#include "config.h"

#include <ctype.h>
#include <dlfcn.h>
#include <glob.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "src/common/env.h"
#include "src/common/job_options.h"
#include "src/common/optz.h"
#include "src/common/plugin.h"
#include "src/common/read_config.h"
#include "src/common/spank.h"
#include "src/common/strlcpy.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/slurmd/slurmstepd/slurmstepd_job.h"

#include "slurm/spank.h"

#define REQUIRED "required"
#define OPTIONAL "optional"
#define INCLUDE  "include"

static bool has_prolog = false;
static bool has_epilog = false;
static bool has_user_init = false;
static bool has_task_post_fork = false;
static bool has_task_exit = false;

struct spank_plugin_operations {
	spank_f *init;
	int *init_failure_mode;
	spank_f *job_prolog;
	spank_f *init_post_opt;
	spank_f *local_user_init;
	spank_f *user_init;
	spank_f *task_init_privileged;
	spank_f *user_task_init;
	spank_f *task_post_fork;
	spank_f *task_exit;
	spank_f *job_epilog;
	spank_f *slurmd_exit;
	spank_f *exit;
};

const int n_spank_syms = 13;
const char *spank_syms[] = {
	"slurm_spank_init",
	"slurm_spank_init_failure_mode",
	"slurm_spank_job_prolog",
	"slurm_spank_init_post_opt",
	"slurm_spank_local_user_init",
	"slurm_spank_user_init",
	"slurm_spank_task_init_privileged",
	"slurm_spank_task_init",
	"slurm_spank_task_post_fork",
	"slurm_spank_task_exit",
	"slurm_spank_job_epilog",
	"slurm_spank_slurmd_exit",
	"slurm_spank_exit"
};

struct spank_plugin {
	const char *name;
	char *fq_path;
	plugin_handle_t plugin;
	bool required;
	int ac;
	char **argv;
	struct spank_plugin_operations ops;
	struct spank_option *opts;
	struct spank_stack *stack;
};

/*
 *  SPANK Plugin options
 */

struct spank_plugin_opt {
	struct spank_option *opt;   /* Copy of plugin option info           */
	struct spank_plugin *plugin;/* Link back to plugin structure        */
	int optval;                 /* Globally unique value                */
	bool found;		    /* 1 if option was found, 0 otherwise   */
	bool disabled;		    /* 1 if option is cached but disabled   */
	char *optarg;               /* Option argument.                     */
	bool set;                   /* true if argument is set              */
	bool set_by_env;            /* true if argument is set by environ   */
};

/*
 *  SPANK plugin context type (local, remote, allocator)
 */
enum spank_context_type {
	S_TYPE_NONE,
	S_TYPE_LOCAL,           /* LOCAL == srun              */
	S_TYPE_REMOTE,          /* REMOTE == slurmstepd       */
	S_TYPE_ALLOCATOR,       /* ALLOCATOR == sbatch/salloc */
	S_TYPE_SLURMD,          /* SLURMD == slurmd           */
	S_TYPE_JOB_SCRIPT,      /* JOB_SCRIPT == prolog/epilog*/
};

/*
 *  Job information in prolog/epilog context:
 */
struct job_script_info {
	uint32_t  jobid;
	uid_t     uid;
	gid_t gid;
};

struct spank_handle {
#   define SPANK_MAGIC 0x00a5a500
	int                  magic;  /* Magic identifier to ensure validity. */
	struct spank_plugin *plugin; /* Current plugin using handle          */
	step_fn_t            phase;  /* Which spank fn are we called from?   */
	void               * job;    /* Reference to current srun|slurmd job */
	stepd_step_task_info_t * task;   /* Reference to current
					      * task (if valid) */
	struct spank_stack  *stack;  /* Reference to the current plugin stack*/
};

/*
 *  SPANK stack. The stack of loaded plugins and associated state.
 */
struct spank_stack {
	enum spank_context_type type;/*  Type of context for this stack      */
	list_t *plugin_list;         /*  Stack of spank plugins              */
	list_t *option_cache;        /*  Cache of plugin options in this ctx */
	int  spank_optval;           /*  optvalue for next plugin option     */
	const char * plugin_path;    /*  default path to search for plugins  */
};

/*
 *  The global spank plugin stack:
 */
static struct spank_stack *global_spank_stack = NULL;

/*
 *  Forward declarations
 */
static int _spank_option_register(struct spank_plugin *p,
				  struct spank_option *opt);
static int _spank_stack_load (struct spank_stack *stack, const char *file);
static void _spank_plugin_destroy (struct spank_plugin *);
static void _spank_plugin_opt_destroy (struct spank_plugin_opt *);
static void _spank_stack_get_remote_options(struct spank_stack *, list_t *,
					    list_t *);
static void _spank_stack_get_remote_options_env(struct spank_stack *, char **,
						list_t *);
static void _spank_stack_set_remote_options_env(struct spank_stack *stack);
static int dyn_spank_set_job_env (const char *var, const char *val, int ovwt);
static char *_opt_env_name(struct spank_plugin_opt *p, char *buf, size_t siz);

static void spank_stack_destroy (struct spank_stack *stack)
{
	FREE_NULL_LIST (stack->plugin_list);
	FREE_NULL_LIST (stack->option_cache);
	xfree (stack->plugin_path);
	xfree (stack);
}

static struct spank_stack *
spank_stack_create (const char *file, enum spank_context_type type)
{
	slurm_conf_t *conf;
	struct spank_stack *stack = xmalloc (sizeof (*stack));

	conf = slurm_conf_lock();
	stack->plugin_path = xstrdup (conf->plugindir);
	slurm_conf_unlock();

	stack->type = type;
	stack->spank_optval = 0xfff;
	stack->plugin_list =
		list_create ((ListDelF) _spank_plugin_destroy);
	stack->option_cache =
		list_create ((ListDelF) _spank_plugin_opt_destroy);

	if (_spank_stack_load (stack, file) < 0) {
		spank_stack_destroy (stack);
		return (NULL);
	}

	return (stack);
}

static list_t *get_global_option_cache(void)
{
	if (global_spank_stack)
		return (global_spank_stack->option_cache);
	else
		return (NULL);
}


static int plugin_in_list(list_t *l, struct spank_plugin *sp)
{
	int rc = 0;
	struct spank_plugin *p;
	list_itr_t *i = list_iterator_create(l);
	while ((p = list_next (i))) {
		if (p->fq_path == sp->fq_path) {
			rc = 1;
			break;
		}
	}
	list_iterator_destroy (i);
	return (rc);
}

static void _argv_append(char ***argv, int ac, const char *newarg)
{
	*argv = xrealloc(*argv, (++ac + 1) * sizeof(char *));
	(*argv)[ac] = NULL;
	(*argv)[ac - 1] = xstrdup(newarg);
	return;
}

typedef enum {
   CF_ERROR = 0,
   CF_OPTIONAL,
   CF_REQUIRED,
   CF_INCLUDE,
} cf_line_t;

static cf_line_t _plugin_stack_line_type (const char *str)
{
	if (xstrcmp(str, REQUIRED) == 0)
		return (CF_REQUIRED);
	else if (xstrcmp(str, OPTIONAL) == 0)
		return (CF_OPTIONAL);
	else if (xstrcmp(str, INCLUDE) == 0)
		return (CF_INCLUDE);
	else {
		error("spank: Invalid option \"%s\". Must be %s, %s or %s",
		     str, REQUIRED, OPTIONAL, INCLUDE);
		return (CF_ERROR);
	}
}


static int
_plugin_stack_parse_line(char *line, char **plugin, int *acp, char ***argv,
			 cf_line_t * type)
{
	int ac;
	const char *separators = " \t\n";
	char *path;
	char *option;
	char *s;
	char **av;
	char *sp;

	*plugin = NULL;
	*argv = NULL;
	*acp = 0;

	/* Nullify any comments
	 */
	if ((s = strchr(line, '#')))
		*s = '\0';

	if (!(option = strtok_r(line, separators, &sp)))
		return (0);

	if (((*type) = _plugin_stack_line_type(option)) == CF_ERROR)
		return (-1);

	if (!(path = strtok_r(NULL, separators, &sp)))
		return (-1);

	ac = 0;
	av = NULL;

	while ((s = strtok_r(NULL, separators, &sp)))
		_argv_append(&av, ac++, s);

	*plugin = xstrdup(path);
	*argv = av;
	*acp = ac;

	return (0);
}

static struct spank_plugin *_spank_plugin_create(struct spank_stack *stack,
						 char *path, int ac,
						 char **av, bool required)
{
	struct spank_plugin *plugin;
	plugin_handle_t p;
	int e;
	struct spank_plugin_operations ops;

	if ((e = plugin_load_from_file(&p, path)) != SLURM_SUCCESS) {
		error("spank: %s: %s", path, slurm_strerror(e));
		return NULL;
	}

	if (plugin_get_syms(p, n_spank_syms, spank_syms, (void **)&ops) == 0) {
		error("spank: \"%s\" exports 0 symbols", path);
		return NULL;
	}

	plugin = xmalloc(sizeof(struct spank_plugin));

	plugin->fq_path = path;	/* fq_path is xstrdup'd in *process_line */
	plugin->plugin = p;
	plugin->name = plugin_get_name(p);	/* no need to dup */
	plugin->required = required;
	plugin->ac = ac;
	plugin->argv = av;
	plugin->ops = ops;
	plugin->stack = stack;

	/*
	 *  Do not load static plugin options table in allocator context.
	 */
	if (stack->type != S_TYPE_ALLOCATOR)
		plugin->opts = plugin_get_sym(p, "spank_options");

	return (plugin);
}

void _spank_plugin_destroy(struct spank_plugin *sp)
{
	if (sp == NULL)
		return;

	xfree(sp->fq_path);

	/* No need to free "name" it was defined within plugin */
	sp->name = NULL;

	plugin_unload(sp->plugin);
	sp->plugin = NULL;
	if (sp->argv) {
		int i;
		for (i = 0; sp->argv[i]; i++)
			xfree(sp->argv[i]);
		xfree(sp->argv);
	}
	xfree(sp);
	return;
}

static char *
_spank_plugin_find (const char *path, const char *file)
{
	char dir[PATH_MAX];
	char *p, *entry;
	int pathlen = strlen (path);

	if (strlcpy(dir, path, sizeof (dir)) > sizeof (dir))
		return (NULL);

	/*
	 * Ensure PATH ends with a :
	 */
	if (dir[pathlen - 1] != ':') {
		dir[pathlen] = ':';
		dir[pathlen+1] = '\0';
	}

	entry = dir;
	while ((p = strchr(entry, ':'))) {
		char *fq_path;
		*(p++) = '\0';

		fq_path = xstrdup (entry);
		if (entry [strlen(entry) - 1] != '/')
			xstrcatchar (fq_path, '/');
		xstrcat (fq_path, file);

		if (plugin_peek(fq_path, NULL, 0) == SLURM_SUCCESS)
			return (fq_path);

		xfree (fq_path);
		entry = p;
	}

	return (NULL);
}

static int _spank_conf_include (struct spank_stack *,
	const char *, int, const char *);

static int
spank_stack_plugin_valid_for_context (struct spank_stack *stack,
	struct spank_plugin *p)
{
	switch (stack->type) {
	case S_TYPE_JOB_SCRIPT:
		if (p->ops.job_prolog || p->ops.job_epilog)
			return (1);
		break;
	case S_TYPE_SLURMD:
		/*
		 * Set flags if prolog/epilog exist, but only return 1 if
		 * slurmd_exit is defined so that spank_init is only called when
		 * slurmd_exit exists. slurmd needs to know whether to create a
		 * spank stepd to run spank prolog/epilog in job_script context.
		 */
		if (p->ops.job_prolog)
			has_prolog = true;
		if (p->ops.job_epilog)
			has_epilog = true;

		if (p->ops.init || p->ops.slurmd_exit)
			return (1);
		break;
	case S_TYPE_LOCAL:
	case S_TYPE_ALLOCATOR:
	case S_TYPE_REMOTE:
		/*
		 *  For backwards compatibility: All plugins were
		 *   always loaded in these contexts, so continue
		 *   to do so
		 */
		if (p->ops.user_init)
			has_user_init = true;
		if (p->ops.task_post_fork)
			has_task_post_fork = true;
		if (p->ops.task_exit)
			has_task_exit = true;
		return (1);
	default:
		return (0);
	}
	return (0);
}

static int
_spank_stack_process_line(struct spank_stack *stack,
	const char *file, int line, char *buf)
{
	char **argv;
	int ac;
	char *path;
	cf_line_t type = CF_REQUIRED;
	bool required;

	struct spank_plugin *p;

	if (_plugin_stack_parse_line(buf, &path, &ac, &argv, &type) < 0) {
		error("spank: %s:%d: Invalid line. Ignoring.", file, line);
		return (0);
	}

       if (type == CF_INCLUDE) {
	       int rc = _spank_conf_include (stack, file, line, path);
	       xfree (path);
	       return (rc);
       }

	if (path == NULL)	/* No plugin listed on this line */
		return (0);

	if (path[0] != '/') {
		char *f;

		if ((f = _spank_plugin_find (stack->plugin_path, path))) {
			xfree (path);
			path = f;
		}
	}

	required = (type == CF_REQUIRED);
	if (!(p = _spank_plugin_create(stack, path, ac, argv, required))) {
		if (required)
			error ("spank: %s:%d:"
			       " Failed to load plugin %s. Aborting.",
			       file, line, path);
		else
			verbose ("spank: %s:%d:"
				 "Failed to load optional plugin %s. Ignored.",
				 file, line, path);
		return (required ? -1 : 0);
	}

	if (plugin_in_list (stack->plugin_list, p)) {
		error ("spank: %s: cowardly refusing to load a second time",
			p->fq_path);
		_spank_plugin_destroy (p);
		return (0);
	}

	if (!spank_stack_plugin_valid_for_context (stack, p)) {
		debug2 ("spank: %s: no callbacks in this context", p->fq_path);
		_spank_plugin_destroy (p);
		return (0);
	}

	debug ("spank: %s:%d: Loaded plugin %s",
			file, line, xbasename (p->fq_path));

	list_append (stack->plugin_list, p);
	for (struct spank_option *opt = p->opts; opt && opt->name; opt++)
		_spank_option_register(p, opt);

	return (0);
}

static int _spank_stack_load(struct spank_stack *stack, const char *path)
{
	int rc = 0;
	int line;
	char buf[PATH_MAX];
	int fd;
	FILE *fp;

	debug ("spank: opening plugin stack %s", path);

	/*
	 *  Try to open plugstack.conf. A missing config file is not an
	 *   error, but is equivalent to an empty file.
	 */
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0 ||
	    (fp = fdopen(fd, "r")) == NULL) {
		if (errno == ENOENT)
			return (0);
		error("spank: Failed to open %s: %m", path);
		return (-1);
	}

	line = 1;
	while (fgets(buf, sizeof(buf), fp)) {
		rc = _spank_stack_process_line(stack, path, line, buf);
		if (rc < 0)
			break;
		line++;
	}

	fclose(fp);
	return (rc);
}

static int _spank_conf_include (struct spank_stack *stack,
		const char *file, int lineno, const char *pattern)
{
	int rc = 0;
	glob_t gl;
	size_t i;
	char *copy = NULL;

	if (pattern == NULL) {
		error ("%s: %d: Invalid include directive", file, lineno);
		return (SLURM_ERROR);
	}

	if (pattern[0] != '/') {
		char *dirc = xstrdup (file);
		char *dname = dirname (dirc);

		if (dname != NULL)  {
			xstrfmtcat (copy, "%s/%s", dname, pattern);
			pattern = copy;
		}
		xfree (dirc);
	}

	debug ("%s: %d: include \"%s\"", file, lineno, pattern);

	rc = glob (pattern, 0, NULL, &gl);
	switch (rc) {
	  case 0:
	  	for (i = 0; i < gl.gl_pathc; i++) {
			rc = _spank_stack_load (stack, gl.gl_pathv[i]);
			if (rc < 0)
				break;
		}
	  	break;
	  case GLOB_NOMATCH:
		break;
	  case GLOB_NOSPACE:
		errno = ENOMEM;
		break;
	  case GLOB_ABORTED:
		verbose ("%s:%d: cannot read dir %s: %m",
			file, lineno, pattern);
		break;
	  default:
		error ("Unknown glob(3) return code = %d", rc);
		break;
	}

	xfree (copy);
	globfree (&gl);
	return (rc);
}

static void _spank_handle_init(struct spank_handle *spank,
			       struct spank_stack *stack, void *arg, int taskid,
			       step_fn_t fn)
{
	memset(spank, 0, sizeof(*spank));
	spank->magic = SPANK_MAGIC;
	spank->plugin = NULL;

	spank->phase = fn;
	spank->stack = stack;

	if (arg != NULL) {
		spank->job = arg;
		if (stack->type == S_TYPE_REMOTE && taskid >= 0) {
			spank->task = ((stepd_step_rec_t *) arg)->task[taskid];
		}
	}
}

static const char *_step_fn_name(step_fn_t type)
{
	switch (type) {
	case SPANK_INIT:
		return ("init");
	case SPANK_JOB_PROLOG:
		return ("job_prolog");
	case SPANK_INIT_POST_OPT:
		return ("init_post_opt");
	case SPANK_LOCAL_USER_INIT:
		return ("local_user_init");
	case SPANK_STEP_USER_INIT:
		return ("user_init");
	case SPANK_STEP_TASK_INIT_PRIV:
		return ("task_init_privileged");
	case SPANK_STEP_USER_TASK_INIT:
		return ("task_init");
	case SPANK_STEP_TASK_POST_FORK:
		return ("task_post_fork");
	case SPANK_STEP_TASK_EXIT:
		return ("task_exit");
	case SPANK_JOB_EPILOG:
		return ("job_epilog");
	case SPANK_SLURMD_EXIT:
		return ("slurmd_exit");
	case SPANK_EXIT:
		return ("exit");
	}

	/* NOTREACHED */
	return ("unknown");
}

static spank_f *spank_plugin_get_fn (struct spank_plugin *sp, step_fn_t type)
{
	switch (type) {
	case SPANK_INIT:
		return (sp->ops.init);
	case SPANK_JOB_PROLOG:
		return (sp->ops.job_prolog);
	case SPANK_INIT_POST_OPT:
		return (sp->ops.init_post_opt);
	case SPANK_LOCAL_USER_INIT:
		return (sp->ops.local_user_init);
	case SPANK_STEP_USER_INIT:
		return (sp->ops.user_init);
	case SPANK_STEP_TASK_INIT_PRIV:
		return (sp->ops.task_init_privileged);
	case SPANK_STEP_USER_TASK_INIT:
		return (sp->ops.user_task_init);
	case SPANK_STEP_TASK_POST_FORK:
		return (sp->ops.task_post_fork);
	case SPANK_STEP_TASK_EXIT:
		return (sp->ops.task_exit);
	case SPANK_JOB_EPILOG:
		return (sp->ops.job_epilog);
	case SPANK_SLURMD_EXIT:
		return (sp->ops.slurmd_exit);
	case SPANK_EXIT:
		return (sp->ops.exit);
	default:
		error("Unhandled spank function type=%d", type);
		return (NULL);
	}
	return (NULL);
}

static int _do_call_stack(struct spank_stack *stack,
	step_fn_t type, void * job, int taskid)
{
	int rc = SLURM_SUCCESS;
	list_itr_t *i;
	struct spank_plugin *sp;
	struct spank_handle spank[1];
	const char *fn_name;

	if (!stack)
		return ESPANK_BAD_ARG;

	_spank_handle_init(spank, stack, job, taskid, type);
	fn_name = _step_fn_name(type);

	i = list_iterator_create(stack->plugin_list);
	while ((sp = list_next(i))) {
		const char *name = xbasename(sp->fq_path);
		spank_f *spank_fn;

		spank->plugin = sp;

		spank_fn = spank_plugin_get_fn (sp, type);
		if (!spank_fn)
			continue;

		rc = (*spank_fn) (spank, sp->ac, sp->argv);
		debug2("spank: %s: %s = %d", name, fn_name, rc);

		if (rc && sp->required) {
			error("spank: required plugin %s: "
			      "%s() failed with rc=%d", name, fn_name, rc);
			if ((type == SPANK_INIT) && sp->ops.init_failure_mode)
				rc = *(sp->ops.init_failure_mode);
			break;
		} else
			rc = SLURM_SUCCESS;
	}

	list_iterator_destroy(i);

	return rc;
}

struct spank_stack *spank_stack_init(enum spank_context_type context)
{
	char *path;
	struct spank_stack *stack = NULL;

	if (!(path = xstrdup(slurm_conf.plugstack)))
		path = get_extra_conf_path("plugstack.conf");

	stack = spank_stack_create(path, context);
	xfree(path);

	return stack;
}

int _spank_init(enum spank_context_type context, stepd_step_rec_t *step)
{
	struct spank_stack *stack;

	if (!(stack = spank_stack_init (context)))
		return (-1);
	global_spank_stack = stack;

	return (_do_call_stack(stack, SPANK_INIT, step, -1));
}

static int spank_stack_post_opt (struct spank_stack * stack,
				 stepd_step_rec_t *step)
{
	list_t *found_opts = job_options_create();

	/*
	 *  Get any remote options from job launch message:
	 */
	_spank_stack_get_remote_options(stack, step->options, found_opts);

	/*
	 *  Get any remote option passed thru environment
	 */
	_spank_stack_get_remote_options_env(stack, step->env, found_opts);
	list_destroy(found_opts);

	/*
	 * Now clear any remaining options passed through environment
	 */
	spank_clear_remote_options_env(step->env);

	/*
	 *  Now that all options have been processed, we can
	 *   call the post_opt handlers here in remote context.
	 */
	return (_do_call_stack(stack, SPANK_INIT_POST_OPT, step, -1));

}

static int spank_init_remote (stepd_step_rec_t *step)
{
	int rc = SLURM_SUCCESS;

	if ((rc = _spank_init(S_TYPE_REMOTE, step)))
		return rc;

	/*
	 * _spank_init initializes global_spank_stack
	 */
	return (spank_stack_post_opt (global_spank_stack, step));
}

int spank_init(stepd_step_rec_t *step)
{
	if (step)
		return spank_init_remote(step);
	else
		return _spank_init (S_TYPE_LOCAL, NULL);
}

int spank_init_allocator (void)
{
	return _spank_init (S_TYPE_ALLOCATOR, NULL);
}

int spank_slurmd_init (void)
{
	return _spank_init (S_TYPE_SLURMD, NULL);
}

int spank_init_post_opt (void)
{
	struct spank_stack *stack = global_spank_stack;

	/*
	 *  Set remote options in our environment and the
	 *   spank_job_env so that we can always pull them out
	 *   on the remote side and/or job prolog epilog.
	 */
	_spank_stack_set_remote_options_env(stack);

	return (_do_call_stack(stack, SPANK_INIT_POST_OPT, NULL, -1));
}

int spank_user(stepd_step_rec_t *step)
{
	return (_do_call_stack(global_spank_stack, SPANK_STEP_USER_INIT,
			       step, -1));
}

int spank_local_user(struct spank_launcher_job_info *job)
{
	return (_do_call_stack(global_spank_stack, SPANK_LOCAL_USER_INIT,
			       job, -1));
}

int spank_task_privileged(stepd_step_rec_t *step, int taskid)
{
	return (_do_call_stack(global_spank_stack, SPANK_STEP_TASK_INIT_PRIV,
			       step, taskid));
}

int spank_user_task(stepd_step_rec_t *step, int taskid)
{
	return (_do_call_stack(global_spank_stack, SPANK_STEP_USER_TASK_INIT,
			       step, taskid));
}

int spank_task_post_fork(stepd_step_rec_t *step, int taskid)
{
	return (_do_call_stack(global_spank_stack, SPANK_STEP_TASK_POST_FORK,
			       step, taskid));
}

int spank_task_exit(stepd_step_rec_t *step, int taskid)
{
	return (_do_call_stack(global_spank_stack, SPANK_STEP_TASK_EXIT,
			       step, taskid));
}

int spank_slurmd_exit (void)
{
	int rc;
	rc =  _do_call_stack (global_spank_stack, SPANK_SLURMD_EXIT, NULL, 0);
	spank_stack_destroy (global_spank_stack);
	global_spank_stack = NULL;
	return (rc);
}

int spank_fini(stepd_step_rec_t *step)
{
	int rc = _do_call_stack(global_spank_stack, SPANK_EXIT, step, -1);

	spank_stack_destroy (global_spank_stack);
	global_spank_stack = NULL;

	return (rc);
}

/*
 *  Run job_epilog or job_prolog callbacks in a private spank context.
 */
static int spank_job_script(step_fn_t fn, uint32_t jobid, uid_t uid, gid_t gid)
{
	int rc = 0;
	struct spank_stack *stack;
	struct job_script_info jobinfo = { jobid, uid, gid };

	stack = spank_stack_init (S_TYPE_JOB_SCRIPT);
	if (!stack)
		return (-1);
	global_spank_stack = stack;

	rc = _do_call_stack (stack, fn, &jobinfo, -1);

	spank_stack_destroy (stack);
	global_spank_stack = NULL;
	return (rc);
}

int spank_job_prolog(uint32_t jobid, uid_t uid, gid_t gid)
{
	return spank_job_script(SPANK_JOB_PROLOG, jobid, uid, gid);
}

int spank_job_epilog(uint32_t jobid, uid_t uid, gid_t gid)
{
	return spank_job_script(SPANK_JOB_EPILOG, jobid, uid, gid);
}

/*
 *  SPANK options functions
 */

static int _spank_next_option_val(struct spank_stack *stack)
{
	return (stack->spank_optval++);
}

static struct spank_option * _spank_option_copy(struct spank_option *opt)
{
	struct spank_option *copy = xmalloc (sizeof (*copy));

	memset (copy, 0, sizeof (*copy));

	copy->name = xstrdup (opt->name);
	copy->has_arg = opt->has_arg;
	copy->val = opt->val;
	copy->cb = opt->cb;

	if (opt->arginfo)
		copy->arginfo = xstrdup (opt->arginfo);
	if (opt->usage)
		copy->usage = xstrdup (opt->usage);

	return (copy);
}

static void _spank_option_destroy(struct spank_option *opt)
{
	xfree (opt->name);
	xfree (opt->arginfo);
	xfree (opt->usage);
	xfree (opt);
}

static struct spank_plugin_opt *_spank_plugin_opt_create(struct
							 spank_plugin *p,
							 struct
							 spank_option *opt,
							 int disabled)
{
	struct spank_plugin_opt *spopt = xmalloc(sizeof(*spopt));
	spopt->opt = _spank_option_copy (opt);
	spopt->plugin = p;
	spopt->optval = _spank_next_option_val(p->stack);
	spopt->found = 0;
	spopt->optarg = NULL;
	spopt->set = false;
	spopt->set_by_env = false;

	spopt->disabled = disabled;

	return (spopt);
}

void _spank_plugin_opt_destroy(struct spank_plugin_opt *spopt)
{
	_spank_option_destroy (spopt->opt);
	xfree(spopt->optarg);
	xfree(spopt);
}

static int _opt_by_val(struct spank_plugin_opt *opt, int *optvalp)
{
	return (opt->optval == *optvalp);
}

static int _opt_by_name(struct spank_plugin_opt *opt, char *optname)
{
	return (xstrcmp(opt->opt->name, optname) == 0);
}

static int
_spank_option_register(struct spank_plugin *p, struct spank_option *opt)
{
	int disabled = 0;
	struct spank_plugin_opt *spopt;
	struct spank_stack *stack;
	list_t *option_cache;

	stack = p->stack;
	if (stack == NULL) {
		error ("spank: %s: can't determine plugin context", p->name);
		return (ESPANK_BAD_ARG);
	}
	option_cache = stack->option_cache;

	spopt = list_find_first(option_cache,
			(ListFindF) _opt_by_name, opt->name);
	if (spopt) {
		struct spank_plugin *q = spopt->plugin;
		info("spank: option \"%s\" provided by both %s and %s",
				opt->name, xbasename(p->fq_path),
				xbasename(q->fq_path));
		/*
		 *  Disable this option, but still cache it, in case
		 *    options are loaded in a different order on the
		 *    remote side.
		 */
		disabled = 1;
	}

	if ((strlen(opt->name) > SPANK_OPTION_MAXLEN)) {
		error("spank: option \"%s\" provided by %s too long. "
		      "Ignoring.", opt->name, p->name);
		return (ESPANK_NOSPACE);
	}

	debug ("SPANK: appending plugin option \"%s\"", opt->name);
	list_append(option_cache, _spank_plugin_opt_create(p, opt, disabled));

	return (ESPANK_SUCCESS);
}

spank_err_t spank_option_register(spank_t sp, struct spank_option *opt)
{
	if (sp->phase != SPANK_INIT)
		return (ESPANK_BAD_ARG);

	if (!sp->plugin)
		error ("Uh, oh, no current plugin!");

	if (!opt || !opt->name || !opt->usage)
		return (ESPANK_BAD_ARG);

	return (_spank_option_register(sp->plugin, opt));
}

static int _add_one_option(struct option **optz,
			   struct spank_plugin_opt *spopt)
{
	struct option opt;

	opt.name = spopt->opt->name;
	opt.has_arg = spopt->opt->has_arg;
	opt.flag = NULL;
	opt.val = spopt->optval;

	if (optz_add(optz, &opt) < 0) {
		if (errno == EEXIST) {
			error ("Ignoring conflicting option \"%s\" "
			       "in plugin \"%s\"",
			       opt.name, spopt->plugin->name);
		} else {
			error("Unable to add option \"%s\" "
			      "from plugin \"%s\"",
			      opt.name, spopt->plugin->name);
		}

		return (-1);
	}

	return (0);
}


struct option *spank_option_table_create(const struct option *orig)
{
	struct spank_plugin_opt *spopt;
	struct option *opts = NULL;
	list_itr_t *i = NULL;

	list_t *option_cache = get_global_option_cache();
	if (option_cache == NULL)
		return (NULL);

	opts = optz_create();

	/*
	 *  Start with original options:
	 */
	if ((orig != NULL) && (optz_append(&opts, orig) < 0)) {
		optz_destroy(opts);
		return (NULL);
	}

	if (option_cache == NULL || (list_count(option_cache) == 0))
		return (opts);

	i = list_iterator_create(option_cache);
	while ((spopt = list_next(i))) {
		if (!spopt->disabled && (_add_one_option (&opts, spopt) < 0))
			spopt->disabled = 1;
	}

	list_iterator_destroy(i);

	return (opts);
}

void spank_option_table_destroy(struct option *optz)
{
	optz_destroy(optz);
}

static int _do_option_cb(struct spank_plugin_opt *opt, const char *arg,
			 int remote)
{
	int rc = 0;

	xassert(opt);
	xassert(arg);

	/*
	 *  Call plugin callback if such a one exists
	 */
	if (opt->opt->cb
	    && (rc = ((*opt->opt->cb) (opt->opt->val, arg, remote))))
		return (rc);

	/*
	 *  Set optarg and "found" so that option will be forwarded
	 *    to remote side.
	 */
	if (opt->opt->has_arg) {
		xfree(opt->optarg);
		opt->optarg = xstrdup(arg);
	}
	opt->found = 1;
	opt->set = true;

	return rc;
}

extern int spank_process_option(int optval, const char *arg)
{
	struct spank_plugin_opt *opt;
	int rc = 0;
	list_t *option_cache = get_global_option_cache();

	if (option_cache == NULL || (list_count(option_cache) == 0)) {
		debug("No spank option cache");
		return (-1);
	}

	opt = list_find_first(option_cache, (ListFindF)_opt_by_val, &optval);
	if (!opt) {
		error("Failed to find spank option for optval: %d", optval);
		return (-1);
	}

	if ((rc = _do_option_cb(opt, arg, 0))) {
		error("Invalid --%s argument: %s", opt->opt->name, arg);
		return (rc);
	}

	return (0);
}

extern int spank_process_env_options(void)
{
	char var[1024];
	const char *arg;
	struct spank_plugin_opt *option;
	list_itr_t *i;
	list_t *option_cache = get_global_option_cache();
	int rc = 0;

	if (option_cache == NULL || (list_count(option_cache) == 0))
		return 0;

	i = list_iterator_create(option_cache);
	while ((option = list_next(i))) {
		char *env_name;
		env_name = xstrdup_printf("SLURM_SPANK_%s",
					  _opt_env_name(option, var,
							sizeof(var)));
		if (!(arg = getenv(env_name))) {
			xfree(env_name);
			continue;
		}

		if ((rc = _do_option_cb(option, arg, 0))) {
			error("Invalid argument (%s) for environment variable: %s",
			      arg, env_name);
			xfree(env_name);
			break;
		}
		option->set_by_env = true;
		xfree(env_name);
	}
	list_iterator_destroy(i);

	return rc;
}

static char *
_find_word_boundary(char *str, char *from, char **next)
{
	char *p = from;

	/*
	 * Back up past any non-whitespace if we are pointing in
	 *  the middle of a word.
	 */
	while ((p != str) && !isspace ((int)*p))
		--p;

	/*
	 * Next holds next word boundary
	 */
	*next = p+1;

	/*
	 * Now move back to the end of the previous word
	 */
	while ((p != str) && isspace ((int)*p))
		--p;

	if (p == str) {
		*next = str;
		return (NULL);
	}

	return (p+1);
}

static char *
_get_next_segment (char **from, int width, char *buf, int bufsiz)
{
	int len;
	char * seg = *from;
	char *p;

	if (**from == '\0')
		return (NULL);

	if ((len = strlen (*from)) <= width) {
		*from = *from + len;
		return (seg);
	}

	if (!(p = _find_word_boundary (seg, *from + width, from))) {
		/*
		 *	Need to break up a word. Use user-supplied buffer.
		 */
		strlcpy (buf, seg, width+1);
		buf [width - 1]  = '-';
		/*
		 * Adjust from to character eaten by '-'
		 *  And return pointer to buf.
		 */
		*from = seg + width - 1;
		return (buf);
	}

	*p = '\0';

	return (seg);
}

static int
_term_columns (void)
{
	char *val;
	int  cols = 80;

	if ((val = getenv ("COLUMNS"))) {
		char *p;
		long lval = strtol (val, &p, 10);

		if (p && (*p == '\0'))
			cols = (int) lval;
	}

	return (cols);
}

static void
_spank_opt_print(struct spank_option *opt, FILE * fp, int left_pad, int width)
{
	int n;
	char *equals = "";
	char *arginfo = "";
	char *p, *q;
	char info [81];
	char seg [81];
	char buf [4096];

	int  columns = _term_columns ();
	int  descrsiz = columns - width;

	if (opt->arginfo) {
		equals = "=";
		arginfo = opt->arginfo;
	}

	n = snprintf(info, sizeof(info), "%*s--%s%s%s",
		     left_pad, "", opt->name, equals, arginfo);

	if ((n < 0) || (n > columns)) {
		const char trunc[] = "+";
		int len = strlen(trunc);
		p = info + columns - len - 1;
		snprintf(p, len + 1, "%s", trunc);
	}


	q = buf;
	strlcpy (buf, opt->usage, sizeof (buf));

	p = _get_next_segment (&q, descrsiz, seg, sizeof (seg));

	if (n < width)
		fprintf(fp, "%-*s%s\n", width, info, p);
	else
		fprintf(fp, "\n%s\n%*s%s\n", info, width, "", p);

	/* Get remaining line-wrapped lines.
	 */
	while ((p = _get_next_segment (&q, descrsiz, seg, sizeof (seg))))
		fprintf(fp, "%*s%s\n", width, "", p);

	return;
}

void spank_print_options(FILE * fp, int left_pad, int width)
{
	struct spank_plugin_opt *p;
	list_itr_t *i;
	list_t *option_cache = get_global_option_cache();

	if ((option_cache == NULL) || (list_count(option_cache) == 0))
		return;

	fprintf(fp, "\nOptions provided by plugins:\n");

	i = list_iterator_create(option_cache);
	while ((p = list_next(i))) {
		if (p->disabled)
			continue;
		_spank_opt_print(p->opt, fp, left_pad, width);
	}
	list_iterator_destroy(i);

	return;
}

#define OPT_TYPE_SPANK 0x4400

static char _canonical_char (char c)
{
	if (!isalnum ((int)c))
		return '_';
	else
		return c;
}

/*
 *  Create spank option environment variable name from option name.
 */
static char * _opt_env_name (struct spank_plugin_opt *p, char *buf, size_t siz)
{
	const char * name = p->opt->name;
	const char * pname = p->plugin->name;
	int i, n;

	strlcpy (buf, SPANK_OPTION_ENV_PREFIX, siz);

	/*
	 *  First append the plugin name associated with this option:
	 */
	n = 0;
	for (i = strlen (buf); i < siz - 1 && n < strlen (pname); i++)
	    buf[i] = _canonical_char (pname[n++]);

	/*
	 *  Append _
	 */
	buf[i] = '_';
	buf[i+1] = '\0';

	/*
	 *  Now incorporate the option name:
	 */
	n = 0;
	for (i = strlen (buf); i < siz - 1 && n < strlen (name); i++)
	    buf[i] = _canonical_char (name[n++]);
	buf[i] = '\0';

	return (buf);
}

static int _option_setenv (struct spank_plugin_opt *option)
{
	char var[1024];
	char *arg = option->optarg;

	_opt_env_name(option, var, sizeof(var));

	/*
	 * Old glibc behavior was to set the variable with an empty value if
	 * the option was NULL. Newer glibc versions will segfault instead,
	 * so feed it an empty string when necessary to maintain backwards
	 * compatibility.
	 */
	if (!option->optarg)
		arg = "";

	if (setenv(var, arg, 1) < 0)
		error("failed to set %s=%s in env", var, arg);

	/*
	 * Use the possibly-NULL value and let the command itself figure
	 * out how to handle it. This will usually result in "(null)"
	 * instead of "" used above.
	 */

	if (dyn_spank_set_job_env(var, option->optarg, 1) < 0)
		error("failed to set %s=%s in env", var, option->optarg);

	return (0);
}

static void _spank_stack_set_remote_options_env(struct spank_stack *stack)
{
	struct spank_plugin_opt *p;
	list_itr_t *i;
	list_t *option_cache;

	if (stack == NULL)
		return;

	option_cache = stack->option_cache;

	if ((option_cache == NULL) || (list_count(option_cache) == 0))
		return;

	i = list_iterator_create(option_cache);
	while ((p = list_next(i))) {
		if (p->found)
			_option_setenv (p);
	}
	list_iterator_destroy(i);
}

void spank_set_remote_options(list_t *opts)
{
	struct spank_plugin_opt *p;
	list_itr_t *i;
	list_t *option_cache;

	if (global_spank_stack == NULL)
		return;
	option_cache = global_spank_stack->option_cache;

	if ((option_cache == NULL) || (list_count(option_cache) == 0))
		return;

	i = list_iterator_create(option_cache);
	while ((p = list_next(i))) {
		char *optstr;

		if (!p->found)
			continue;

		optstr = xstrdup_printf("%s:%s", p->opt->name, p->plugin->name);

		job_options_append(opts, OPT_TYPE_SPANK, optstr,
				   p->optarg);
		xfree(optstr);
	}
	list_iterator_destroy(i);
	return;
}

struct opt_find_args {
	const char *optname;
	const char *plugin_name;
};

static int _opt_find(struct spank_plugin_opt *p,
		     struct opt_find_args *args)
{
	if (xstrcmp(p->plugin->name, args->plugin_name) != 0)
		return (0);
	if (xstrcmp(p->opt->name, args->optname) != 0)
		return (0);
	return (1);
}

static struct spank_plugin_opt *
spank_stack_find_option_by_name(struct spank_stack *stack, const char *str)
{
	struct spank_plugin_opt *opt = NULL;
	struct opt_find_args args;
	char *buf;
	char *name;
	list_t *option_cache = stack->option_cache;

	buf = xstrdup(str);
	if (!(name = xstrchr(buf, ':'))) {
		error("Malformed plugin option \"%s\" received. Ignoring",
		      str);
		xfree(buf);
		return (NULL);
	}

	*(name++) = '\0';

	args.optname = buf;
	args.plugin_name = name;

	if (option_cache) {
		opt = list_find_first(option_cache, (ListFindF) _opt_find,
				      &args);
		if (opt == NULL)
			warning("SPANK plugin \"%s\" option \"%s\" not found",
				name, buf);
	} else {
		warning("no SPANK plugin found to process option \"%s\"",
			name);
	}

	/* opt is NULL on warning */
	xfree(buf);
	return (opt);
}

spank_err_t
spank_option_getopt (spank_t sp, struct spank_option *opt, char **argp)
{
	const char *val;
	char var[1024];
	list_t *option_cache;
	struct spank_plugin_opt *spopt;

	if (argp)
		*argp = NULL;

	if (!sp->plugin) {
		error ("spank_option_getopt: Not called from a plugin!?");
		return (ESPANK_NOT_AVAIL);
	}

	if ((sp->phase == SPANK_INIT) ||
	    (sp->phase == SPANK_INIT_POST_OPT) ||
	    (sp->phase == SPANK_STEP_TASK_POST_FORK) ||
	    (sp->phase == SPANK_SLURMD_EXIT) ||
	    (sp->phase == SPANK_EXIT))
		return (ESPANK_NOT_AVAIL);

	if (!opt || !opt->name)
		return (ESPANK_BAD_ARG);

	if (opt->has_arg && !argp)
		return (ESPANK_BAD_ARG);

	/*
	 *   First check the cache:
	 */
	option_cache = sp->stack->option_cache;
	spopt = list_find_first (option_cache,
				 (ListFindF) _opt_by_name,
				 opt->name);
	if (spopt) {
		/*
		 *  Return failure if option is cached but hasn't been
		 *   used on the command line or specified by user.
		 */
		if (!spopt->found)
			return (ESPANK_ERROR);

		if (opt->has_arg && argp)
			*argp = spopt->optarg;
		return (ESPANK_SUCCESS);
	}

	/*
	 *  Otherwise, check current environment:
	 *
	 *  We need to check for variables that start with either
	 *   the default spank option env prefix, or the default
	 *   prefix + an *extra* prefix of SPANK_, in case we're
	 *   running in prolog/epilog, where Slurm prepends SPANK_
	 *   to all spank job environment variables.
	 */
	spopt = _spank_plugin_opt_create (sp->plugin, opt, 0);
	memcpy (var, "SPANK_", 6);
	if ((val = getenv (_opt_env_name(spopt, var+6, sizeof (var) - 6))) ||
	    (val = getenv (var))) {
		spopt->optarg = xstrdup (val);
		spopt->found = 1;
		if (opt->has_arg && argp)
			*argp = spopt->optarg;
	}

	/*
	 *  Cache the result
	 */
	list_append (option_cache, spopt);

	if (!spopt->found)
		return (ESPANK_ERROR);

	return (ESPANK_SUCCESS);
}

static int _opt_info_find(struct job_option_info *info,
			  struct spank_plugin_opt *opt)
{
	char *buf;
	char *name = NULL;
	bool match = true;

	buf = xstrdup(info->option);
	if ((name = xstrchr(buf, ':')))
		*(name++) = '\0';

	if (xstrcmp(opt->plugin->name, name) ||
	    xstrcmp(opt->opt->name, buf))
		match = false;

	xfree(buf);
	return match;
}

static void _spank_stack_get_remote_options_env(struct spank_stack *stack,
						char **env, list_t *found_opts)
{
	char var [1024];
	const char *arg;
	struct spank_plugin_opt *option;
	list_itr_t *i;
	list_t *option_cache = stack->option_cache;

	if (!option_cache)
		return;

	i = list_iterator_create (option_cache);
	while ((option = list_next (i))) {
		if (list_find_first(found_opts, (ListFindF) _opt_info_find,
				    option))
			continue;

		if (!(arg = getenvp (env, _opt_env_name (option, var, sizeof(var)))))
			continue;

		if (_do_option_cb(option, arg, 1)) {
			error ("spank: failed to process option %s=%s",
			       option->opt->name, arg);
		}

		/*
		 *  Now remove the environment variable.
		 *   It is no longer needed.
		 */
		unsetenvp (env, var);

	}
	list_iterator_destroy (i);
}

static void _spank_stack_get_remote_options(struct spank_stack *stack,
					    list_t *opts, list_t *found_opts)
{
	const struct job_option_info *j;
	list_itr_t *li;

	if (!opts)
		return;

	li = list_iterator_create(opts);
	while ((j = list_next(li))) {
		struct spank_plugin_opt *opt;

		if (j->type != OPT_TYPE_SPANK)
			continue;

		if (!(opt = spank_stack_find_option_by_name(stack, j->option)))
			continue;

		if (_do_option_cb(opt, j->optarg, 1)) {
			error("spank: failed to process option %s=%s",
			      opt->opt->name, j->optarg);
		}

		job_options_append(found_opts, j->type, j->option, j->optarg);
	}
	list_iterator_destroy(li);
}

/*
 *  Clear any environment variables for spank options.
 *   spank option env vars  have a prefix of SPANK_OPTION_ENV_PREFIX,
 *   or SPANK_ + SPANK_OPTION_ENV_PREFIX
 */
void spank_clear_remote_options_env (char **env)
{
	char **ep;
	int len = strlen (SPANK_OPTION_ENV_PREFIX);

	for (ep = env; *ep; ep++) {
		char *p = *ep;
		if (xstrncmp (*ep, "SPANK_", 6) == 0)
			p = *ep+6;
		if (xstrncmp (p, SPANK_OPTION_ENV_PREFIX, len) == 0) {
			char *end = strchr (p+len, '=');
			if (end) {
				char name[1024];
				memcpy (name, *ep, end - *ep);
				name [end - *ep] = '\0';
				debug("unsetenv (%s)", name);
				unsetenvp (env, name);
			}
		}
	}
	return;
}



static int tasks_execd (spank_t spank)
{
	return ( (spank->phase == SPANK_STEP_TASK_POST_FORK)
	      || (spank->phase == SPANK_STEP_TASK_EXIT)
	      || (spank->phase == SPANK_EXIT) );
}

static spank_err_t
_global_to_local_id(stepd_step_rec_t *step, uint32_t gid, uint32_t *p2uint32)
{
	int i;
	*p2uint32 = (uint32_t) -1;
	if (!step || (gid >= step->ntasks))
		return (ESPANK_BAD_ARG);
	for (i = 0; i < step->node_tasks; i++) {
		if (step->task[i]->gtid == gid) {
			*p2uint32 = step->task[i]->id;
			return (ESPANK_SUCCESS);
		}
	}
	return (ESPANK_NOEXIST);
}


/*
 *  Return 1 if spank_item_t is valid for S_TYPE_LOCAL
 */
static int _valid_in_local_context (spank_item_t item)
{
	int rc = 0;
	switch (item) {
	case S_JOB_UID:
	case S_JOB_GID:
	case S_JOB_ID:
	case S_JOB_STEPID:
	case S_JOB_ARGV:
	case S_JOB_ENV:
	case S_JOB_TOTAL_TASK_COUNT:
	case S_JOB_NNODES:
		rc = 1;
		break;
	default:
		rc = 0;
	}
	return (rc);
}

static int _valid_in_allocator_context (spank_item_t item)
{
	switch (item) {
	  case S_JOB_UID:
	  case S_JOB_GID:
		  return 1;
	  default:
		  return 0;
	}
}

static spank_err_t _check_spank_item_validity (spank_t spank, spank_item_t item)
{
	/*
	 *  Valid in all contexts:
	 */
	switch (item) {
	  case S_SLURM_VERSION:
	  case S_SLURM_VERSION_MAJOR:
	  case S_SLURM_VERSION_MINOR:
	  case S_SLURM_VERSION_MICRO:
		  return ESPANK_SUCCESS;
	  default:
		  break; /* fallthru */
	}

	/*
	 *  No spank_item_t is available in slurmd context at this time.
	 */
	if (spank->stack->type == S_TYPE_SLURMD)
		return ESPANK_NOT_AVAIL;
	else if (spank->stack->type == S_TYPE_JOB_SCRIPT) {
		if (item != S_JOB_GID && item != S_JOB_UID && item != S_JOB_ID)
			return ESPANK_NOT_AVAIL;
	}
	else if (spank->stack->type == S_TYPE_LOCAL) {
		if (!_valid_in_local_context (item))
			return ESPANK_NOT_REMOTE;
		else if (spank->job == NULL)
			return ESPANK_NOT_AVAIL;
	}
	else if (spank->stack->type == S_TYPE_ALLOCATOR) {
		if (_valid_in_allocator_context (item)) {
			if (spank->job)
				return ESPANK_SUCCESS;
			else
				return ESPANK_NOT_AVAIL;
		}
		else if (_valid_in_local_context (item))
			return ESPANK_BAD_ARG;
		else
			return ESPANK_NOT_REMOTE;
	}

	/* All items presumably valid in remote context */
	return ESPANK_SUCCESS;
}

/*
 *  Global functions for SPANK plugins
 */

const char * spank_strerror (spank_err_t err)
{
	return slurm_strerror(err);
}

int spank_symbol_supported (const char *name)
{
	int i;

	if (name == NULL)
		return (-1);

	for (i = 0; i < n_spank_syms; i++) {
		if (xstrcmp (spank_syms [i], name) == 0)
			return (1);
	}

	return (0);
}

int spank_remote(spank_t spank)
{
	if ((spank == NULL) || (spank->magic != SPANK_MAGIC))
		return (-1);
	if (spank->stack->type == S_TYPE_REMOTE)
		return (1);
	else
		return (0);
}

spank_context_t spank_context (void)
{
	if (global_spank_stack == NULL)
		return S_CTX_ERROR;
	switch (global_spank_stack->type) {
	  case S_TYPE_REMOTE:
		  return S_CTX_REMOTE;
	  case S_TYPE_LOCAL:
		  return S_CTX_LOCAL;
	  case S_TYPE_ALLOCATOR:
		  return S_CTX_ALLOCATOR;
	  case S_TYPE_SLURMD:
		  return S_CTX_SLURMD;
	  case S_TYPE_JOB_SCRIPT:
		  return S_CTX_JOB_SCRIPT;
	  default:
		  return S_CTX_ERROR;
	}

	return S_CTX_ERROR;
}

spank_err_t spank_get_item(spank_t spank, spank_item_t item, ...)
{
	int *p2int;
	uint32_t *p2uint32;
	uint64_t *p2uint64;
	uint32_t  uint32;
	uint16_t *p2uint16;
	uid_t *p2uid;
	gid_t *p2gid;
	gid_t **p2gids;
	pid_t *p2pid;
	pid_t  pid;
	char ***p2argv;
	char **p2str;
	char **p2vers;
	stepd_step_task_info_t *task;
	stepd_step_rec_t  *slurmd_job = NULL;
	struct spank_launcher_job_info *launcher_job = NULL;
	struct job_script_info *s_job_info = NULL;
	va_list vargs;
	spank_err_t rc = ESPANK_SUCCESS;

	if ((spank == NULL) || (spank->magic != SPANK_MAGIC))
		return (ESPANK_BAD_ARG);

	/*
	 *  Check for validity of the given item in the current context
	 */
	rc = _check_spank_item_validity (spank, item);
	if (rc != ESPANK_SUCCESS)
		return (rc);

	if (spank->stack->type == S_TYPE_LOCAL)
		launcher_job = spank->job;
	else if (spank->stack->type == S_TYPE_REMOTE)
		slurmd_job = spank->job;
	else if (spank->stack->type == S_TYPE_JOB_SCRIPT)
		s_job_info = spank->job;

	va_start(vargs, item);
	switch (item) {
	case S_JOB_UID:
		p2uid = va_arg(vargs, uid_t *);
		if (spank->stack->type == S_TYPE_LOCAL)
			*p2uid = launcher_job->uid;
		else if (spank->stack->type == S_TYPE_REMOTE)
			*p2uid = slurmd_job->uid;
		else if (spank->stack->type == S_TYPE_JOB_SCRIPT)
			*p2uid = s_job_info->uid;
		else
			*p2uid = getuid();
		break;
	case S_JOB_GID:
		p2gid = va_arg(vargs, gid_t *);
		if (spank->stack->type == S_TYPE_LOCAL)
			*p2gid = launcher_job->gid;
		else if (spank->stack->type == S_TYPE_REMOTE)
			*p2gid = slurmd_job->gid;
		else if (spank->stack->type == S_TYPE_JOB_SCRIPT)
			*p2gid = s_job_info->gid;
		else
			*p2gid = getgid();
		break;
	case S_JOB_SUPPLEMENTARY_GIDS:
		p2gids = va_arg(vargs, gid_t **);
		p2int = va_arg(vargs, int *);
		if (slurmd_job) {
			*p2gids = slurmd_job->gids;
			*p2int = slurmd_job->ngids;
		} else {
			*p2gids = NULL;
			*p2int = 0;
		}
		break;
	case S_JOB_ID:
		p2uint32 = va_arg(vargs, uint32_t *);
		if (spank->stack->type == S_TYPE_LOCAL)
			*p2uint32 = launcher_job->jobid;
		else if (spank->stack->type == S_TYPE_REMOTE)
			*p2uint32 = slurmd_job->step_id.job_id;
		else if (spank->stack->type == S_TYPE_JOB_SCRIPT)
			*p2uint32 = s_job_info->jobid;
		break;
	case S_JOB_STEPID:
		p2uint32 = va_arg(vargs, uint32_t *);
		if (spank->stack->type == S_TYPE_LOCAL)
			*p2uint32 = launcher_job->stepid;
		else if (slurmd_job)
			*p2uint32 = slurmd_job->step_id.step_id;
		else
			*p2uint32 = 0;
		break;
	case S_JOB_ARRAY_ID:
		p2uint32 = va_arg(vargs, uint32_t *);
		if (spank->stack->type == S_TYPE_REMOTE)
			*p2uint32 = slurmd_job->array_job_id;
		else
			*p2uint32 = 0;
		break;
	case S_JOB_ARRAY_TASK_ID:
		p2uint32 = va_arg(vargs, uint32_t *);
		if (spank->stack->type == S_TYPE_REMOTE)
			*p2uint32 = slurmd_job->array_task_id;
		else
			*p2uint32 = 0;
		break;
	case S_JOB_NNODES:
		p2uint32 = va_arg(vargs, uint32_t *);
		if (spank->stack->type == S_TYPE_LOCAL) {
			if (launcher_job->step_layout)
				*p2uint32 = launcher_job->step_layout->
					    node_cnt;
			else {
				*p2uint32 = 0;
				rc = ESPANK_ENV_NOEXIST;
			}
		} else if (slurmd_job)
			*p2uint32 = slurmd_job->nnodes;
		else
			*p2uint32 = 0;
		break;
	case S_JOB_NODEID:
		p2uint32 = va_arg(vargs, uint32_t *);
		if (slurmd_job)
			*p2uint32 = slurmd_job->nodeid;
		else
			*p2uint32 = 0;
		break;
	case S_JOB_LOCAL_TASK_COUNT:
		p2uint32 = va_arg(vargs, uint32_t *);
		if (slurmd_job)
			*p2uint32 = slurmd_job->node_tasks;
		else
			*p2uint32 = 0;
		break;
	case S_JOB_TOTAL_TASK_COUNT:
		p2uint32 = va_arg(vargs, uint32_t *);
		if (spank->stack->type == S_TYPE_LOCAL) {
			if (launcher_job->step_layout)
				*p2uint32 = launcher_job->step_layout->
					    task_cnt;
			else {
				*p2uint32 = 0;
				rc = ESPANK_ENV_NOEXIST;
			}
		} else if (slurmd_job)
			*p2uint32 = slurmd_job->ntasks;
		else
			*p2uint32 = 0;
		break;
	case S_JOB_NCPUS:
		p2uint16 = va_arg(vargs, uint16_t *);
		if (slurmd_job)
			*p2uint16 = slurmd_job->cpus;
		else
			*p2uint16 = 0;
		break;
	case S_STEP_CPUS_PER_TASK:
		p2uint32 = va_arg(vargs, uint32_t *);
		if (slurmd_job)
			*p2uint32 = slurmd_job->cpus_per_task;
		else
			*p2uint32 = 0;
		break;
	case S_JOB_ARGV:
		p2int = va_arg(vargs, int *);
		p2argv = va_arg(vargs, char ***);
		if (spank->stack->type == S_TYPE_LOCAL) {
			*p2int = launcher_job->argc;
			*p2argv = launcher_job->argv;
		} else if (slurmd_job) {
			*p2int = slurmd_job->argc;
			*p2argv = slurmd_job->argv;
		} else {
			*p2int = 0;
			*p2argv = NULL;
		}
		break;
	case S_JOB_ENV:
		p2argv = va_arg(vargs, char ***);
		if (slurmd_job)
			*p2argv = slurmd_job->env;
		else
			*p2argv = NULL;
		break;
	case S_TASK_ID:
		p2int = va_arg(vargs, int *);
		if (!spank->task) {
			*p2int = -1;
			rc = ESPANK_NOT_TASK;
		} else {
			*p2int = spank->task->id;
		}
		break;
	case S_TASK_GLOBAL_ID:
		p2uint32 = va_arg(vargs, uint32_t *);
		if (!spank->task) {
			rc = ESPANK_NOT_TASK;
		} else {
			*p2uint32 = spank->task->gtid;
		}
		break;
	case S_TASK_EXIT_STATUS:
		p2int = va_arg(vargs, int *);
		if (!spank->task || !spank->task->exited) {
			rc = ESPANK_NOT_TASK;
		} else {
			*p2int = spank->task->estatus;
		}
		break;
	case S_TASK_PID:
		p2pid = va_arg(vargs, pid_t *);
		if (!spank->task) {
			rc = ESPANK_NOT_TASK;
			*p2pid = 0;
		} else {
			*p2pid = spank->task->pid;
		}
		break;
	case S_JOB_PID_TO_GLOBAL_ID:
		pid = va_arg(vargs, pid_t);
		p2uint32 = va_arg(vargs, uint32_t *);
		*p2uint32 = (uint32_t) -1;

		if (!tasks_execd(spank))
			rc = ESPANK_NOT_EXECD;
		else if (!(task = job_task_info_by_pid (slurmd_job, pid)))
			rc = ESPANK_NOEXIST;
		else
			*p2uint32 = task->gtid;
		break;
	case S_JOB_PID_TO_LOCAL_ID:
		pid = va_arg(vargs, pid_t);
		p2uint32 = va_arg(vargs, uint32_t *);
		*p2uint32 = (uint32_t) -1;

		if (!tasks_execd(spank))
			rc = ESPANK_NOT_EXECD;
		else if (!(task = job_task_info_by_pid (slurmd_job, pid)))
			rc = ESPANK_NOEXIST;
		else
			*p2uint32 = task->id;
		break;
	case S_JOB_LOCAL_TO_GLOBAL_ID:
		uint32 = va_arg(vargs, uint32_t);
		p2uint32 = va_arg(vargs, uint32_t *);
		*p2uint32 = (uint32_t) -1;

		if (slurmd_job && (uint32 <= slurmd_job->node_tasks) &&
		    slurmd_job->task && slurmd_job->task[uint32]) {
			*p2uint32 = slurmd_job->task[uint32]->gtid;
		} else
			rc = ESPANK_NOEXIST;
		break;
	case S_JOB_GLOBAL_TO_LOCAL_ID:
		uint32 = va_arg(vargs, uint32_t);
		p2uint32 = va_arg(vargs, uint32_t *);
		rc = _global_to_local_id (slurmd_job, uint32, p2uint32);
		break;
	case S_JOB_ALLOC_CORES:
		p2str = va_arg(vargs, char **);
		if (slurmd_job)
			*p2str = slurmd_job->job_alloc_cores;
		else
			*p2str = NULL;
		break;
	case S_JOB_ALLOC_MEM:
		p2uint64 = va_arg(vargs, uint64_t *);
		if (slurmd_job)
			*p2uint64 = slurmd_job->job_mem;
		else
			*p2uint64 = 0;
		break;
	case S_STEP_ALLOC_CORES:
		p2str = va_arg(vargs, char **);
		if (slurmd_job)
			*p2str = slurmd_job->step_alloc_cores;
		else
			*p2str = NULL;
		break;
	case S_STEP_ALLOC_MEM:
		p2uint64 = va_arg(vargs, uint64_t *);
		if (slurmd_job)
			*p2uint64 = slurmd_job->step_mem;
		else
			*p2uint64 = 0;
		break;
	case S_SLURM_RESTART_COUNT:
		p2uint32 = va_arg(vargs, uint32_t *);
		if (slurmd_job)
			*p2uint32 = slurmd_job->restart_cnt;
		else
			*p2uint32 = 0;
		break;
	case S_SLURM_VERSION:
		p2vers = va_arg(vargs, char  **);
		*p2vers = SLURM_VERSION_STRING;
		break;
	case S_SLURM_VERSION_MAJOR:
		p2vers = va_arg(vargs, char  **);
		*p2vers = SLURM_MAJOR;
		break;
	case S_SLURM_VERSION_MINOR:
		p2vers = va_arg(vargs, char  **);
		*p2vers = SLURM_MINOR;
		break;
	case S_SLURM_VERSION_MICRO:
		p2vers = va_arg(vargs, char  **);
		*p2vers = SLURM_MICRO;
		break;
	default:
		rc = ESPANK_BAD_ARG;
		break;
	}
	va_end(vargs);
	return (rc);
}

spank_err_t spank_env_access_check (spank_t spank)
{
	if ((spank == NULL) || (spank->magic != SPANK_MAGIC))
		return (ESPANK_BAD_ARG);
	if (spank->stack->type != S_TYPE_REMOTE)
		return (ESPANK_NOT_REMOTE);
	if (spank->job == NULL)
		return (ESPANK_BAD_ARG);
	return (ESPANK_SUCCESS);

}

spank_err_t spank_getenv(spank_t spank, const char *var, char *buf,
			 int len)
{
	char *val;
	spank_err_t err = spank_env_access_check (spank);

	if (err != ESPANK_SUCCESS)
		return (err);

	if (len < 0)
		return (ESPANK_BAD_ARG);

	if (!(val = getenvp(((stepd_step_rec_t *) spank->job)->env, var)))
		return (ESPANK_ENV_NOEXIST);

	if (strlcpy(buf, val, len) >= len)
		return (ESPANK_NOSPACE);

	return (ESPANK_SUCCESS);
}

spank_err_t spank_setenv(spank_t spank, const char *var, const char *val,
			 int overwrite)
{
	stepd_step_rec_t *step;
	spank_err_t err = spank_env_access_check (spank);

	if (err != ESPANK_SUCCESS)
		return (err);

	if ((var == NULL) || (val == NULL))
		return (ESPANK_BAD_ARG);

	step = spank->job;

	if (getenvp(step->env, var) && !overwrite)
		return (ESPANK_ENV_EXISTS);

	if (setenvf(&step->env, var, "%s", val) < 0)
		return (ESPANK_ERROR);

	return (ESPANK_SUCCESS);
}

spank_err_t spank_unsetenv (spank_t spank, const char *var)
{
	spank_err_t err = spank_env_access_check (spank);

	if (err != ESPANK_SUCCESS)
		return (err);

	if (var == NULL)
		return (ESPANK_BAD_ARG);

	unsetenvp(((stepd_step_rec_t *) spank->job)->env, var);

	return (ESPANK_SUCCESS);
}


/*
 *  Dynamically loaded versions of spank_*_job_env
 */
const char *dyn_spank_get_job_env(const char *name)
{
	void *h = dlopen(NULL, 0);
	char * (*fn)(const char *n);
	char *rc;

	fn = dlsym(h, "spank_get_job_env");
	if (fn == NULL) {
		(void) dlclose(h);
		return NULL;
	}

	rc = ((*fn) (name));
/*	(void) dlclose(h);	NOTE: DO NOT CLOSE OR SPANK WILL BREAK */
	return rc;
}

int dyn_spank_set_job_env(const char *n, const char *v, int overwrite)
{
	void *h = dlopen(NULL, 0);
	int (*fn)(const char *n, const char *v, int overwrite);
	int rc;

	fn = dlsym(h, "spank_set_job_env");
	if (fn == NULL) {
		(void) dlclose(h);
		return (-1);
	}

	rc = ((*fn) (n, v, overwrite));
/*	(void) dlclose(h);	NOTE: DO NOT CLOSE OR SPANK WILL BREAK */
	return rc;
}

extern int dyn_spank_unset_job_env(const char *n)
{
	void *h = dlopen(NULL, 0);
	int (*fn)(const char *n);
	int rc;

	fn = dlsym(h, "spank_unset_job_env");
	if (fn == NULL) {
		(void) dlclose(h);
		return (-1);
	}

	rc = ((*fn) (n));
/*	(void) dlclose(h);	NOTE: DO NOT CLOSE OR SPANK WILL BREAK */
	return rc;
}

static spank_err_t spank_job_control_access_check (spank_t spank)
{
	if ((spank == NULL) || (spank->magic != SPANK_MAGIC))
		return (ESPANK_BAD_ARG);

	if (spank_remote (spank))
		return (ESPANK_NOT_LOCAL);

	if (spank->stack->type == S_TYPE_SLURMD)
		return (ESPANK_NOT_AVAIL);

	return (ESPANK_SUCCESS);
}


spank_err_t spank_job_control_getenv (spank_t spank, const char *var,
			char *buf, int len)
{
	const char *val;
	spank_err_t err;

	if ((err = spank_job_control_access_check (spank)))
		return (err);

	if ((var == NULL) || (buf == NULL) || (len <= 0))
		return (ESPANK_BAD_ARG);

	val = dyn_spank_get_job_env (var);
	if (val == NULL)
		return (ESPANK_ENV_NOEXIST);

	if (strlcpy (buf, val, len) >= len)
		return (ESPANK_NOSPACE);

	return (ESPANK_SUCCESS);
}

spank_err_t spank_job_control_setenv (spank_t spank, const char *var,
			const char *val, int overwrite)
{
	spank_err_t err;

	if ((err = spank_job_control_access_check (spank)))
		return (err);

	if ((var == NULL) || (val == NULL))
		return (ESPANK_BAD_ARG);

	if (dyn_spank_set_job_env (var, val, overwrite) < 0)
		return (ESPANK_BAD_ARG);

	return (ESPANK_SUCCESS);
}

spank_err_t spank_job_control_unsetenv (spank_t spank, const char *var)
{
	spank_err_t err;

	if ((err = spank_job_control_access_check (spank)))
		return (err);

	if (var == NULL)
		return (ESPANK_BAD_ARG);

	if (dyn_spank_unset_job_env (var) < 0)
		return (ESPANK_BAD_ARG);

	return (ESPANK_SUCCESS);
}

spank_err_t spank_prepend_task_argv(spank_t spank, int argc,
				    const char *argv[])
{
	int new_argc, j = 0;
	char **new_argv;

	if (!spank || (spank->magic != SPANK_MAGIC) || !argv)
		return ESPANK_BAD_ARG;

	if (!spank->task || !spank->task->argv ||
	    ((spank->phase != SPANK_STEP_TASK_INIT_PRIV) &&
	     (spank->phase != SPANK_STEP_USER_TASK_INIT)))
		return ESPANK_NOT_TASK;

	new_argc = argc + spank->task->argc;
	new_argv = xcalloc(new_argc + 1, sizeof(char *));

	for (int i = 0; i < argc && argv[i]; i++)
		new_argv[j++] = xstrdup(argv[i]);
	for (int i = 0; i < spank->task->argc && spank->task->argv[i]; i++)
		new_argv[j++] = spank->task->argv[i];
	new_argv[j] = NULL;

	spank->task->argc = new_argc;
	spank->task->argv = new_argv;

	return ESPANK_SUCCESS;
}

/*
 * spank_get_plugin_names
 * Get names of all spank plugins
 *
 * Parameters:
 * IN/OUT names	- Pointer to char ** (should be NULL when called) output of
 *		  function is allocated memory for the array of string
 *		  pointers, and allocated memory for the strings. Array will
 *		  be NULL terminated. Caller should manage the memory.
 * Returns:
 * 		- Number of allocated strings (excluding NULL terminator)
 */
size_t spank_get_plugin_names(char ***names)
{
	struct spank_plugin *p;
	list_itr_t *i;
	size_t n_names = 0;

	if (!global_spank_stack)
		return 0;

	i = list_iterator_create(global_spank_stack->plugin_list);
	while ((p = list_next(i))) {
		*names = xrecalloc(*names, ++n_names + 1, sizeof(char *));
		(*names)[n_names] = NULL;
		(*names)[n_names - 1] = xstrdup(p->name);
	}
	list_iterator_destroy(i);
	return n_names;
}

/*
 * spank_get_plugin_option_names
 * Get names of all spank plugins
 *
 * Parameters:
 * IN plugin_name	- Name of spank plugin being considered
 *			  (e.g., from spank_get_plugin_names)
 * IN/OUT opts		- Pointer to char ** (should be NULL when called)
 *			  output of function is allocated memory for the array
 *			  of string pointers, and allocated memory for the
 *			  strings. Array will be NULL terminated. Caller
 *			  should manage the memory.
 * Returns:
 *			- Number of allocated strings (excluding NULL
 *			  terminator)
 */
size_t spank_get_plugin_option_names(const char *plugin_name, char ***opts)
{
	struct spank_plugin_opt *spopt;
	size_t nopts = 0;

	list_t *options = get_global_option_cache();
	list_itr_t *i;

	i = list_iterator_create(options);
	while ((spopt = list_next(i))) {
		if (spopt->disabled)
			continue;
		if (!xstrcmp(spopt->plugin->name, plugin_name)) {
			*opts = xrecalloc(*opts, ++nopts + 1, sizeof(char *));
			(*opts)[nopts] = NULL;
			(*opts)[nopts - 1] = xstrdup(spopt->opt->name);
			continue;
		}
	}
	list_iterator_destroy(i);
	return nopts;
}

/*
 * Get option value by common option name
 */
extern char *spank_option_get(char *name)
{
	list_t *option_cache = get_global_option_cache();
	struct spank_plugin_opt *spopt;

	if (!option_cache)
		return NULL;

	spopt = list_find_first(option_cache,
			(ListFindF) _opt_by_name, name);

	if (spopt) {
		if (spopt->set && !spopt->optarg)
			return xstrdup("set");
		if (!spopt->set && !spopt->opt->has_arg)
			return xstrdup("unset");
		if (spopt->optarg)
			return xstrdup(spopt->optarg);
	}
	return NULL;
}

/*
 * Get plugin name by common option name
 */
extern char *spank_option_plugin(char *optname)
{
	list_t *option_cache = get_global_option_cache();
	struct spank_plugin_opt *spopt;

	if (!option_cache)
		return NULL;

	spopt = list_find_first(option_cache,
			(ListFindF) _opt_by_name, optname);

	if (spopt)
		return xstrdup(spopt->plugin->name);
	return NULL;
}

/*
 * Is option set? Discover by common option name
 */
extern bool spank_option_isset(char *name)
{
	list_t *option_cache = get_global_option_cache();
	struct spank_plugin_opt *spopt;

	if (!option_cache)
		return NULL;

	spopt = list_find_first(option_cache,
			(ListFindF) _opt_by_name, name);
	if (spopt)
		return spopt->set;
	return false;
}

/*
 * Function for iterating through all the common option data structure
 * and returning (via parameter arguments) the name and value of each
 * set slurm option.
 *
 * OUT plugin	- pointer to string to store the plugin name
 * OUT name	- pointer to string to store the option name
 * OUT value	- pointer to string to store the value
 * IN/OUT state	- internal state, should point to NULL for the first call
 * RETURNS	- true if plugin/name/value set; false if no more options
 */
extern bool spank_option_get_next_set(char **plugin, char **name,
				      char **value, void **state)
{
	list_t *option_cache = get_global_option_cache();
	list_itr_t **iter = *state;
	struct spank_plugin_opt *spopt;

	if (option_cache == NULL)
		return NULL;

	if (!iter) {
		iter = xmalloc(sizeof(*iter));
		*iter = list_iterator_create(option_cache);
		*state = iter;
	}

	while ((spopt = list_next(*iter))) {
		if (!spopt->set)
			continue;
		*plugin = xstrdup(spopt->plugin->name);
		*name = xstrdup(spopt->opt->name);
		if (spopt->optarg)
			*value = xstrdup(spopt->optarg);
		else if (spopt->set)
			*value = xstrdup("set");
		else if (!spopt->set && !spopt->opt->has_arg)
			*value = xstrdup("unset");
		return true;
	}

	list_iterator_destroy(*iter);
	xfree(iter);
	*state = NULL;

	return false;
}

extern bool spank_has_prolog(void)
{
	return has_prolog;
}

extern bool spank_has_epilog(void)
{
	return has_epilog;
}

extern bool spank_has_user_init(void)
{
	return has_user_init;
}

extern bool spank_has_task_post_fork(void)
{
	return has_task_post_fork;
}

extern bool spank_has_task_exit(void)
{
	return has_task_exit;
}
