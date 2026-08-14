#include "server.h"
#include "logger.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <event2/thread.h>
#include <event2/event.h>
#include <stdatomic.h>
#include "worker_pool.h"

static void setup_signals(sigset_t* sigmask) {
    signal(SIGPIPE, SIG_IGN);
    
    sigemptyset(sigmask);
    sigaddset(sigmask, SIGINT);
    sigaddset(sigmask, SIGTERM);
    sigaddset(sigmask, SIGHUP);
    pthread_sigmask(SIG_BLOCK, sigmask, nullptr);
}

static int setup_core_tracking(pthread_t** out_threads, long* out_num_cores) {
    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores <= 0 || num_cores > 64) num_cores = 4;
    *out_num_cores = num_cores;

    *out_threads = calloc((size_t)num_cores, sizeof(pthread_t));

    if (*out_threads == nullptr) {
        LOG_FATAL("Out of memory preparing tracking structures.");
        return -1;
    }
    
    if (server_init_globals((size_t)num_cores) != 0) {
        LOG_FATAL("Initialization of global tracking arrays failed (OOM).");
        free(*out_threads);
        return -1;
    }
    return 0;
}

static int spawn_worker_threads(pthread_t* threads, long num_cores) {
    for (size_t i = 0; i < (size_t)num_cores; ++i) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        // Modulo ensures round-robin affinity if we ever configure num_reactors > num_cores (oversubscription)
        CPU_SET((long)i % num_cores, &cpuset);
        int aff_ret = pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);
        if (aff_ret != 0) {
            LOG_WARN("Could not set CPU affinity for reactor thread %zu (container restriction?). OS will schedule automatically.", i);
        }

        if (pthread_create(&threads[i], &attr, reactor_thread_logic, (void*)i) != 0) {
            LOG_FATAL("Could not bootstrap reactor thread %zu", i);
            pthread_attr_destroy(&attr);
            return -1;
        }
        pthread_attr_destroy(&attr);
    }
    return 0;
}

static void wait_and_shutdown(const sigset_t* sigmask, pthread_t* threads, long num_cores) {
    int caught_sig = 0;

    LOG_INFO("All reactors operational. Waiting for OS signals...");
    
    while (1) {
        sigwait(sigmask, &caught_sig);
        if (caught_sig == SIGHUP) {
            LOG_AUDIT("Caught SIGHUP signal. Hot-reloading configuration...");
            config_reload();
        } else {
            const char* sig_name = "UNKNOWN";
            if (caught_sig == SIGINT) {
                sig_name = "SIGINT";
            } else if (caught_sig == SIGTERM) {
                sig_name = "SIGTERM";
            }
            LOG_INFO("Caught signal %s. Initiating graceful shutdown across all workers...", sig_name);
            break;
        }
    }
    
    server_shutdown_workers();

    for (size_t i = 0; i < (size_t)num_cores; ++i) {
        pthread_join(threads[i], nullptr);
    }

    free(threads);
    server_cleanup_globals();
    LOG_INFO("APIServer system halted safely. Network loops unlinked cleanly.");
    logger_shutdown();
}

int main(void) {
    logger_init();
    
    config_init();

    sigset_t sigmask;
    setup_signals(&sigmask);

    pthread_t* threads = nullptr;
    long num_cores = 0;
    
    if (setup_core_tracking(&threads, &num_cores) != 0) {
        return EXIT_FAILURE;
    }

    LOG_INFO("Spawning Multi-Reactor engine across %ld core-isolated pipes...", num_cores);
    LOG_INFO("Background Async Worker Pool size: %zu threads", worker_pool_get_size());
    LOG_INFO("Server started on http://%s:%d/", SERVER_ADDR, SERVER_PORT);

    if (spawn_worker_threads(threads, num_cores) != 0) {
        return EXIT_FAILURE;
    }

    server_wait_startup_barrier();

    if (server_did_startup_fail()) {
        LOG_FATAL("One or more reactors failed to start. Shutting down immediately.");
        server_shutdown_workers();
        for (size_t i = 0; i < (size_t)num_cores; ++i) {
            pthread_join(threads[i], nullptr);
        }
        free(threads);
        server_cleanup_globals();
        logger_shutdown();
        return EXIT_FAILURE;
    }

    wait_and_shutdown(&sigmask, threads, num_cores);
    return EXIT_SUCCESS;
}
