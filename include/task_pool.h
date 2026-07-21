#pragma once

#include "worker_pool.h"
#include <stddef.h>

/**
 * \file task_pool.h
 * \brief Object Pool (Slab Allocator) for http_task_t structures.
 */

/** \brief Initializes the task pool with a given maximum capacity. \return 0 on success, -1 on failure */
int task_pool_init(size_t pool_size);

/** \brief Gracefully destroys the task pool and frees underlying memory. */
void task_pool_shutdown(void);

/** \brief Leases a zero-initialized http_task_t from the pool, or allocates a new one if exhausted. */
http_task_t* task_pool_alloc(void);

/** \brief Returns a task back to the pool, or frees it if it was dynamically allocated as fallback. */
void task_pool_free(http_task_t* task);
