/*
 * Copyright 2010-2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */
#include <crm_internal.h>
#include <pacemaker.h>

#include <sys/utsname.h>
#include <sys/stat.h>           /* for calls to stat() */
#include <libgen.h>             /* For basename() and dirname() */

#include <sys/types.h>
#include <pwd.h>                /* For getpwname() */

#include <corosync/hdb.h>
#include <corosync/cfg.h>
#include <corosync/cpg.h>
#if HAVE_CONFDB
#  include <corosync/confdb.h>
#endif

#include <crm/cluster/internal.h>
#include <crm/common/ipc.h>     /* for crm_ipc_is_authentic_process */
#include <crm/common/mainloop.h>

#include <crm/common/ipc_internal.h>  /* PCMK__SPECIAL_PID* */

#if SUPPORT_CMAN
#  include <libcman.h>
#endif

#if HAVE_CMAP
#  include <corosync/cmap.h>
#endif

enum cluster_type_e stack = pcmk_cluster_unknown;
static corosync_cfg_handle_t cfg_handle;

/* =::=::=::= CFG - Shutdown stuff =::=::=::= */

static void
cfg_shutdown_callback(corosync_cfg_handle_t h, corosync_cfg_shutdown_flags_t flags)
{
    crm_info("Corosync wants to shut down: %s",
             (flags == COROSYNC_CFG_SHUTDOWN_FLAG_IMMEDIATE) ? "immediate" :
             (flags == COROSYNC_CFG_SHUTDOWN_FLAG_REGARDLESS) ? "forced" : "optional");

    /* Never allow corosync to shut down while we're running */
    corosync_cfg_replyto_shutdown(h, COROSYNC_CFG_SHUTDOWN_FLAG_NO);
}

static corosync_cfg_callbacks_t cfg_callbacks = {
    .corosync_cfg_shutdown_callback = cfg_shutdown_callback,
};

static int
pcmk_cfg_dispatch(gpointer user_data)
{
    corosync_cfg_handle_t *handle = (corosync_cfg_handle_t *) user_data;
    cs_error_t rc = corosync_cfg_dispatch(*handle, CS_DISPATCH_ALL);

    if (rc != CS_OK) {
        return -1;
    }
    return 0;
}

static void
cfg_connection_destroy(gpointer user_data)
{
    crm_err("Connection destroyed");
    cfg_handle = 0;

    pcmk_shutdown(SIGTERM);
}

gboolean
cluster_disconnect_cfg(void)
{
    if (cfg_handle) {
        corosync_cfg_finalize(cfg_handle);
        cfg_handle = 0;
    }

    pcmk_shutdown(SIGTERM);
    return TRUE;
}

#define cs_repeat(counter, max, code) do {		\
	code;						\
	if(rc == CS_ERR_TRY_AGAIN || rc == CS_ERR_QUEUE_FULL) {  \
	    counter++;					\
	    crm_debug("Retrying operation after %ds", counter);	\
	    sleep(counter);				\
	} else {                                        \
            break;                                      \
	}						\
    } while(counter < max)

gboolean
cluster_connect_cfg(uint32_t * nodeid)
{
    cs_error_t rc;
    int fd = -1, retries = 0, rv;
    uid_t found_uid = 0;
    gid_t found_gid = 0;
    pid_t found_pid = 0;

    static struct mainloop_fd_callbacks cfg_fd_callbacks = {
        .dispatch = pcmk_cfg_dispatch,
        .destroy = cfg_connection_destroy,
    };

    cs_repeat(retries, 30, rc = corosync_cfg_initialize(&cfg_handle, &cfg_callbacks));

    if (rc != CS_OK) {
        crm_err("corosync cfg init: %s (%d)", cs_strerror(rc), rc);
        return FALSE;
    }

    rc = corosync_cfg_fd_get(cfg_handle, &fd);
    if (rc != CS_OK) {
        crm_err("corosync cfg fd_get: %s (%d)", cs_strerror(rc), rc);
        goto bail;
    }

    /* CFG provider run as root (in given user namespace, anyway)? */
    if (!(rv = crm_ipc_is_authentic_process(fd, (uid_t) 0,(gid_t) 0, &found_pid,
                                            &found_uid, &found_gid))) {
        crm_err("CFG provider is not authentic:"
                " process %lld (uid: %lld, gid: %lld)",
                (long long) PCMK__SPECIAL_PID_AS_0(found_pid),
                (long long) found_uid, (long long) found_gid);
        goto bail;
    } else if (rv < 0) {
        crm_err("Could not verify authenticity of CFG provider: %s (%d)",
                strerror(-rv), -rv);
        goto bail;
    }

    retries = 0;
    cs_repeat(retries, 30, rc = corosync_cfg_local_get(cfg_handle, nodeid));

    if (rc != CS_OK) {
        crm_err("corosync cfg local_get error %d", rc);
        goto bail;
    }

    crm_debug("Our nodeid: %d", *nodeid);
    mainloop_add_fd("corosync-cfg", G_PRIORITY_DEFAULT, fd, &cfg_handle, &cfg_fd_callbacks);

    return TRUE;

  bail:
    corosync_cfg_finalize(cfg_handle);
    return FALSE;
}

/* =::=::=::= Configuration =::=::=::= */
#if HAVE_CONFDB
static int
get_config_opt(confdb_handle_t config,
               hdb_handle_t object_handle, const char *key, char **value, const char *fallback)
{
    size_t len = 0;
    char *env_key = NULL;
    const char *env_value = NULL;
    char buffer[256];

    if (*value) {
        free(*value);
        *value = NULL;
    }

    if (object_handle > 0) {
        if (CS_OK == confdb_key_get(config, object_handle, key, strlen(key), &buffer, &len)) {
            *value = strdup(buffer);
        }
    }

    if (*value) {
        crm_info("Found '%s' for option: %s", *value, key);
        return 0;
    }

    env_key = crm_concat("HA", key, '_');
    env_value = getenv(env_key);
    free(env_key);

    if (*value) {
        crm_info("Found '%s' in ENV for option: %s", *value, key);
        *value = strdup(env_value);
        return 0;
    }

    if (fallback) {
        crm_info("Defaulting to '%s' for option: %s", fallback, key);
        *value = strdup(fallback);

    } else {
        crm_info("No default for option: %s", key);
    }

    return -1;
}

static confdb_handle_t
config_find_init(confdb_handle_t config)
{
    cs_error_t rc = CS_OK;
    confdb_handle_t local_handle = OBJECT_PARENT_HANDLE;

    rc = confdb_object_find_start(config, local_handle);
    if (rc == CS_OK) {
        return local_handle;
    } else {
        crm_err("Couldn't create search context: %d", rc);
    }
    return 0;
}

static hdb_handle_t
config_find_next(confdb_handle_t config, const char *name, confdb_handle_t top_handle)
{
    cs_error_t rc = CS_OK;
    hdb_handle_t local_handle = 0;

    if (top_handle == 0) {
        crm_err("Couldn't search for %s: no valid context", name);
        return 0;
    }

    crm_trace("Searching for %s in " HDB_X_FORMAT, name, top_handle);
    rc = confdb_object_find(config, top_handle, name, strlen(name), &local_handle);
    if (rc != CS_OK) {
        crm_info("No additional configuration supplied for: %s", name);
        local_handle = 0;
    } else {
        crm_info("Processing additional %s options...", name);
    }
    return local_handle;
}
#else
static int
get_config_opt(uint64_t unused, cmap_handle_t object_handle, const char *key, char **value,
               const char *fallback)
{
    int rc = 0, retries = 0;

    cs_repeat(retries, 5, rc = cmap_get_string(object_handle, key, value));
    if (rc != CS_OK) {
        crm_trace("Search for %s failed %d, defaulting to %s", key, rc, fallback);
        if (fallback) {
            *value = strdup(fallback);
        } else {
            *value = NULL;
        }
    }
    crm_trace("%s: %s", key, *value);
    return rc;
}

#endif

#if HAVE_CONFDB
#  define KEY_PREFIX ""
#elif HAVE_CMAP
#  define KEY_PREFIX "logging."
#endif

gboolean
mcp_read_config(void)
{
    cs_error_t rc = CS_OK;
    int retries = 0;

    const char *const_value = NULL;

#if HAVE_CONFDB
    char *value = NULL;
    confdb_handle_t config = 0;
    confdb_handle_t top_handle = 0;
    hdb_handle_t local_handle;
    static confdb_callbacks_t callbacks = { };

    do {
        rc = confdb_initialize(&config, &callbacks);
        if (rc != CS_OK) {
            retries++;
            printf("confdb connection setup failed: %s.  Retrying in %ds\n", ais_error2text(rc), retries);
            crm_info("confdb connection setup failed: %s.  Retrying in %ds", ais_error2text(rc), retries);
            sleep(retries);

        } else {
            break;
        }
    } while (retries < 5);

#elif HAVE_CMAP
    cmap_handle_t local_handle;
    uint64_t config = 0;
    int fd = -1;
    uid_t found_uid = 0;
    gid_t found_gid = 0;
    pid_t found_pid = 0;
    int rv;

    /* There can be only one (possibility if confdb isn't around) */
    do {
        rc = cmap_initialize(&local_handle);
        if (rc != CS_OK) {
            retries++;
            printf("cmap connection setup failed: %s.  Retrying in %ds\n", cs_strerror(rc), retries);
            crm_info("cmap connection setup failed: %s.  Retrying in %ds", cs_strerror(rc), retries);
            sleep(retries);

        } else {
            break;
        }

    } while (retries < 5);
#endif

    if (rc != CS_OK) {
        printf("Could not connect to Cluster Configuration Database API, error %d\n", rc);
        crm_warn("Could not connect to Cluster Configuration Database API, error %d", rc);
        return FALSE;
    }

    rc = cmap_fd_get(local_handle, &fd);
    if (rc != CS_OK) {
        crm_err("Could not obtain the CMAP API connection: %s (%d)",
                cs_strerror(rc), rc);
        cmap_finalize(local_handle);
        return FALSE;
    }

    /* CMAP provider run as root (in given user namespace, anyway)? */
    if (!(rv = crm_ipc_is_authentic_process(fd, (uid_t) 0,(gid_t) 0, &found_pid,
                                            &found_uid, &found_gid))) {
        crm_err("CMAP provider is not authentic:"
                " process %lld (uid: %lld, gid: %lld)",
                (long long) PCMK__SPECIAL_PID_AS_0(found_pid),
                (long long) found_uid, (long long) found_gid);
        cmap_finalize(local_handle);
        return FALSE;
    } else if (rv < 0) {
        crm_err("Could not verify authenticity of CMAP provider: %s (%d)",
                strerror(-rv), -rv);
        cmap_finalize(local_handle);
        return FALSE;
    }

    stack = get_cluster_type();
    crm_info("Reading configure for stack: %s", name_for_cluster_type(stack));

    /* =::=::= Should we be here =::=::= */
    if (stack == pcmk_cluster_corosync) {
        set_daemon_option("cluster_type", "corosync");
        set_daemon_option("quorum_type", "corosync");

#if HAVE_CONFDB
    } else if (stack == pcmk_cluster_cman) {
        set_daemon_option("cluster_type", "cman");
        set_daemon_option("quorum_type", "cman");
        enable_crmd_as_root(TRUE);

    } else if (stack == pcmk_cluster_classic_ais) {
        set_daemon_option("cluster_type", "openais");
        set_daemon_option("quorum_type", "pcmk");

        /* Look for a service block to indicate our plugin is loaded */
        top_handle = config_find_init(config);
        local_handle = config_find_next(config, "service", top_handle);

        while (local_handle) {
            get_config_opt(config, local_handle, "name", &value, NULL);
            if (safe_str_eq("pacemaker", value)) {
                get_config_opt(config, local_handle, "ver", &value, "0");
                if (safe_str_eq(value, "1")) {
                    get_config_opt(config, local_handle, "use_logd", &value, "no");
                    set_daemon_option("use_logd", value);
                    set_daemon_option("LOGD", value);

                    get_config_opt(config, local_handle, "use_mgmtd", &value, "no");
                    enable_mgmtd(crm_is_true(value));

                } else {
                    crm_err("We can only start Pacemaker from init if using version 1"
                            " of the Pacemaker plugin for Corosync.  Terminating.");
                    crm_exit(DAEMON_RESPAWN_STOP);
                }
                break;
            }
            local_handle = config_find_next(config, "service", top_handle);
        }
        free(value);

#endif
    } else {
        crm_err("Unsupported stack type: %s", name_for_cluster_type(stack));
        return FALSE;
    }

#if HAVE_CONFDB
    top_handle = config_find_init(config);
    local_handle = config_find_next(config, "logging", top_handle);
#endif

    /* =::=::= Logging =::=::= */
    if (daemon_option("debug")) {
        /* Syslog logging is already setup by crm_log_init() */

    } else {
        /* Check corosync */
        char *debug_enabled = NULL;

        get_config_opt(config, local_handle, KEY_PREFIX "debug", &debug_enabled, "off");

        if (crm_is_true(debug_enabled)) {
            set_daemon_option("debug", "1");
            if (get_crm_log_level() < LOG_DEBUG) {
                set_crm_log_level(LOG_DEBUG);
            }

        } else {
            set_daemon_option("debug", "0");
        }

        free(debug_enabled);
    }

    /* If the user didn't explicitly configure a Pacemaker log file, check
     * whether they configured a heartbeat or corosync log file, and use that.
     *
     * @COMPAT This should all go away, and we should just rely on the logging
     * set up by crm_log_init(). We aren't doing this yet because it is a
     * significant user-visible change that will need to be publicized.
     */
    const_value = daemon_option("debugfile");
    if (daemon_option("logfile")) {
        /* File logging is already setup by crm_log_init() */

    } else if(const_value) {
        /* From when we cared what options heartbeat used */
        set_daemon_option("logfile", const_value);
        crm_add_logfile(const_value);

    } else {
        /* Check corosync */
        char *logfile = NULL;
        char *logfile_enabled = NULL;

        get_config_opt(config, local_handle, KEY_PREFIX "to_logfile", &logfile_enabled, "on");
        get_config_opt(config, local_handle, KEY_PREFIX "logfile", &logfile, "/var/log/pacemaker.log");

        if (crm_is_true(logfile_enabled) == FALSE) {
            crm_trace("File logging disabled in corosync");

        } else if (crm_add_logfile(logfile)) {
            set_daemon_option("logfile", logfile);

        } else {
            crm_err("Couldn't create logfile: %s", logfile);
            set_daemon_option("logfile", "none");
        }

        free(logfile);
        free(logfile_enabled);
    }

    if (daemon_option("logfacility")) {
        /* Syslog logging is already setup by crm_log_init() */

    } else {
        /* Check corosync */
        char *syslog_enabled = NULL;
        char *syslog_facility = NULL;

        get_config_opt(config, local_handle, KEY_PREFIX "to_syslog", &syslog_enabled, "on");
        get_config_opt(config, local_handle, KEY_PREFIX "syslog_facility", &syslog_facility, "daemon");

        if (crm_is_true(syslog_enabled) == FALSE) {
            qb_log_ctl(QB_LOG_SYSLOG, QB_LOG_CONF_ENABLED, QB_FALSE);
            set_daemon_option("logfacility", "none");

        } else {
            qb_log_ctl(QB_LOG_SYSLOG, QB_LOG_CONF_FACILITY, qb_log_facility2int(syslog_facility));
            qb_log_ctl(QB_LOG_SYSLOG, QB_LOG_CONF_ENABLED, QB_TRUE);
            set_daemon_option("logfacility", syslog_facility);
        }

        free(syslog_enabled);
        free(syslog_facility);
    }

    const_value = daemon_option("logfacility");
    if (const_value) {
        /* cluster-glue module needs HA_LOGFACILITY */
        setenv("HA_LOGFACILITY", const_value, 1);
    }

#if HAVE_CONFDB
    confdb_finalize(config);
#elif HAVE_CMAP
    if(local_handle){
        gid_t gid = 0;
        if (crm_user_lookup(CRM_DAEMON_USER, NULL, &gid) < 0) {
            crm_warn("Could not authorize group with corosync " CRM_XS
                     " No group found for user %s", CRM_DAEMON_USER);

        } else {
            char key[PATH_MAX];
            snprintf(key, PATH_MAX, "uidgid.gid.%u", gid);
            rc = cmap_set_uint8(local_handle, key, 1);
            if (rc != CS_OK) {
                crm_warn("Could not authorize group with corosync "CRM_XS
                         " group=%u rc=%d (%s)", gid, rc, ais_error2text(rc));
            }
        }
    }
    cmap_finalize(local_handle);
#endif

    return TRUE;
}
