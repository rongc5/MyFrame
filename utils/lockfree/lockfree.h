#pragma once

// Umbrella header for MyFrame's C++11 concurrent ring queues:
//   - SpscQueue: wait-free bounded single-producer single-consumer ring queue
//   - MpmcQueue: mutex-free CAS-based Vyukov bounded MPMC ring queue
//     (not formally lock-free; see mpmc_queue.h for progress-guarantee details)
#include "spsc_queue.h"
#include "mpmc_queue.h"
