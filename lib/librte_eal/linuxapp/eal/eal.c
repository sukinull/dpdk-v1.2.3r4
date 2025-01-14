/*-
 *   BSD LICENSE
 * 
 *   Copyright(c) 2010-2012 Intel Corporation. All rights reserved.
 *   Copyright(c) 2012-2013 6WIND.
 *   All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without 
 *   modification, are permitted provided that the following conditions 
 *   are met:
 * 
 *     * Redistributions of source code must retain the above copyright 
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright 
 *       notice, this list of conditions and the following disclaimer in 
 *       the documentation and/or other materials provided with the 
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its 
 *       contributors may be used to endorse or promote products derived 
 *       from this software without specific prior written permission.
 * 
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR 
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 *  version: DPDK.L.1.2.3-3
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <syslog.h>
#include <getopt.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stddef.h>
#include <errno.h>
#include <limits.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/queue.h>

#include <rte_common.h>
#include <rte_debug.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_random.h>
#include <rte_cycles.h>
#include <rte_string_fns.h>
#include <rte_cpuflags.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_common.h>
#include <rte_version.h>

#include "eal_private.h"
#include "eal_thread.h"
#include "eal_internal_cfg.h"
#include "eal_fs_paths.h"
#include "eal_hugepages.h"

#define OPT_HUGE_DIR    "huge-dir"
#define OPT_PROC_TYPE   "proc-type"
#define OPT_NO_SHCONF   "no-shconf"
#define OPT_NO_HPET     "no-hpet"
#define OPT_VMWARE_TSC_MAP   "vmware-tsc-map"
#define OPT_NO_PCI      "no-pci"
#define OPT_NO_HUGE     "no-huge"
#define OPT_FILE_PREFIX "file-prefix"
#define OPT_SYSLOG      "syslog"

#define RTE_EAL_BLACKLIST_SIZE	0x100

#define MEMSIZE_IF_NO_HUGE_PAGE (64ULL * 1024ULL * 1024ULL)

#define GET_BLACKLIST_FIELD(in, fd, lim, dlm)                   \
{                                                               \
	unsigned long val;                                      \
	char *end;                                              \
	errno = 0;                                              \
	val = strtoul((in), &end, 16);                          \
	if (errno != 0 || end[0] != (dlm) || val > (lim))       \
		return (-EINVAL);                               \
	(fd) = (typeof (fd))val;                                \
	(in) = end + 1;                                         \
}

TAILQ_HEAD(shared_driver_list, shared_driver);

/* Definition for shared object drivers. */
struct shared_driver {
	TAILQ_ENTRY(shared_driver) next;

	char    name[PATH_MAX];
	void*   lib_handle;
};

/* List of external loadable drivers */
static struct shared_driver_list solib_list =
TAILQ_HEAD_INITIALIZER(solib_list);

/* early configuration structure, when memory config is not mmapped */
static struct rte_mem_config early_mem_config;

/* define fd variable here, because file needs to be kept open for the
 * duration of the program, as we hold a write lock on it in the primary proc */
static int mem_cfg_fd = -1;

static struct flock wr_lock = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = offsetof(struct rte_mem_config, memseg),
		.l_len = sizeof(early_mem_config.memseg),
};

/* Address of global and public configuration */
static struct rte_config rte_config = {
		.mem_config = &early_mem_config,
};

static struct rte_pci_addr eal_dev_blacklist[RTE_EAL_BLACKLIST_SIZE];

/* internal configuration (per-core) */
struct lcore_config lcore_config[RTE_MAX_LCORE];

/* internal configuration */
struct internal_config internal_config;

/* used by rte_rdtsc() */
int rte_cycles_vmware_tsc_map;

/* Return a pointer to the configuration structure */
struct rte_config *
rte_eal_get_configuration(void)
{
	return &rte_config;
}

/* create memory configuration in shared/mmap memory. Take out
 * a write lock on the memsegs, so we can auto-detect primary/secondary.
 * This means we never close the file while running (auto-close on exit).
 * We also don't lock the whole file, so that in future we can use read-locks
 * on other parts, e.g. memzones, to detect if there are running secondary
 * processes. */
static void
rte_eal_config_create(void)
{
	void *rte_mem_cfg_addr;
	int retval;

	const char *pathname = eal_runtime_config_path();

	if (internal_config.no_shconf)
		return;

	if (mem_cfg_fd < 0){
		mem_cfg_fd = open(pathname, O_RDWR | O_CREAT, 0660);
		if (mem_cfg_fd < 0)
			rte_panic("Cannot open '%s' for rte_mem_config\n", pathname);
	}

	retval = ftruncate(mem_cfg_fd, sizeof(*rte_config.mem_config));
	if (retval < 0){
		close(mem_cfg_fd);
		rte_panic("Cannot resize '%s' for rte_mem_config\n", pathname);
	}

	retval = fcntl(mem_cfg_fd, F_SETLK, &wr_lock);
	if (retval < 0){
		close(mem_cfg_fd);
		rte_exit(EXIT_FAILURE, "Cannot create lock on '%s'. Is another primary "
				"process running?\n", pathname);
	}

	rte_mem_cfg_addr = mmap(NULL, sizeof(*rte_config.mem_config),
			   PROT_READ | PROT_WRITE, MAP_SHARED, mem_cfg_fd, 0);

	if (rte_mem_cfg_addr == MAP_FAILED){
		rte_panic("Cannot mmap memory for rte_config\n");
	}
	rte_config.mem_config = (struct rte_mem_config *) rte_mem_cfg_addr;
	memcpy(rte_config.mem_config, &early_mem_config,
			sizeof(early_mem_config));
}

/* attach to an existing shared memory config */
static void
rte_eal_config_attach(void)
{
	void *rte_mem_cfg_addr;
	const char *pathname = eal_runtime_config_path();

	if (internal_config.no_shconf)
		return;

	if (mem_cfg_fd < 0){
		mem_cfg_fd = open(pathname, O_RDONLY);
		if (mem_cfg_fd < 0)
			rte_panic("Cannot open '%s' for rte_mem_config\n", pathname);
	}

	rte_mem_cfg_addr = mmap(NULL, sizeof(*rte_config.mem_config), PROT_READ,
			MAP_SHARED, mem_cfg_fd, 0);
	close(mem_cfg_fd);
	if (rte_mem_cfg_addr == MAP_FAILED)
		rte_panic("Cannot mmap memory for rte_config\n");

	rte_config.mem_config = (struct rte_mem_config *) rte_mem_cfg_addr;
}

/* Detect if we are a primary or a secondary process */
static enum rte_proc_type_t
eal_proc_type_detect(void)
{
	enum rte_proc_type_t ptype = RTE_PROC_PRIMARY;
	const char *pathname = eal_runtime_config_path();

	/* if we can open the file but not get a write-lock we are a secondary
	 * process. NOTE: if we get a file handle back, we keep that open
	 * and don't close it to prevent a race condition between multiple opens */
	if (((mem_cfg_fd = open(pathname, O_RDWR)) >= 0) &&
			(fcntl(mem_cfg_fd, F_SETLK, &wr_lock) < 0))
		ptype = RTE_PROC_SECONDARY;

	RTE_LOG(INFO, EAL, "Auto-detected process type: %s\n",
			ptype == RTE_PROC_PRIMARY ? "PRIMARY" : "SECONDARY");

	return ptype;
}

/* Sets up rte_config structure with the pointer to shared memory config.*/
static void
rte_config_init(void)
{
	/* set the magic in configuration structure */
	rte_config.magic = RTE_MAGIC;
	rte_config.process_type = (internal_config.process_type == RTE_PROC_AUTO) ?
			eal_proc_type_detect() : /* for auto, detect the type */
			internal_config.process_type; /* otherwise use what's already set */

	switch (rte_config.process_type){
	case RTE_PROC_PRIMARY:
		rte_eal_config_create();
		break;
	case RTE_PROC_SECONDARY:
		rte_eal_config_attach();
		break;
	case RTE_PROC_AUTO:
	case RTE_PROC_INVALID:
		rte_panic("Invalid process type\n");
	}
}

/* display usage */
static void
eal_usage(const char *prgname)
{
	printf("\nUsage: %s -c COREMASK -n NUM [-m NB] [-r NUM] [-b <domain:bus:devid.func>]"
	       "[--proc-type primary|secondary|auto] \n\n"
	       "EAL options:\n"
	       "  -c COREMASK: A hexadecimal bitmask of cores to run on\n"
	       "  -n NUM     : Number of memory channels\n"
		   "  -v         : Display version information on startup\n"
	       "  -b <domain:bus:devid.func>: to prevent EAL from using specified PCI device\n"
	       "               (multiple -b options are alowed)\n"
	       "  -m MB      : memory to allocate (default = size of hugemem)\n"
	       "  -r NUM     : force number of memory ranks (don't detect)\n"
	       "  -d LIB.so  : add driver (can be used multiple times)\n"
	       "  --"OPT_SYSLOG"   : set syslog facility\n"
	       "  --"OPT_HUGE_DIR" : directory where hugetlbfs is mounted\n"
	       "  --"OPT_PROC_TYPE": type of this process\n"
	       "  --"OPT_FILE_PREFIX": prefix for hugepage filenames\n"
	       "  --"OPT_VMWARE_TSC_MAP"  : use VMware TSC map instead of native RDTSC\n\n"
	       "\nEAL options for DEBUG use only:\n"
	       "  --"OPT_NO_HUGE"  : use malloc instead of hugetlbfs\n"
	       "  --"OPT_NO_PCI"   : disable pci\n"
	       "  --"OPT_NO_HPET"  : disable hpet\n"
	       "  --"OPT_NO_SHCONF": no shared config (mmap'd files)\n\n",
	       prgname);
}

/*
 * Parse the coremask given as argument (hexadecimal string) and fill
 * the global configuration (core role and core count) with the parsed
 * value.
 */
static int
eal_parse_coremask(const char *coremask)
{
	struct rte_config *cfg = rte_eal_get_configuration();
	unsigned i;
	char *end = NULL;
	unsigned long long cm;
	unsigned count = 0;

	/* parse hexadecimal string */
	cm = strtoull(coremask, &end, 16);
	if ((coremask[0] == '\0') || (end == NULL) || (*end != '\0') || (cm == 0))
		return -1;

	RTE_LOG(DEBUG, EAL, "coremask set to %llx\n", cm);
	/* set core role and core count */
	for (i = 0; i < RTE_MAX_LCORE; i++) {
		if ((1ULL << i) & cm) {
			if (count == 0)
				cfg->master_lcore = i;
			cfg->lcore_role[i] = ROLE_RTE;
			count++;
		}
		else {
			cfg->lcore_role[i] = ROLE_OFF;
		}
	}
	return 0;
}

static inline uint64_t
eal_get_hugepage_mem_size(void)
{
	uint64_t size = 0;
	unsigned i;

	for (i = 0; i < internal_config.num_hugepage_sizes; i++){
		struct hugepage_info *hpi = &internal_config.hugepage_info[i];
		if (hpi->hugedir != NULL)
			size += hpi->hugepage_sz * hpi->num_pages;
	}

	return (size);
}

static enum rte_proc_type_t
eal_parse_proc_type(const char *arg)
{
	if (strncasecmp(arg, "primary", sizeof("primary")) == 0)
		return RTE_PROC_PRIMARY;
	if (strncasecmp(arg, "secondary", sizeof("secondary")) == 0)
		return RTE_PROC_SECONDARY;
	if (strncasecmp(arg, "auto", sizeof("auto")) == 0)
		return RTE_PROC_AUTO;

	return RTE_PROC_INVALID;
}

static int
eal_parse_blacklist(const char *input,  struct rte_pci_addr *dev2bl)
{
	GET_BLACKLIST_FIELD(input, dev2bl->domain, UINT16_MAX, ':');
	GET_BLACKLIST_FIELD(input, dev2bl->bus, UINT8_MAX, ':');
	GET_BLACKLIST_FIELD(input, dev2bl->devid, UINT8_MAX, '.');
	GET_BLACKLIST_FIELD(input, dev2bl->function, UINT8_MAX, 0);
	return (0);
}

static ssize_t
eal_parse_blacklist_opt(const char *optarg, size_t idx)
{
	if (idx >= sizeof (eal_dev_blacklist) / sizeof (eal_dev_blacklist[0])) {
		RTE_LOG(ERR, EAL,
		    "%s - too many devices to blacklist...\n",
		    optarg);
		return (-EINVAL);
	} else if (eal_parse_blacklist(optarg, eal_dev_blacklist + idx) != 0) {
		RTE_LOG(ERR, EAL,
		    "%s - invalid device to blacklist...\n",
		    optarg);
		return (-EINVAL);
	}

	idx += 1;
	return (idx);
}

static int
eal_parse_syslog(const char *facility)
{
	int i;
	static struct {
		const char *name;
		int value;
	} map[] = {
		{ "auth", LOG_AUTH },
		{ "cron", LOG_CRON },
		{ "daemon", LOG_DAEMON },
		{ "ftp", LOG_FTP },
		{ "kern", LOG_KERN },
		{ "lpr", LOG_LPR },
		{ "mail", LOG_MAIL },
		{ "news", LOG_NEWS },
		{ "syslog", LOG_SYSLOG },
		{ "user", LOG_USER },
		{ "uucp", LOG_UUCP },
		{ "local0", LOG_LOCAL0 },
		{ "local1", LOG_LOCAL1 },
		{ "local2", LOG_LOCAL2 },
		{ "local3", LOG_LOCAL3 },
		{ "local4", LOG_LOCAL4 },
		{ "local5", LOG_LOCAL5 },
		{ "local6", LOG_LOCAL6 },
		{ "local7", LOG_LOCAL7 },
		{ NULL, 0 }
	};

	for (i = 0; map[i].name; i++) {
		if (!strcmp(facility, map[i].name)) {
			internal_config.syslog_facility = map[i].value;
			return 0;
		}
	}
	return -1;
}


/* Parse the argument given in the command line of the application */
static int
eal_parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	int option_index;
	int coremask_ok = 0;
	ssize_t blacklist_index = 0;;
	char *prgname = argv[0];
	static struct option lgopts[] = {
		{OPT_NO_HUGE, 0, 0, 0},
		{OPT_NO_PCI, 0, 0, 0},
		{OPT_NO_HPET, 0, 0, 0},
		{OPT_VMWARE_TSC_MAP, 0, 0, 0},
		{OPT_HUGE_DIR, 1, 0, 0},
		{OPT_NO_SHCONF, 0, 0, 0},
		{OPT_PROC_TYPE, 1, 0, 0},
		{OPT_FILE_PREFIX, 1, 0, 0},
		{OPT_SYSLOG, 1, 0, 0},
		{0, 0, 0, 0}
	};
	struct shared_driver *solib;

	argvopt = argv;

	internal_config.memory = 0;
	internal_config.force_nrank = 0;
	internal_config.force_nchannel = 0;
	internal_config.hugefile_prefix = HUGEFILE_PREFIX_DEFAULT;
	internal_config.hugepage_dir = NULL;
	internal_config.syslog_facility = LOG_DAEMON;
#ifdef RTE_LIBEAL_USE_HPET
	internal_config.no_hpet = 0;
#else
	internal_config.no_hpet = 1;
#endif

	internal_config.vmware_tsc_map = 0;

	while ((opt = getopt_long(argc, argvopt, "b:c:d:m:n:r:v",
				  lgopts, &option_index)) != EOF) {

		switch (opt) {
		/* blacklist */
		case 'b':
			if ((blacklist_index = eal_parse_blacklist_opt(optarg,
			    blacklist_index)) < 0) {
				eal_usage(prgname);
				return (-1);
			}
			break;
		/* coremask */
		case 'c':
			if (eal_parse_coremask(optarg) < 0) {
				RTE_LOG(ERR, EAL, "invalid coremask\n");
				eal_usage(prgname);
				return -1;
			}
			coremask_ok = 1;
			break;
		/* force loading of external driver */
		case 'd':
			solib = malloc(sizeof(*solib));
			if (solib == NULL) {
				RTE_LOG(ERR, EAL, "malloc(solib) failed\n");
				return -1;
			}
			memset(solib, 0, sizeof(*solib));
			strncpy(solib->name, optarg, PATH_MAX-1);
			solib->name[PATH_MAX-1] = 0;
			TAILQ_INSERT_TAIL(&solib_list, solib, next);
			break;
		/* size of memory */
		case 'm':
			internal_config.memory = atoi(optarg);
			internal_config.memory *= 1024ULL;
			internal_config.memory *= 1024ULL;
			break;
		/* force number of channels */
		case 'n':
			internal_config.force_nchannel = atoi(optarg);
			if (internal_config.force_nchannel == 0 ||
			    internal_config.force_nchannel > 4) {
				RTE_LOG(ERR, EAL, "invalid channel number\n");
				eal_usage(prgname);
				return -1;
			}
			break;
		/* force number of ranks */
		case 'r':
			internal_config.force_nrank = atoi(optarg);
			if (internal_config.force_nrank == 0 ||
			    internal_config.force_nrank > 16) {
				RTE_LOG(ERR, EAL, "invalid rank number\n");
				eal_usage(prgname);
				return -1;
			}
			break;
		case 'v':
			/* since message is explicitly requested by user, we
			 * write message at highest log level so it can always be seen
			 * even if info or warning messages are disabled */
			RTE_LOG(CRIT, EAL, "RTE Version: '%s'\n", rte_version());
			break;

		/* long options */
		case 0:
			if (!strcmp(lgopts[option_index].name, OPT_NO_HUGE)) {
				internal_config.no_hugetlbfs = 1;
			}
			else if (!strcmp(lgopts[option_index].name, OPT_NO_PCI)) {
				internal_config.no_pci = 1;
			}
			else if (!strcmp(lgopts[option_index].name, OPT_NO_HPET)) {
				internal_config.no_hpet = 1;
			}
			else if (!strcmp(lgopts[option_index].name, OPT_VMWARE_TSC_MAP)) {
				internal_config.vmware_tsc_map = 1;
			}
			else if (!strcmp(lgopts[option_index].name, OPT_NO_SHCONF)) {
				internal_config.no_shconf = 1;
			}
			else if (!strcmp(lgopts[option_index].name, OPT_HUGE_DIR)) {
				internal_config.hugepage_dir = optarg;
			}
			else if (!strcmp(lgopts[option_index].name, OPT_PROC_TYPE)) {
				internal_config.process_type = eal_parse_proc_type(optarg);
			}
			else if (!strcmp(lgopts[option_index].name, OPT_FILE_PREFIX)) {
				internal_config.hugefile_prefix = optarg;
			}
			else if (!strcmp(lgopts[option_index].name, OPT_SYSLOG)) {
				if (eal_parse_syslog(optarg) < 0) {
					RTE_LOG(ERR, EAL, "invalid parameters for --"
							OPT_SYSLOG "\n");
					eal_usage(prgname);
					return -1;
				}
			}
			break;

		default:
			eal_usage(prgname);
			return -1;
		}
	}

	/* sanity checks */
	if (!coremask_ok) {
		RTE_LOG(ERR, EAL, "coremask not specified\n");
		eal_usage(prgname);
		return -1;
	}
	if (internal_config.process_type == RTE_PROC_AUTO){
		internal_config.process_type = eal_proc_type_detect();
	}
	if (internal_config.process_type == RTE_PROC_INVALID){
		RTE_LOG(ERR, EAL, "Invalid process type specified\n");
		eal_usage(prgname);
		return -1;
	}
	if (internal_config.process_type == RTE_PROC_PRIMARY &&
			internal_config.force_nchannel == 0) {
		RTE_LOG(ERR, EAL, "Number of memory channels (-n) not specified\n");
		eal_usage(prgname);
		return -1;
	}
	if (index(internal_config.hugefile_prefix,'%') != NULL){
		RTE_LOG(ERR, EAL, "Invalid char, '%%', in '"OPT_FILE_PREFIX"' option\n");
		eal_usage(prgname);
		return -1;
	}

	if (blacklist_index > 0)
		rte_eal_pci_set_blacklist(eal_dev_blacklist, blacklist_index);

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 0; /* reset getopt lib */
	return ret;
}

/* Launch threads, called at application init(). */
int
rte_eal_init(int argc, char **argv)
{
	int i, fctret, ret;
	pthread_t thread_id;
	const char *logid;
	struct shared_driver *solib = NULL;

	logid = strrchr(argv[0], '/');
	logid = strdup(logid ? logid + 1: argv[0]);

	thread_id = pthread_self();

	if (rte_eal_log_early_init() < 0)
		rte_panic("Cannot init early logs\n");

	fctret = eal_parse_args(argc, argv);
	if (fctret < 0)
		exit(1);

	if (eal_hugepage_info_init() < 0)
		rte_panic("Cannot get hugepage information\n");

	if (internal_config.memory == 0) {
		if (internal_config.no_hugetlbfs)
			internal_config.memory = MEMSIZE_IF_NO_HUGE_PAGE;
		else
			internal_config.memory = eal_get_hugepage_mem_size();
	}

	if (internal_config.vmware_tsc_map == 1) {
#ifdef RTE_LIBRTE_EAL_VMWARE_TSC_MAP_SUPPORT
		rte_cycles_vmware_tsc_map = 1;
		RTE_LOG (DEBUG, EAL, "Using VMware TSC map, "
				"you must have monitor_control.pseudo_perfctr = TRUE\n");
#else
		RTE_LOG (WARNING, EAL, "Ignoring --vmware-tsc-map because "
				"RTE_LIBRTE_EAL_VMWARE_TSC_MAP_SUPPORT is not set\n");
#endif
	} else {
		RTE_LOG (DEBUG, EAL, "Using native RDTSC\n");
	}
	rte_srand(rte_rdtsc());
	rte_config_init();

	if (rte_eal_cpu_init() < 0)
		rte_panic("Cannot detect lcores\n");

	if (rte_eal_memory_init() < 0)
		rte_panic("Cannot init memory\n");

	if (rte_eal_memzone_init() < 0)
		rte_panic("Cannot init memzone\n");

	if (rte_eal_tailqs_init() < 0)
		rte_panic("Cannot init tail queues for objects\n");

	if (rte_eal_log_init(logid, internal_config.syslog_facility) < 0)
		rte_panic("Cannot init logs\n");

	if (rte_eal_alarm_init() < 0)
		rte_panic("Cannot init interrupt-handling thread\n");

	if (rte_eal_intr_init() < 0)
		rte_panic("Cannot init interrupt-handling thread\n");

	if (rte_eal_hpet_init() < 0)
		rte_panic("Cannot init HPET\n");

	if (rte_eal_pci_init() < 0)
		rte_panic("Cannot init PCI\n");

	TAILQ_FOREACH(solib, &solib_list, next) {
		solib->lib_handle = dlopen(solib->name, RTLD_NOW);
		if ((solib->lib_handle == NULL) && (solib->name[0] != '/')) {
			/* relative path: try again with "./" prefix */
			char sopath[PATH_MAX];
			snprintf(sopath, sizeof(sopath), "./%s", solib->name);
			solib->lib_handle = dlopen(sopath, RTLD_NOW);
		}
		if (solib->lib_handle == NULL)
			RTE_LOG(WARNING, EAL, "%s\n", dlerror());
	}

	RTE_LOG(DEBUG, EAL, "Master core %u is ready (tid=%x)\n",
		rte_config.master_lcore, (int)thread_id);

	RTE_LCORE_FOREACH_SLAVE(i) {

		/*
		 * create communication pipes between master thread
		 * and children
		 */
		if (pipe(lcore_config[i].pipe_master2slave) < 0)
			rte_panic("Cannot create pipe\n");
		if (pipe(lcore_config[i].pipe_slave2master) < 0)
			rte_panic("Cannot create pipe\n");

		lcore_config[i].state = WAIT;

		/* create a thread for each lcore */
		ret = pthread_create(&lcore_config[i].thread_id, NULL,
				     eal_thread_loop, NULL);
		if (ret != 0)
			rte_panic("Cannot create thread\n");
	}

	eal_thread_init_master(rte_config.master_lcore);

	return fctret;
}

/* get core role */
enum rte_lcore_role_t
rte_eal_lcore_role(unsigned lcore_id)
{
	return (rte_config.lcore_role[lcore_id]);
}

enum rte_proc_type_t
rte_eal_process_type(void)
{
	return (rte_config.process_type);
}

