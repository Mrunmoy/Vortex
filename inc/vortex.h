#ifndef VORTEX_H
#define VORTEX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VORTEX_SUCCESS 0
#define VORTEX_ERR_INVALID_ARGUMENT -1
#define VORTEX_ERR_ALREADY_INIT -2
#define VORTEX_ERR_NOT_INIT -3

typedef struct vortex_impl *vortex_t;

typedef void (*vortex_source_cb)(void *user_data);
typedef void (*vortex_callable_cb)(void *user_data);
typedef void (*vortex_timer_cb)(void *user_data);

int vortex_create(vortex_t *out);
int vortex_init(vortex_t loop, const char *name);
int vortex_run(vortex_t loop);
void vortex_stop(vortex_t loop);
void vortex_destroy(vortex_t loop);

int vortex_post(vortex_t loop, vortex_callable_cb cb, void *user_data);

int vortex_add_source(vortex_t loop, int fd, vortex_source_cb cb, void *user_data);
void vortex_remove_source(vortex_t loop, int fd);

// Schedule a timer. Returns timer id via `out_id` (>= 1) on success.
// If `repeating` is non-zero, the timer fires repeatedly.
int vortex_add_timer(vortex_t loop, uint32_t interval_ms, int repeating,
                     vortex_timer_cb cb, void *user_data, uint64_t *out_id);

// Cancel a previously scheduled timer.
void vortex_remove_timer(vortex_t loop, uint64_t timer_id);

int vortex_is_running(vortex_t loop);
const char *vortex_name(vortex_t loop);

void vortex_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* VORTEX_H */
