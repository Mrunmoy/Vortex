#include "RunLoop.h"

#include <cstdio>
#include <thread>

int main()
{
    ms::RunLoop loop;
    loop.init("Example");

    // Start the run loop on a background thread
    std::thread t([&] { loop.run(); });

    // Post work to the run loop thread
    loop.executeOnRunLoop([&] { std::printf("Hello from the run loop thread!\n"); });

    // Post multiple items — they execute in FIFO order
    for (int i = 0; i < 5; ++i)
    {
        loop.executeOnRunLoop([i] { std::printf("  task %d\n", i); });
    }

    // Stop the loop (also posted, so previous tasks finish first)
    loop.executeOnRunLoop([&] {
        std::printf("Stopping...\n");
        loop.stop();
    });

    t.join();
    std::printf("Done.\n");

    return 0;
}
