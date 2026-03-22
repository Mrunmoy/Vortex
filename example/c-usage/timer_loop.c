#include "vortex.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#error "timer_loop.c currently requires a Unix-like backend"
#else
#include <unistd.h>
#endif

struct context
{
    vortex_t loop;
    int read_fd;
    int write_fd;
};

static void on_source(void *user_data)
{
    struct context *ctx = (struct context *)user_data;
    char byte = 0;
    (void)read(ctx->read_fd, &byte, 1);
    printf("source callback fired\n");
    vortex_stop(ctx->loop);
}

static void post_write(void *user_data)
{
    struct context *ctx = (struct context *)user_data;
    char byte = 1;
    (void)write(ctx->write_fd, &byte, 1);
    printf("posted write\n");
}

int main(void)
{
    int fds[2] = {-1, -1};
    vortex_t loop = NULL;
    struct context ctx;

    if (pipe(fds) != 0)
    {
        perror("pipe");
        return 1;
    }

    if (vortex_create(&loop) != VORTEX_SUCCESS)
    {
        fprintf(stderr, "vortex_create failed\n");
        return 1;
    }
    if (vortex_init(loop, "c-example") != VORTEX_SUCCESS)
    {
        fprintf(stderr, "vortex_init failed\n");
        vortex_destroy(loop);
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.loop = loop;
    ctx.read_fd = fds[0];
    ctx.write_fd = fds[1];

    if (vortex_add_source(loop, fds[0], on_source, &ctx) != VORTEX_SUCCESS)
    {
        fprintf(stderr, "vortex_add_source failed\n");
        vortex_destroy(loop);
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    if (vortex_post(loop, post_write, &ctx) != VORTEX_SUCCESS)
    {
        fprintf(stderr, "vortex_post failed\n");
        vortex_destroy(loop);
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    printf("running loop %s\n", vortex_name(loop));
    if (vortex_run(loop) != VORTEX_SUCCESS)
    {
        fprintf(stderr, "vortex_run failed\n");
        vortex_destroy(loop);
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    vortex_remove_source(loop, fds[0]);
    vortex_destroy(loop);
    close(fds[0]);
    close(fds[1]);
    printf("done\n");
    return 0;
}
